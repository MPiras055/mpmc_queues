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
- [-] LFCASLoopSegment: linked segment implementation of a _lock-free_ linear buffer, based on [[insert-reference]] that supposedly avoids ABA, by not reusing buffer cells.

> Note 1: With `PTR` we mark implementations that as far as now, allow only for generic pointer storage

> Note 2: SCQ implements implicit storage (for arbitrary size items) that requires two underlying queues (_lock-free_ `lfring`). The enqueue of an item is not an atomic transaction (i.e. a crashed thread may permanently invalidate a queue slot).

## Unbounded Proxy Implementations

We include a protable agnostic `UnboundedProxy` that allows to use different linked segments in a plug-and-play fashon. The Proxy is strongly inspired from [[insert LPRQ reference]], which implements a variant of the Michael-Scott Queue [[insert-reference]] with Hazard Pointers [[insert reference]] as concurrent memory management system

```cpp
/// ==============================
/// UnboundedQueue Usage Example
/// ==============================

// See include/linked/UnboundedProxy.hpp for the full template specification
#include <UnboundedProxy.hpp>   //proxy header
#include <HQSegment.hpp>        //segment header

using Segment = queue::segment::HQSegment;

size_t node_size    = 1024;
size_t max_threads  = 12;
UnboundedProxy<void*,Segment> u_queue(node_size,max_threads);

assert(u_queue.acquire());
const void *dummy = 0xABCD;
size_t ops  = 1000;

//simulate some enqueues
for(size_t i = 2; i <= ops + 1; i++) {
    void *dummy = (uintptr_t)i;
    // discard the return value (always successful)
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

## Bounded Proxy Implementations

Following implmentations apply changes to the _Unbounded Proxy_ in order to achieve a memory bound. If not explicitly mentioned, the API specification is
the same as _Unbounded Proxy_.

### BoundedCounterProxy

Internally uses a shared counter in order to keep track of the number of items actually installed in the queue. This serves more as a toy example since a global counter acts a hot contention point, which limits scalability as the number of concurrent processes increases.

The actual capacity of the queue may differ from the bound specified up to a
certain value, due to the counter's update concurrent timing. This means that
some enqueues may spuriously fail due to the read of a stale version of the counter.

The memory footprint of the queue is actually loose. In this particular implementation, we track item count, but each item insertion, may trigger
the allocation of a new segment (when using _obstruction-free segments_).
In order to impose a limit, we can split the capacity of the queue in

```cpp
#include <BoundedCounterProxy>  //proxy header
#include <HQSegment>            //segment header
#include <OptionsPack.hpp>      //Options

using Segment   = queue::segment::HQSegment;

constexpr size_t capacity     = 1024;
size_t max_threads  = 16;

/// Basic Queue instantiation
BoundedCounterProxy<void*,Segment> b_queue(capacity,max_threads);

constexpr size_t chunkF       = 8;
static_assert(chunkF < capacity && capacity % chunkF == 0,"Bad chunkFactor");

using ChunkOption = meta::OptionsPack<
    BoundedCounterProxyOpt<
        ChunkFactor<chunkF>
    >
>;

/// Queue instantiation with chunked allocation
///
/// allocates segments of capacity/chunkF size
/// capacity must be a direct multiple of chunkF
BoundedCounterProxy<void*,Segment,ChunkOption> b_queue_chunked(capacity,max_threads);

/// Refer to UnboundedProxy for usage example;
/// Mind that in this case enqueue calls may return false (don't discard the return value)
```

### BoundedChunkProxy

Splits the queue capacity in chunk (default 4), and uses an _optimistic counter_ that tracks the
current number of segments that are linked in the queue. This allows to reduce the counter related contention. The adjective "optimistic" refers to the fact that the counter will be decremented as soon
as an empty segment is unlinked from the queue, but this doesn't track the actual deallocation of the segment. The implementation employs a variant of Hazard Pointers [[insert-referece]] that may defer actual deallocation of a segment up.

This is necessary, since tracking actual segment deallocation is a non-trivial task that would add a lot
more overhead.

> Refer to `BoundedCounterProxy` for usage examples.

## BoundedPoolProxy [ WORK IN PROGRESS ]

> Refer to `Bounded Memory Proxy` for now
> `BoundedPoolProxy` is included as a lockless Bounded Linked Queue. It implements the best of both worlds being:

1. the employment of fast bounded segments that may not be used as standalone
2. all memory allocated upfront
3. constant initialization overhead whenever linking a new segment

In order to achieve this, we implement a Memory Pool that holds previously unlinked segments in a quarantine state (similar to previous deallocation defer). It is based off-of a Epoch-Based Reclamation Scheme, optimized for the track of a bounded number of immutable references.

> One of the downsides of EBR-Reclamation, is the variable latency, since a single thread will perform the destructions of all memory deallocation deferred by the whole system up until that point. This is no longer an issue, since we employ bounded circular queues as quarantine lists, that allow for parallel thread processing.

#### EBR-Reclamation Overview

To know more about EBR-Reclamation see [[insert-keir-fraser-reference]]. As refresher, EBR uses a shared global counter as a synchronization point for threads. When threads required to perform Shared Memory related operations, they acquire a copy of the global counter. Whenever threads need to delete a memory location, they must check that all threads that may be using it are done. The deallocation is deferred by inserting the pointer in a list, then the global counter (monotonic) is advanced, with the constraint that no threads possesses a stale version of the counter. In the original scheme only three lists are needed: _Quarantine_, _Grace_, _Free_. When _retiring_ a memory location, this is inserted in the quarantine list, and the counter needs to be successfuly advanced 2 times.

Variation to this scheme, involves in using bounded MPMC queues instead of linked lists, this allows to track a bounded number of memory locations, potentially by adding only one padding list while still beneficiating of the 2 counter advancements to perform a deallocation.

#### Qualities of the Proxy

Since segments are never returned to the allocator we can employ the `open()` method to perform a constant time re-initialization, mitigating the overhead associated to a full-fledged allocation.

Moreover, the memory pool implicitly imposes the memory bound, since if no segments are present in the pool then they must be all linked. The pool also imposes backpressure effects, since only threads that successfuly got a segment, will try to perform a link, limiting the number of threads that concurrently try to link new segments.

#### Work In Progress

Right now the most challenging desing part, is to determine the right bounded Queue to employ. Ideally we look for a bounded queue with strong progress guarantees (lock-freedom), which allows for parallel cell inspection. A good implementation is `lfring` supplied by `SCQueue` which is a portable implementation that supposedly satisfies our requirements.
