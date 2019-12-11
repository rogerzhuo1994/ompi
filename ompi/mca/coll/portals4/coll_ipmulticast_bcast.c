#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>

#include "ompi_config.h"

#include "mpi.h"
#include "ompi/constants.h"
#include "ompi/datatype/ompi_datatype.h"
#include "ompi/datatype/ompi_datatype_internal.h"
#include "ompi/op/op.h"
#include "ompi/mca/mca.h"
#include "opal/datatype/opal_convertor.h"
#include "ompi/mca/coll/coll.h"
#include "ompi/request/request.h"
#include "ompi/communicator/communicator.h"
#include "ompi/mca/coll/base/base.h"
#include "ompi/datatype/ompi_datatype.h"

#include "coll_ipmulticast_bcast.h"
#include "coll_portals4.h"

#include "coll_rudp_bcast.h"

static int rank;
static int globalrank;
static int initialized;

typedef struct {
	bool is_root;
	bool needs_pack; 
	opal_convertor_t convertor; 

	char* data;
	size_t data_size; // In bytes
} ompi_coll_ipmulticast_request_t;

//---------------------------QUEUE---------------------------------
void* deQueue(Queue* queue){
    // TracePrintf(0, "queue: addr %d\n", queue);
    return pop(queue, queue->head);
};


void* enQueue(Queue* queue, void* data){

    QueueNode* node = (QueueNode *)malloc(sizeof(QueueNode));
    node->data = data;

    if(queue->length == 0){
        queue->head = node;
        node->prev = -1;
    }else{
        queue->tail->next = node;
        node->prev = queue->tail;
    }

    queue->tail = node;
    node->next = -1;

    queue->length++;

    if (queue->length > BUFFER_SIZE){
        return deQueue(queue);
    }else{
        return -1;
    }
};

void* pop(Queue* queue, QueueNode* curNode){

//    TracePrintf(0, "pop: queue length = %d\n", queue->length);

    if(queue->length == 0){
        return -1;
    }

    void* data = curNode->data;
    if(queue->length == 1){
        queue->head = -1;
        queue->tail = -1;
        queue->cur = -1;
    }else{
        if(queue->head == curNode){
            queue->head = curNode->next;
            queue->head->prev = -1;
            if(curNode == queue->cur){
                queue->cur = queue->head;
            }
        }else if(queue->tail == curNode){
            queue->tail = curNode->prev;
            queue->tail->next = -1;
            if(curNode == queue->cur){
                queue->cur = -1;
            }
        }else{
            curNode->next->prev = curNode->prev;
            curNode->prev->next = curNode->next;
            if(curNode == queue->cur){
                queue->cur = queue->cur->next;
            }
        }
    }

    queue->length--;
    free(curNode);
    return data;
}

void moveToHead(Queue* queue){
    queue->cur = queue->head;
}

int moveToNext(Queue* queue){
    if (queue->cur == -1){
        return -1;
    } else{
        queue->cur = queue->cur->next;
        return 0;
    }
}

Queue* initQueue(){
    Queue* myQueuePtr = (Queue *)malloc(sizeof(Queue));

    if (myQueuePtr == NULL){
        return NULL;
    }

    myQueuePtr->head = -1;
    myQueuePtr->tail = -1;
    myQueuePtr->cur = -1;
    myQueuePtr->length = 0;
    return myQueuePtr;
}

void freeQueue(Queue* queue){
    void *data = deQueue(queue);
//    while (data != -1){
//        free(data);
//    }
    free(queue);
}

void traverseQueue(Queue* queue){
    int count = 0;
    moveToHead(queue);
    while(queue->cur != -1){
        TracePrintf(0, "traverseQueue: queue node %d: addr = %d", count, queue->cur);
        moveToNext(queue);
        count++;
    }
    TracePrintf(0, "traverseQueue: total count = %d, queue length = %d", count, queue->length);
    if(count != queue->length){
        Halt();
    }
}



