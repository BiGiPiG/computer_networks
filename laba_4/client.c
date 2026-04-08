#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <pthread.h>
#include <stdatomic.h>
#include "message.h"

#define RECONNECT_DELAY 2

static int g_sock = -1;
static atomic_int g_running = 0;
static char g_nickname[64];
static int g_intentional_disconnect = 0;

void *receive_thread(void *arg) {
    (void)arg;
    Message msg;
    
    while (atomic_load(&g_running)) {
        if (recv_message(g_sock, &msg) < 0) {
            atomic_store(&g_running, 0);
            break;
        }
        
        switch (msg.type) {
            case MSG_TEXT:
                printf("\r%s\n> ", msg.payload);
                fflush(stdout);
                break;
                
            case MSG_PRIVATE:
                printf("\r%s\n> ", msg.payload);
                fflush(stdout);
                break;
                
            case MSG_SERVER_INFO:
                printf("\r[SERVER] %s\n> ", msg.payload);
                fflush(stdout);
                break;
                
            case MSG_WELCOME:
                printf("\r[SERVER] %s\n> ", msg.payload);
                fflush(stdout);
                break;
                
            case MSG_PONG:
                printf("\r[PONG] latency check\n> ");
                fflush(stdout);
                break;
                
            case MSG_ERROR:
                fprintf(stdout, "\r[ERROR] %s\n> ", msg.payload);
                fflush(stdout);
                break;
                
            default:
                fprintf(stdout, "\r[WARN] Unknown message type: %d\n> ", msg.type);
                fflush(stdout);
        }
    }
    return NULL;
}

int connect_to_server(const char *ip, int port) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("socket");
        return -1;
    }
    
    struct sockaddr_in server_addr = {0};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    
    if (inet_pton(AF_INET, ip, &server_addr.sin_addr) <= 0) {
        fprintf(stdout, "Invalid address: %s\n", ip);
        close(sock);
        return -1;
    }
    
    if (connect(sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("connect");
        close(sock);
        return -1;
    }
    
    return sock;
}

int do_auth(int sock, const char *nickname) {
    
    if (send_message(sock, MSG_AUTH, nickname, strlen(nickname)) < 0) {
        return -1;
    }
    
    Message msg;
    if (recv_message(sock, &msg) < 0) {
        return -1;
    }
    
    switch (msg.type) {
        case MSG_WELCOME:
            printf("[SERVER] %s\n", msg.payload);
            return 0;
            
        case MSG_ERROR:
            fprintf(stdout, "[AUTH ERROR] %s\n", msg.payload);
            return -1;
            
        default:
            fprintf(stdout, "[ERROR] Unexpected response type: %d\n", msg.type);
            return -1;
    }
}

void delete_last_line() {
    printf("\r");
    printf("\033[K");
    printf("> ");
    fflush(stdout);
}

void input_loop(int sock) {
    char input[2048];
    
    while (atomic_load(&g_running)) {
        fd_set readfds;
        struct timeval timeout = {1, 0};
        FD_ZERO(&readfds);
        FD_SET(STDIN_FILENO, &readfds);
        
        int ret = select(STDIN_FILENO + 1, &readfds, NULL, NULL, &timeout);
        if (ret == 0) continue;
        if (ret < 0) break;
        
        if (FD_ISSET(STDIN_FILENO, &readfds)) {
            if (!fgets(input, sizeof(input), stdin)) break;
            input[strcspn(input, "\n")] = '\0';
            
            if (!atomic_load(&g_running)) break;
            if (strlen(input) == 0) continue;
            
            if (strncmp(input, "/w ", 3) == 0) {
                char *rest = input + 3;
                char *space = strchr(rest, ' ');
                if (!space) {
                    printf("[ERROR] Format: /w <nick> <message>\n");
                    printf("> ");
                    continue;
                }
                *space = '\0';
                const char *target = rest;
                const char *message = space + 1;
                
                char payload[2048];
                snprintf(payload, sizeof(payload), "%s:%s", target, message);
                
                send_message(sock, MSG_PRIVATE, payload, strlen(payload));
                
            } else if (strcmp(input, "/quit") == 0) {
                send_message(sock, MSG_BYE, "bye", 3);
                g_intentional_disconnect = 1;
                delete_last_line();
                break;
                
            } else if (strcmp(input, "/ping") == 0) {
                send_message(sock, MSG_PING, "ping", 4);
                
            } else {
                send_message(sock, MSG_TEXT, input, strlen(input));
            }
            
            delete_last_line();
        }
    }
}

int main(int argc, char *argv[]) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <server_ip> <nickname>\n", argv[0]);
        return 1;
    }
    
    const char *server_ip = argv[1];
    strncpy(g_nickname, argv[2], sizeof(g_nickname) - 1);
    
    while (1) {
        printf("[CLIENT] Connecting to %s:%d...\n", server_ip, SERVER_PORT);
        
        g_sock = connect_to_server(server_ip, SERVER_PORT);
        if (g_sock < 0) {
            printf("[CLIENT] Reconnecting in %d seconds...\n", RECONNECT_DELAY);
            sleep(RECONNECT_DELAY);
            continue;
        }
        
        if (do_auth(g_sock, g_nickname) < 0) {
            close(g_sock);
            printf("[CLIENT] auth failed, reconnecting...\n");
            sleep(RECONNECT_DELAY);
            continue;
        }
        
        printf("[CLIENT] Connected as '%s'\n> ", g_nickname);
        fflush(stdout);
        
        // Запуск сессии
        atomic_store(&g_running, 1);
        
        pthread_t recv_thr;
        if (pthread_create(&recv_thr, NULL, receive_thread, NULL) != 0) {
            fprintf(stderr, "Failed to create receive thread\n");
            close(g_sock);
            sleep(RECONNECT_DELAY);
            continue;
        }
        
        input_loop(g_sock);

        atomic_store(&g_running, 0);
        pthread_join(recv_thr, NULL);
        close(g_sock);               
        g_sock = -1;     
        
        if (g_intentional_disconnect) {
            break;
        }
        
        printf("\n[CLIENT] Disconnected. Reconnecting in %d seconds...\n", RECONNECT_DELAY);
        sleep(RECONNECT_DELAY);
    }

    printf("Goodbye!\n");
    
    return 0;
}