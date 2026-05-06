#define _DEFAULT_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/time.h>
#include <pthread.h>
#include <stdatomic.h>
#include <errno.h>
#include <math.h>
#include <time.h>
#include "message.h"

#define RECONNECT_DELAY 2
#define ACK_TIMEOUT_MS  2000
#define PING_TIMEOUT_MS 2000
#define MAX_RETRIES     3
#define MAX_PENDING     128
#define MAX_PING_STATS  1024

typedef struct {
    uint32_t msg_id;
    int received;
} AckWait;

typedef struct {
    uint32_t msg_id;
    double send_ms;
    double rtt_ms;
    int received;
} PingWait;

typedef struct {
    uint32_t msg_id;
    double rtt_ms;
    double jitter_ms;
    int has_jitter;
    int received;
} PingStat;

static int g_sock = -1;
static atomic_int g_running = 0;
static char g_nickname[MAX_NAME];
static int g_intentional_disconnect = 0;
static uint32_t g_client_msg_id = 1;

static AckWait g_acks[MAX_PENDING];
static PingWait g_pings[MAX_PENDING];
static PingStat g_stats[MAX_PING_STATS];
static int g_stats_count = 0;
static double g_last_rtt = -1.0;

static pthread_mutex_t g_ack_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_ack_cond = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t g_ping_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_ping_cond = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t g_stats_mutex = PTHREAD_MUTEX_INITIALIZER;

static double now_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec * 1000.0 + (double)tv.tv_usec / 1000.0;
}

static void make_abs_deadline(struct timespec *ts, int timeout_ms) {
    timespec_get(ts, TIME_UTC);
    ts->tv_sec += timeout_ms / 1000;
    ts->tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (ts->tv_nsec >= 1000000000L) {
        ts->tv_sec++;
        ts->tv_nsec -= 1000000000L;
    }
}

static uint32_t next_msg_id(void) {
    return g_client_msg_id++;
}

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
        tag = "[PRIVATE]";
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
           "  /w <nick> <message>   - send private message with ACK/retry\n"
           "  /ping [N]             - run N latency checks (default 10)\n"
           "  /netdiag              - print and save RTT/jitter/loss stats\n> ");
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

static void track_ack(uint32_t msg_id) {
    pthread_mutex_lock(&g_ack_mutex);
    for (int i = 0; i < MAX_PENDING; i++) {
        if (g_acks[i].msg_id == 0) {
            g_acks[i].msg_id = msg_id;
            g_acks[i].received = 0;
            break;
        }
    }
    pthread_mutex_unlock(&g_ack_mutex);
}

static void untrack_ack(uint32_t msg_id) {
    pthread_mutex_lock(&g_ack_mutex);
    for (int i = 0; i < MAX_PENDING; i++) {
        if (g_acks[i].msg_id == msg_id) {
            memset(&g_acks[i], 0, sizeof(g_acks[i]));
            break;
        }
    }
    pthread_mutex_unlock(&g_ack_mutex);
}

static int wait_ack(uint32_t msg_id, int timeout_ms) {
    struct timespec deadline;
    make_abs_deadline(&deadline, timeout_ms);

    pthread_mutex_lock(&g_ack_mutex);
    while (atomic_load(&g_running)) {
        for (int i = 0; i < MAX_PENDING; i++) {
            if (g_acks[i].msg_id == msg_id && g_acks[i].received) {
                pthread_mutex_unlock(&g_ack_mutex);
                return 1;
            }
        }
        int rc = pthread_cond_timedwait(&g_ack_cond, &g_ack_mutex, &deadline);
        if (rc == ETIMEDOUT) break;
    }
    pthread_mutex_unlock(&g_ack_mutex);
    return 0;
}

static void mark_ack(uint32_t msg_id) {
    pthread_mutex_lock(&g_ack_mutex);
    for (int i = 0; i < MAX_PENDING; i++) {
        if (g_acks[i].msg_id == msg_id) {
            g_acks[i].received = 1;
            break;
        }
    }
    pthread_cond_broadcast(&g_ack_cond);
    pthread_mutex_unlock(&g_ack_mutex);
}

