#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <pthread.h>
#include <time.h>
#include <sys/stat.h>
#include "message.h"

#define MAX_CLIENTS         64
#define THREAD_POOL_SIZE    10
#define MAX_OFFLINE_MSGS    100

typedef struct {
    int sock;
    char nickname[MAX_NAME];
    char ip[INET_ADDRSTRLEN];
    int port;
    int active;
    int authenticated;
} Client;

typedef struct {
    char sender[MAX_NAME];
    char receiver[MAX_NAME];
    char text[MAX_PAYLOAD];
    time_t timestamp;
    uint32_t msg_id;
    int delivered;
} OfflineMsg;

typedef struct {
    int sockets[1024];
    int head, tail, count;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
} ConnectionQueue;

static Client clients[MAX_CLIENTS];
static pthread_mutex_t clients_mutex = PTHREAD_MUTEX_INITIALIZER;

static OfflineMsg offline_queue[MAX_OFFLINE_MSGS];
static int offline_count = 0;
static pthread_mutex_t offline_mutex = PTHREAD_MUTEX_INITIALIZER;

static ConnectionQueue conn_queue = {
    .head = 0, .tail = 0, .count = 0,
    .mutex = PTHREAD_MUTEX_INITIALIZER,
    .cond = PTHREAD_COND_INITIALIZER
};

static uint32_t g_msg_counter = 1;
static pthread_mutex_t msg_counter_mutex = PTHREAD_MUTEX_INITIALIZER;


int send_message_ex(int sock, const MessageEx *msg);
int recv_message_ex(int sock, MessageEx *msg);
int save_to_history(const MessageEx *msg, int delivered, int is_offline);
int load_last_history(int count, MessageEx *out_msgs, int max_count);
void log_tcp_ip(const char *direction, const char *app_msg,
                const char *src_ip, const char *dst_ip, int src_port, int dst_port);

void *worker_thread(void *arg);
void *handle_client(void *arg);
int add_client(int sock, const char *nickname, const char *ip, int port);
void remove_client(int sock);
Client *find_client_by_nickname(const char *nickname);
int get_count_active(void);
void add_to_offline_queue(const char *sender, const char *receiver, 
                          const char *text, uint32_t msg_id);
void deliver_offline_messages(const char *nickname, int client_sock);
void handle_text_message(int client_idx, const MessageEx *msg);
void handle_private_message(int sender_idx, const MessageEx *msg);
void handle_history_request(int client_sock, int count, Client *sender_client);
void handle_list_request(int client_sock, Client *sender);
void broadcast_system(const char *text, int exclude_sock);
uint32_t generate_msg_id(void);
const char *type_to_string(uint8_t type);


uint32_t generate_msg_id(void) {
    pthread_mutex_lock(&msg_counter_mutex);
    uint32_t id = g_msg_counter++;
    pthread_mutex_unlock(&msg_counter_mutex);
    return id;
}

const char *type_to_string(uint8_t type) {
    switch (type) {
        case MSG_TEXT: return "MSG_TEXT";
        case MSG_PRIVATE: return "MSG_PRIVATE";
        case MSG_SERVER_INFO: return "MSG_SERVER_INFO";
        case MSG_WELCOME: return "MSG_WELCOME";
        case MSG_HISTORY_DATA: return "MSG_HISTORY_DATA";
        default: return "MSG_UNKNOWN";
    }
}

void add_to_offline_queue(const char *sender, const char *receiver, 
                          const char *text, uint32_t msg_id) {
    pthread_mutex_lock(&offline_mutex);
    if (offline_count < MAX_OFFLINE_MSGS) {
        OfflineMsg *om = &offline_queue[offline_count++];
        strncpy(om->sender, sender, MAX_NAME - 1);
        om->sender[MAX_NAME - 1] = '\0';
        strncpy(om->receiver, receiver, MAX_NAME - 1);
        om->receiver[MAX_NAME - 1] = '\0';
        strncpy(om->text, text, MAX_PAYLOAD - 1);
        om->text[MAX_PAYLOAD - 1] = '\0';
        om->timestamp = time(NULL);
        om->msg_id = msg_id;
        om->delivered = 0;
    }
    pthread_mutex_unlock(&offline_mutex);
}

