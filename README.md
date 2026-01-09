# mpmc_queues

This repository serves as testing playground specifically for blocking and non-blocking concurrent queue implementations.

Specifically, we focus on unbounded queues aiming to analyze performance and overhead under different settings.

## Interface Specification

In order to manage different implementations in a portable way, we use abstract interfaces to define API contracts.

All interface specifications can be found in `include/queue/base`. Mainly we leverage three abstracts classes:

- `IQueue<T>`: defines basic bounded queue contracts:
  - `bool enqueue(const T)`
  - `bool dequeue(T&)`
  - `size_t size()`
  - `size_t capacity()`
- `ILinkedSegment<T,Next>`: defines basic linked segments contracts:
  - `bool enqueue(const T, bool info)`: see _Note 2_
  - `bool dequeue(T&, bool info)`: see _Note 2_
  - `D getNext()`: getter for the next field of the linked list (requires D to be default and trivially constructible)
  - `bool close()`: setter that disables further enqueues on the segment. See \*_Note 1_
  - `bool open()`: setter that re-enables enqueues on the segment. See _Note 1_
  - `bool isClosed()`
  - `bool isOpen()`

> Note 1: Closing a segment is necessary to enforce FIFO semantics across a linked list of bounded contigous segments. In an unbounded fashon, after a segment has been successfuly closed, it will be drained and eventually unlinked from the linked list and deleted. We empirically found that a given segment, after being drained, can be reinitialized with constant overhead (in opposition to a constructor call), mainly via index realignment. The `bool open()` method can be used just to do that. As a caveat, the method is _NOT_ MT-Safe and will result in undefined behaviour if called on a segment that was not drained.

> Note 2: When attempting to construct bounded linked queue, it happens that some failed insertions (blocked by a previous _closure_ of the segment) trigger the allocation of a new linked segment. As a way to enforce a memory bound, these allocations may fail. When this happens, further insertions may be attempted on the first segment, resulting in index misalignment and/or induced livelock phoenomena in some implementations. The `bool info` parameter of the method is used in some implmentation as a hint on the potential closed state of the segment. This info is handled differently by implementations. Whether a linked segment _requires_ info, can be toggled by overriding a static boolean `info_required` defined as a field of `ILinkedSegment<T,D>`. If not defined, this defaults to false

- `IProxy<T> overloads IQueue<T>`: defines basic contracts for a given unbounded/bounded queue. In addition to the `IQueue<T>` contracts specifies:
  - `bool acquire()`: allow a thread to get an implicit ticket in order to perform operations on the queue.
  - `void release()`: allow for thread ticket reassingment

> LinkedSegments implementations may not be used directly, as the construction of a Linked Queue most certainly requires for additional construct (i.e. Concurrent memory-reclamation) that are implementation specific thus should not be exposed to final users. For this reason a concrete LinkedSegment implementation may be used only when coupled with a `IProxy<T>` specification. As a default failsafe, all methods are marked private.

## Supported Implementations

- [x] _CASLoopQueue - LinkedCASLoop_ `PTR`: respectively standalone queue and linked segment implementations of the lockless Vyukov buffer.
- [x] _PRQueue - LinkedPRQ_ `PTR`: respectively standalone queue and linked segment implementations of the (_obstruction-free_) [[insert-reference]]
- [x] _SCQueue_ - _LinkedSCQ_: respectively standalone queue and linked segment implementations of the (_non-blocking_ See Note 2) [[insert-reference]].
- [x] _FAAArray_: linked segment implementation of the (_obstruction-free_) [[insert-reference]]
- [x] HQSegment: linked segment implementation of an _obstruction-free_ linear buffer, that supposedly allows for a better memory footprint. Strongly based on _FAAArray_
- [] LFCASLoopSegment: linked segment implementation of a _lock-free_ linear buffer, based on [[insert-reference]] that supposedly avoids ABA, by not reusing buffer cells.

> Note 1: With `PTR` we mark implementations that as far as now, allow only for generic pointer storage

> Note 2: SCQ implements implicit storage (for arbitrary size items) that requires two underlying queues (_lock-free_ `lfring`). The enqueue of an item is not an atomic transaction (i.e. a crashed thread may permanently invalidate a queue slot).

## Unbounded Proxy Implementations

We include a protable agnostic `UnboundedProxy` that allows to use different linked segments in a plug-and-play fashon.

```cpp
/// ==============================
/// UnboundedQueue Usage Example
/// ==============================

// See include/linked/UnboundedProxy.hpp for the full template specification
#include <UnboundedProxy.hpp>   //proxy header
#include <HQSegment.hpp>        //segment header

size_t node_size    = 1024;
size_t max_threads  = 12;
UnboundedProxy<void*,HQSegment> u_queue(node_size,max_threads);

assert(u_queue.acquire());
const void *dummy = 0xABCD;
size_t ops  = 1000;

//simulate some enqueues
for(size_t i = 2; i <= ops + 1; i++) {
    void *dummy = (uintptr_t)i;
    (void) u_queue.enqueue(dummy);
}

//simulate some dequeues
for(size_t i = 2; i <= ops + 1; i++) {
    void *cmp = nullptr;
    bool deq_ok = u_queue.dequeue(cmp);
    assert(deq_ok && (uintptr_t)i == (uintptr_t)cmp);
}

void* unsetted = nullptr;
bool empty = !u_queue.dequeue(unsetted);
//- size() - exact if called sequentially (good approx while concurrent usage)
size_t size = u_queue.size();
assert(empty && size == 0);

bool u_queue.release();
```
