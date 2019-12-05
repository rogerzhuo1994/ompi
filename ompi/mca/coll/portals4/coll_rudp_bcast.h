//
// Created by Roger ZHUO on 2019-12-04.
//

#ifndef OMPI_COLL_R_BCAST_H
#define OMPI_COLL_R_BCAST_H

#define START_MSG   0
#define DT_MSG      1
#define NACK_MSG    2
#define END_MSG     3

#define NUM_PROCESS 4
#define MAX_COMM    4

#define MSG_LIVE_TIME 128

#define BUFFER_SIZE 256

typedef struct start_msg {
    int msg_type;
    int comm_id;
    int sender;
    int sequence;
    int size;
};

typedef struct dt_msg_hdr {
    int msg_type;
    int comm_id;
    int sender;
    long sequence;
    int size;
    int index;
};

typedef struct nack_msg {
    int msg_type;
    int comm_id;
    int sender;
    int receiver;
    long nack_sequence;
};

typedef struct end_msg {
    int msg_type;
    int comm_id;
    int sender;
    long sequence;
    int receiver;
};

int comm_process_seq[NUM_PROCESS][MAX_COMM];

Queue* msg_buffer;

#endif //OMPI_COLL_R_BCAST_H
