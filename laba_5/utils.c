/* utils.c — безопасная сериализация MessageEx (чистый C11) */
#include "message.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <time.h>

#ifndef HISTORY_FILE
#define HISTORY_FILE "chat_history.txt"
#endif

static int send_all(int sock, const void *buf, size_t size) {
    size_t sent = 0;
    while (sent < size) {
        ssize_t n = send(sock, (const char*)buf + sent, size - sent, 0);
        if (n <= 0) return -1;
        sent += n;
    }
    return 0;
}

static int recv_all(int sock, void *buf, size_t size) {
    size_t received = 0;
    while (received < size) {
        ssize_t n = recv(sock, (char*)buf + received, size - received, 0);
        if (n <= 0) return -1;
        received += n;
    }
    return 0;
}

static int send_uint32(int sock, uint32_t val) {
    uint32_t net = htonl(val);
    return send_all(sock, &net, sizeof(net));
}

static int send_uint8(int sock, uint8_t val) {
    return send_all(sock, &val, sizeof(val));
}

static int send_fixed_string(int sock, const char *str, size_t fixed_len) {
    size_t len = 0;
    while (len < fixed_len && str[len] != '\0') len++;

    if (len > 0 && send_all(sock, str, len) < 0) return -1;

    char zero = '\0';
    for (size_t i = len; i < fixed_len; i++) {
        if (send_all(sock, &zero, 1) < 0) return -1;
    }
    return 0;
}

static int recv_uint32(int sock, uint32_t *out) {
    uint32_t net;
    if (recv_all(sock, &net, sizeof(net)) < 0) return -1;
    *out = ntohl(net);
    return 0;
}

static int recv_uint8(int sock, uint8_t *out) {
    return recv_all(sock, out, sizeof(*out));
}

static int recv_fixed_string(int sock, char *buf, size_t fixed_len) {
    if (recv_all(sock, buf, fixed_len) < 0) return -1;
    buf[fixed_len - 1] = '\0';
    return 0;
}

int send_message_ex(int sock, const MessageEx *msg) {
    if (send_uint32(sock, msg->length) < 0) return -1;
    if (send_uint8(sock, msg->type) < 0) return -1;
    if (send_uint32(sock, msg->msg_id) < 0) return -1;
    if (send_uint32(sock, (uint32_t)msg->timestamp) < 0) return -1;
    if (send_fixed_string(sock, msg->sender, MAX_NAME) < 0) return -1;
    if (send_fixed_string(sock, msg->receiver, MAX_NAME) < 0) return -1;
    
    if (msg->length > 0 && msg->length <= MAX_PAYLOAD) {
        if (send_all(sock, msg->payload, msg->length) < 0) return -1;
    }
    return 0;
}

int recv_message_ex(int sock, MessageEx *msg) {
    memset(msg, 0, sizeof(MessageEx));
    
    if (recv_uint32(sock, &msg->length) < 0) return -1;
    if (recv_uint8(sock, &msg->type) < 0) return -1;
    if (recv_uint32(sock, &msg->msg_id) < 0) return -1;
    
    uint32_t ts_net;
    if (recv_uint32(sock, &ts_net) < 0) return -1;
    msg->timestamp = (time_t)ts_net;
    
    if (recv_fixed_string(sock, msg->sender, MAX_NAME) < 0) return -1;
    if (recv_fixed_string(sock, msg->receiver, MAX_NAME) < 0) return -1;
    
    if (msg->length > MAX_PAYLOAD) return -1;
    
    if (msg->length > 0) {
        if (recv_all(sock, msg->payload, msg->length) < 0) return -1;
    }
    msg->payload[msg->length] = '\0';
    return 0;
}

void log_tcp_ip(const char *direction, const char *app_msg,
                const char *src_ip, const char *dst_ip, int src_port, int dst_port) {
    time_t now = time(NULL);
    char time_buf[MAX_TIME_STR];
    strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", localtime(&now));
    
    printf("[%s][Network Access] frame %s via network interface\n", time_buf, direction);
    printf("[%s][Internet] src=%s dst=%s proto=TCP\n", time_buf, src_ip, dst_ip);
    printf("[%s][Transport] %s port=%d -> port=%d\n", time_buf, direction, src_port, dst_port);
    printf("[%s][Application] %s\n", time_buf, app_msg);
    fflush(stdout);
}

static const char *type_to_string(uint8_t type) {
    switch (type) {
        case MSG_TEXT: return "MSG_TEXT";
        case MSG_PRIVATE: return "MSG_PRIVATE";
        case MSG_SERVER_INFO: return "MSG_SERVER_INFO";
        case MSG_WELCOME: return "MSG_WELCOME";
        case MSG_HISTORY_DATA: return "MSG_HISTORY_DATA";
        default: return "MSG_UNKNOWN";
    }
}

int save_to_history(const MessageEx *msg, int delivered, int is_offline) {
    FILE *f = fopen(HISTORY_FILE, "a");
    if (!f) return -1;
    
    char time_buf[MAX_TIME_STR];
    strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", localtime(&msg->timestamp));
    
    char safe_payload[MAX_PAYLOAD];
    size_t j = 0;
    for (size_t i = 0; msg->payload[i] && j < sizeof(safe_payload)-1; i++) {
        if (msg->payload[i] == '|' || msg->payload[i] == '\n') safe_payload[j++] = ' ';
        else safe_payload[j++] = msg->payload[i];
    }
    safe_payload[j] = '\0';
    
    fprintf(f, "%u|%s|%s|%s|%s|%s|%d|%d\n",
            msg->msg_id, time_buf, msg->sender, msg->receiver,
            type_to_string(msg->type), safe_payload, delivered, is_offline);
    fclose(f);
    return 0;
}

int load_last_history(int count, MessageEx *out_msgs, int max_count) {
    if (count <= 0) count = 20;
    if (count > max_count) count = max_count;
    
    FILE *f = fopen(HISTORY_FILE, "r");
    if (!f) return 0;
    
    char lines[2000][1024];
    int line_count = 0;
    while (fgets(lines[line_count], sizeof(lines[line_count]), f) && line_count < 2000) {
        lines[line_count][strcspn(lines[line_count], "\n")] = '\0';
        line_count++;
    }
    fclose(f);
    
    int start = (line_count > count) ? line_count - count : 0;
    int loaded = 0;
    
    for (int i = start; i < line_count && loaded < max_count; i++, loaded++) {
        char *saveptr;
        char *token = strtok_r(lines[i], "|", &saveptr);
        if (!token) continue;
        out_msgs[loaded].msg_id = (uint32_t)strtoul(token, NULL, 10);
        token = strtok_r(NULL, "|", &saveptr);
        token = strtok_r(NULL, "|", &saveptr);
        if (token) strncpy(out_msgs[loaded].sender, token, MAX_NAME-1);
        token = strtok_r(NULL, "|", &saveptr);
        if (token) strncpy(out_msgs[loaded].receiver, token, MAX_NAME-1);
        token = strtok_r(NULL, "|", &saveptr);
        token = strtok_r(NULL, "|", &saveptr);
        if (token) strncpy(out_msgs[loaded].payload, token, MAX_PAYLOAD-1);
        
        out_msgs[loaded].type = MSG_TEXT;
        out_msgs[loaded].timestamp = time(NULL);
        out_msgs[loaded].length = strlen(out_msgs[loaded].payload);
        if (out_msgs[loaded].length >= MAX_PAYLOAD) out_msgs[loaded].length = MAX_PAYLOAD-1;
    }
    return loaded;
}