void deliver_offline_messages(const char *nickname, int client_sock) {
    pthread_mutex_lock(&offline_mutex);
    
    for (int i = 0; i < offline_count; ) {
        if (strcmp(offline_queue[i].receiver, nickname) == 0 && !offline_queue[i].delivered) {
            MessageEx msg = {0};
            msg.type = MSG_PRIVATE;
            msg.msg_id = offline_queue[i].msg_id;
            msg.timestamp = offline_queue[i].timestamp;
            strncpy(msg.sender, offline_queue[i].sender, MAX_NAME - 1);
            msg.sender[MAX_NAME - 1] = '\0';
            strncpy(msg.receiver, offline_queue[i].receiver, MAX_NAME - 1);
            msg.receiver[MAX_NAME - 1] = '\0';
            
            const char *prefix = "[OFFLINE] ";
            size_t prefix_len = strlen(prefix);
            size_t max_text = MAX_PAYLOAD - prefix_len - 1;
            size_t text_len = strlen(offline_queue[i].text);
            if (text_len > max_text) text_len = max_text;
            
            memcpy(msg.payload, prefix, prefix_len);
            memcpy(msg.payload + prefix_len, offline_queue[i].text, text_len);
            msg.payload[prefix_len + text_len] = '\0';
            msg.length = prefix_len + text_len;
            
            log_tcp_ip("SEND", "deliver offline MSG_PRIVATE", 
                      "127.0.0.1", "127.0.0.1", SERVER_PORT, 0);
            send_message_ex(client_sock, &msg);
            
            offline_queue[i].delivered = 1;
            
            for (int j = i; j < offline_count - 1; j++) {
                offline_queue[j] = offline_queue[j + 1];
            }
            offline_count--;
        } else {
            i++;
        }
    }
    pthread_mutex_unlock(&offline_mutex);
}

int get_count_active(void) {
    pthread_mutex_lock(&clients_mutex);
    int count = 0;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].active) count++;
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

int queue_pop(void) {
    pthread_mutex_lock(&conn_queue.mutex);
    while (conn_queue.count == 0)
        pthread_cond_wait(&conn_queue.cond, &conn_queue.mutex);
    int sock = conn_queue.sockets[conn_queue.head];
    conn_queue.head = (conn_queue.head + 1) % 1024;
    conn_queue.count--;
    pthread_mutex_unlock(&conn_queue.mutex);
    return sock;
}

int add_client(int sock, const char *nickname, const char *ip, int port) {
    pthread_mutex_lock(&clients_mutex);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (!clients[i].active) {
            clients[i].sock = sock;
            clients[i].active = 1;
            clients[i].authenticated = 0;
            strncpy(clients[i].nickname, nickname, MAX_NAME - 1);
            clients[i].nickname[MAX_NAME - 1] = '\0';
            strncpy(clients[i].ip, ip, sizeof(clients[i].ip) - 1);
            clients[i].ip[sizeof(clients[i].ip) - 1] = '\0';
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
            clients[i].authenticated = 0;
            close(sock);
            break;
        }
    }
    pthread_mutex_unlock(&clients_mutex);
}

Client *find_client_by_nickname(const char *nickname) {
    pthread_mutex_lock(&clients_mutex);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].active && strcmp(clients[i].nickname, nickname) == 0) {
            pthread_mutex_unlock(&clients_mutex);
            return &clients[i];
        }
    }
    pthread_mutex_unlock(&clients_mutex);
    return NULL;
}

void broadcast_system(const char *text, int exclude_sock) {
    pthread_mutex_lock(&clients_mutex);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].active && clients[i].authenticated && clients[i].sock != exclude_sock) {
            MessageEx msg = {0};
            msg.type = MSG_SERVER_INFO;
            msg.msg_id = generate_msg_id();
            msg.timestamp = time(NULL);
            strncpy(msg.payload, text, MAX_PAYLOAD - 1);
            msg.payload[MAX_PAYLOAD - 1] = '\0';
            msg.length = strlen(msg.payload);
            send_message_ex(clients[i].sock, &msg);
        }
    }
    pthread_mutex_unlock(&clients_mutex);
}

