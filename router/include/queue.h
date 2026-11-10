#ifndef QUEUE_H
#define QUEUE_H

#ifdef __cplusplus
extern "C" {
#endif

struct queue;
typedef struct queue *queue_t;

/* create an empty queue */
extern queue_t create_queue(void);

/* insert an element at the end of the queue */
extern void queue_enq(queue_t q, void *element);

/* delete the front element on the queue and return it */
extern void *queue_deq(queue_t q);

/* return a true value if and only if the queue is empty */
extern int queue_empty(queue_t q);

#ifdef __cplusplus
}
#endif

#endif
