//
// Created by Roger ZHUO on 2019-12-04.
//

#include <stdio.h>

#ifndef OMPI_COLL_R_BCAST_H
#define OMPI_COLL_R_BCAST_H

#define IP_MULTICAST_PORT 12441
#define IP_MULTICAST_ADDR "224.0.0.7"
#define MAX_BCAST_SIZE 512

#define START_MSG   0
#define DT_MSG      1
#define NACK_MSG    2
#define END_MSG     3

#define NUM_PROCESS 4
#define MAX_COMM    4

#define MSG_LIVE_TIME 128

#define BUFFER_SIZE 256

#define RECVFROM_TIMEOUT_MILLS 20

#define SENDING_STATUS 0
#define RECEIVING_METADATA_STATUS 1
#define RECEIVING_DATA_STATUS 2

#define MAX_MSG_SIZE (sizeof(_dt_msg_t) + MAX_BCAST_SIZE)

typedef struct _start_msg_t {
    int msg_type;
    int comm_id;
    int sender;
    int sequence;
    size_t size;
} start_msg_t;

typedef struct _dt_msg_t {
    int msg_type;
    int comm_id;
    int sender;
    long sequence;
    int size;
    int index;
    char data[];
} dt_msg_t;

typedef struct _nack_msg_t {
    int msg_type;
    int comm_id;
    int sender;
    int receiver;
    long nack_sequence;
} nack_msg_t;

typedef struct _end_msg_t {
    int msg_type;
    int comm_id;
    int sender;
    long sequence;
    int receiver;
} end_msg_t;

typedef struct _msg_header_t {
    int msg_type;
    int comm_id;
    int sender;
} msg_header_t;

typedef struct _bcast_msg_t {
    int msg_type;
    int comm_id;
    int sender;
    int receiver;
    long sequence;
    size_t t_size;
    size_t dt_size;
    int index;
    char data[];
} bcast_msg_t;


int comm_process_seq[NUM_PROCESS][MAX_COMM];
int comm_rank_num[MAX_COMM];

Queue* msg_buffer[MAX_COMM];

dt_msg_t* dt_msg;
start_msg_t* start_msg;
end_msg_t* end_msg;
nack_msg_t* nack_msg;

void* recv_msg;
dt_msg_t* recv_dt_msg;
start_msg_t* recv_start_msg;
end_msg_t* recv_end_msg;
nack_msg_t* recv_nack_msg;

#endif //OMPI_COLL_R_BCAST_H
