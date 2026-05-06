#define _DEFAULT_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <pthread.h>
#include <time.h>
#include "message.h"

#define MAX_CLIENTS      64
#define THREAD_POOL_SIZE 10
#define PROCESSED_IDS    32

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

typedef struct {
    int delay_ms;
    double drop_rate;
    double corrupt_rate;
} SimConfig;

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
static SimConfig g_sim = {0, 0.0, 0.0};
static pthread_mutex_t history_mutex = PTHREAD_MUTEX_INITIALIZER;

static uint32_t generate_msg_id(void) {
    pthread_mutex_lock(&msg_counter_mutex);
    uint32_t id = g_msg_counter++;
    pthread_mutex_unlock(&msg_counter_mutex);
    return id;
}

static double rand_unit(void) {
    return (double)rand() / (double)RAND_MAX;
}

static int simulate_network(MessageEx *msg) {
    if (g_sim.delay_ms > 0) {
        printf("[Transport][SIM] DELAY %dms (id=%u)\n", g_sim.delay_ms, msg->msg_id);
        fflush(stdout);
        usleep((useconds_t)g_sim.delay_ms * 1000U);
    }

    if (g_sim.drop_rate > 0.0 && rand_unit() < g_sim.drop_rate) {
        printf("[Transport][SIM] DROP (id=%u, rate=%.2f)\n", msg->msg_id, g_sim.drop_rate);
        fflush(stdout);
        return 0;
    }

    if (g_sim.corrupt_rate > 0.0 && msg->length > 0 && rand_unit() < g_sim.corrupt_rate) {
        size_t pos = rand() % msg->length;
        msg->payload[pos] = (uint8_t)(rand() % 256);
        printf("[Transport][SIM] CORRUPT pos=%zu (id=%u, rate=%.2f)\n", pos, msg->msg_id, g_sim.corrupt_rate);
        fflush(stdout);
    }

    return 1;
}

static int id_seen(uint32_t *ids, int count, uint32_t id) {
    for (int i = 0; i < count; i++) {
        if (ids[i] == id) return 1;
    }
    return 0;
}

static void remember_id(uint32_t *ids, int *count, uint32_t id) {
    if (*count < PROCESSED_IDS) {
        ids[(*count)++] = id;
        return;
    }

    memmove(ids, ids + 1, sizeof(uint32_t) * (PROCESSED_IDS - 1));
    ids[PROCESSED_IDS - 1] = id;
}

static void send_ack(int sock, const MessageEx *src) {
    MessageEx ack = {0};
    ack.type = MSG_ACK;
    ack.msg_id = src->msg_id;
    ack.timestamp = time(NULL);
    strncpy(ack.sender, "server", MAX_NAME - 1);
    strncpy(ack.receiver, src->sender, MAX_NAME - 1);
    snprintf(ack.payload, MAX_PAYLOAD, "ACK %u", src->msg_id);
    ack.length = strlen(ack.payload);
    printf("[Transport][ACK] send MSG_ACK (id=%u)\n", src->msg_id);
    fflush(stdout);
    send_message_ex(sock, &ack);
}

