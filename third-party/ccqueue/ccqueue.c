#include <stdlib.h>
#include "ccqueue.h"
#include "ccsync_f.h"

static inline
void serialEnqueue(void * state, void * data)
{
  queue_t * queue = (queue_t *)state;
  node_t * volatile * tail = &queue->tail;
  node_t * node = (node_t *) data;

  (*tail)->next = node;
  *tail = node;
  RELEASE(&queue->numEnqs, queue->numEnqs + 1);
}

static inline
void serialDequeue(void * state, void * data)
{
  queue_t * queue = (queue_t *)state;
  node_t * volatile * head = &queue->head;
  node_t ** ptr = (node_t **) data;

  node_t * node = *head;
  node_t * next = node->next;

  if (next) {
    node->data = next->data;
    *head = next;
    RELEASE(&queue->numDeqs, queue->numDeqs + 1);
  } else {
    node = (void *) -1;
  }

  *ptr = node;
}

void queue_init(queue_t * queue, int nprocs)
{
  ccsynch_init(&queue->enq);
  ccsynch_init(&queue->deq);

  node_t * dummy = align_malloc(CACHE_LINE_SIZE, sizeof(node_t));
  dummy->data = 0;
  dummy->next = NULL;

  queue->head = dummy;
  queue->tail = dummy;
  queue->numDeqs = 0;
  queue->numEnqs = 0;
}

void queue_register(queue_t * queue, handle_t * handle, int id)
{
  ccsynch_handle_init(&handle->enq);
  ccsynch_handle_init(&handle->deq);

  handle->next = align_malloc(CACHE_LINE_SIZE, sizeof(node_t));
}

void enqueue(queue_t * queue, handle_t * handle, void * data)
{
  node_t * node = handle->next;

  if (node) handle->next = NULL;
  else node = align_malloc(CACHE_LINE_SIZE, sizeof(node_t));

  node->data = data;
  node->next = NULL;

  ccsynch_apply(&queue->enq, &handle->enq, &serialEnqueue, queue, node);
}

void * dequeue(queue_t * queue, handle_t * handle)
{
  node_t * node;
  ccsynch_apply(&queue->deq, &handle->deq, &serialDequeue, queue, &node);

  void * data;

  if (node == (void *) -1) {
    data = (void *) -1;
  } else {
    data = node->data;
    if (handle->next) free(node);
    else handle->next = node;
  }

  return data;
}

size_t queue_size(queue_t * q, handle_t * h) {
    size_t numDeqs = ACQUIRE(&q->numDeqs);
    size_t numEnqs = ACQUIRE(&q->numEnqs);
    return numEnqs - numDeqs;
}

void queue_free(queue_t * q, handle_t * h) {
    void* r;
    do {
        r = dequeue(q, h);
    } while (r && r != EMPTY);
    free(q->head);
    ccsync_free(&q->enq);
    ccsync_free(&q->deq);
}

void handle_free(handle_t * h) {
    if (h->next)
        free(h->next);
    ccsync_handle_free(&h->enq);
    ccsync_handle_free(&h->deq);
}
