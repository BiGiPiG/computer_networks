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
#define MAX_NICKNAME 32

#define LOG_L4(fmt, ...)  fprintf(stdout, "[Layer 4 - Transport] " fmt "\n", ##__VA_ARGS__)
#define LOG_L5(fmt, ...)  fprintf(stdout, "[Layer 5 - Session] " fmt "\n", ##__VA_ARGS__)
#define LOG_L6(fmt, ...)  fprintf(stdout, "[Layer 6 - Presentation] " fmt "\n", ##__VA_ARGS__)
#define LOG_L7(fmt, ...)  fprintf(stdout, "[Layer 7 - Application] " fmt "\n", ##__VA_ARGS__)

typedef struct {
    int sock;
    char nickname[MAX_NICKNAME];
    char ip[INET_ADDRSTRLEN];
    int port;
    int active;
    int authenticated;
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

void handle_text_message(int client_idx, const char *text) {
    pthread_mutex_lock(&clients_mutex);
    
    char formatted[2048];
    snprintf(formatted, sizeof(formatted), "[%s]: %s", 
             clients[client_idx].nickname, text);
    
    LOG_L7("broadcast: %s", formatted);
    
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].active && clients[i].authenticated && i != client_idx) {
            LOG_L6("serialize MSG_TEXT for %s", clients[i].nickname);
            LOG_L4("send() to %s", clients[i].nickname);
            send_message(clients[i].sock, MSG_TEXT, formatted, strlen(formatted));
        }
    }
    pthread_mutex_unlock(&clients_mutex);
}

void handle_private_message(int sender_idx, const char *payload) {
    // Парсинг: "target:message"
    char *colon = strchr(payload, ':');
    if (!colon) {
        send_message(clients[sender_idx].sock, MSG_ERROR, 
                    "Format: /w <nick> <message>", 29);
        return;
    }
    
    char target_nick[MAX_NICKNAME];
    size_t nick_len = colon - payload;
    if (nick_len >= MAX_NICKNAME) nick_len = MAX_NICKNAME - 1;
    strncpy(target_nick, payload, nick_len);
    target_nick[nick_len] = '\0';
    
    const char *message = colon + 1;
    
    pthread_mutex_lock(&clients_mutex);
    
    int found = -1;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].active && clients[i].authenticated && 
            strcmp(clients[i].nickname, target_nick) == 0) {
            found = i;
            break;
        }
    }
    
    if (found < 0) {
        pthread_mutex_unlock(&clients_mutex);
        LOG_L7("private msg failed: user '%s' not found", target_nick);
        char error[128];
        snprintf(error, sizeof(error), "User '%s' not online", target_nick);
        send_message(clients[sender_idx].sock, MSG_ERROR, error, strlen(error));
        return;
    }
    
    char private_msg[2048];
    snprintf(private_msg, sizeof(private_msg), "[PRIVATE][%s]: %s", 
             clients[sender_idx].nickname, message);
    
    LOG_L7("routing private message to %s", target_nick);
    LOG_L6("serialize MSG_PRIVATE");
    LOG_L4("send() to %s", target_nick);
    send_message(clients[found].sock, MSG_PRIVATE, private_msg, strlen(private_msg));
    
    char confirm[128];
    snprintf(confirm, sizeof(confirm), "[To %s]: %s", target_nick, message);
    send_message(clients[sender_idx].sock, MSG_PRIVATE, confirm, strlen(confirm));
    
    pthread_mutex_unlock(&clients_mutex);
}

void broadcast_system(const char *text, int exclude_sock) {
    pthread_mutex_lock(&clients_mutex);
    
    LOG_L7("broadcast system message: %s", text);
    
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].active && clients[i].authenticated && 
            clients[i].sock != exclude_sock) {
            LOG_L6("serialize MSG_SERVER_INFO");
            LOG_L4("send() system msg to %s", clients[i].nickname);
            send_message(clients[i].sock, MSG_SERVER_INFO, text, strlen(text));
        }
    }
    pthread_mutex_unlock(&clients_mutex);
}

