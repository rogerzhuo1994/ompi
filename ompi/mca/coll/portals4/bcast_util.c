//
// Created by Roger ZHUO on 2019-12-05.
//

#include <stdlib.h>
#include "coll_rudp_bcast.h"
#include "bcast_util.h"

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