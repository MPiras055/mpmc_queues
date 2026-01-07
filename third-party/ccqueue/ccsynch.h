#ifndef _CCSYNCH_H_
#define _CCSYNCH_H_

#include <stdlib.h>
#include "align.h"

typedef struct _ccsynch_node_t {
  struct _ccsynch_node_t * volatile next CACHE_ALIGNED;
  void * volatile data;
  int volatile status CACHE_ALIGNED;
} ccsynch_node_t;

typedef struct _ccsynch_handle_t {
  struct _ccsynch_node_t * next;
} ccsynch_handle_t;

typedef struct _ccsynch_t {
  struct _ccsynch_node_t * volatile tail DOUBLE_CACHE_ALIGNED;
} ccsynch_t;
#endif