void *handle_client(void *arg) {
    int sock = *(int*)arg;
    free(arg);
    
    Message msg;
    char client_ip[INET_ADDRSTRLEN];
    int client_port;
    char nickname[MAX_NICKNAME] = "";
    
    struct sockaddr_in addr;
    socklen_t addr_len = sizeof(addr);
    if (getpeername(sock, (struct sockaddr*)&addr, &addr_len) == 0) {
        inet_ntop(AF_INET, &addr.sin_addr, client_ip, sizeof(client_ip));
        client_port = ntohs(addr.sin_port);
    }
    
    printf("[SERVER] Client connected from %s:%d\n", client_ip, client_port);
    LOG_L4("accept() new connection");
    
    LOG_L4("recv() waiting for MSG_AUTH");
    if (recv_message(sock, &msg) < 0) {
        LOG_L6("deserialize failed");
        close(sock);
        return NULL;
    }
    
    LOG_L6("deserialize Message, type=%d", msg.type);
    
    if (msg.type != MSG_AUTH) {
        LOG_L7("reject: expected MSG_AUTH, got %d", msg.type);
        send_message(sock, MSG_ERROR, "Authentication required", 21);
        close(sock);
        return NULL;
    }
    
    strncpy(nickname, msg.payload, MAX_NICKNAME - 1);
    nickname[MAX_NICKNAME - 1] = '\0';

    if (strlen(nickname) == 0) {
        LOG_L5("auth failed: empty nickname");
        send_message(sock, MSG_ERROR, "Nickname cannot be empty", 24);
        close(sock);
        return NULL;
    }
    
    pthread_mutex_lock(&clients_mutex);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].active && strcmp(clients[i].nickname, nickname) == 0) {
            pthread_mutex_unlock(&clients_mutex);
            LOG_L5("auth failed: nickname '%s' already taken", nickname);
            send_message(sock, MSG_ERROR, "Nickname already in use", 23);
            close(sock);
            return NULL;
        }
    }
    
    int client_idx = -1;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (!clients[i].active) {
            client_idx = i;
            break;
        }
    }
    
    if (client_idx < 0) {
        pthread_mutex_unlock(&clients_mutex);
        send_message(sock, MSG_ERROR, "Server full", 11);
        close(sock);
        return NULL;
    }
    
    clients[client_idx].sock = sock;
    clients[client_idx].active = 1;
    clients[client_idx].authenticated = 1;
    strncpy(clients[client_idx].nickname, nickname, MAX_NICKNAME - 1);
    strncpy(clients[client_idx].ip, client_ip, sizeof(client_ip) - 1);
    clients[client_idx].port = client_port;
    pthread_mutex_unlock(&clients_mutex);
    
    LOG_L5("authentication success: %s", nickname);
    LOG_L7("handle MSG_AUTH -> welcome client");
    
    char welcome[256];
    snprintf(welcome, sizeof(welcome), "Welcome %s! Connected: %d", 
             nickname, get_count_active());
    
    LOG_L7("prepare welcome message");
    LOG_L6("serialize MSG_WELCOME");
    LOG_L4("send() welcome");
    send_message(sock, MSG_WELCOME, welcome, strlen(welcome));
    
    char sys_msg[256];
    snprintf(sys_msg, sizeof(sys_msg), "User [%s] connected", nickname);
    broadcast_system(sys_msg, sock);
    
    printf("[INFO] Client authenticated: %s [%s:%d]\n", nickname, client_ip, client_port);
    
    int running = 1;
    while (running) {
        LOG_L4("recv() waiting for message");
        if (recv_message(sock, &msg) < 0) {
            printf("[INFO] Connection lost: %s\n", nickname);
            break;
        }
        
        LOG_L6("deserialize Message, type=%d", msg.type);
        
        if (!clients[client_idx].authenticated) {
            LOG_L5("reject: client not authenticated");
            continue;
        }
        
        switch (msg.type) {
            case MSG_TEXT:
                LOG_L7("handle MSG_TEXT -> broadcast");
                handle_text_message(client_idx, msg.payload);
                break;
                
            case MSG_PRIVATE:
                LOG_L7("handle MSG_PRIVATE -> direct routing");
                handle_private_message(client_idx, msg.payload);
                break;
                
            case MSG_PING:
                LOG_L7("handle MSG_PING -> send PONG");
                send_message(sock, MSG_PONG, "pong", 4);
                break;
                
            case MSG_BYE:
                LOG_L7("handle MSG_BYE -> disconnect");
                running = 0;
                break;
                
            default:
                LOG_L7("unknown message type: %d", msg.type);
                send_message(sock, MSG_ERROR, "Unknown command", 15);
        }
    }
    
    snprintf(sys_msg, sizeof(sys_msg), "User [%s] disconnected", nickname);
    broadcast_system(sys_msg, sock);
    
    remove_client(sock);
    LOG_L5("session terminated: %s", nickname);
    
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