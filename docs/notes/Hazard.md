# Hazard Pointers in general
non-blocking data structures that rely on memory that is dynamically allocated and dellocated at runtime, need special support so ensure the correctness of deallocation.

Since the memory is not globally protected (like we could using a mutex, to ensure that only one thread holds the memory and can modify it), we don't know when we can actually delete memory. Some threads could hold stale references to that memory or could still be accessing it after the delete is performed.

In order to control this we have to provide additional logic to allow us to track what memory each thread is using at any given time.

> This problem can be highly specific due to the memory access patterns of any specific data structures, in queues is actually straightforward.

## Non-blocking dynamically allocated queues
Let's consider the problem of producing an unbounded queue. This can be efficiently solved by imagining a linked list where each node contains a contigous segment that employs the queue semantics.

Each segment is singly linked (contains a `next` reference), and the linked structure holds 2 global (atomic) pointers `head,tail` that grant access to the first and last segments of the list for `dequeue/enqueue` operations respectively.

Since we're designing an unbounded queue, insertions can never fail. When the current tail segment is full, we want to allocate a new one and link it as the next one, then update the global tail pointer. 

This is easy, each thread that sees the queue as full, will perform the allocation and push its element inside (this guarantees a progress condition since exactely one thread will successfuly link the segment). The linking process is performed via a CAS operation, the thread which succeds can update the current tail pointer via another CAS.

> To recap: the linking of a new segment works as follows:
> - each threads allocates a private segment and pushes its element inside
> - only one thread will succesfuly update (via CAS) the `next` pointer of the current segment
> - only one thread will succesfuly update (via CAS) the global `tail` pointer

Since the linking works as 3 separate operations (it's not atomic), there are some situations where even though the new segment was succesfuly added