void handle_text_message(int client_idx, const MessageEx *in_msg) {
    pthread_mutex_lock(&clients_mutex);
    
    MessageEx msg = {0};
    msg.type = MSG_TEXT;
    msg.msg_id = generate_msg_id();
    msg.timestamp = time(NULL);
    strncpy(msg.sender, clients[client_idx].nickname, MAX_NAME - 1);
    msg.sender[MAX_NAME - 1] = '\0';
    strncpy(msg.payload, in_msg->payload, MAX_PAYLOAD - 1);
    msg.payload[MAX_PAYLOAD - 1] = '\0';
    msg.length = strlen(msg.payload);
    
    save_to_history(&msg, 1, 0);
    
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].active && clients[i].authenticated && i != client_idx) {
            log_tcp_ip("SEND", "broadcast MSG_TEXT", 
                      clients[client_idx].ip, clients[i].ip, 
                      clients[client_idx].port, SERVER_PORT);
            send_message_ex(clients[i].sock, &msg);
        }
    }
    pthread_mutex_unlock(&clients_mutex);
}

void handle_private_message(int sender_idx, const MessageEx *msg) {
    if (strlen(msg->receiver) == 0) {
        MessageEx err = {0};
        err.type = MSG_ERROR;
        err.msg_id = generate_msg_id();
        err.timestamp = time(NULL);
        strncpy(err.payload, "Receiver not specified", MAX_PAYLOAD - 1);
        err.payload[MAX_PAYLOAD - 1] = '\0';
        err.length = strlen(err.payload);
        send_message_ex(clients[sender_idx].sock, &err);
        return;
    }

    pthread_mutex_lock(&clients_mutex);
    
    int found_idx = -1;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].active && clients[i].authenticated &&
            strcmp(clients[i].nickname, msg->receiver) == 0) {
            found_idx = i;
            break;
        }
    }
    
    if (found_idx < 0) {
        pthread_mutex_unlock(&clients_mutex);
        
        add_to_offline_queue(msg->sender, msg->receiver, msg->payload, msg->msg_id);
        save_to_history(msg, 0, 1);
        
        MessageEx confirm = {0};
        confirm.type = MSG_SERVER_INFO;
        confirm.msg_id = generate_msg_id();
        confirm.timestamp = time(NULL);
        snprintf(confirm.payload, MAX_PAYLOAD, "Message to [%s] queued (offline)", msg->receiver);
        confirm.payload[MAX_PAYLOAD - 1] = '\0';
        confirm.length = strlen(confirm.payload);
        send_message_ex(clients[sender_idx].sock, &confirm);
        return;
    }

    log_tcp_ip("SEND", "route MSG_PRIVATE", 
              clients[sender_idx].ip, clients[found_idx].ip,
              clients[sender_idx].port, SERVER_PORT);
    send_message_ex(clients[found_idx].sock, msg);
    
    MessageEx confirm = {0};
    confirm.type = MSG_PRIVATE;
    confirm.msg_id = generate_msg_id();
    confirm.timestamp = time(NULL);
    strncpy(confirm.sender, msg->sender, MAX_NAME - 1);
    confirm.sender[MAX_NAME - 1] = '\0';
    strncpy(confirm.receiver, msg->receiver, MAX_NAME - 1);
    confirm.receiver[MAX_NAME - 1] = '\0';

    int prefix_len = MAX_NAME + 10;
    int max_payload = MAX_PAYLOAD - prefix_len;
    if (max_payload < 1) max_payload = 1;
    
    snprintf(confirm.payload, MAX_PAYLOAD, "[To %.*s]: %.*s", 
             MAX_NAME - 1, msg->receiver, 
             max_payload, msg->payload);
    confirm.payload[MAX_PAYLOAD - 1] = '\0';
    confirm.length = strlen(confirm.payload);
    
    send_message_ex(clients[sender_idx].sock, &confirm);
    
    save_to_history(msg, 1, 0);
    pthread_mutex_unlock(&clients_mutex);
}

void handle_history_request(int client_sock, int count, Client *sender_client) {
    MessageEx msgs[50];
    int loaded = load_last_history(count, msgs, 50);
    
    for (int i = 0; i < loaded; i++) {
        MessageEx resp = {0};
        resp.type = MSG_HISTORY_DATA;
        resp.msg_id = msgs[i].msg_id;
        resp.timestamp = msgs[i].timestamp;
        strncpy(resp.sender, msgs[i].sender, MAX_NAME - 1);
        resp.sender[MAX_NAME - 1] = '\0';
        strncpy(resp.receiver, msgs[i].receiver, MAX_NAME - 1);
        resp.receiver[MAX_NAME - 1] = '\0';
        strncpy(resp.payload, msgs[i].payload, MAX_PAYLOAD - 1);
        resp.payload[MAX_PAYLOAD - 1] = '\0';
        resp.length = strlen(resp.payload);
        
        log_tcp_ip("SEND", "send MSG_HISTORY_DATA", 
                  "127.0.0.1", sender_client->ip, 
                  SERVER_PORT, sender_client->port);
        send_message_ex(client_sock, &resp);
    }
    
    MessageEx end = {0};
    end.type = MSG_SERVER_INFO;
    end.msg_id = generate_msg_id();
    end.timestamp = time(NULL);
    snprintf(end.payload, MAX_PAYLOAD, "History end (%d messages)", loaded);
    end.payload[MAX_PAYLOAD - 1] = '\0';
    end.length = strlen(end.payload);
    send_message_ex(client_sock, &end);
}

