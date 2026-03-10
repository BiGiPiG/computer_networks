#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include "message.h"

int main() {
    int server_sock, client_sock;
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_len = sizeof(client_addr);
    
    server_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (server_sock < 0) {
        perror("socket");
        return 1;
    }
    
    int opt = 1;
    setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons(SERVER_PORT);
    
    if (bind(server_sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind");
        close(server_sock);
        return 1;
    }
    
    if (listen(server_sock, 1) < 0) {
        perror("listen");
        close(server_sock);
        return 1;
    }
    
    printf("Server listening on port %d...\n", SERVER_PORT);
    
    client_sock = accept(server_sock, (struct sockaddr*)&client_addr, &client_len);
    if (client_sock < 0) {
        perror("accept");
        close(server_sock);
        return 1;
    }
    
    char client_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip));
    printf("Client connected [%s:%d]\n", client_ip, ntohs(client_addr.sin_port));
    
    Message msg;
    int running = 1;
    
    if (recv_message(client_sock, &msg) < 0 || msg.type != MSG_HELLO) {
        fprintf(stderr, "Expected MSG_HELLO\n");
        close(client_sock);
        close(server_sock);
        return 1;
    }
    printf("[%s:%d]: %s\n", client_ip, ntohs(client_addr.sin_port), msg.payload);
    
    char welcome[64];
    snprintf(welcome, sizeof(welcome), "Welcome %s:%d", client_ip, ntohs(client_addr.sin_port));
    send_message(client_sock, MSG_WELCOME, welcome, strlen(welcome));
    
    while (running) {
        if (recv_message(client_sock, &msg) < 0) {
            printf("Connection lost\n");
            break;
        }
        
        switch (msg.type) {
            case MSG_TEXT:
                printf("[%s:%d]: %s\n", client_ip, ntohs(client_addr.sin_port), msg.payload);
                break;
            case MSG_PING:
                printf("[%s:%d]: %s\n", client_ip, ntohs(client_addr.sin_port), "/test");
                send_message(client_sock, MSG_PONG, "PONG", 4);
                break;
            case MSG_BYE:
                running = 0;
                break;
            default:
                fprintf(stderr, "Unknown message type: %d\n", msg.type);
        }
    }
    
    close(client_sock);
    close(server_sock);
    printf("Client disconnected\n");
    
    return 0;
}