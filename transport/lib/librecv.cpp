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
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <unistd.h>
#include <unordered_map>

using namespace std;

std::map<int, struct connection *> cons;

struct pollfd data_fds[MAX_CONNECTIONS];
struct pollfd timer_fds[MAX_CONNECTIONS];
int fdmax = 0;
extern pthread_mutex_t core_lock;

int global_listener_sockfd;
static int conn_id_counter = 0;
std::unordered_map<int, int> fd_to_conn_id;

static inline int ring_idx(int seq) { return seq % MAX_WINDOW_SLOTS; }

int recv_data(int conn_id, char *buffer, int len) {
    int bytes_recv = 0;

    pthread_mutex_lock(&cons[conn_id]->con_lock);

    while (bytes_recv == 0) {
        while (bytes_recv < len) {
            int idx = ring_idx(cons[conn_id]->expected_seq);
            struct recv_slot *slot = &cons[conn_id]->recv_ring[idx];
            if (!slot->occupied)
                break;

            int avail = slot->len - slot->consumed;
            int copy = min(avail, len - bytes_recv);
            memcpy(buffer + bytes_recv, slot->data + slot->consumed, copy);
            bytes_recv += copy;
            slot->consumed += copy;

            if (slot->consumed >= slot->len) {
                slot->occupied = false;
                slot->consumed = 0;
                cons[conn_id]->expected_seq++;
            }
        }
        if (bytes_recv == 0) {
            pthread_cond_wait(&cons[conn_id]->data_avail,
                              &cons[conn_id]->con_lock);
        }
    }

    pthread_mutex_unlock(&cons[conn_id]->con_lock);

    return bytes_recv;
}

void *receiver_handler(void *arg) {

    char segment[MAX_SEGMENT_SIZE];
    int res;

    while (1) {

        int conn_id = -1;
        do {
            res = recv_message_or_timeout(segment, MAX_SEGMENT_SIZE, &conn_id);
        } while (res == -14);

        pthread_mutex_lock(&cons[conn_id]->con_lock);

        if (res >= (int)sizeof(struct poli_tcp_data_hdr)) {
            struct poli_tcp_data_hdr *seq_hdr =
                (struct poli_tcp_data_hdr *)segment;
            if (seq_hdr->type == TYPE_DATA) {
                int data_len = res - sizeof(struct poli_tcp_data_hdr);

                if (seq_hdr->seq_num >= cons[conn_id]->expected_seq) {
                    int idx = ring_idx(seq_hdr->seq_num);
                    struct recv_slot *slot = &cons[conn_id]->recv_ring[idx];
                    if (!slot->occupied) {
                        memcpy(slot->data,
                               segment + sizeof(struct poli_tcp_data_hdr),
                               data_len);
                        slot->len = data_len;
                        slot->consumed = 0;
                        slot->occupied = true;
                        pthread_cond_signal(&cons[conn_id]->data_avail);
                    }
                }

                int temp_seq = cons[conn_id]->expected_seq;
                while (cons[conn_id]->recv_ring[ring_idx(temp_seq)].occupied) {
                    temp_seq++;
                }

                struct poli_tcp_ctrl_hdr ack_hdr;
                memset(&ack_hdr, 0, sizeof(struct poli_tcp_ctrl_hdr));
                ack_hdr.type = TYPE_ACK;
                ack_hdr.protocol_id = POLI_PROTOCOL_ID;
                ack_hdr.conn_id = conn_id;
                ack_hdr.ack_num = temp_seq;

                sendto(cons[conn_id]->sockfd, &ack_hdr,
                       sizeof(struct poli_tcp_ctrl_hdr), 0,
                       (struct sockaddr *)&cons[conn_id]->clientvaddr,
                       cons[conn_id]->client_len);
            }
        }

        pthread_mutex_unlock(&cons[conn_id]->con_lock);
    }
}

