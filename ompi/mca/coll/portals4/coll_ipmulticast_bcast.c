#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include <math.h>

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
static int root_localrank;
static int root_globalrank;

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

int find_msg_in_buffer(comm_info_t* comm_info, int sequence){
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
//    print_rank_info();
//    printf(" [initialize_comm_info] Finding new comm location.\n");

    int idx = -1;
    for(int i = 0; i < MAX_COMM; i++){

//        print_rank_info();
//        printf(" [initialize_comm_info] looping %dth comm, ", i);
//        print_comm_info(&(comm_infos[i]));

        if (comm_infos[i].initialized == 0) {
            idx = i;
            break;
        }
    }

//    print_rank_info();
//    printf(" [initialize_comm_info] Location found: %d.\n", idx);

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

//    print_rank_info();
//    printf(" [find_comm_info] Finding comm_info...\n");

    ompi_group_t *thisgroup, *worldgroup;
    ompi_comm_group((ompi_communicator_t*)comm, &thisgroup);
    ompi_comm_group((ompi_communicator_t*)ompi_mpi_comm_world_addr, &worldgroup);
    int size = ompi_group_size(thisgroup);
    int* globalranks = malloc(NUM_PROCESS*sizeof(int));
    for (int i = 0; i < NUM_PROCESS; i++){
        globalranks[i] = -1;
    }

    int* localranks = malloc(size*sizeof(int));
    for (int i = 0; i < size; i++) {
        localranks[i] = i;
    }
    ompi_group_translate_ranks(thisgroup, size, localranks, worldgroup, globalranks);

//    print_rank_info();
//    printf(" [find_comm_info] global ranks of comm: ");
//    print_arr(globalranks, NUM_PROCESS);
//    printf("\n");

//    print_rank_info();
//    printf(" [find_comm_info] Start matching comms...\n");

    for(int i = 0; i < MAX_COMM; i++){
//        print_rank_info();
//        printf(" [find_comm_info] loopinf %dth comm, ", i);
//        print_comm_info(&(comm_infos[i]));

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
//            print_rank_info();
//            printf(" [find_comm_info] Comm found! %dth comm, ", i);
//            print_comm_info(&(comm_infos[i]));
//            printf("\n");

            *comm_info = &(comm_infos[i]);
            return 0;
        }
    }

//    print_rank_info();
//    printf(" [find_comm_info] Matching comm not found, start creating one\n");

    int initialized = initialize_comm_info(comm_info, size, globalranks);

//    print_rank_info();
//    printf(" [find_comm_info] New comm created, idx = %d, ", initialized);
//    print_comm_info(&comm_infos[initialized]);

    if (initialized == -1){
        perror("Communicator array is full, cannot use new communicator...");
    }
    return initialized;
}

int find_msg_comm_info(comm_info_t** comm_info, bcast_msg_t* msg){

//    print_rank_info();
//    printf(" [find_msg_comm_info] Finding comm_info for msg...\n");

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

//            print_rank_info();
//            printf(" [find_msg_comm_info] Comm_info found, %d the comm...", i);
//            print_comm_info(*comm_info);

            return 0;
        }
    }

//    print_rank_info();
//    printf(" [find_msg_comm_info] Comm_info not found, start creating one...\n");

    int initialized = initialize_comm_info(comm_info, size, globalranks);

//    print_rank_info();
//    printf(" [find_msg_comm_info] New comm created, idx = %d, ", initialized);
//    print_comm_info(&comm_infos[initialized]);

    if (initialized == -1){
        perror("Communicator array is full, cannot use new communicator...");
    }
    return initialized;

}

