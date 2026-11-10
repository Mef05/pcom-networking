#include "lib.h"
#include "protocol.h"
#include "utils.h"
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <poll.h>
#include <pthread.h>
#include <sys/timerfd.h>
#include <unistd.h>
#include <unordered_map>

using namespace std;

std::map<int, struct connection *> cons;

struct pollfd data_fds[MAX_CONNECTIONS];
struct pollfd timer_fds[MAX_CONNECTIONS];
int fdmax = 0;
extern pthread_mutex_t core_lock;
int global_window_size;
int global_delay_ms;
std::unordered_map<int, int> fd_to_conn_id;

static inline int ring_idx(int seq) { return seq % MAX_WINDOW_SLOTS; }

static void update_rto(struct connection *con, long sample_ns) {
    if (!con->srtt_init) {
        con->srtt_ns = sample_ns;
        con->srtt_init = true;
    } else {
        con->srtt_ns = (7 * con->srtt_ns + sample_ns) / 8;
    }
    con->rto_ns = con->srtt_ns * 2;
    if (con->rto_ns < 5000000L)
        con->rto_ns = 5000000L;
    if (con->rto_ns > 500000000L)
        con->rto_ns = 500000000L;
}

static void arm_timer(struct connection *con) {
    struct itimerspec spec;
    spec.it_value.tv_sec = 0;
    spec.it_value.tv_nsec = con->rto_ns;
    spec.it_interval.tv_sec = 0;
    spec.it_interval.tv_nsec = con->rto_ns;
    timerfd_settime(con->timer_fd, 0, &spec, NULL);
}

int send_data(int conn_id, char *buffer, int len) {
    int bytes_sent = 0;

    pthread_mutex_lock(&cons[conn_id]->con_lock);

    while (bytes_sent < len) {
        if (cons[conn_id]->next_seq_num <=
            cons[conn_id]->last_ack + cons[conn_id]->max_window_seq) {

            int data_size = min(MAX_DATA_SIZE, len - bytes_sent);

            struct poli_tcp_data_hdr data_hdr;
            memset(&data_hdr, 0, sizeof(struct poli_tcp_data_hdr));
            data_hdr.type = TYPE_DATA;
            data_hdr.seq_num = cons[conn_id]->next_seq_num;
            data_hdr.protocol_id = POLI_PROTOCOL_ID;
            data_hdr.conn_id = conn_id;

            int idx = ring_idx(cons[conn_id]->next_seq_num);
            struct pkt_slot *slot = &cons[conn_id]->send_ring[idx];
            memcpy(slot->buf, &data_hdr, sizeof(struct poli_tcp_data_hdr));
            memcpy(slot->buf + sizeof(struct poli_tcp_data_hdr),
                   buffer + bytes_sent, data_size);
            slot->len = sizeof(struct poli_tcp_data_hdr) + data_size;
            clock_gettime(CLOCK_MONOTONIC_RAW, &slot->send_time);
            slot->occupied = true;

            bytes_sent += data_size;
            cons[conn_id]->next_seq_num++;

            pthread_mutex_unlock(&cons[conn_id]->con_lock);

            sendto(cons[conn_id]->sockfd, slot->buf, slot->len, 0,
                   (struct sockaddr *)&cons[conn_id]->servaddr,
                   cons[conn_id]->serv_len);

            pthread_mutex_lock(&cons[conn_id]->con_lock);
        } else {
            pthread_cond_wait(&cons[conn_id]->window_avail,
                              &cons[conn_id]->con_lock);
        }
    }

    pthread_mutex_unlock(&cons[conn_id]->con_lock);

    return bytes_sent;
}

void *sender_handler(void *arg) {
    char buf[MAX_SEGMENT_SIZE];

    while (1) {
        if (cons.size() == 0) {
            continue;
        }
        int conn_id = -1;
        int res;
        do {
            res = recv_message_or_timeout(buf, MAX_SEGMENT_SIZE, &conn_id);
        } while (res == -14);

        pthread_mutex_lock(&cons[conn_id]->con_lock);

        if (res == -1) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC_RAW, &now);
            for (int seq = cons[conn_id]->last_ack + 1;
                 seq < cons[conn_id]->next_seq_num; seq++) {
                int idx = ring_idx(seq);
                struct pkt_slot *slot = &cons[conn_id]->send_ring[idx];
                if (!slot->occupied)
                    continue;
                long elapsed_ns =
                    (now.tv_sec - slot->send_time.tv_sec) * 1000000000L +
                    (now.tv_nsec - slot->send_time.tv_nsec);
                if (elapsed_ns > cons[conn_id]->rto_ns) {
                    pthread_mutex_unlock(&cons[conn_id]->con_lock);
                    sendto(cons[conn_id]->sockfd, slot->buf, slot->len, 0,
                           (struct sockaddr *)&cons[conn_id]->servaddr,
                           cons[conn_id]->serv_len);
                    pthread_mutex_lock(&cons[conn_id]->con_lock);
                    clock_gettime(CLOCK_MONOTONIC_RAW, &slot->send_time);
                }
            }
        } else {
            while (1) {
                if (res >= (int)sizeof(struct poli_tcp_ctrl_hdr)) {
                    struct poli_tcp_ctrl_hdr *ack_hdr =
                        (struct poli_tcp_ctrl_hdr *)buf;
                    if (ack_hdr->type == TYPE_ACK) {
                        int new_ack = (int)ack_hdr->ack_num - 1;
                        if (new_ack > cons[conn_id]->last_ack) {
                            struct timespec now;
                            clock_gettime(CLOCK_MONOTONIC_RAW, &now);
                            int idx = ring_idx(new_ack);
                            struct pkt_slot *slot =
                                &cons[conn_id]->send_ring[idx];
                            if (slot->occupied) {
                                long sample =
                                    (now.tv_sec - slot->send_time.tv_sec) *
                                        1000000000L +
                                    (now.tv_nsec - slot->send_time.tv_nsec);
                                update_rto(cons[conn_id], sample);
                            }

                            for (int i = cons[conn_id]->last_ack + 1;
                                 i <= new_ack; i++) {
                                cons[conn_id]->send_ring[ring_idx(i)].occupied =
                                    false;
                            }
                            cons[conn_id]->last_ack = new_ack;
                            pthread_cond_signal(&cons[conn_id]->window_avail);
                            arm_timer(cons[conn_id]);
                        }
                    }
                }
                res = recvfrom(cons[conn_id]->sockfd, buf, MAX_SEGMENT_SIZE,
                               MSG_DONTWAIT, NULL, NULL);
                if (res <= 0)
                    break;
            }
        }

        pthread_mutex_unlock(&cons[conn_id]->con_lock);
    }
}