// These next two functions are mostly taken from ompi/mca/coll/portals4/coll_portals4_bcast.c
// They handle serializing and deserializing in cases where that is non-trivial.
static int prepare_bcast_data (struct ompi_communicator_t *comm,
        void *buff, int count,
        struct ompi_datatype_t *datatype, int root,
        ompi_coll_ipmulticast_request_t *request) {
    int rank = ompi_comm_rank(comm);
    int ret;
    size_t max_data;
    unsigned int iov_count;
    struct iovec iovec;

    request->is_root = (rank == root);
    request->needs_pack = !ompi_datatype_is_contiguous_memory_layout(datatype, count);

	// Is this a special datatype that needs code to serialize and de-serialize it?
	if (request->needs_pack) {
		// If this is the root of the broadcast, we actually need to serialize the data now.
        if (request->is_root) {
            OBJ_CONSTRUCT(&request->convertor, opal_convertor_t);
            opal_convertor_copy_and_prepare_for_send(ompi_mpi_local_convertor,
                    &(datatype->super), count,
                    buff, 0, &request->convertor);
            opal_convertor_get_packed_size(&request->convertor, &request->data_size);
			// Allocate the buffer that we pack the data into
            request->data = malloc(request->data_size);
            if (OPAL_UNLIKELY(NULL == request->data)) {
                OBJ_DESTRUCT(&request->convertor);
                return opal_stderr("malloc failed", __FILE__, __LINE__, OMPI_ERR_OUT_OF_RESOURCE);
            }

            iovec.iov_base = request->data;
            iovec.iov_len = request->data_size;
            iov_count = 1;
            max_data = request->data_size;
            ret = opal_convertor_pack(&request->convertor, &iovec, &iov_count, &max_data);
            OBJ_DESTRUCT(&request->convertor);
            if (OPAL_UNLIKELY(ret < 0)) {
                return opal_stderr("opal_convertor_pack failed", __FILE__, __LINE__, ret);	}
        }
        else {
			// Construct the object converter to prepare for when we receive data
            OBJ_CONSTRUCT(&request->convertor, opal_convertor_t);
            opal_convertor_copy_and_prepare_for_recv(ompi_mpi_local_convertor,
                    &(datatype->super), count,
                    buff, 0, &request->convertor);

			// Philip's note: seems like the original code has a slight bug here.
            opal_convertor_get_packed_size(&request->convertor, &request->data_size);

            request->data = malloc(request->data_size);
            if (OPAL_UNLIKELY(NULL == request->data)) {
                OBJ_DESTRUCT(&request->convertor);
                return opal_stderr("malloc failed", __FILE__, __LINE__, OMPI_ERR_OUT_OF_RESOURCE);
            }
        }
    }
    else {
        request->data = buff;

		// Total size of message is (size of one element) * count
        ompi_datatype_type_size(datatype, &request->data_size);
        request->data_size *= count;
    }

    return (OMPI_SUCCESS);
}

static int post_bcast_data(ompi_coll_ipmulticast_request_t *request) {

    int ret;
    size_t max_data;
    unsigned int iov_count;
    struct iovec iovec;

    if (request->needs_pack) {
        if (!request->is_root) {
			// We received data (since we're not the root) and need to de-serialize it into the right buffer
            opal_convertor_get_packed_size(&request->convertor, &request->data_size);

			// Convert the data we received to an iovec
            iovec.iov_base = request->data;
            iovec.iov_len = request->data_size;
            iov_count = 1;
            ret = opal_convertor_unpack(&request->convertor, &iovec, &iov_count, &max_data);
            OBJ_DESTRUCT(&request->convertor);
            if (OPAL_UNLIKELY(ret < 0)) {
                return opal_stderr("opal_convertor_unpack failed", __FILE__, __LINE__, ret);
            }
        }
		// This was a special buffer we allocated, so free it.
        free(request->data);
    }
    return (OMPI_SUCCESS);
}