int bcast_bulk_data(ompi_coll_ipmulticast_request_t *request,
        comm_info_t* comm_info,
        int start_index,
        int end_index,
        int root,
        struct ompi_datatype_t *datatype,
        int fd,
        struct sockaddr_in* addr){

//    print_rank_info();
//    printf(" [bcast_bulk_data] Start sending bulk data, startIndex=%d, endIndex=%d...\n", start_index, end_index);

    bcast_msg_t *msg = (bcast_msg_t*)send_msg;
    size_t dt_size;
    ssize_t nbytes;
    char* send_next = request->data + start_index * MAX_BCAST_SIZE;
    int startSeq = comm_info->proc_seq[globalrank];
    size_t size_remaining = request->data_size - start_index * MAX_BCAST_SIZE;

    msg->msg_type = DT_MSG;
    msg->sender = globalrank;
    msg->t_size = request->data_size;
    msg->sequence = comm_info->proc_seq[globalrank];
    memcpy(msg->receiver, comm_info->global_ranks, sizeof(comm_info->global_ranks));

    // printf("Sent %zd for size\n", nbytes);
    for (int cur_index = start_index; cur_index < end_index; cur_index++){
        // TODO: UDP does not guarauntee ordering!!
        msg->index = cur_index;

        dt_size = MIN(size_remaining, MAX_BCAST_SIZE);
        msg->dt_size = dt_size;
        memcpy(&(msg->data), send_next, dt_size);

        nbytes = sendto(fd, send_msg, sizeof(bcast_msg_t) + msg->dt_size, 0, (struct sockaddr*) addr, sizeof(*addr));

//        print_rank_info();
//        printf(" [bcast_bulk_data] Message sent: %dth msg, nbytes: %d, ", msg->index, nbytes);
//        print_msg(msg);

        if (nbytes < 0 || nbytes != sizeof(bcast_msg_t) + msg->dt_size)
            perror("sendto");

        // printf("Sent %zd\n", nbytes);
        send_next += msg->dt_size;
        size_remaining -= msg->dt_size;
    }
    return 1;
}

int receive_msg(int fd,
                struct sockaddr_in* addr){
    ssize_t nbytes;
    int addrlen = sizeof(*addr);
    nbytes = recvfrom(fd, recv_msg, MAX_MSG_SIZE, 0, (struct sockaddr *) addr, &addrlen);
    if(nbytes < 0){
      // timeout return -1
//      print_rank_info();
//      printf(" [receive_msg] receive msg timeout: nbytes = %d\n", nbytes);
      return -1;
    }

//    print_rank_info();
//    printf(" [receive_msg] receive a msg: nbytes = %d, ", nbytes);
//    print_msg(recv_msg);

//    print_rank_info();
//    printf(" [receive_msg] received data: ");
//    print_arr(recv_msg->data, 20);
//    printf("\n");

    if (recv_msg->msg_type == DT_MSG && nbytes != sizeof(bcast_msg_t) + recv_msg->dt_size){
        perror("Received invalid dt_msg...");
    }
    if (recv_msg->msg_type != DT_MSG && nbytes != sizeof(bcast_msg_t)){
        perror("Received invalid non-dt_msg...");
    }

    return 0;
}

int preprocess_recv_msg(int comm_info_index){
    // whether it's from myself
    // if from myself, skip

//    print_rank_info();
//    printf(" [preprocess_recv_msg] Preprocess msg: ");
//    print_msg(recv_msg);

    if (recv_msg->sender == globalrank){
//        print_rank_info();
//        printf(" [preprocess_recv_msg] Sender is from myself, skip\n");
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
//        print_rank_info();
//        printf(" [preprocess_recv_msg] I'm not the receiver, skip\n");
        return -1;
    }

    // now can confirm the message is not from me and I'm one of the receiver
    // find the communicator info
    comm_info_t* recv_msg_comm_info;
    int recv_msg_comm_info_index = find_msg_comm_info(&recv_msg_comm_info, recv_msg);

//    print_rank_info();
//    printf(" [preprocess_recv_msg] Comm_info got, idx = %d...\n", recv_msg_comm_info_index);


    // whether the message is from the same communicator
    if (recv_msg_comm_info_index != comm_info_index){

//        print_rank_info();
//        printf(" [preprocess_recv_msg] not the same comm_info with current Bcast\n", recv_msg_comm_info_index);

        // not the same communicator
        if (recv_msg->msg_type == NACK_MSG || recv_msg->msg_type == END_MSG){
            // nack_msg from other communicator, probably stale message or early message
            // stale message: skip
            // early message: will be resent later, can be skipped as well

            // end_msg from other communicator,
            // when I'm the root, this end_msg is definitely not for me, skip

//            print_rank_info();
//            printf(" [preprocess_recv_msg] non-dt_msg, skip...\n");

            return -1;

        }else if (recv_msg->msg_type == DT_MSG){
            // probably something in the future or stale message, need check
            // if stale message, skip, otherwise add to buffer
            int sender = recv_msg->sender;
            int seq = recv_msg->sequence;
            if (seq < recv_msg_comm_info->proc_seq[sender]){
                // stale message, skip
//                print_rank_info();
//                printf(" [preprocess_recv_msg] stale dt_msg, skip, seq = %d, cur = %d...\n", recv_msg->sequence, recv_msg_comm_info->proc_seq[sender]);
                return -1;
            }else {
                // future message, add to buffer

//                print_rank_info();
//                printf(" [preprocess_recv_msg] future dt_msg, buffer, seq = %d, cur = %d...\n", recv_msg->sequence, recv_msg_comm_info->proc_seq[sender]);

                recv_msg = enQueue(recv_msg_comm_info->msg_buffer, recv_msg);
                if (recv_msg == -1){
                    recv_msg = (bcast_msg_t*)malloc(MAX_MSG_SIZE);
                }
            }
        }else {
//            print_rank_info();
//            printf(" [preprocess_recv_msg] wrong msg found... ");
//            print_msg(recv_msg);
            perror("Wrong msg found, exit...");
        }
        return -1;
    }

    return recv_msg_comm_info_index;
}

