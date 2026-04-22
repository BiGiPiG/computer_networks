#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <pthread.h>
#include <stdatomic.h>
#include <time.h>
#include "message.h"

#define RECONNECT_DELAY 2
#define MAX_TIME_STR    32

static int g_sock = -1;
static atomic_int g_running = 0;
static char g_nickname[MAX_NAME];
static int g_intentional_disconnect = 0;
static uint32_t g_client_msg_id = 1;

static void format_timestamp(time_t ts, char *out, size_t out_size) {
    struct tm *tm_info = localtime(&ts);
    if (tm_info) strftime(out, out_size, "%Y-%m-%d %H:%M:%S", tm_info);
    else strncpy(out, "0000-00-00 00:00:00", out_size - 1);
}

static void print_message_ex(const MessageEx *msg) {
    char time_buf[MAX_TIME_STR];
    format_timestamp(msg->timestamp, time_buf, sizeof(time_buf));
    
    const char *tag = "";
    if (msg->type == MSG_PRIVATE) {
        tag = (strstr(msg->payload, "[OFFLINE]") ? "[OFFLINE]" : "[PRIVATE]");
    }

    if (strlen(msg->receiver) > 0) {
        printf("\r[%s][id=%u]%s[%s -> %s]: %s\n> ",
               time_buf, msg->msg_id, tag, msg->sender, msg->receiver, msg->payload);
    } else {
        printf("\r[%s][id=%u]%s[%s]: %s\n> ",
               time_buf, msg->msg_id, tag, msg->sender, msg->payload);
    }
    fflush(stdout);
}

static void delete_last_line(void) {
    printf("\r\033[K> ");
    fflush(stdout);
}

static void print_help(void) {
    printf("\rAvailable commands:\n"
           "  /help                 - show this help\n"
           "  /list                 - show online users\n"
           "  /history              - show last 20 messages\n"
           "  /history N            - show last N messages (1-100)\n"
           "  /quit                 - disconnect from server\n"
           "  /w <nick> <message>   - send private message\n"
           "  /ping                 - check connection latency\n"
           "Tip: packets never sleep\n> ");
    fflush(stdout);
}

static int connect_to_server(const char *ip, int port) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) { perror("socket"); return -1; }
    
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, ip, &addr.sin_addr) <= 0) { close(sock); return -1; }
    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) { perror("connect"); close(sock); return -1; }
    return sock;
}

static int do_auth(int sock, const char *nickname) {
    MessageEx auth = {0};
    auth.type = MSG_AUTH;
    auth.msg_id = g_client_msg_id++;
    auth.timestamp = time(NULL);
    strncpy(auth.sender, nickname, MAX_NAME - 1);
    strncpy(auth.payload, nickname, MAX_PAYLOAD - 1);
    auth.length = strlen(auth.payload);
    if (auth.length >= MAX_PAYLOAD) auth.length = MAX_PAYLOAD - 1;

    if (send_message_ex(sock, &auth) < 0) return -1;
    
    MessageEx resp;
    if (recv_message_ex(sock, &resp) < 0) return -1;
    
    if (resp.type == MSG_WELCOME) {
        printf("[SERVER] %s\n", resp.payload);
        return 0;
    } else if (resp.type == MSG_ERROR) {
        fprintf(stderr, "[AUTH ERROR] %s\n", resp.payload);
        return -1;
    }
    return -1;
}

static void *receive_thread(void *arg) {
    (void)arg;
    MessageEx msg;
    
    while (atomic_load(&g_running)) {
        if (recv_message_ex(g_sock, &msg) < 0) {
            atomic_store(&g_running, 0);
            break;
        }
        
        switch (msg.type) {
            case MSG_TEXT:
            case MSG_PRIVATE:
                print_message_ex(&msg); break;
            case MSG_SERVER_INFO:
            case MSG_WELCOME:
                printf("\r[SERVER] %s\n> ", msg.payload); fflush(stdout); break;
            case MSG_HISTORY_DATA:
                printf("[%s][id=%u][%s]: %s\n", "HISTORY", msg.msg_id, msg.sender, msg.payload); fflush(stdout); break;
            case MSG_PONG:
                printf("\r[PONG] latency check OK\n> "); fflush(stdout); break;
            case MSG_ERROR:
                fprintf(stdout, "\r[ERROR] %s\n> ", msg.payload); fflush(stdout); break;
            case MSG_HELP:
                printf("\r[HELP] %s\n> ", msg.payload); fflush(stdout); break;
            default: break;
        }
    }
    return NULL;
}