int find_msg_in_buffer(comm_info_t* comm_info, int root_globalrank, int sequence){
    Queue* buf = comm_info->msg_buffer;
    bcast_msg_t* msg;
    int count = 0;

    moveToHead(buf);
    while(buf->cur != -1){
        msg = buf->cur->data;
        if (msg->sequence == sequence && msg->sender == root_globalrank){
            free(recv_msg);
            recv_msg = pop(buf, buf->cur);
            return 1;
        }
        moveToNext(buf);
        count++;
    }
    if(count != buf->length){
        perror("buffer length error");
    }
    return -1;
}

int initialize_comm_info(comm_info_t** comm_info, int size, int globalranks[]){
    print_rank_info();
    printf("Comm not found! Finding new comm location.\n");

    int idx = -1;
    for(int i = 0; i < MAX_COMM; i++){

        print_rank_info();
        printf("%dth comm, ", i);
        print_comm_info(&(comm_infos[i]));
        printf("\n");

        if (comm_infos[i].initialized == 0) {
            idx = i;
            break;
        }
    }

    print_rank_info();
    printf("Location found: %d.\n", idx);

    if (idx == -1) return -1;

    comm_infos[idx].size = size;
    comm_infos[idx].initialized = 1;
    comm_infos[idx].msg_buffer = initQueue();
    memset(comm_infos[idx].proc_seq, 0, sizeof(comm_infos[idx].proc_seq));
    for (int i = 0; i < NUM_PROCESS; i++){
        if(i < size){
            comm_infos[idx].global_ranks[i] = globalranks[i];
        }else{
            comm_infos[idx].global_ranks[i] = -1;
        }
    }

    (*comm_info) = &(comm_infos[idx]);
    return idx;
}

int find_comm_info(comm_info_t** comm_info, ompi_communicator_t *comm){
    // get the global information of current comm to find them

    print_rank_info();
    printf("Finding comm_info...\n");

    ompi_group_t *thisgroup, *worldgroup;
    ompi_comm_group((ompi_communicator_t*)comm, &thisgroup);
    ompi_comm_group((ompi_communicator_t*)ompi_mpi_comm_world_addr, &worldgroup);
    int size = ompi_group_size(thisgroup);
    int* globalranks = malloc(NUM_PROCESS*sizeof(int));
    for (int i = 0; i < NUM_PROCESS; i++){
        globalranks[i] = -1;
    }

    print_rank_info();
    printf("Global ranks created...");
    print_arr(globalranks, NUM_PROCESS);
    printf("\n");

    int* localranks = malloc(size*sizeof(int));
    for (int i = 0; i < size; i++) {
        localranks[i] = i;
    }
    ompi_group_translate_ranks(thisgroup, size, localranks, worldgroup, globalranks);

    print_rank_info();
    printf("Global ranks translated...");
    print_arr(globalranks, NUM_PROCESS);
    printf("\n");

    print_rank_info();
    printf("Matching comms...\n");

    for(int i = 0; i < MAX_COMM; i++){
        print_rank_info();
        printf("%dth comm, ", i);
        print_comm_info(&(comm_infos[i]));
        printf("\n");

        int flag = 1;
        if (comm_infos[i].initialized == 0) break;
        if (comm_infos[i].size != size) continue;

        for(int j = 0; j < NUM_PROCESS; j++){
            if (comm_infos[i].global_ranks[j] != globalranks[j]){
                flag = 0;
                break;
            }
        }
        if (flag == 1){
            print_rank_info();
            printf("Comm found! %dth comm, ", i);
            print_comm_info(&(comm_infos[i]));
            printf("\n");

            *comm_info = &(comm_infos[i]);
            return 0;
        }
    }

    int initialized = initialize_comm_info(comm_info, size, globalranks);

    print_rank_info();
    printf("New comm created, idx = %d\n", initialized);

    if (initialized == -1){
        perror("Communicator array is full, cannot use new communicator...");
    }
    return initialized;
}

