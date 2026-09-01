
This document stores benchmark insights needed to evaluate and compare various concurrent queue implementations. It includes brief descriptions of each queue, focusing on their progress guarantees, ABA-safety mechanisms, contention avoidance strategies, and specific trade-offs.

## Overview of Implementations

Some queues are stand-alone (lock-less, lock-free), while others require an external mechanism for lock-freedom, such as a LinkedProxy (e.g., MS-Queue).

### PRQ (Portable Ring Queue)
Portable Ring Queue is an obstruction-free bounded buffer with FIFO semantics. It borrows heavily from CRQ (Morrison & Afek 2015), using `fetch_add` (FAA) to manage the increment of the two authoritative monotonic counters for enqueue and dequeue. 
It employs a 3-step transaction that emulates CAS2 (unsupported in most RISC architectures) to lock and update a 128-bit cell packing a `value` field and a `seq` field. The `seq` field manages ABA safety in enqueue-dequeue transactions. 
Using FAA eliminates usual CAS hot-spots, but may cause an incoherent transaction regarding a dequeue on an empty queue. Since FAA cannot validate if the head counter has surpassed the tail counter, the tail counter must be advanced (possibly wasting an empty slot). This can lead to livelock if empty dequeues are frequent. To avoid this, enqueue operations stop looking for an empty slot after a starvation threshold ($2n$) and signal to all enqueuers that the buffer is permanently closed (though it stays open for drain operations). Thus, it needs to be coupled with a linked proxy to provide true lock-freedom.
*   **Bounded?**: No
*   **ABA-Safety**: Yes (via 3-step CAS2 transaction on 128-bit cell)
*   **Progress Guarantee**: Obstruction-free (P-C clash possible on frequent empty dequeues)
*   **Contention Avoidance**: FAA for monotonic indexes (prevents P-P/C-C clashes); unsafe enqueue prevents P-C clashes.
*   **Magic Constants**: Starvation threshold; tail/cell reload for unlocking a locked cell.
*   **Value Type**: Valid pointer type (64-bit). Requires dirty pointers for CAS2 emulation (LSB set).

### LFRing (Lock-Free Ring Queue)
Borrows from CRQ (Afek & Morrison 2015) but handles CAS2 non-universal support using single-word CAS operations and bit-packing (32-bit `seq` and `value` into a 64-bit cell). Uses an `eq_threshold` to limit empty dequeue attempts, achieving lock-freedom. The threshold is initially set to $4n$; empty dequeues decrement it, successful enqueues reset it. If negative, the operation aborts. To linearize the empty check, it needs $2n$ cells for $n$ items, resulting in an unlinearizable full check (full state may hold $n$ to $2n-1$ items). Can be composed with a linked-proxy for unbounded/chunked forms.
*   **ABA-Safety**: Yes (bit-packing 32-bit seq and value, single-word CAS)
*   **Progress Guarantee**: Lock-free
*   **Contention Avoidance**: FAA for monotonic indexes; unsafe enqueue to prevent P-C clashes.
*   **Magic Constants**: No (`eq_threshold` is a conservative upper bound).
*   **Value Type**: 32-bit unsigned type.

### SCQ (Scalable Circular Queue)
Built using two LFRing queues, resulting in a non-blocking arbitrary-type queue. Uses `free_slots` and `data_items` LFrings to hold slot indices (`uint32_t`) for an underlying contiguous linear buffer. 
Enqueue: 1) Get slot from `free_slots`, 2) Copy/move item to buffer, 3) Enqueue slot to `data_items`.
Dequeue is symmetrical. Not strictly lock-free due to the 3-step operations (potential priority inversion), but livelock-free assuming crash-free threads. Has a massive memory footprint: $\sim 513n$ bytes per item (assuming 128-byte padding and $2 \times$ capacity rule from LFRing).
*   **ABA-Safety**: Yes (lock-free composition of LFRing)
*   **Progress Guarantee**: Livelock-free (assuming crash-free threads).
*   **Contention Avoidance**: Same as LFRing.
*   **Magic Constants**: No.
*   **Value Type**: 32-bit unsigned type (for slots), arbitrary for actual buffer.