static int send_reliable(int sock, const MessageEx *msg) {
    track_ack(msg->msg_id);
    printf("\r[Transport][SEND] send %s (id=%u)\n> ",
           msg->type == MSG_PRIVATE ? "MSG_PRIVATE" : "MSG_TEXT", msg->msg_id);
    fflush(stdout);

    for (int attempt = 0; attempt <= MAX_RETRIES; attempt++) {
        if (send_message_ex(sock, msg) < 0) {
            untrack_ack(msg->msg_id);
            return -1;
        }

        if (wait_ack(msg->msg_id, ACK_TIMEOUT_MS)) {
            fflush(stdout);
            untrack_ack(msg->msg_id);
            return 0;
        }

        printf("\r[Transport][RETRY] wait ACK timeout\n> ");
        if (attempt < MAX_RETRIES) {
            printf("\r[Transport][RETRY] resend %d/%d (id=%u)\n> ",
                   attempt + 1, MAX_RETRIES, msg->msg_id);
        } else {
            printf("\r[Transport][RETRY] delivery failed (id=%u)\n> ", msg->msg_id);
        }
        fflush(stdout);
    }

    untrack_ack(msg->msg_id);
    return -1;
}

static void track_ping(uint32_t msg_id, double sent_ms) {
    pthread_mutex_lock(&g_ping_mutex);
    for (int i = 0; i < MAX_PENDING; i++) {
        if (g_pings[i].msg_id == 0) {
            g_pings[i].msg_id = msg_id;
            g_pings[i].send_ms = sent_ms;
            g_pings[i].rtt_ms = 0.0;
            g_pings[i].received = 0;
            break;
        }
    }
    pthread_mutex_unlock(&g_ping_mutex);
}

static int wait_ping(uint32_t msg_id, double *rtt_ms) {
    struct timespec deadline;
    make_abs_deadline(&deadline, PING_TIMEOUT_MS);

    pthread_mutex_lock(&g_ping_mutex);
    while (atomic_load(&g_running)) {
        for (int i = 0; i < MAX_PENDING; i++) {
            if (g_pings[i].msg_id == msg_id && g_pings[i].received) {
                *rtt_ms = g_pings[i].rtt_ms;
                memset(&g_pings[i], 0, sizeof(g_pings[i]));
                pthread_mutex_unlock(&g_ping_mutex);
                return 1;
            }
        }
        int rc = pthread_cond_timedwait(&g_ping_cond, &g_ping_mutex, &deadline);
        if (rc == ETIMEDOUT) break;
    }

    for (int i = 0; i < MAX_PENDING; i++) {
        if (g_pings[i].msg_id == msg_id) {
            memset(&g_pings[i], 0, sizeof(g_pings[i]));
            break;
        }
    }
    pthread_mutex_unlock(&g_ping_mutex);
    return 0;
}

static void mark_pong(uint32_t msg_id) {
    double recv_ms = now_ms();
    pthread_mutex_lock(&g_ping_mutex);
    for (int i = 0; i < MAX_PENDING; i++) {
        if (g_pings[i].msg_id == msg_id) {
            g_pings[i].rtt_ms = recv_ms - g_pings[i].send_ms;
            g_pings[i].received = 1;
            break;
        }
    }
    pthread_cond_broadcast(&g_ping_cond);
    pthread_mutex_unlock(&g_ping_mutex);
}

static void add_ping_stat(uint32_t msg_id, int received, double rtt_ms, double *jitter_out, int *has_jitter) {
    pthread_mutex_lock(&g_stats_mutex);
    double jitter = 0.0;
    int has = 0;
    if (received && g_last_rtt >= 0.0) {
        jitter = fabs(rtt_ms - g_last_rtt);
        has = 1;
    }
    if (received) g_last_rtt = rtt_ms;

    if (g_stats_count < MAX_PING_STATS) {
        g_stats[g_stats_count].msg_id = msg_id;
        g_stats[g_stats_count].rtt_ms = rtt_ms;
        g_stats[g_stats_count].jitter_ms = jitter;
        g_stats[g_stats_count].has_jitter = has;
        g_stats[g_stats_count].received = received;
        g_stats_count++;
    }
    pthread_mutex_unlock(&g_stats_mutex);

    *jitter_out = jitter;
    *has_jitter = has;
}