int find_msg_comm_info(comm_info_t** comm_info, bcast_msg_t* msg){
    int* globalranks = msg->receiver;

    int size = 0;
    for (int i = 0; i < NUM_PROCESS; i++){
        if (globalranks[i] != -1) {
            size += 1;
        }
    }

    for(int i = 0; i < MAX_COMM; i++){
        int flag = 1;
        if (comm_infos[i].initialized == 0) break;

        for(int j = 0; j < NUM_PROCESS; j++){
            if (comm_infos[i].global_ranks[j] != msg->receiver[j]){
                flag = 0;
                break;
            }
        }
        if (flag == 1){
            *comm_info = &(comm_infos[i]);
            return 0;
        }
    }

    int initialized = initialize_comm_info(comm_info, size, globalranks);
    if (initialized == -1){
        perror("Communicator array is full, cannot use new communicator...");
    }
    return initialized;

}

int bcast_bulk_data(ompi_coll_ipmulticast_request_t *request,
        comm_info_t* comm_info,
        int root,
        struct ompi_datatype_t *datatype,
        int fd,
        struct sockaddr_in* addr){

    print_rank_info();
    printf("Start sending bulk data...\n");

    size_t size_remaining = request->data_size;
    bcast_msg_t *msg = (bcast_msg_t*)send_msg;
    size_t dt_size;
    ssize_t nbytes;
    char* send_next = request->data;
    int index = 0;
    int startSeq = comm_info->proc_seq[globalrank];

    print_rank_info();
    printf("Bulk metadata: size %d, startSeq %d\n", size_remaining, startSeq);

    msg->msg_type = DT_MSG;
    msg->sender = globalrank;
    msg->t_size = size_remaining;
    memcpy(msg->receiver, comm_info->global_ranks, sizeof(comm_info->global_ranks));



    // printf("Sent %zd for size\n", nbytes);
    while (size_remaining > 0) {
        // TODO: UDP does not guarauntee ordering!!
        msg->index = index;

        msg->sequence = comm_info->proc_seq[globalrank];


        dt_size = MIN(size_remaining, MAX_BCAST_SIZE);
        msg->dt_size = dt_size;
        memcpy(&(msg->data), send_next, dt_size);

        print_rank_info();
        print_msg(msg);
        printf("\n");

        index += 1;
        comm_info->proc_seq[globalrank] += 1;

        nbytes = sendto(fd, send_msg, sizeof(bcast_msg_t) + msg->dt_size, 0, (struct sockaddr*) addr, sizeof(*addr));

        print_rank_info();
        printf("Sent: %dth msg, nbytes: %d", msg->index, nbytes);
        printf("\n");

        if (nbytes < 0 || nbytes != sizeof(bcast_msg_t) + msg->dt_size)
            perror("sendto");

        // printf("Sent %zd\n", nbytes);
        size_remaining -= msg->dt_size;
        send_next += msg->dt_size;
    }
    return 1;
}

int receive_msg(int fd,
                struct sockaddr_in* addr){
    ssize_t nbytes;
    int addrlen = sizeof(*addr);
    nbytes = recvfrom(fd, recv_msg, sizeof(bcast_msg_t), 0, (struct sockaddr *) addr, &addrlen);
    if(nbytes < 0){
      // timeout return -1
      return -1;
    } else if(nbytes != sizeof(bcast_msg_t)){
        perror("Receiving invalid msg header");
    }

    print_rank_info();
    printf(" receive a msg header: ");
    print_msg(recv_msg);
    printf("\n");

    nbytes = recvfrom(fd, ((void*)recv_msg)+sizeof(bcast_msg_t), ((bcast_msg_t*)recv_msg)->dt_size, 0, (struct sockaddr *) addr, &addrlen);

    print_rank_info();
    printf(" receive msg data: nbytes = %d\n", nbytes);

    if(nbytes != ((bcast_msg_t*)recv_msg)->dt_size){
        perror("Receiving invalid msg data");
    }

    return 0;
}

