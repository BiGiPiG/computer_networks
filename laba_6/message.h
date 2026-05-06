#ifndef MESSAGE_H
#define MESSAGE_H

#include <stdint.h>
#include <stddef.h>
#include <time.h>

#define MAX_PAYLOAD      256
#define MAX_NAME         32
#define MAX_TIME_STR     32
#define SERVER_PORT      8888
#define HISTORY_FILE     "chat_history.txt"
#define MAX_OFFLINE_MSGS 100

enum {
    MSG_HELLO        = 1,
    MSG_WELCOME      = 2,
    MSG_TEXT         = 3,
    MSG_PING         = 4,
    MSG_PONG         = 5,
    MSG_BYE          = 6,
    MSG_AUTH         = 7,
    MSG_PRIVATE      = 8,
    MSG_ERROR        = 9,
    MSG_SERVER_INFO  = 10,
    MSG_LIST         = 11,
    MSG_HISTORY      = 12,
    MSG_HISTORY_DATA = 13,
    MSG_HELP         = 14,
    MSG_ACK          = 15
};

typedef struct {
    uint32_t length;
    uint8_t  type;
    uint32_t msg_id;
    char     sender[MAX_NAME];
    char     receiver[MAX_NAME];
    time_t   timestamp;
    char     payload[MAX_PAYLOAD];
} MessageEx;

int send_message_ex(int sock, const MessageEx *msg);
int recv_message_ex(int sock, MessageEx *msg);

int save_to_history(const MessageEx *msg, int delivered, int is_offline);
int load_last_history(int count, MessageEx *out_msgs, int max_count);

void log_tcp_ip(const char *direction, const char *app_msg,
                const char *src_ip, const char *dst_ip, int src_port, int dst_port);

#endif
