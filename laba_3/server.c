#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <pthread.h>
#include "message.h"

#define MAX_CLIENTS 64
#define THREAD_POOL_SIZE 10

typedef struct {
    int sock;
    char nickname[64];
    char ip[INET_ADDRSTRLEN];
    int port;
    int active;
} Client;

static Client clients[MAX_CLIENTS];
static pthread_mutex_t clients_mutex = PTHREAD_MUTEX_INITIALIZER;

typedef struct {
    int sockets[1024];
    int head, tail, count;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
} ConnectionQueue;

static ConnectionQueue conn_queue = {
    .head = 0, .tail = 0, .count = 0,
    .mutex = PTHREAD_MUTEX_INITIALIZER,
    .cond = PTHREAD_COND_INITIALIZER
};

void *worker_thread(void *arg);
void *handle_client(void *arg);
void broadcast_message(const char *sender, const char* ip, int port, const char *text, int excluded_sock);
int add_client(int sock, const char *nickname, const char *ip, int port);
void remove_client(int sock);
Client *find_client(int sock);
int get_count_active();

int get_count_active() {
    pthread_mutex_lock(&clients_mutex);
    int count = 0;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].active) {
            count++;
        }
    }
    pthread_mutex_unlock(&clients_mutex);
    return count;
}

void queue_push(int sock) {
    pthread_mutex_lock(&conn_queue.mutex);
    while (conn_queue.count >= 1024)
        pthread_cond_wait(&conn_queue.cond, &conn_queue.mutex);
    conn_queue.sockets[conn_queue.tail] = sock;
    conn_queue.tail = (conn_queue.tail + 1) % 1024;
    conn_queue.count++;
    pthread_cond_signal(&conn_queue.cond);
    pthread_mutex_unlock(&conn_queue.mutex);
}

int queue_pop() {
    pthread_mutex_lock(&conn_queue.mutex);
    while (conn_queue.count == 0)
        pthread_cond_wait(&conn_queue.cond, &conn_queue.mutex);
    int sock = conn_queue.sockets[conn_queue.head];
    conn_queue.head = (conn_queue.head + 1) % 1024;
    conn_queue.count--;
    pthread_mutex_unlock(&conn_queue.mutex);
    return sock;
}

void broadcast_message(const char *sender, const char *ip, int port, const char *text, int excluded_sock) {
    pthread_mutex_lock(&clients_mutex);
    char formatted[2048];
    snprintf(formatted, sizeof(formatted), "%s [%s:%d]: %s", sender, ip, port, text);
    
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].active && clients[i].sock != excluded_sock) {
            send_message(clients[i].sock, MSG_TEXT, formatted, strlen(formatted));
        }
    }
    pthread_mutex_unlock(&clients_mutex);
}

int add_client(int sock, const char *nickname, const char *ip, int port) {
    pthread_mutex_lock(&clients_mutex);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (!clients[i].active) {
            clients[i].sock = sock;
            clients[i].active = 1;
            strncpy(clients[i].nickname, nickname, sizeof(clients[i].nickname) - 1);
            strncpy(clients[i].ip, ip, sizeof(clients[i].ip) - 1);
            clients[i].port = port;
            pthread_mutex_unlock(&clients_mutex);
            return 0;
        }
    }
    pthread_mutex_unlock(&clients_mutex);
    return -1;
}

void remove_client(int sock) {
    pthread_mutex_lock(&clients_mutex);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].active && clients[i].sock == sock) {
            clients[i].active = 0;
            close(sock);
            break;
        }
    }
    pthread_mutex_unlock(&clients_mutex);
}

Client *find_client(int sock) {
    pthread_mutex_lock(&clients_mutex);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].active && clients[i].sock == sock) {
            pthread_mutex_unlock(&clients_mutex);
            return &clients[i];
        }
    }
    pthread_mutex_unlock(&clients_mutex);
    return NULL;
}