int preprocess_recv_msg(int comm_info_index){
    // whether it's from myself
    // if from myself, skip
    if (recv_msg->sender == globalrank){
        return -1;
    }

    // whether I'm one of the receivers
    // if I'm not a receiver, skip
    int flag = -1;
    for (int i = 0; i < NUM_PROCESS; i++){
        if ((recv_msg->receiver)[i] == globalrank){
            flag = 1;
            break;
        }
    }
    if (flag == -1){
        return -1;
    }

    // now can confirm the message is not from me and I'm one of the receiver
    // find the communicator info
    comm_info_t* recv_msg_comm_info;
    int recv_msg_comm_info_index = find_msg_comm_info(&recv_msg_comm_info, recv_msg);

    // whether the message is from the same communicator
    if (recv_msg_comm_info_index != comm_info_index){
        // not the same communicator
        if (recv_msg->msg_type == NACK_MSG || recv_msg->msg_type == END_MSG){
            // nack_msg from other communicator, probably stale message or early message
            // stale message: skip
            // early message: will be resent later, can be skipped as well

            // end_msg from other communicator,
            // when I'm the root, this end_msg is definitely not for me, skip
            return -1;

        }else if (recv_msg->msg_type == DT_MSG){
            // probably something in the future or stale message, need check
            // if stale message, skip, otherwise add to buffer
            int sender = recv_msg->sender;
            int seq = recv_msg->sequence;
            if (seq < recv_msg_comm_info->proc_seq[sender]){
                // stale message, skip
                return -1;
            }else {
                // future message, add to buffer
                recv_msg = enQueue(recv_msg_comm_info->msg_buffer, recv_msg);
                if (recv_msg == -1){
                    recv_msg = (bcast_msg_t*)malloc(MAX_MSG_SIZE);
                }
            }
        }else {
            perror("Wrong msg found, exit...");
        }
        return -1;
    }

    return recv_msg_comm_info_index;
}

double calElapseTime(struct timeval* start_time, struct timeval* end_time){
    return ((end_time->tv_sec - start_time->tv_sec) * 1000 + (end_time->tv_usec - start_time->tv_usec) / 1000.0);
}


void print_arr(int arr[], int size){
    printf("arr ");
    for (int i = 0; i < size; i++){
        printf("%d ", arr[i]);
    }
}

void print_comm_info(comm_info_t *comm_info){
    printf("size %d, initialized %d", comm_info->size, comm_info->initialized);
    print_arr(comm_info->proc_seq, NUM_PROCESS);
    print_arr(comm_info->global_ranks, NUM_PROCESS);
    printf("\n");
}

void print_rank_info(){
    printf("Rank %d, global %d, ", rank, globalrank);
}

void print_msg(bcast_msg_t* msg){
    printf("type %d, sender %d, sequence %d, t_size %d, dt_size %d, index %d, ",
            msg->msg_type, msg->sender, msg->sequence, msg->t_size, msg->dt_size, msg->index);
    print_arr(msg->receiver, NUM_PROCESS);
    printf("\n");
}