### PSCQ
Combines PRQ (for ABA avoidance and 64-bit items) and LFRing (`eq_threshold` for lock-free bounding). Substitutes bit-packing with PRQ's CAS2 emulation. Maintains similar size to LFRing due to cache-line padding.
*   **ABA-Safety**: Yes (PRQ composition).
*   **Progress Guarantee**: Lock-free (LFRing composition).
*   **Contention Avoidance**: Same as PRQ.
*   **Magic Constants**: No.
*   **Value Type**: Same as PRQ.

### Vyukov
Implements the Vyukov Buffer policy. Uses two authoritative counters incremented via CAS-retry loops. Handles ABA via classic 64-bit `seq` tagged cells. Threads increment a monotonic counter to book a cell, write the value, then atomically update the `seq` field. This 3-step transition can create priority inversion.
*   **ABA-Safety**: Yes (64-bit seq tagged cells).
*   **Progress Guarantee**: Livelock-free (assuming crash-free threads).
*   **Contention Avoidance**: Poor (CAS-retry loop to get a unique slot).
*   **Magic Constants**: No (tunable with SPIN_HINT).
*   **Value Type**: Arbitrary.

### Vyukov-DCAS
Makes Vyukov lock-free using the x86 `CMPXCHG16B` instruction. Atomically writes 128 bits (value + seq) and employs a helping scheme.
*   **ABA-Safety**: Yes (via `CMPXCHG16B` - **NOT PORTABLE**).
*   **Progress Guarantee**: Lock-free.
*   **Contention Avoidance**: Poor.
*   **Magic Constants**: No.
*   **Value Type**: 64-bit unsigned pointer type.

### Vyukov-NO-ABA
Attempts a lock-free, portable Vyukov by leveraging the absence of ABA (e.g., via LL/SC on RISC) or assuming unique data. Uses dirty-tagging on empty cells and substitutes DCAS with `compare_exchange_weak`.
*   **ABA-Safety**: Assumed.
*   **Progress Guarantee**: Lock-free.
*   **Contention Avoidance**: Poor.
*   **Magic Constants**: No.
*   **Value Type**: 64-bit unsigned pointer type.

### CacheRing
Useful for static memory pools. Essentially implements Vyukov-DCAS policy but substitutes DCAS with single CAS via bit-packing.
*   **ABA-Safety**: Yes (version indexing).
*   **Progress Guarantee**: Lock-free.
*   **Contention Avoidance**: Poor.
*   **Magic Constants**: No.
*   **Value Type**: 32-bit unsigned types.

### Phased Bucket
Wait-free bounded FIFO queue for explicit phases (MP-0C or 0P-MC). Since producers and consumers never run concurrently, both authoritative counters are packed in a single word. Uses FAA for increments and features an implicit reset mechanism.
*   **ABA-Safety**: Yes (due to strict phase barriers; buffer is not circular).
*   **Progress Guarantee**: Wait-free.
*   **Contention Avoidance**: Yes.
*   **Magic Constants**: No.
*   **Value Type**: Templated (typically 32-bit uint, but arbitrary works).

### Mutex
Baseline implementation. Synchronizes a circular buffer with a global mutex and condition variables (`notify_one()` policy).

### Spin
Baseline implementation testing optimistic mutex holding via a global spinlock. Uses `atomic_wait` to park threads operating on full/empty queues.

