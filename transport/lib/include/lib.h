
#pragma once

#include <arpa/inet.h>
#include <cstdint>
#include <ctime>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>

#define MAX_DATA_SIZE 512
#define POLI_TCP_DATA_HDR_SIZE 7
#define MAX_SEGMENT_SIZE (MAX_DATA_SIZE + POLI_TCP_DATA_HDR_SIZE)

#define MAX_CONNECTIONS 32
#define MAX_WINDOW_SLOTS 512

struct pkt_slot {
    char buf[MAX_SEGMENT_SIZE];
    int len;
    struct timespec send_time;
    bool occupied;
};

struct recv_slot {
    char data[MAX_DATA_SIZE];
    int len;
    int consumed;
    bool occupied;
};

struct connection {
    int sockfd;
    int conn_id;
    struct sockaddr_in servaddr;
    socklen_t serv_len;
    struct sockaddr_in clientvaddr;
    socklen_t client_len;
    pthread_mutex_t con_lock;
    pthread_cond_t window_avail;
    pthread_cond_t data_avail;

    int max_window_seq;
    int next_seq_num;
    int last_ack;
    int timer_fd;

    struct pkt_slot send_ring[MAX_WINDOW_SLOTS];
    struct recv_slot recv_ring[MAX_WINDOW_SLOTS];
    int expected_seq;

    long rto_ns;
    long srtt_ns;
    bool srtt_init;
};

int wait4connect(uint32_t ip, uint16_t port);
int setup_connection(uint32_t ip, uint16_t port);
int recv_data(int connectionid, char *buffer, int len);
int send_data(int conn_id, char *buffer, int len);
void init_receiver(int recv_buffer_bytes);
void init_sender(int speed, int delay);

int recv_message_or_timeout(char *buff, size_t len, int *conn_id);