static void add_to_offline_queue(const char *sender, const char *receiver,
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

static void deliver_offline_messages(const char *nickname, int client_sock) {
    pthread_mutex_lock(&offline_mutex);

    for (int i = 0; i < offline_count; ) {
        if (strcmp(offline_queue[i].receiver, nickname) == 0 && !offline_queue[i].delivered) {
            MessageEx msg = {0};
            msg.type = MSG_PRIVATE;
            msg.msg_id = offline_queue[i].msg_id;
            msg.timestamp = offline_queue[i].timestamp;
            strncpy(msg.sender, offline_queue[i].sender, MAX_NAME - 1);
            strncpy(msg.receiver, offline_queue[i].receiver, MAX_NAME - 1);

            snprintf(msg.payload, MAX_PAYLOAD, "[OFFLINE] %.*s", MAX_PAYLOAD - 11, offline_queue[i].text);
            msg.payload[MAX_PAYLOAD - 1] = '\0';
            msg.length = strlen(msg.payload);

            log_tcp_ip("SEND", "deliver offline MSG_PRIVATE",
                       "127.0.0.1", "127.0.0.1", SERVER_PORT, 0);
            send_message_ex(client_sock, &msg);

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

static int get_count_active(void) {
    pthread_mutex_lock(&clients_mutex);
    int count = 0;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].active) count++;
    }
    pthread_mutex_unlock(&clients_mutex);
    return count;
}

static void queue_push(int sock) {
    pthread_mutex_lock(&conn_queue.mutex);
    while (conn_queue.count >= 1024)
        pthread_cond_wait(&conn_queue.cond, &conn_queue.mutex);
    conn_queue.sockets[conn_queue.tail] = sock;
    conn_queue.tail = (conn_queue.tail + 1) % 1024;
    conn_queue.count++;
    pthread_cond_signal(&conn_queue.cond);
    pthread_mutex_unlock(&conn_queue.mutex);
}

static int queue_pop(void) {
    pthread_mutex_lock(&conn_queue.mutex);
    while (conn_queue.count == 0)
        pthread_cond_wait(&conn_queue.cond, &conn_queue.mutex);
    int sock = conn_queue.sockets[conn_queue.head];
    conn_queue.head = (conn_queue.head + 1) % 1024;
    conn_queue.count--;
    pthread_mutex_unlock(&conn_queue.mutex);
    return sock;
}

static void remove_client(int sock) {
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

static void broadcast_system(const char *text, int exclude_sock) {
    pthread_mutex_lock(&clients_mutex);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].active && clients[i].authenticated && clients[i].sock != exclude_sock) {
            MessageEx msg = {0};
            msg.type = MSG_SERVER_INFO;
            msg.msg_id = generate_msg_id();
            msg.timestamp = time(NULL);
            strncpy(msg.payload, text, MAX_PAYLOAD - 1);
            msg.length = strlen(msg.payload);
            send_message_ex(clients[i].sock, &msg);
        }
    }
    pthread_mutex_unlock(&clients_mutex);
}

static void history_save_unique(const MessageEx *msg, int delivered, int is_offline) {
    if (!delivered) return;
    if (!(msg->type == MSG_TEXT || msg->type == MSG_PRIVATE)) return;

    pthread_mutex_lock(&history_mutex);
    save_to_history(msg, delivered, is_offline);
    pthread_mutex_unlock(&history_mutex);
}