double calElapseTime(struct timeval* start_time){
    struct timeval end_time;
    gettimeofday(&end_time, NULL);
    return ((end_time.tv_sec - start_time->tv_sec) * 1000 + (end_time.tv_usec - start_time->tv_usec) / 1000.0);
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

//        print_rank_info();
//        printf(" [ompi_coll_ipmulticast_bcast] Initialize: allocating recv_msg & send_msg\n");
        recv_msg = (bcast_msg_t*)malloc(MAX_MSG_SIZE);
        send_msg = (bcast_msg_t*)malloc(MAX_MSG_SIZE);
        initialized = 1;
    }

    comm_info_t* comm_info;
    int comm_info_index = find_comm_info(&comm_info, comm);

    rank = ompi_comm_rank(comm);
    globalrank = comm_info->global_ranks[rank];
    root_localrank = root;
    root_globalrank = comm_info->global_ranks[root];

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

//        print_rank_info();
//        printf(" [ompi_coll_ipmulticast_bcast] Enter into root...\n");

        addr.sin_addr.s_addr = inet_addr(IP_MULTICAST_ADDR);
        int startSeq = comm_info->proc_seq[globalrank];
        int endSeq = startSeq + ((int)ceil(request.data_size / (float)MAX_BCAST_SIZE)) - 1;
        int total_index = (int)ceil(request.data_size / (double)MAX_BCAST_SIZE);

        // First send the size so that the receivers know how many messages to expect
		// TODO: There are better ways to do this, no?
		bcast_bulk_data(&request, comm_info, 0, total_index, root, datatype, fd, &addr);

        //-------------------EDITED BY ROGER STARTS-----------------------
        //-------------------WAITING REPLIES------------------------------

        int end_received = 0;
        int end_to_received = comm_info->size - 1;
        int end_received_proc[NUM_PROCESS];
        struct timeval start_time;
        double elapsedTime;
        gettimeofday(&start_time, NULL);

        for (int i = 0; i < NUM_PROCESS; i++){
            end_received_proc[i] = -1;
            if (comm_info->global_ranks[i] > -1 && comm_info->global_ranks[i] != globalrank) {
                end_received_proc[comm_info->global_ranks[i]] = 0;
            } else {
                end_received_proc[comm_info->global_ranks[i]] = -1;
            }
        }
