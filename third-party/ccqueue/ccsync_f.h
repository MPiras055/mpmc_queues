#ifndef _CCSYNCH_F_H_
#define _CCSYNCH_F_H_

#include "ccsynch.h"
#include "primitives.h"


#define CCSYNCH_WAIT  0x0
#define CCSYNCH_READY 0x1
#define CCSYNCH_DONE  0x3

static inline void ccsynch_apply(ccsynch_t * synch, ccsynch_handle_t * handle,
                                 void (*apply)(void *, void *), void * state, void * data)
{
    ccsynch_node_t * next = handle->next;
    next->next = NULL;
    next->status = CCSYNCH_WAIT;

    ccsynch_node_t * curr = SWAPra(&synch->tail, next);
    handle->next = curr;

    int status = ACQUIRE(&curr->status);

    if (status == CCSYNCH_WAIT) {
        curr->data = data;
        RELEASE(&curr->next, next);

        do {
            PAUSE();
            status = ACQUIRE(&curr->status);
        } while (status == CCSYNCH_WAIT);
    }

    if (status != CCSYNCH_DONE) {
        apply(state, data);

        curr = next;
        next = ACQUIRE(&curr->next);

        int count = 0;
        const int CCSYNCH_HELP_BOUND = 256;

        while (next && count++ < CCSYNCH_HELP_BOUND) {
            apply(state, curr->data);
            RELEASE(&curr->status, CCSYNCH_DONE);

            curr = next;
            next = ACQUIRE(&curr->next);
        }

        RELEASE(&curr->status, CCSYNCH_READY);
    }
}

static inline void ccsynch_init(ccsynch_t * synch)
{
    ccsynch_node_t * node = (ccsynch_node_t*) align_malloc(CACHE_LINE_SIZE, sizeof(ccsynch_node_t));
    node->next = NULL;
    node->status = CCSYNCH_READY;

    synch->tail = node;
}

static inline void ccsynch_handle_init(ccsynch_handle_t * handle)
{
    handle->next = (ccsynch_node_t*) align_malloc(CACHE_LINE_SIZE, sizeof(ccsynch_node_t));
}

static inline void ccsync_free(ccsynch_t * synch) {
    ccsynch_node_t * node = synch->tail;
    synch->tail = NULL;
    if (node)
        free(node);
}

static inline void ccsync_handle_free(ccsynch_handle_t * handle) {
    ccsynch_node_t * node = handle->next;
    handle->next = NULL;
    if (node)
        free(node);
}

#endif