int setup_connection(uint32_t ip, uint16_t port) {
    struct connection *con = new connection();
    memset(con->send_ring, 0, sizeof(con->send_ring));
    con->max_window_seq = global_window_size;
    con->next_seq_num = 0;
    con->last_ack = -1;
    con->rto_ns = global_delay_ms * 2 * 1000000L * 2;
    con->srtt_ns = global_delay_ms * 2 * 1000000L;
    con->srtt_init = false;
    int conn_id = 0;
    con->sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (con->sockfd < 0) {
        perror("setup connection socket error");
        exit(1);
    }

    struct timeval tv;
    tv.tv_sec = 2;
    tv.tv_usec = 100000;
    if (setsockopt(con->sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
        perror("Error");
    }

    memset(&con->servaddr, 0, sizeof(con->servaddr));
    con->servaddr.sin_family = AF_INET;
    con->servaddr.sin_addr.s_addr = ip;
    con->servaddr.sin_port = port;

    con->serv_len = sizeof(con->servaddr);

    struct poli_tcp_ctrl_hdr syn_hdr;
    memset(&syn_hdr, 0, sizeof(struct poli_tcp_ctrl_hdr));
    syn_hdr.type = TYPE_SYN;
    syn_hdr.ack_num = 0;

    int syn_ack_bytes;
    struct poli_tcp_ctrl_hdr syn_ack_hdr;
    do {
        sendto(con->sockfd, &syn_hdr, sizeof(struct poli_tcp_ctrl_hdr), 0,
               (struct sockaddr *)&con->servaddr, sizeof(con->servaddr));

        syn_ack_bytes = recvfrom(
            con->sockfd, &syn_ack_hdr, sizeof(struct poli_tcp_ctrl_hdr), 0,
            (struct sockaddr *)&con->servaddr, &con->serv_len);

    } while (syn_ack_bytes < 0);

    con->servaddr.sin_port = syn_ack_hdr.ack_num;
    conn_id = syn_ack_hdr.conn_id;
    con->conn_id = conn_id;

    struct poli_tcp_ctrl_hdr ack_hdr;
    memset(&ack_hdr, 0, sizeof(struct poli_tcp_ctrl_hdr));
    ack_hdr.type = TYPE_ACK;
    ack_hdr.ack_num = 0;
    ack_hdr.protocol_id = POLI_PROTOCOL_ID;

    sendto(con->sockfd, &ack_hdr, sizeof(struct poli_tcp_ctrl_hdr), 0,
           (struct sockaddr *)&con->servaddr, sizeof(con->servaddr));

    data_fds[fdmax].fd = con->sockfd;
    data_fds[fdmax].events = POLLIN;

    timer_fds[fdmax].fd = timerfd_create(CLOCK_REALTIME, 0);
    con->timer_fd = timer_fds[fdmax].fd;
    timer_fds[fdmax].events = POLLIN;
    arm_timer(con);
    pthread_mutex_init(&con->con_lock, NULL);
    pthread_cond_init(&con->window_avail, NULL);
    pthread_mutex_lock(&core_lock);
    cons.insert({conn_id, con});
    fd_to_conn_id[con->sockfd] = conn_id;
    fdmax++;
    pthread_mutex_unlock(&core_lock);

    tv.tv_sec = 0;
    tv.tv_usec = 0;
    if (setsockopt(con->sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
        perror("Error");
    }

    return conn_id;
}

void init_sender(int speed, int delay) {
    pthread_t thread1;
    int ret;

    int BDP = (speed * 1e6 / 8) * (delay * 2 / 1e3);

    global_window_size = BDP / MAX_SEGMENT_SIZE;
    global_delay_ms = delay;

    ret = pthread_create(&thread1, NULL, sender_handler, NULL);
    assert(ret == 0);
}