static void handle_text_message(int client_idx, const MessageEx *in_msg) {
    pthread_mutex_lock(&clients_mutex);

    MessageEx msg = {0};
    msg.type = MSG_TEXT;
    msg.msg_id = in_msg->msg_id;
    msg.timestamp = time(NULL);
    strncpy(msg.sender, clients[client_idx].nickname, MAX_NAME - 1);
    strncpy(msg.payload, in_msg->payload, MAX_PAYLOAD - 1);
    msg.length = strlen(msg.payload);

    printf("[Application][ACK] process MSG_TEXT (id=%u)\n", msg.msg_id);
    fflush(stdout);
    history_save_unique(&msg, 1, 0);

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

static void handle_private_message(int sender_idx, const MessageEx *msg) {
    if (strlen(msg->receiver) == 0) {
        MessageEx err = {0};
        err.type = MSG_ERROR;
        err.msg_id = generate_msg_id();
        err.timestamp = time(NULL);
        strncpy(err.payload, "Receiver not specified", MAX_PAYLOAD - 1);
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

    printf("[Application][ACK] process MSG_PRIVATE (id=%u)\n", msg->msg_id);
    fflush(stdout);

    if (found_idx < 0) {
        pthread_mutex_unlock(&clients_mutex);

        add_to_offline_queue(msg->sender, msg->receiver, msg->payload, msg->msg_id);
        MessageEx confirm = {0};
        confirm.type = MSG_SERVER_INFO;
        confirm.msg_id = generate_msg_id();
        confirm.timestamp = time(NULL);
        snprintf(confirm.payload, MAX_PAYLOAD, "Message to [%s] queued (offline)", msg->receiver);
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
    strncpy(confirm.receiver, msg->receiver, MAX_NAME - 1);
    snprintf(confirm.payload, MAX_PAYLOAD, "%.*s", MAX_PAYLOAD - 1, msg->payload);
    confirm.payload[MAX_PAYLOAD - 1] = '\0';
    confirm.length = strlen(confirm.payload);

    send_message_ex(clients[sender_idx].sock, &confirm);
    history_save_unique(msg, 1, 0);
    pthread_mutex_unlock(&clients_mutex);
}

static void handle_history_request(int client_sock, int count, Client *sender_client) {
    MessageEx msgs[50];
    pthread_mutex_lock(&history_mutex);
    int loaded = load_last_history(count, msgs, 50);
    pthread_mutex_unlock(&history_mutex);

    for (int i = 0; i < loaded; i++) {
        MessageEx resp = {0};
        resp.type = MSG_HISTORY_DATA;
        resp.msg_id = msgs[i].msg_id;
        resp.timestamp = msgs[i].timestamp;
        strncpy(resp.sender, msgs[i].sender, MAX_NAME - 1);
        strncpy(resp.receiver, msgs[i].receiver, MAX_NAME - 1);
        strncpy(resp.payload, msgs[i].payload, MAX_PAYLOAD - 1);
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
    end.length = strlen(end.payload);
    send_message_ex(client_sock, &end);
}

static void handle_list_request(int client_sock, Client *sender) {
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
    resp.length = strlen(resp.payload);

    log_tcp_ip("SEND", "send MSG_LIST response",
               "127.0.0.1", sender->ip, SERVER_PORT, sender->port);
    send_message_ex(client_sock, &resp);
}

static void *handle_client(void *arg) {
    int sock = *(int*)arg;
    free(arg);

    MessageEx msg;
    char client_ip[INET_ADDRSTRLEN] = "0.0.0.0";
    int client_port = 0;
    char nickname[MAX_NAME] = "";
    uint32_t processed_ids[PROCESSED_IDS] = {0};
    int processed_count = 0;

    struct sockaddr_in addr;
    socklen_t addr_len = sizeof(addr);
    if (getpeername(sock, (struct sockaddr*)&addr, &addr_len) == 0) {
        inet_ntop(AF_INET, &addr.sin_addr, client_ip, sizeof(client_ip));
        client_port = ntohs(addr.sin_port);
    }

    log_tcp_ip("RECV", "accept() new connection",
               client_ip, "127.0.0.1", client_port, SERVER_PORT);

    if (recv_message_ex(sock, &msg) < 0) {
        close(sock);
        return NULL;
    }

    if (msg.type != MSG_AUTH) {
        MessageEx err = {0};
        err.type = MSG_ERROR;
        err.msg_id = generate_msg_id();
        err.timestamp = time(NULL);
        strncpy(err.payload, "Authentication required", MAX_PAYLOAD - 1);
        err.length = strlen(err.payload);
        send_message_ex(sock, &err);
        close(sock);
        return NULL;
    }

    strncpy(nickname, msg.payload, MAX_NAME - 1);

    pthread_mutex_lock(&clients_mutex);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].active && strcmp(clients[i].nickname, nickname) == 0) {
            pthread_mutex_unlock(&clients_mutex);
            MessageEx err = {0};
            err.type = MSG_ERROR;
            err.msg_id = generate_msg_id();
            err.timestamp = time(NULL);
            strncpy(err.payload, "Nickname already in use", MAX_PAYLOAD - 1);
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
        err.length = strlen(err.payload);
        send_message_ex(sock, &err);
        close(sock);
        return NULL;
    }

    clients[client_idx].sock = sock;
    clients[client_idx].active = 1;
    clients[client_idx].authenticated = 1;
    strncpy(clients[client_idx].nickname, nickname, MAX_NAME - 1);
    strncpy(clients[client_idx].ip, client_ip, sizeof(clients[client_idx].ip) - 1);
    clients[client_idx].port = client_port;
    pthread_mutex_unlock(&clients_mutex);

    MessageEx welcome = {0};
    welcome.type = MSG_WELCOME;
    welcome.msg_id = generate_msg_id();
    welcome.timestamp = time(NULL);
    snprintf(welcome.payload, MAX_PAYLOAD, "Welcome %s! Connected: %d", nickname, get_count_active());
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

        if (!simulate_network(&msg)) {
            continue;
        }

        if (!clients[client_idx].authenticated) continue;

        if ((msg.type == MSG_TEXT || msg.type == MSG_PRIVATE) &&
            id_seen(processed_ids, processed_count, msg.msg_id)) {
                log_tcp_ip("APP", "DEDUP check", client_ip, "127.0.0.1", client_port, SERVER_PORT);
                printf("[Application][DEDUP] duplicate ignored (id=%u)\n", msg.msg_id);
                printf("[Transport][RETRY] resend detected (id=%u)\n", msg.msg_id);
                fflush(stdout);
                send_ack(sock, &msg);
                continue;
        }

        switch (msg.type) {
            case MSG_TEXT:
                log_tcp_ip("RECV", "handle MSG_TEXT -> broadcast",
                           client_ip, "127.0.0.1", client_port, SERVER_PORT);
                handle_text_message(client_idx, &msg);
                remember_id(processed_ids, &processed_count, msg.msg_id);
                send_ack(sock, &msg);
                break;

            case MSG_PRIVATE:
                log_tcp_ip("RECV", "handle MSG_PRIVATE -> direct routing",
                           client_ip, "127.0.0.1", client_port, SERVER_PORT);
                handle_private_message(client_idx, &msg);
                remember_id(processed_ids, &processed_count, msg.msg_id);
                send_ack(sock, &msg);
                break;

            case MSG_PING:
                {
                    printf("[Transport][PING] recv MSG_PING (id=%u)\n", msg.msg_id);
                    MessageEx pong = {0};
                    pong.type = MSG_PONG;
                    pong.msg_id = msg.msg_id;
                    pong.timestamp = time(NULL);
                    strncpy(pong.sender, "server", MAX_NAME - 1);
                    strncpy(pong.receiver, msg.sender, MAX_NAME - 1);
                    strncpy(pong.payload, "pong", MAX_PAYLOAD - 1);
                    pong.length = 4;
                    printf("[Transport][PING] send MSG_PONG (id=%u)\n", pong.msg_id);
                    fflush(stdout);
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
                    err.length = strlen(err.payload);
                    send_message_ex(sock, &err);
                }
        }
    }

    snprintf(sys_msg, sizeof(sys_msg), "User [%s] disconnected", nickname);
    broadcast_system(sys_msg, sock);

    remove_client(sock);
    return NULL;
}

static void *worker_thread(void *arg) {
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
        if (!sock_ptr) {
            close(client_sock);
            pthread_attr_destroy(&attr);
            continue;
        }
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

static void parse_args(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--delay=", 8) == 0) {
            g_sim.delay_ms = atoi(argv[i] + 8);
        } else if (strncmp(argv[i], "--drop=", 7) == 0) {
            g_sim.drop_rate = atof(argv[i] + 7);
        } else if (strncmp(argv[i], "--corrupt=", 10) == 0) {
            g_sim.corrupt_rate = atof(argv[i] + 10);
        }
    }

    if (g_sim.delay_ms < 0) g_sim.delay_ms = 0;
    if (g_sim.drop_rate < 0.0) g_sim.drop_rate = 0.0;
    if (g_sim.drop_rate > 1.0) g_sim.drop_rate = 1.0;
    if (g_sim.corrupt_rate < 0.0) g_sim.corrupt_rate = 0.0;
    if (g_sim.corrupt_rate > 1.0) g_sim.corrupt_rate = 1.0;
}

int main(int argc, char **argv) {
    parse_args(argc, argv);
    srand((unsigned int)time(NULL));

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
    printf("[Transport][SIM] delay=%dms drop=%.2f corrupt=%.2f\n",
           g_sim.delay_ms, g_sim.drop_rate, g_sim.corrupt_rate);

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