void *handle_client(void *arg) {
    int sock = *(int*)arg;
    free(arg);
    
    Message msg;
    char client_ip[INET_ADDRSTRLEN];
    int client_port;
    char nickname[64] = "Unknown";
    
    struct sockaddr_in addr;
    socklen_t addr_len = sizeof(addr);
    if (getpeername(sock, (struct sockaddr*)&addr, &addr_len) == 0) {
        inet_ntop(AF_INET, &addr.sin_addr, client_ip, sizeof(client_ip));
        client_port = ntohs(addr.sin_port);
    }
    
    if (recv_message(sock, &msg) < 0 || msg.type != MSG_HELLO) {
        fprintf(stderr, "[ERROR] Expected MSG_HELLO\n");
        close(sock);
        return NULL;
    }
    strncpy(nickname, msg.payload, sizeof(nickname) - 1);
    printf("[INFO] Client connected: %s [%s:%d]\n", nickname, client_ip, client_port);

    if (add_client(sock, nickname, client_ip, client_port) < 0) {
        fprintf(stderr, "[ERROR] Client list full, rejecting %s\n", nickname);
        close(sock);
        return NULL;
    }

    char welcome[128];
    snprintf(welcome, sizeof(welcome), "Welcome %s! Connected clients: %d", 
             nickname, get_count_active());
    send_message(sock, MSG_WELCOME, welcome, strlen(welcome));
    broadcast_message(nickname, client_ip, client_port, "connected", sock);
    
    
    int running = 1;
    while (running) {
        if (recv_message(sock, &msg) < 0) {
            printf("[INFO] Connection lost: %s\n", nickname);
            broadcast_message(nickname, client_ip, client_port, "disconnected", sock);
            break;
        }
        
        switch (msg.type) {
            case MSG_TEXT:
                printf("[%s]: %s\n", nickname, msg.payload);
                broadcast_message(nickname, client_ip, client_port, msg.payload, -1);
                break;
                
            default:
                fprintf(stderr, "[WARN] Unknown message type: %d\n", msg.type);
        }
    }
    
    remove_client(sock);
    return NULL;
}

void *worker_thread(void *arg) {
    (void)arg;
    printf("[THREAD] Worker started\n");
    
    while (1) {
        int client_sock = queue_pop();
        if (client_sock < 0) continue;
        
        pthread_t handler;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        
        int *sock_ptr = malloc(sizeof(int));
        *sock_ptr = client_sock;
        
        if (pthread_create(&handler, &attr, handle_client, sock_ptr) != 0) {
            fprintf(stderr, "[ERROR] Failed to create handler thread\n");
            free(sock_ptr);
            close(client_sock);
        }
        pthread_attr_destroy(&attr);
    }
    return NULL;
}


int main() {
    int server_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (server_sock < 0) {
        perror("socket");
        return 1;
    }
    
    int opt = 1;
    setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    struct sockaddr_in server_addr = {0};
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons(SERVER_PORT);
    
    if (bind(server_sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind");
        close(server_sock);
        return 1;
    }
    
    if (listen(server_sock, 128) < 0) {
        perror("listen");
        close(server_sock);
        return 1;
    }
    
    printf("[SERVER] Listening on port %d...\n", SERVER_PORT);
    
    pthread_t workers[THREAD_POOL_SIZE];
    for (int i = 0; i < THREAD_POOL_SIZE; i++) {
        if (pthread_create(&workers[i], NULL, worker_thread, NULL) != 0) {
            fprintf(stderr, "[ERROR] Failed to create worker %d\n", i);
        }
    }
    
    while (1) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_sock = accept(server_sock, (struct sockaddr*)&client_addr, &client_len);
        
        if (client_sock < 0) {
            perror("accept");
            continue;
        }
        
        printf("[SERVER] New connection accepted, queueing...\n");
        queue_push(client_sock);
    }
    
    close(server_sock);
    return 0;
}