void handle_list_request(int client_sock, Client *sender) {
    char list_buf[2048] = "Online users:\n";
    pthread_mutex_lock(&clients_mutex);
    
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].active && clients[i].authenticated) {
            char entry[64];
            snprintf(entry, sizeof(entry), "  - %s [%s:%d]\n", 
                    clients[i].nickname, clients[i].ip, clients[i].port);
            strncat(list_buf, entry, sizeof(list_buf) - strlen(list_buf) - 1);
        }
    }
    pthread_mutex_unlock(&clients_mutex);
    
    MessageEx resp = {0};
    resp.type = MSG_SERVER_INFO;
    resp.msg_id = generate_msg_id();
    resp.timestamp = time(NULL);
    strncpy(resp.payload, list_buf, MAX_PAYLOAD - 1);
    resp.payload[MAX_PAYLOAD - 1] = '\0';
    resp.length = strlen(resp.payload);
    
    log_tcp_ip("SEND", "send MSG_LIST response", 
              "127.0.0.1", sender->ip, SERVER_PORT, sender->port);
    send_message_ex(client_sock, &resp);
}

void *handle_client(void *arg) {
    int sock = *(int*)arg;
    free(arg);
    
    MessageEx msg;
    char client_ip[INET_ADDRSTRLEN];
    int client_port;
    char nickname[MAX_NAME] = "";
    
    struct sockaddr_in addr;
    socklen_t addr_len = sizeof(addr);
    if (getpeername(sock, (struct sockaddr*)&addr, &addr_len) == 0) {
        inet_ntop(AF_INET, &addr.sin_addr, client_ip, sizeof(client_ip));
        client_port = ntohs(addr.sin_port);
    }

    log_tcp_ip("RECV", "accept() new connection", 
              client_ip, "127.0.0.1", client_port, SERVER_PORT);

    if (recv_message_ex(sock, &msg) < 0) {
        log_tcp_ip("RECV", "deserialize failed", client_ip, "127.0.0.1", client_port, SERVER_PORT);
        close(sock);
        return NULL;
    }
    
    log_tcp_ip("RECV", "deserialize MessageEx", client_ip, "127.0.0.1", client_port, SERVER_PORT);
    
    if (msg.type != MSG_AUTH) {
        MessageEx err = {0};
        err.type = MSG_ERROR;
        err.msg_id = generate_msg_id();
        err.timestamp = time(NULL);
        strncpy(err.payload, "Authentication required", MAX_PAYLOAD - 1);
        err.payload[MAX_PAYLOAD - 1] = '\0';
        err.length = strlen(err.payload);
        send_message_ex(sock, &err);
        close(sock);
        return NULL;
    }
    
    strncpy(nickname, msg.payload, MAX_NAME - 1);
    nickname[MAX_NAME - 1] = '\0';
    
    pthread_mutex_lock(&clients_mutex);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].active && strcmp(clients[i].nickname, nickname) == 0) {
            pthread_mutex_unlock(&clients_mutex);
            MessageEx err = {0};
            err.type = MSG_ERROR;
            err.msg_id = generate_msg_id();
            err.timestamp = time(NULL);
            strncpy(err.payload, "Nickname already in use", MAX_PAYLOAD - 1);
            err.payload[MAX_PAYLOAD - 1] = '\0';
            err.length = strlen(err.payload);
            send_message_ex(sock, &err);
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
        MessageEx err = {0};
        err.type = MSG_ERROR;
        err.msg_id = generate_msg_id();
        err.timestamp = time(NULL);
        strncpy(err.payload, "Server full", MAX_PAYLOAD - 1);
        err.payload[MAX_PAYLOAD - 1] = '\0';
        err.length = strlen(err.payload);
        send_message_ex(sock, &err);
        close(sock);
        return NULL;
    }
    
    clients[client_idx].sock = sock;
    clients[client_idx].active = 1;
    clients[client_idx].authenticated = 1;
    strncpy(clients[client_idx].nickname, nickname, MAX_NAME - 1);
    clients[client_idx].nickname[MAX_NAME - 1] = '\0';
    strncpy(clients[client_idx].ip, client_ip, sizeof(client_ip) - 1);
    clients[client_idx].ip[sizeof(client_ip) - 1] = '\0';
    clients[client_idx].port = client_port;
    pthread_mutex_unlock(&clients_mutex);
    
    MessageEx welcome = {0};
    welcome.type = MSG_WELCOME;
    welcome.msg_id = generate_msg_id();
    welcome.timestamp = time(NULL);
    snprintf(welcome.payload, MAX_PAYLOAD, "Welcome %s! Connected: %d", 
             nickname, get_count_active());
    welcome.payload[MAX_PAYLOAD - 1] = '\0';
    welcome.length = strlen(welcome.payload);
    send_message_ex(sock, &welcome);
    
    deliver_offline_messages(nickname, sock);
    
    char sys_msg[256];
    snprintf(sys_msg, sizeof(sys_msg), "User [%s] connected", nickname);
    broadcast_system(sys_msg, sock);
    
    printf("[INFO] Client authenticated: %s [%s:%d]\n", nickname, client_ip, client_port);
    
    int running = 1;
    while (running) {
        if (recv_message_ex(sock, &msg) < 0) {
            printf("[INFO] Connection lost: %s\n", nickname);
            break;
        }
        
        log_tcp_ip("RECV", "deserialize MessageEx", 
                  client_ip, "127.0.0.1", client_port, SERVER_PORT);
        
        if (!clients[client_idx].authenticated) continue;
        
        switch (msg.type) {
            case MSG_TEXT:
                log_tcp_ip("RECV", "handle MSG_TEXT -> broadcast", 
                          client_ip, "127.0.0.1", client_port, SERVER_PORT);
                handle_text_message(client_idx, &msg);
                break;
                
            case MSG_PRIVATE:
                log_tcp_ip("RECV", "handle MSG_PRIVATE -> direct routing", 
                          client_ip, "127.0.0.1", client_port, SERVER_PORT);
                handle_private_message(client_idx, &msg);
                break;
                
            case MSG_PING:
                {
                    MessageEx pong = {0};
                    pong.type = MSG_PONG;
                    pong.msg_id = generate_msg_id();
                    pong.timestamp = time(NULL);
                    strncpy(pong.payload, "pong", MAX_PAYLOAD - 1);
                    pong.payload[MAX_PAYLOAD - 1] = '\0';
                    pong.length = 4;
                    send_message_ex(sock, &pong);
                }
                break;
                
            case MSG_HISTORY:
                {
                    int count = 20;
                    if (strlen(msg.payload) > 0) {
                        int parsed = atoi(msg.payload);
                        if (parsed > 0 && parsed <= 100) count = parsed;
                    }
                    handle_history_request(sock, count, &clients[client_idx]);
                }
                break;
                
            case MSG_LIST:
                handle_list_request(sock, &clients[client_idx]);
                break;
                
            case MSG_BYE:
                running = 0;
                break;
                
            default:
                {
                    MessageEx err = {0};
                    err.type = MSG_ERROR;
                    err.msg_id = generate_msg_id();
                    err.timestamp = time(NULL);
                    strncpy(err.payload, "Unknown command", MAX_PAYLOAD - 1);
                    err.payload[MAX_PAYLOAD - 1] = '\0';
                    err.length = strlen(err.payload);
                    send_message_ex(sock, &err);
                }
        }
    }
    
    snprintf(sys_msg, sizeof(sys_msg), "User [%s] disconnected", nickname);
    broadcast_system(sys_msg, sock);
    
    MessageEx disconnect_msg = {0};
    disconnect_msg.type = MSG_SERVER_INFO;
    disconnect_msg.msg_id = generate_msg_id();
    disconnect_msg.timestamp = time(NULL);
    strncpy(disconnect_msg.sender, nickname, MAX_NAME - 1);
    disconnect_msg.sender[MAX_NAME - 1] = '\0';
    snprintf(disconnect_msg.payload, MAX_PAYLOAD, "DISCONNECT");
    disconnect_msg.payload[MAX_PAYLOAD - 1] = '\0';
    disconnect_msg.length = strlen(disconnect_msg.payload);
    save_to_history(&disconnect_msg, 1, 0);
    
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

int main(void) {
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