### FAAArrayQueue
Logically unbounded array (linear buffers) that inherently disregards ABA safety since cells are never reused. Uses FAA to obtain unique cells and `EMPTY`/`SEEN` tags. Highly memory efficient (no padding needed for a linear buffer). Requires constant buffer provisioning.
*   **ABA-Safety**: Yes (cells never reused).
*   **Progress Guarantee**: Lock-free (assuming MS-linkage).
*   **Contention Avoidance**: Yes (FAA on indexes).
*   **Magic Constants**: SPIN_HINT to prevent empty cell invalidation.
*   **Value Type**: Architecture pointer type.

### HybridQueue
Improves FAAArrayQueue with backpressure (`slowDequeue`/`fastDequeue`). If a buffer's `next` pointer is set, it stops accepting enqueues and switches to `fastDequeue`, reducing the invalidation upper bound to $t$ (thread count) instead of $n$ (capacity). `slowDequeue` uses the Vyukov-NO-ABA protocol, preventing automatic cell invalidation on empty queues and introducing backpressure (consumers clash, distancing from producers).
*   **ABA-Safety**: Yes (cells never reused).
*   **Progress Guarantee**: Lock-free (assuming MS-linkage).
*   **Contention Avoidance**: Yes.
*   **Magic Constants**: `SPIN_HINT`.
*   **Value Type**: Architecture pointer type.

## Summary Matrix

| Queue Family      | Progress Guarantee | ABA Strategy                   | Primary Trade-off / Bottleneck                             |
| :---------------- | :----------------- | :----------------------------- | :--------------------------------------------------------- |
| **PRQ**           | Obstruction-free   | 128-bit cell (CAS2 emulated)   | P-C clashes on frequent empty dequeues                     |
| **LFRing / PSCQ** | Lock-free          | Bitpacking / CAS2              | Needs $2n$ cells for $n$ items (unlinearizable full state) |
| **SCQ**           | Livelock-free      | Lock-free composition          | Massive footprint ($\sim 513n$ bytes/item) due to padding  |
| **Vyukov (DCAS)** | Lock-free          | `CMPXCHG16B`                   | Zero portability outside x86                               |
| **Phased Bucket** | Wait-free          | Epoch strict barrier           | Extremely narrow use case (MP-0C / 0P-MC)                  |
| **FAAArrayQueue** | Lock-free          | Infinite linear buffer         | Requires fine-tuned `SPIN-HINT` + unbounded only           |
| **HybridQueue**   | Lock-free          | Infinite buffer + backpressure | unbounded only                                             |
# Benchmark Summary
## FAAArrayQueue vs HybridQueue
I would want to check how much is the difference in performance in a no-tuned FAAArrayQueue as well as a no tuned HybridQueue. I'd want to know how many cells are wasted in both cases by enqueue operation, in order to do this i could make the enqueue operation return or store elsewhere the number of wasted attempts in enqueing an item (due to a cell being `SEEN`)

### Memory Pressure
PRQ and poorly tuned FAAArrayQueue suffer from a constant allocations of new segment (in an oversubscriber scenario) due to using that as progress guarantee. LinkedProxy already exposes an interface to count the number of segments that the queue globally instantiates, it would be interesting to see how many segments on average are needed for a transfer of $n$ items. Knowing the number of cells of each segment we could test an efficieny metric in the number of cells needed to store an item given a workload

### Throughput
I want to test against median throughput given a transfer of $n$ items. I can already toggle a delay in producer and consumers to simulate work being done to produce and consume said items. The work can be tuned as random, as a distribution of a deterministic amplitude given a random center within bounds. I would also like to test for dynamic threading since i fixed the linked proxy to being able to support an arbitrary number of threads.

### Dynamic Workload
Given a fixed number of trasfer that each thread can get via chunk-stealing, i want to test the event of a dynamic number of threads. I was thinking randombly within a prefixed distribution, to check if a thread should be periodically allowed to get a new chunk of items or not. Since most my cases are around pathological cases about oversubscriber scenarios, I should probably try to produce caothic cases where threads randomly choose wheter to consume a certain set of items hoping that threads will pile up and produce said scenarios.