static void input_loop(int sock) {
    char input[2048];
    
    while (atomic_load(&g_running)) {
        fd_set readfds;
        struct timeval timeout = {1, 0};
        FD_ZERO(&readfds);
        FD_SET(STDIN_FILENO, &readfds);
        
        int ret = select(STDIN_FILENO + 1, &readfds, NULL, NULL, &timeout);
        if (ret <= 0) continue;
        
        if (FD_ISSET(STDIN_FILENO, &readfds)) {
            if (!fgets(input, sizeof(input), stdin)) break;
            input[strcspn(input, "\n")] = '\0';
            if (!atomic_load(&g_running) || strlen(input) == 0) continue;
            
            if (strcmp(input, "/help") == 0) {
                print_help(); continue;
            } else if (strcmp(input, "/list") == 0) {
                MessageEx req = {0};
                req.type = MSG_LIST; req.msg_id = g_client_msg_id++; req.timestamp = time(NULL);
                strncpy(req.sender, g_nickname, MAX_NAME - 1);
                send_message_ex(sock, &req); delete_last_line(); continue;
            } else if (strncmp(input, "/history", 8) == 0) {
                MessageEx req = {0};
                req.type = MSG_HISTORY; req.msg_id = g_client_msg_id++; req.timestamp = time(NULL);
                strncpy(req.sender, g_nickname, MAX_NAME - 1);
                char *endptr; long n = strtol(input + 8, &endptr, 10);
                if (n > 0 && n <= 100 && (*endptr == '\0' || *endptr == ' ')) {
                    snprintf(req.payload, MAX_PAYLOAD, "%ld", n); req.length = strlen(req.payload);
                } else req.length = 0;
                send_message_ex(sock, &req); delete_last_line(); continue;
            } else if (strncmp(input, "/w", 2) == 0) {
                printf("%s\n", input);
                fflush(stdout);
                char *ptr = input + 2;
                while (*ptr == ' ') ptr++;
                
                if (*ptr == '\0') {
                    printf("\r[ERROR1] Format: /w <nick> <message>\n> ");
                    fflush(stdout); continue;
                }
                
                char *space = strchr(ptr, ' ');
                if (!space) {
                    printf("\r[ERROR2] Format: /w <nick> <message>\n> ");
                    fflush(stdout); continue;
                }
                
                *space = '\0';
                char *nick = ptr;
                char *msg = space + 1;
                while (*msg == ' ') msg++;
                
                if (*msg == '\0') {
                    printf("\r[ERROR3] Format: /w <nick> <message>\n> ");
                    fflush(stdout); continue;
                }

                MessageEx priv = {0};
                priv.type = MSG_PRIVATE;
                priv.msg_id = g_client_msg_id++;
                priv.timestamp = time(NULL);
                strncpy(priv.sender, g_nickname, MAX_NAME - 1);
                strncpy(priv.receiver, nick, MAX_NAME - 1);
                strncpy(priv.payload, msg, MAX_PAYLOAD - 1);
                priv.length = strlen(priv.payload);
                
                send_message_ex(sock, &priv);
                delete_last_line();
                continue;
            } else if (strcmp(input, "/quit") == 0) {
                MessageEx bye = {0};
                bye.type = MSG_BYE; bye.msg_id = g_client_msg_id++; bye.timestamp = time(NULL);
                strncpy(bye.sender, g_nickname, MAX_NAME - 1);
                strncpy(bye.payload, "bye", MAX_PAYLOAD - 1); bye.length = 3;
                send_message_ex(sock, &bye); g_intentional_disconnect = 1; delete_last_line(); break;
            } else if (strcmp(input, "/ping") == 0) {
                MessageEx ping = {0};
                ping.type = MSG_PING; ping.msg_id = g_client_msg_id++; ping.timestamp = time(NULL);
                strncpy(ping.sender, g_nickname, MAX_NAME - 1);
                strncpy(ping.payload, "ping", MAX_PAYLOAD - 1); ping.length = 4;
                send_message_ex(sock, &ping); delete_last_line(); continue;
            } else {
                MessageEx text = {0};
                text.type = MSG_TEXT; text.msg_id = g_client_msg_id++; text.timestamp = time(NULL);
                strncpy(text.sender, g_nickname, MAX_NAME - 1);
                strncpy(text.payload, input, MAX_PAYLOAD - 1);
                text.length = strlen(text.payload);
                if (text.length >= MAX_PAYLOAD) text.length = MAX_PAYLOAD - 1;
                send_message_ex(sock, &text); delete_last_line();
            }
        }
    }
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <server_ip>\n", argv[0]);
        return 1;
    }
    const char *server_ip = argv[1];

    while (1) {
        printf("Connected\n");
        g_sock = connect_to_server(server_ip, SERVER_PORT);
        if (g_sock < 0) { sleep(RECONNECT_DELAY); continue; }

        printf("Enter nickname: ");
        if (!fgets(g_nickname, sizeof(g_nickname), stdin)) { close(g_sock); return 1; }
        g_nickname[strcspn(g_nickname, "\n")] = '\0';

        if (do_auth(g_sock, g_nickname) < 0) { close(g_sock); continue; }
        printf("> "); fflush(stdout);

        atomic_store(&g_running, 1);
        pthread_t recv_thr;
        if (pthread_create(&recv_thr, NULL, receive_thread, NULL) != 0) {
            fprintf(stderr, "Failed to create receive thread\n");
            close(g_sock); sleep(RECONNECT_DELAY); continue;
        }

        input_loop(g_sock);

        atomic_store(&g_running, 0);
        pthread_join(recv_thr, NULL);
        close(g_sock); g_sock = -1;
        if (g_intentional_disconnect) break;
        sleep(RECONNECT_DELAY);
    }
    printf("Goodbye!\n");
    return 0;
}