int ompi_coll_ipmulticast_bcast(void *buff, int count,
        struct ompi_datatype_t *datatype, int root,
        struct ompi_communicator_t *comm,mca_coll_base_module_t *module) {
    printf("Calling custom bcast\n");

    if (initialized == 0){
        print_rank_info();
        printf("Initialize: allocating recv_msg & send_msg\n");
        recv_msg = (bcast_msg_t*)malloc(MAX_MSG_SIZE);
        send_msg = (bcast_msg_t*)malloc(MAX_MSG_SIZE);
        initialized = 1;
    }

    comm_info_t* comm_info;
    int comm_info_index = find_comm_info(&comm_info, comm);

    rank = ompi_comm_rank(comm);
    globalrank = comm_info->global_ranks[rank];

    print_rank_info();
    printf("Comms got...");
    print_comm_info(comm_info);
    printf("\n");

    ompi_coll_ipmulticast_request_t request;

	prepare_bcast_data(comm, buff, count, datatype, root, &request);

	// TODO: We don't need to create and destroy the socket every time
    int fd;
    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        perror("socket");
        return 1;
    }
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(IP_MULTICAST_PORT);

    //-------------------EDITED BY ROGER STARTS-----------------------
    //-------------------SET SOCKET TIMEOUT---------------------------
    struct timeval tv;
    tv.tv_sec = 0;                              // secs Timeout
    tv.tv_usec = RECVFROM_TIMEOUT_MILLS;        // milliseconds Timeout
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(struct timeval));
    //-------------------EDITED BY ROGER ENDS-------------------------

	// If we're not sending, get ready to receive
    if (!request.is_root) {
        bool yes = true;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

        addr.sin_addr.s_addr = htonl(INADDR_ANY); 

        if (bind(fd, (struct sockaddr*) &addr, sizeof(addr)) < 0) {
            perror("bind");
            return 1;
        }
        // Request that the kernel join a multicast group
        struct ip_mreq mreq;
        mreq.imr_multiaddr.s_addr = inet_addr(IP_MULTICAST_ADDR);
        mreq.imr_interface.s_addr = htonl(INADDR_ANY);
        if (setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, (char*) &mreq, sizeof(mreq)) < 0) {
            perror("setsockopt");
            return 1;
        }
    }

	// This kills performance but makes sure that all receivers are ready before the sender starts
    ompi_coll_portals4_barrier_intra(comm, module);

    ssize_t nbytes;
    if (request.is_root) {

        print_rank_info();
        printf("Enter into root...\n");

        addr.sin_addr.s_addr = inet_addr(IP_MULTICAST_ADDR);
        int startSeq = comm_info->proc_seq[globalrank];
        int endSeq = startSeq + ((int)ceil(request.data_size / (float)MAX_BCAST_SIZE)) - 1;

		// First send the size so that the receivers know how many messages to expect
		// TODO: There are better ways to do this, no?
		bcast_bulk_data(&request, comm_info, root, datatype, fd, &addr);

        //-------------------EDITED BY ROGER STARTS-----------------------
        //-------------------WAITING REPLIES------------------------------

        int end_received = 0;
        int end_to_received = comm_info->size - 1;
        int end_received_proc[NUM_PROCESS];
        struct timeval start_time, end_time;
        double elapsedTime;
        gettimeofday(&start_time, NULL);
        for (int i = 0; i < NUM_PROCESS; i++){
            if (comm_info->global_ranks[i] > -1 && comm_info->global_ranks[i] != globalrank) {
                end_received_proc[comm_info->global_ranks[i]] = 0;
            } else {
                end_received_proc[comm_info->global_ranks[i]] = -1;
            }
        }

        while (end_received < end_to_received) {
            int res = receive_msg(fd, &addr);

            gettimeofday(&end_time, NULL);
            elapsedTime = (end_time.tv_sec - start_time.tv_sec) * 1000 + (end_time.tv_usec - start_time.tv_usec) / 1000.0;
            if (elapsedTime > SENDER_HEARTBEAT_MILLS){
                // send out a heartbeat
                send_msg->msg_type = NACK_MSG;
                send_msg->sender = globalrank;
                send_msg->t_size = -1;
                send_msg->index = -1;
                send_msg->sequence = startSeq;
                send_msg->dt_size = -1;
                memcpy(send_msg->receiver, comm_info->global_ranks, sizeof(comm_info->global_ranks));
                nbytes = sendto(fd, send_msg, sizeof(bcast_msg_t), 0, (struct sockaddr*) &addr, sizeof(addr));
                if (nbytes < 0) perror("sendto");

                gettimeofday(&start_time, NULL);
            }

            if (res == -1){
                continue;
            }


            res = preprocess_recv_msg(comm_info_index);
            if (res == -1){
                continue;
            }

            // the same communicator
            if (recv_msg->msg_type == NACK_MSG){
                // nack_msg from the same communicator
                // check whether is current bcast
                // if is current bcast, retransmit the message
                if (recv_msg->sequence >= startSeq && recv_msg->sequence <= endSeq){
                    // current bcast
                    send_msg->msg_type = DT_MSG;
                    send_msg->sender = globalrank;
                    send_msg->t_size = request.data_size;
                    send_msg->index = recv_msg->index;
                    send_msg->sequence = recv_msg->sequence;
                    send_msg->dt_size = MIN(request.data_size - send_msg->index * MAX_BCAST_SIZE, MAX_BCAST_SIZE);
                    memcpy(send_msg->receiver, comm_info->global_ranks, sizeof(comm_info->global_ranks));

                    memcpy(&(send_msg->data), request.data+(send_msg->index*MAX_BCAST_SIZE), send_msg->dt_size);
                    nbytes = sendto(fd, send_msg, sizeof(bcast_msg_t)+send_msg->dt_size, 0, (struct sockaddr*) &addr, sizeof(addr));

                    if (nbytes < 0) perror("sendto");

                } else{
                    // not current bcast, skip
                    continue;
                }
            }else if (recv_msg->msg_type == END_MSG){
                // nack_msg from the same communicator
                // check whether is current bcast
                // if is current bcast, add 1
                if (recv_msg->sequence != startSeq){
                    // probably not current bcast, skip
                    continue;
                }

                if (end_received_proc[recv_msg->sender] == 0){
                    end_received_proc[recv_msg->sender] = 1;
                    end_received += 1;
                } else if (end_received_proc[recv_msg->sender] == -1){
                    perror("receiving wrong end_msg");
                }

            }else if (recv_msg->msg_type == DT_MSG){
                // dt_msg from the same communicator
                // check whether is stale message
                // if is, skip, otherwise add to buffer
                int sender = recv_msg->sender;
                int seq = recv_msg->sequence;
                if (seq >= comm_info->proc_seq[sender]){
                    // future message, add to buffer
                    recv_msg = enQueue(comm_info->msg_buffer, recv_msg);
                    if (recv_msg == -1){
                        recv_msg = (bcast_msg_t*)malloc(MAX_MSG_SIZE);
                    }
                }
            }else {
                perror("Wrong msg found, exit...");
            }

        }
        //-------------------EDITED BY ROGER ENDS-------------------------

    }


	else {
		int addrlen = sizeof(addr);

		int root_globalrank = comm_info->global_ranks[root];
		if (root_globalrank < 0){
		    perror("wrong root globalrank");
		}
        int startSeq = comm_info->proc_seq[root_globalrank];
		int first_msg_received = 0;
		int buf_flag = 1;
		int res;
		size_t total_size;
		int cur_index = 0;
		int total_index = 0;
		void* receive_next = request.data;
        struct timeval start_time, end_time;
        double elapsedTime;
        gettimeofday(&start_time, NULL);

		while (first_msg_received == 0 || cur_index < total_index) {
		    // receiving status

            gettimeofday(&end_time, NULL);
            elapsedTime = (end_time.tv_sec - start_time.tv_sec) * 1000 + (end_time.tv_usec - start_time.tv_usec) / 1000.0;
            if (elapsedTime > SENDER_HEARTBEAT_MILLS){
                // send out a NACK msg
                send_msg->msg_type = NACK_MSG;
                send_msg->sender = globalrank;
                send_msg->t_size = -1;
                send_msg->index = cur_index;
                send_msg->sequence = comm_info->proc_seq[root_globalrank];
                send_msg->dt_size = -1;

                memcpy(send_msg->receiver, comm_info->global_ranks, sizeof(comm_info->global_ranks));
                nbytes = sendto(fd, send_msg, sizeof(bcast_msg_t), 0, (struct sockaddr*) &addr, sizeof(addr));
                if (nbytes < 0) perror("sendto");

                gettimeofday(&start_time, NULL);
            }

		    if (buf_flag == 1){
                buf_flag = find_msg_in_buffer(comm_info, root_globalrank, comm_info->proc_seq[root_globalrank]);
            }

            if (buf_flag == -1){
                res = receive_msg(fd, &addr);
            }

            if (res == -1){
                continue;
            }

            res = preprocess_recv_msg(comm_info_index);
            if (res == -1){
                continue;
            }

            // the same communicator
            if (recv_msg->msg_type == DT_MSG){
                int sender = recv_msg->sender;
                int seq = recv_msg->sequence;

                // dt_msg from the same communicator
                // check sender & sequence
                if (sender == root_globalrank){
                    if(seq == comm_info->proc_seq[sender]) {
                        // current message, process
                        // TODO: process right message and buffer lookup
                        if (first_msg_received == 0){
                            first_msg_received = 1;
                            total_size = recv_msg->t_size;
                            total_index = (int)ceil(total_size / (double)MAX_BCAST_SIZE);
                        }

                        memcpy(receive_next, recv_msg->data, recv_msg->dt_size);
                        receive_next += recv_msg->dt_size;
                        cur_index += 1;
                        comm_info->proc_seq[sender] += 1;
                        buf_flag = 1;
                        gettimeofday(&start_time, NULL);
                    } else if (seq > comm_info->proc_seq[sender]) {
                        // future message, add to buffer
                        recv_msg = enQueue(comm_info->msg_buffer, recv_msg);
                        if (recv_msg == -1){
                            recv_msg = (bcast_msg_t*)malloc(MAX_MSG_SIZE);
                        }
                    }

                } else {
                    if (seq >= comm_info->proc_seq[sender]){
                        // future message, add to buffer
                        recv_msg = enQueue(comm_info->msg_buffer, recv_msg);
                        if (recv_msg == -1){
                            recv_msg = (bcast_msg_t*)malloc(MAX_MSG_SIZE);
                        }
                    }
                    // do nothing for stale message
                }
            } else if (recv_msg->msg_type == NACK_MSG || recv_msg->msg_type == END_MSG) {
                continue;
            } else {
                perror("wrong message");
            }
		}

		// ack stage
        gettimeofday(&start_time, NULL);
        gettimeofday(&end_time, NULL);
		while (calElapseTime(&start_time, &end_time) < MSG_LIVE_TIME){
            res = receive_msg(fd, &addr);
            if (res == -1){
                continue;
            }

            res = preprocess_recv_msg(comm_info_index);
            if (res == -1){
                continue;
            }

            // the same communicator
            if (recv_msg->msg_type == DT_MSG){
                int sender = recv_msg->sender;
                int seq = recv_msg->sequence;

                if(seq >= comm_info->proc_seq[sender]){
                    recv_msg = enQueue(comm_info->msg_buffer, recv_msg);
                    if (recv_msg == -1){
                        recv_msg = (bcast_msg_t*)malloc(MAX_MSG_SIZE);
                    }
                }

            } else if (recv_msg->msg_type == NACK_MSG) {
                if (recv_msg->sequence == startSeq) {
                    // receive a NACK msg from sender, send a end msg
                    send_msg->msg_type = END_MSG;
                    send_msg->sender = globalrank;
                    send_msg->t_size = -1;
                    send_msg->index = -1;
                    send_msg->sequence = startSeq;
                    send_msg->dt_size = -1;
                    memcpy(send_msg->receiver, comm_info->global_ranks, sizeof(comm_info->global_ranks));
                    nbytes = sendto(fd, send_msg, sizeof(bcast_msg_t), 0, (struct sockaddr*) &addr, sizeof(addr));
                    if (nbytes < 0) perror("sendto");

                    gettimeofday(&start_time, NULL);
                }
            } else if (recv_msg->msg_type == END_MSG) {
              continue;
            } else {
                perror("wrong message");
            }
		}
	}
    close(fd);

	post_bcast_data(&request);
    return (OMPI_SUCCESS);
}

END_C_DECLS
