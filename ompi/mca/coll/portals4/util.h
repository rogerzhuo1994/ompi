//
// Created by Roger ZHUO on 2019-12-05.
//

#ifndef OMPI_UTIL_H
#define OMPI_UTIL_H

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

#endif //OMPI_UTIL_H