static void print_and_save_netdiag(void) {
    pthread_mutex_lock(&g_stats_mutex);
    int sent = g_stats_count;
    int received = 0;
    int jitter_count = 0;
    double rtt_sum = 0.0;
    double jitter_sum = 0.0;

    for (int i = 0; i < g_stats_count; i++) {
        if (g_stats[i].received) {
            received++;
            rtt_sum += g_stats[i].rtt_ms;
            if (g_stats[i].has_jitter) {
                jitter_sum += g_stats[i].jitter_ms;
                jitter_count++;
            }
        }
    }

    double rtt_avg = received ? rtt_sum / received : 0.0;
    double jitter_avg = jitter_count ? jitter_sum / jitter_count : 0.0;
    double loss = sent ? ((double)(sent - received) / (double)sent) * 100.0 : 0.0;

    printf("\rRTT avg : %.1f ms\nJitter  : %.1f ms\nLoss    : %.1f%%\n> ",
           rtt_avg, jitter_avg, loss);

    char filename[128];
    snprintf(filename, sizeof(filename), "net_diag_%s.json", g_nickname);
    FILE *f = fopen(filename, "w");
    if (f) {
        fprintf(f,
                "{\n"
                "  \"nickname\": \"%s\",\n"
                "  \"sent\": %d,\n"
                "  \"received\": %d,\n"
                "  \"rtt_avg_ms\": %.3f,\n"
                "  \"jitter_avg_ms\": %.3f,\n"
                "  \"loss_percent\": %.3f\n"
                "}\n",
                g_nickname, sent, received, rtt_avg, jitter_avg, loss);
        fclose(f);
        printf("[Application] saved %s\n> ", filename);
    }
    fflush(stdout);
    pthread_mutex_unlock(&g_stats_mutex);
}

static int do_auth(int sock, const char *nickname) {
    MessageEx auth = {0};
    auth.type = MSG_AUTH;
    auth.msg_id = next_msg_id();
    auth.timestamp = time(NULL);
    strncpy(auth.sender, nickname, MAX_NAME - 1);
    strncpy(auth.payload, nickname, MAX_PAYLOAD - 1);
    auth.length = strlen(auth.payload);

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
            pthread_cond_broadcast(&g_ack_cond);
            pthread_cond_broadcast(&g_ping_cond);
            break;
        }

        switch (msg.type) {
            case MSG_TEXT:
            case MSG_PRIVATE:
                print_message_ex(&msg);
                break;
            case MSG_ACK:
                {
                    int is_pending = 0;
                    pthread_mutex_lock(&g_ack_mutex);
                    for (int i = 0; i < MAX_PENDING; i++) {
                        if (g_acks[i].msg_id == msg.msg_id && !g_acks[i].received) {
                            is_pending = 1;
                            break;
                        }
                    }
                    pthread_mutex_unlock(&g_ack_mutex);

                    if (is_pending) {
                        printf("\r[Transport][ACK] recv MSG_ACK (id=%u)\n> ", msg.msg_id);
                        fflush(stdout);
                        mark_ack(msg.msg_id);
                    }
                }
                break;
            case MSG_PONG:
                printf("\r[Transport][PING] recv MSG_PONG (id=%u)\n> ", msg.msg_id);
                fflush(stdout);
                mark_pong(msg.msg_id);
                break;
            case MSG_SERVER_INFO:
            case MSG_WELCOME:
                printf("\r[SERVER] %s\n> ", msg.payload);
                fflush(stdout);
                break;
            case MSG_HISTORY_DATA:
                printf("[%s][id=%u][%s]: %s\n", "HISTORY", msg.msg_id, msg.sender, msg.payload);
                fflush(stdout);
                break;
            case MSG_ERROR:
                fprintf(stdout, "\r[ERROR] %s\n> ", msg.payload);
                fflush(stdout);
                break;
            case MSG_HELP:
                printf("\r[HELP] %s\n> ", msg.payload);
                fflush(stdout);
                break;
            default:
                break;
        }
    }
    return NULL;
}

