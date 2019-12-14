//
// Created by Roger ZHUO on 2019-12-04.
//

#include <stdio.h>

#ifndef OMPI_COLL_R_BCAST_H
#define OMPI_COLL_R_BCAST_H

#define IP_MULTICAST_PORT 12450
#define IP_MULTICAST_ADDR "224.0.0.7"
#define MAX_BCAST_SIZE 512

#define DT_MSG      0
#define NACK_MSG    1
#define END_MSG     2

#define NUM_PROCESS 4
#define MAX_COMM    4

#define MSG_LIVE_TIME 20

#define BUFFER_SIZE 256

#define RECVFROM_TIMEOUT_MILLS 5

#define SENDER_HEARTBEAT_MILLS 5

#define MIN_BCAST_PACKETS 20

#define SENDING_STATUS 0
#define RECEIVING_METADATA_STATUS 1
#define RECEIVING_DATA_STATUS 2

typedef struct _QueueNode{
    void* data;
    struct _QueueNode* next;
    struct _QueueNode* prev;
}QueueNode;

typedef struct _Queue{
    QueueNode* head;
    QueueNode* tail;
    QueueNode* cur;
    unsigned long length;
}Queue;

void* deQueue(Queue* queue);

void* enQueue(Queue* queue, void* data);

Queue* initQueue();

void freeQueue(Queue* queue);

void* pop(Queue* queue, QueueNode* curNode);

void moveToHead(Queue* queue);

int moveToNext(Queue* queue);

void traverseQueue(Queue* queue);



typedef struct _bcast_msg_t {
    int msg_type;               // message type, for different purpose
    int sender;                 // global rank of sender
    int receiver[NUM_PROCESS];  // global rank of receivers
    long sequence;              // sequence number
    size_t t_size;              // current Bcast's total data size. I found it redundant.
    size_t dt_size;             // current packet's data size. I found it redundant.
    int index;                  // index of packet in Bcast
    char data[];                // data
} bcast_msg_t;

#define MAX_MSG_SIZE (sizeof(bcast_msg_t) + MAX_BCAST_SIZE)


typedef struct _comm_info_t {
    int proc_seq[NUM_PROCESS];      // process's sequence number in this communicator
    int size;                       // # of ranks in this communicator
    int global_ranks[NUM_PROCESS];  // global ranks of processes
    int initialized;                // initialized or not
    Queue* msg_buffer;              // buffer
} comm_info_t;

static comm_info_t comm_infos[MAX_COMM];

static bcast_msg_t* recv_msg;
static bcast_msg_t* send_msg;

void print_arr(int arr[], int size);

void print_comm_info(comm_info_t *comm_info);

void print_rank_info();

void print_msg(bcast_msg_t* msg);

#endif //OMPI_COLL_R_BCAST_H