int wait4connect(uint32_t ip, uint16_t port) {
    struct connection *con = new connection();
    memset(con->recv_ring, 0, sizeof(con->recv_ring));
    int conn_id = conn_id_counter++;
    con->expected_seq = 0;

    con->client_len = sizeof(con->clientvaddr);

    struct poli_tcp_ctrl_hdr syn_hdr;
    memset(&syn_hdr, 0, sizeof(struct poli_tcp_ctrl_hdr));
    int syn_bytes = recvfrom(
        global_listener_sockfd, &syn_hdr, sizeof(struct poli_tcp_ctrl_hdr), 0,
        (struct sockaddr *)&con->clientvaddr, &con->client_len);
    if (syn_bytes < 0) {
        perror("wait4connect recvfrom");
        exit(1);
    }

    con->sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (con->sockfd < 0) {
        perror("wait4connect socket");
        exit(1);
    }
    struct timeval tv;
    tv.tv_sec = 2;
    tv.tv_usec = 100000;
    if (setsockopt(con->sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
        perror("Error");
    }

    con->servaddr.sin_family = AF_INET;
    con->servaddr.sin_port = htons(0);
    con->servaddr.sin_addr.s_addr = ip;

    if (bind(con->sockfd, (struct sockaddr *)&con->servaddr,
             sizeof(con->servaddr)) < 0) {
        perror("wait4connect bind");
        exit(1);
    }

    con->serv_len = sizeof(con->servaddr);
    int sockname_succes = getsockname(
        con->sockfd, (struct sockaddr *)&con->servaddr, &con->serv_len);
    if (sockname_succes < 0) {
        perror("wait4connect getsockname");
        exit(1);
    }

    struct poli_tcp_ctrl_hdr syn_ack_hdr;
    memset(&syn_ack_hdr, 0, sizeof(struct poli_tcp_ctrl_hdr));
    syn_ack_hdr.type = TYPE_SYN_ACK;
    syn_ack_hdr.ack_num = con->servaddr.sin_port;
    syn_ack_hdr.conn_id = conn_id;

    int ack_bytes;
    do {
        sendto(global_listener_sockfd, &syn_ack_hdr,
               sizeof(struct poli_tcp_ctrl_hdr), 0,
               (struct sockaddr *)&con->clientvaddr, con->client_len);

        ack_bytes =
            recvfrom(con->sockfd, &syn_hdr, sizeof(struct poli_tcp_ctrl_hdr), 0,
                     (struct sockaddr *)&con->clientvaddr, &con->client_len);
    } while (ack_bytes < 0);

    data_fds[fdmax].fd = con->sockfd;
    data_fds[fdmax].events = POLLIN;

    timer_fds[fdmax].fd = timerfd_create(CLOCK_REALTIME, 0);
    con->timer_fd = timer_fds[fdmax].fd;
    timer_fds[fdmax].events = POLLIN;
    struct itimerspec spec;
    spec.it_value.tv_sec = 0;
    spec.it_value.tv_nsec = 10000000;
    spec.it_interval.tv_sec = 0;
    spec.it_interval.tv_nsec = 10000000;
    timerfd_settime(timer_fds[fdmax].fd, 0, &spec, NULL);
    pthread_mutex_init(&con->con_lock, NULL);
    pthread_cond_init(&con->data_avail, NULL);
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

void init_receiver(int recv_buffer_bytes) {
    pthread_t thread1;
    int ret;

    global_listener_sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (global_listener_sockfd < 0) {
        perror("global socket listen");
        exit(1);
    }

    struct sockaddr_in recv_addr;
    recv_addr.sin_family = AF_INET;
    recv_addr.sin_port = htons(RECV_PORT);
    recv_addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(global_listener_sockfd, (struct sockaddr *)&recv_addr,
             sizeof(recv_addr)) < 0) {
        perror("bind");
        exit(1);
    }

    ret = pthread_create(&thread1, NULL, receiver_handler, NULL);
    assert(ret == 0);
}
