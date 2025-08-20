# Brief
This project aims to explore multithread programming, specifically targeting queue data structures. The focus of this study is to expand on different multithread synchronization techniques to better understand the cost of thread synchronization and different hardware capabilities. 

The three main approaches on thread synchronization are:
- **lock-based algorithms**: use OS managed locks to guarantee data consistency. With the term of lock-based, we refer to a lock semantics that relies on `mutex` based primitives.
- **lockless algorithms**: instead of using mutexes, which most often than not rely on `syscalls` (so frequent context switches and OS pressure), threads implement critical sections using `spinlocks`.
- **lock-free algorithms**: ad-hoc algorithms where a data structure is specifically designed to allow for concurrent reading and writings by using `atomic` operations. The lock free paradigm specifies a *progress condition*: in a countable amout of concurrent iterations at least one thread makes progerss (makes livelock impossible but still suffer starvation)


# Layer Structure
Here follows a brief description of the codebase layers

## Domain Layer
Defines basic abstractions. A needed **interface** is `IMPMCQueue<T>` that expones virtual basic queue contracts:
- `bool enqueue(T item)`
- `bool dequeue(&T item)`
- `size_t capacity()`
- `size_t size()` *may be approximate in some cases*

> All abstracts and concrete implementations extend this interface

### Follow-up: Unbounded Queues
We will focus on both bounded and unbounded queues (since we can restrict the unbounded case to a bounded one). Most basics undounded queues may be constructed from bounded ones.

In order to do this we can use a linked list approach (MS-Queue), that links nodes that point to bounded queues. When a queue is full, threads allocate and initialize a new segment and link it as new list tail (via lock or `CAS`) updating the corresponding pointer.

#### Hazard Pointers
The linked list is only used to manage dynamic memory. This is non-trivial in a non-blocking enviroments. Since multiple thread can obtain the same reference to a memory location the deallocation of it becomes difficult. We have to rely on some mechanism that allow us to discriminate between empty segments that will never be accessed again (the pointer has been updated, dead memory) and segments that threads can still hold reference to.

A simple and easy way to do this would be a reference counter that gets incremented and decremented for every (`enqueue/dequeue` operation). Though having a single memory location that gets accessed and modified concurrently generates a hotspot (cache-coherent protocols require to fetch the value from a further memory layer), so harming the throughput. 

We can trade the constant space of a reference counter, with a list of single writer locations (each thread marks it with the pointer it's working on). Since the container are single writer, cache invalidation is much more mitigated.

To check no thread hold any stale reference (after updating the pointer), a linear scan is performed over all memory locations. In order to make them truly single writer we have to apply padding (this also makes the linear scan be slower since we can't rely on the cache lines optimization).

## Infrastructure Layer
Implements the business logic of different queues implementations. All queues (bounded or unbounded) must extend the abstract class `IMPMCQueue<T>` and implement basic queue contracts (`enqueue,dequeue,size`) defined.

When instantiating the queues we use a `QueueFactory` that calls a specific queue constructor based on a string identifier that is passed as a parameter.

## Testing and Simulation Layer
### Testing Parameter
The most basic parameters we aim to test are:
- **throughput**: based on different measures (varying contention) we can estimate the specific implementation reliance and scaling capabilities
- **pinning effect**: testing on hardware that has an elevated number of cores can make memory loadings a bottleneck. OS schedulers are generally non optimized for cache-awareness, and NUMA-SMT systems (non-uniform memory loading time) are generally hard to work with expecially for high throughput and fixed data structures. Pinning threads to physical cores can mitigate memory bottlenecks.
- **stress**: enstablish realistic scenarios that stress test the queues either by doing simulated work (no memory, so no cache interference) or importing existing projects
- **static vs dynamic memory** we can preallocate static stack allocated containers for the queue storage or use runtime allocation

### Unit Testing
All queues must be compliant with the abstract data structure specification. Testing for correct semantics in a multithreaded enviroment is challenging since the FIFO ordering of `enqueue/dequeue` is hard to track over different threads. In order to do this, test associate a timestamp (incremental starting by 1) and each thread checks the current timestamp with the last received. We can't test for concurrent FIFO so we just check for perceived FIFO ordering