static void run_ping_series(int sock, int count) {
    if (count <= 0) count = 10;
    if (count > 1000) count = 1000;

    for (int i = 1; i <= count && atomic_load(&g_running); i++) {
        MessageEx ping = {0};
        ping.type = MSG_PING;
        ping.msg_id = next_msg_id();
        ping.timestamp = time(NULL);
        strncpy(ping.sender, g_nickname, MAX_NAME - 1);
        strncpy(ping.payload, "ping", MAX_PAYLOAD - 1);
        ping.length = 4;

        track_ping(ping.msg_id, now_ms());
        printf("\r[Transport][PING] send MSG_PING (id=%u)\n> ", ping.msg_id);
        fflush(stdout);

        if (send_message_ex(sock, &ping) < 0) {
            atomic_store(&g_running, 0);
            break;
        }

        double rtt = 0.0;
        double jitter = 0.0;
        int has_jitter = 0;
        if (wait_ping(ping.msg_id, &rtt)) {
            add_ping_stat(ping.msg_id, 1, rtt, &jitter, &has_jitter);
            if (has_jitter) printf("\rPING %d -> RTT=%.1fms | Jitter=%.1fms\n> ", i, rtt, jitter);
            else printf("\rPING %d -> RTT=%.1fms\n> ", i, rtt);
        } else {
            add_ping_stat(ping.msg_id, 0, 0.0, &jitter, &has_jitter);
            printf("\rPING %d -> timeout\n> ", i);
        }
        fflush(stdout);
        usleep(100000);
    }
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
                print_help();
                continue;
            } else if (strcmp(input, "/list") == 0) {
                MessageEx req = {0};
                req.type = MSG_LIST;
                req.msg_id = next_msg_id();
                req.timestamp = time(NULL);
                strncpy(req.sender, g_nickname, MAX_NAME - 1);
                send_message_ex(sock, &req);
                delete_last_line();
                continue;
            } else if (strncmp(input, "/history", 8) == 0) {
                MessageEx req = {0};
                req.type = MSG_HISTORY;
                req.msg_id = next_msg_id();
                req.timestamp = time(NULL);
                strncpy(req.sender, g_nickname, MAX_NAME - 1);
                char *endptr;
                long n = strtol(input + 8, &endptr, 10);
                if (n > 0 && n <= 100 && (*endptr == '\0' || *endptr == ' ')) {
                    snprintf(req.payload, MAX_PAYLOAD, "%ld", n);
                    req.length = strlen(req.payload);
                }
                send_message_ex(sock, &req);
                delete_last_line();
                continue;
            } else if (strncmp(input, "/w", 2) == 0) {
                char *ptr = input + 2;
                while (*ptr == ' ') ptr++;
                
                char *space = strchr(ptr, ' ');
                if (*ptr == '\0' || !space) {
                    printf("\r[ERROR] Format: /w <nick> <message>\n> ");
                    fflush(stdout);
                    continue;
                }

                *space = '\0';
                char *nick = ptr;
                char *msg_text = space + 1;
                while (*msg_text == ' ') msg_text++;
                if (*msg_text == '\0') {
                    printf("\r[ERROR] Format: /w <nick> <message>\n> ");
                    fflush(stdout);
                    continue;
                }

                MessageEx priv = {0};
                priv.type = MSG_PRIVATE;
                priv.msg_id = next_msg_id();
                priv.timestamp = time(NULL);

                snprintf(priv.sender,   MAX_NAME, "%s", g_nickname);
                snprintf(priv.receiver, MAX_NAME, "%s", nick);
                snprintf(priv.payload,  MAX_PAYLOAD, "%s", msg_text);
                priv.length = strlen(priv.payload);

                send_reliable(sock, &priv);
                delete_last_line();
                continue;
            } else if (strcmp(input, "/quit") == 0) {
                MessageEx bye = {0};
                bye.type = MSG_BYE;
                bye.msg_id = next_msg_id();
                bye.timestamp = time(NULL);
                strncpy(bye.sender, g_nickname, MAX_NAME - 1);
                strncpy(bye.payload, "bye", MAX_PAYLOAD - 1);
                bye.length = 3;
                send_message_ex(sock, &bye);
                g_intentional_disconnect = 1;
                delete_last_line();
                break;
            } else if (strncmp(input, "/ping", 5) == 0) {
                int count = 10;
                if (strlen(input) > 5) {
                    int parsed = atoi(input + 5);
                    if (parsed > 0) count = parsed;
                }
                run_ping_series(sock, count);
                delete_last_line();
                continue;
            } else if (strcmp(input, "/netdiag") == 0) {
                print_and_save_netdiag();
                continue;
            } else {
                MessageEx text = {0};
                text.type = MSG_TEXT;
                text.msg_id = next_msg_id();
                text.timestamp = time(NULL);
                strncpy(text.sender, g_nickname, MAX_NAME - 1);
                strncpy(text.payload, input, MAX_PAYLOAD - 1);
                text.length = strlen(text.payload);
                send_reliable(sock, &text);
                delete_last_line();
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
        printf("> ");
        fflush(stdout);

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
        pthread_cond_broadcast(&g_ack_cond);
        pthread_cond_broadcast(&g_ping_cond);
        shutdown(g_sock, SHUT_RDWR);
        pthread_join(recv_thr, NULL);
        close(g_sock);
        g_sock = -1;
        if (g_intentional_disconnect) break;
        sleep(RECONNECT_DELAY);
    }
    printf("Goodbye!\n");
    return 0;
}