//        print_rank_info();
//        printf(" [ompi_coll_ipmulticast_bcast] Start receiving response from receivers, startSeq = %d, receive_num = %d, receive_arr ", startSeq, end_to_received);
//        print_arr(end_received_proc, NUM_PROCESS);
//        printf("\n");

        while (end_received < end_to_received) {
            int res = receive_msg(fd, &addr);

            if (calElapseTime(&start_time) > SENDER_HEARTBEAT_MILLS){
                // send out a heartbeat
                send_msg->msg_type = NACK_MSG;
                send_msg->sender = globalrank;
                send_msg->t_size = -1;
                send_msg->index = -1;
                send_msg->sequence = startSeq;
                send_msg->dt_size = -1;
                memcpy(send_msg->receiver, comm_info->global_ranks, sizeof(comm_info->global_ranks));

//                print_rank_info();
//                printf(" [ompi_coll_ipmulticast_bcast] Timeout and sendout a heartbeat, received_num = %d, receive_arr ", end_received);
//                print_arr(end_received_proc, NUM_PROCESS);
//                printf("\n");

                nbytes = sendto(fd, send_msg, sizeof(bcast_msg_t), 0, (struct sockaddr*) &addr, sizeof(addr));

//                print_rank_info();
//                printf(" [ompi_coll_ipmulticast_bcast] Sent a heartbeat, ");
//                print_msg(send_msg);

                if (nbytes < 0) perror("Sending heartbeat failure...");

                gettimeofday(&start_time, NULL);
            }

            if (res == -1){
//                print_rank_info();
//                printf(" [ompi_coll_ipmulticast_bcast] A recv_msg is timeout...\n");
                continue;
            }

            res = preprocess_recv_msg(comm_info_index);

//            print_rank_info();
//            printf(" [ompi_coll_ipmulticast_bcast] preprocessing ends, comm_id = %d...\n", res);
            if (res == -1){
                continue;
            }

            // the same communicator
            if (recv_msg->msg_type == NACK_MSG){
                // nack_msg from the same communicator
                // check whether is current bcast
                // if is current bcast, retransmit the message

//                print_rank_info();
//                printf(" [ompi_coll_ipmulticast_bcast] NACK_MSG for current comm ");

                if (recv_msg->sequence == startSeq && recv_msg->index < total_index && recv_msg->index >= 0){
                    // current bcast

//                    print_rank_info();
//                    printf(" [ompi_coll_ipmulticast_bcast] NACK_MSG: inside seq\n");

                    int end_index = MIN(total_index, recv_msg->index+MAX(MIN_BCAST_PACKETS, ceil(total_index*0.01)));
                    bcast_bulk_data(&request, comm_info, recv_msg->index, end_index, root, datatype, fd, &addr);
                } else{
                    // not current bcast, skip
//                    print_rank_info();
//                    printf(" [ompi_coll_ipmulticast_bcast] NACK_MSG: outside seq, skip\n");
                    continue;
                }
            }else if (recv_msg->msg_type == END_MSG){
                // nack_msg from the same communicator
                // check whether is current bcast
                // if is current bcast, add 1

//                print_rank_info();
//                printf(" [ompi_coll_ipmulticast_bcast] END_MSG for current comm...\n");

                if (recv_msg->sequence != startSeq){
                    // probably not current bcast, skip
//                    print_rank_info();
//                    printf(" [ompi_coll_ipmulticast_bcast] END_MSG not correct seq, seq=%d, startSeq=%d, skip...\n", recv_msg->sequence, startSeq);
                    continue;
                }

                if (end_received_proc[recv_msg->sender] == 0){
                    end_received_proc[recv_msg->sender] = 1;
                    end_received += 1;

//                    print_rank_info();
//                    printf(" [ompi_coll_ipmulticast_bcast] END_MSG updated, seq=%d, startSeq=%d, end_received=%d, end_to_recv=%d... ", recv_msg->sequence, startSeq, end_received, end_to_received);
//                    print_arr(end_received_proc, NUM_PROCESS);
//                    printf("\n");

                } else if (end_received_proc[recv_msg->sender] == 1) {
//                    print_rank_info();
//                    printf(" [ompi_coll_ipmulticast_bcast] END_MSG has been seen before, skip, seq=%d, startSeq=%d, end_received=%d, end_to_recv=%d... ", recv_msg->sequence, startSeq, end_received, end_to_received);
//                    print_arr(end_received_proc, NUM_PROCESS);
//                    printf("\n");
                    continue;
                } else {
                    perror("receiving wrong end_msg");
                }

            }else if (recv_msg->msg_type == DT_MSG){
                // dt_msg from the same communicator
                // check whether is stale message
                // if is, skip, otherwise add to buffer

//                print_rank_info();
//                printf(" [ompi_coll_ipmulticast_bcast] DT_MSG for current comm...\n");

                int sender = recv_msg->sender;
                int seq = recv_msg->sequence;
                if (seq >= comm_info->proc_seq[sender]){
                    // future message, add to buffer
//                    print_rank_info();
//                    printf(" [ompi_coll_ipmulticast_bcast] DT_MSG added to buffer...\n");
                    recv_msg = enQueue(comm_info->msg_buffer, recv_msg);
                    if (recv_msg == -1){
                        recv_msg = (bcast_msg_t*)malloc(MAX_MSG_SIZE);
                    }
                }
            }else {
//                print_rank_info();
//                print(" [ompi_coll_ipmulticast_bcast] invalid msg type, error \n");
                perror("Wrong msg found, exit...");
            }

//            print_rank_info();
//            printf(" [ompi_coll_ipmulticast_bcast] Processed a msg: startSeq=%d, end_received=%d, end_to_recv=%d... \n", startSeq, end_received, end_to_received);
        }
        comm_info->proc_seq[globalrank] += 1;
        //-------------------EDITED BY ROGER ENDS-------------------------
    }


	else {
		int addrlen = sizeof(addr);

		if (root_globalrank < 0){
		    perror("wrong root globalrank");
		}

        size_t total_size = request.data_size;
        int startSeq = comm_info->proc_seq[root_globalrank];
		int endSeq = startSeq + total_size / MAX_MSG_SIZE;
		int buf_flag = 1;
		int res;
		int total_index = (int)ceil(total_size / (double)MAX_BCAST_SIZE);
        int cur_index = 0;
        int received_flags[total_index];
        int received_num = 0;
        memset(received_flags, 0, sizeof(received_flags));

        struct timeval start_time;
        double elapsedTime;
        gettimeofday(&start_time, NULL);

//        print_rank_info();
//        printf(" [ompi_coll_ipmulticast_bcast] Start receiving data as receiver...\n");

		while (cur_index < total_index) {
		    // receiving status

            if (calElapseTime(&start_time) > SENDER_HEARTBEAT_MILLS){
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

//                print_rank_info();
//                printf(" [ompi_coll_ipmulticast_bcast] Time for receiving elapsed, sending a NACK msg, nbytes=%d...", nbytes);
//                print_msg(send_msg);

                gettimeofday(&start_time, NULL);
            }

		    if (buf_flag == 1){
//                print_rank_info();
//                printf(" [ompi_coll_ipmulticast_bcast] check buffer first, buf_flag = %d...\n", buf_flag);
                buf_flag = find_msg_in_buffer(comm_info, comm_info->proc_seq[root_globalrank]);
            }

            if (buf_flag == -1){
//                print_rank_info();
//                printf(" [ompi_coll_ipmulticast_bcast] buffer does not have msg, start receiving...\n");
                res = receive_msg(fd, &addr);
            }

            if (res == -1){
                continue;
            }

            res = preprocess_recv_msg(comm_info_index);
            if (res == -1){
                continue;
            }

//            print_rank_info();
//            printf(" [ompi_coll_ipmulticast_bcast] preprocessing ends, comm_id = %d...\n", res);

            // the same communicator
            if (recv_msg->msg_type == DT_MSG){

//                print_rank_info();
//                printf(" [ompi_coll_ipmulticast_bcast] DT_MSG in the same communicator...\n");

                int sender = recv_msg->sender;
                int seq = recv_msg->sequence;
                int idx = recv_msg->index;

                // dt_msg from the same communicator
                // check sender & sequence
                if (sender == root_globalrank){
//                    print_rank_info();
//                    printf(" [ompi_coll_ipmulticast_bcast] The DT_MSG received is from the current sender\n");

                    if(seq == comm_info->proc_seq[sender] && idx < total_index) {

//                        print_rank_info();
//                        printf(" [ompi_coll_ipmulticast_bcast] The DT_MSG received is in current bcast\n");

                        // current message, process
                        // TODO: process right message and buffer lookup

                        if (received_flags[idx] == 1) {
//                            print_rank_info();
//                            printf(" [ompi_coll_ipmulticast_bcast] The DT_MSG received is already received, skip, received_num=%d, cur_index=%d..\n", received_num, cur_index);
                            continue;
                        }

                        void* data = request.data;
                        data += idx * MAX_BCAST_SIZE;
                        memcpy(data, recv_msg->data, recv_msg->dt_size);
                        received_flags[idx] = 1;
                        received_num += 1;

                        while (received_flags[cur_index] == 1 && cur_index < total_index){
                            cur_index += 1;
                        }

//                        print_rank_info();
//                        printf(" [ompi_coll_ipmulticast_bcast] The DT_MSG received is not received, received_num=%d, cur_index=%d..\n", received_num, cur_index);

                        gettimeofday(&start_time, NULL);
                    } else if (seq > comm_info->proc_seq[sender]) {
//                        print_rank_info();
//                        printf(" [ompi_coll_ipmulticast_bcast] The DT_MSG received is in future bcast, add to buffer\n");

                        // future message, add to buffer
                        recv_msg = enQueue(comm_info->msg_buffer, recv_msg);
                        if (recv_msg == -1){
                            recv_msg = (bcast_msg_t*)malloc(MAX_MSG_SIZE);
                        }
                    }

                } else {
//                    print_rank_info();
//                    printf(" [ompi_coll_ipmulticast_bcast] MSG is not from the current sender\n");

                    if (seq >= comm_info->proc_seq[sender]){

//                        print_rank_info();
//                        printf(" [ompi_coll_ipmulticast_bcast] MSG is in future bcast, add to buffer\n");

                        // future message, add to buffer
                        recv_msg = enQueue(comm_info->msg_buffer, recv_msg);
                        if (recv_msg == -1){
                            recv_msg = (bcast_msg_t*)malloc(MAX_MSG_SIZE);
                        }
                    }
                    // do nothing for stale message
                }
            } else if (recv_msg->msg_type == NACK_MSG || recv_msg->msg_type == END_MSG) {
//                print_rank_info();
//                printf(" [ompi_coll_ipmulticast_bcast] NACK or END MSG in the same communicator, skip...\n");
                continue;
            } else {
                perror("wrong message");
            }
		}

//        print_rank_info();
//        printf(" [ompi_coll_ipmulticast_bcast] -------------------------------------\n");
//        print_rank_info();
//        printf(" [ompi_coll_ipmulticast_bcast] -------------------------------------\n");
//        print_rank_info();
//        printf(" [ompi_coll_ipmulticast_bcast] Entering into acking stage\n");

		// ack stage
        gettimeofday(&start_time, NULL);
		while (calElapseTime(&start_time) < MSG_LIVE_TIME){
            // frequently send END_MSG
            send_msg->msg_type = END_MSG;
            send_msg->sender = globalrank;
            send_msg->t_size = -1;
            send_msg->index = -1;
            send_msg->sequence = startSeq;
            send_msg->dt_size = -1;
            memcpy(send_msg->receiver, comm_info->global_ranks, sizeof(comm_info->global_ranks));
            nbytes = sendto(fd, send_msg, sizeof(bcast_msg_t), 0, (struct sockaddr*) &addr, sizeof(addr));

//            print_rank_info();
//            printf(" [ompi_coll_ipmulticast_bcast] send out an END_MSG...");
//            print_msg(send_msg);

            if (nbytes < 0) perror("sendto");

            res = receive_msg(fd, &addr);

            if (res == -1){
                continue;
            }

            res = preprocess_recv_msg(comm_info_index);
//            print_rank_info();
//            printf(" [ompi_coll_ipmulticast_bcast] preprocessing ends, comm_id = %d...\n", res);

            if (res == -1){
                continue;
            }

//            print_rank_info();
//            printf(" [ompi_coll_ipmulticast_bcast] preprocessing ends, comm_id = %d...\n", res);

            int sender = recv_msg->sender;
            int seq = recv_msg->sequence;

            // the same communicator
            if (recv_msg->msg_type == DT_MSG){

//                print_rank_info();
//                printf(" [ompi_coll_ipmulticast_bcast] DT_MSG from the same communicator\n");

                if ((sender == root_globalrank && seq > comm_info->proc_seq[sender])
                    || (sender != root_globalrank && seq >= comm_info->proc_seq[sender])) {

//                    print_rank_info();
//                    printf(" [ompi_coll_ipmulticast_bcast] DT_MSG in future bcast, add to buffer\n");

                    recv_msg = enQueue(comm_info->msg_buffer, recv_msg);
                    if (recv_msg == -1){
                        recv_msg = (bcast_msg_t*)malloc(MAX_MSG_SIZE);
                    }
                }
            } else if (recv_msg->msg_type == NACK_MSG) {
                if (sender == root_globalrank && recv_msg->sequence == startSeq) {
                    // reset the timer
//                    print_rank_info();
//                    printf(" [ompi_coll_ipmulticast_bcast] NACK_MSG in the same communicator and bcast, reset timer...\n");
//                    print_msg(send_msg);
                    gettimeofday(&start_time, NULL);
                }
            } else if (recv_msg->msg_type == END_MSG) {
//                print_rank_info();
//                printf(" [ompi_coll_ipmulticast_bcast] END_MSG in the same communicator skip...\n");
                continue;
            } else {
                perror("wrong message");
            }
//            print_rank_info();
//            printf(" [ompi_coll_ipmulticast_bcast] Acking Elapsed time: %f\n", calElapseTime(&start_time));
		}
        comm_info->proc_seq[root_globalrank] += 1;
    }
    close(fd);
	post_bcast_data(&request);

//    print_rank_info();
//    printf(" [ompi_coll_ipmulticast_bcast] Jump out of received or send...\n");

//    if (rank != root){
//        int *data = (int *)request.data;
//        print_rank_info();
//        for (int i = 0; i < 1000; i++){
//            printf(" %d", data[i]);
//        }
//    }

    return (OMPI_SUCCESS);
}

END_C_DECLS
