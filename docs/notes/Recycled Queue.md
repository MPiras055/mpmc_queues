# Basic Linked Queue
Linked queues are special data structures that offer the semantics of a FIFO queue in 2 ways. We can see the DS as a linked list of queue buffers, which support a `close()` method, that rejects all further enqueue operations after the call.

Any enqueue or dequeue operation can be handle in one of 2 ways:
1. Fast path: we directly access to the underlying FIFO buffer and execute the operation there
2. Slow path: the current buffer appears full or empty (respectively enqueue and dequeue) so a node transition has to happen.

The slow path is a critical operation, because it involves not contigous memory traversal and the invalidation of some shared memory location.

> All threads share a head and tail pointer that respectively point to the current tail node and head node.

Here is pseudocode of a linked queue, not accounting for memory reclamation detail or specific operations:
```cpp
//assuming
struct Queue {
	std::atomic<Queue*> next;
	
	bool enqueue(T item);
	bool dequeue(T& item);
}

//global shared pointers
std::atomic<Queue*> tail;
std::atomic<Queue*> head;

bool enqueue(T item) {
	Queue* currTail = tail.load();
	while(1) {
		Queue* tail2 = tail.load();
		//the current tail could been changed in a previous iteration
		if(currTail != tail2) {
			currTail = tail2;
			continue;
		}
		
		Queue* tNext = tail->next.load();
		
		//next pointer was setted (current tail was closed)
		if(tNext != nullptr) {
			//update the global pointer
			if(tail.compare_exchange_strong(currTail,tNext)) {
				currTail = tNext;
			} else {
				//someone succeded so the currTail was updated to the current
				//value of tail;
			}
			continue;
		}
		
		//attempt to place and enqueue on the current tail
		if(currTail->enqueue(item)) {
			return true;
		} else {
			//the current tail was closed, we have to link a new one
		}
		
		//each thread gets a private new tail
		Queue *newTail(...);
		//each thread enqueues its item (always successful since no
		//interference)
		(void)newTail.enqueue(item);
		
		//Try to link the new queue
		Queue *nullNode = nullptr; //value to be filled in case of failed cas
		if(currTail->next.compare_exchange_strong(nullNode, newTail)) {
			//we successfuly linked the new node
			
			//we try to update the shared tail pointer
			(void)tail.compare_exchange_strong(currTail,newTail);
			return true;
		} else {
			currTail = nullNode; //will be filled by the failed CAS;
			//try again
		}
	}
}

bool dequeue(T& item) {
	Queue* currHead = head.load();
	
	while(1) {
		Queue* head2 = head.load();
		//the current pointer could have been changed in a previous iteration
		if(currHead != head2){
			currHead = head2;
			continue;
		}
		
		//attempt to dequeue from the current head (FIFO semantics)
		if(currHead->dequeue(item)){
			return true;
		}
		
		//the current segment is empty (check if a next exists)
		Queue *headNext = currHead->next.load();
		if(headNext == nullptr) {
			//no-one linked a new next so the queue is empty
			return false;
		}
		//check if the current segment is still empty
		/*
			seems redundant but there's a window from the last time
			we found the segment empty where a thread could have inserted
			then linked a new queue. If the next queue is setted then we 
			have the strong guarantee nobody will insert in the headQueue anymore
		*/
		if(currHead->dequeue(item))
			return true;
			
		//we have to advance the head global pointer
		if(head.compare_exchange_strong(currHead,headNext)) {
			//we successguly removed the currentHead from the queue
			//try to update the global head pointer
			delete currHead; //ERROR (we'll see later)
			currHead = headNext;
		}
		//try on next head
	}
}
```

## Overlooked Details
### Shared Memory Management
As mentioned in the pseudocode of `dequeue`, when removing a node from the linked list we cannot blindly delete it. This happens because (since the algorithm is non-blocking) we have no guarantee that all other threads are not operating on that particular queue.
> For example there could be some late threads that are still checking if the current queue supports dequeue operation. Or there could be some late threads that are checking if the current queue supports enqueue operations.

In order to handle this, we have to define on a method that allows the threads to share informations, in this case, which queue they're working on, to delay the memory reclamation.

#### Tracking Pointers

A naive but simple solution would be to use Reference counters for each queue pointer. We find out that this is not our case since of the high access frequency to the reference counter, which would generate much more cache ping pong that needed.

A follow-up methods would be to use Reference SWL (Single Writer locations), where each thread would write (`protect`) the pointer they're working on by writing its value. A call to protect will delay memory reclamation, since a thread that really wants to delete a pointer will check if all the SWL do not contain that pointer.

> This also ensures the memory tracking is ABA free. A potential source of ABA related issues would the the allocator cache (we have no guarantee that the allocator always gives us fresh memory pointers). Explicitly protecting a pointer guarantees this doesn't get allocated so, pointer comparisons can be made blindly while we're under the protection status.

This looks great, but we're not considering that if a thread is protecting a pointer that needs to be deallocated, the thread that checks this condition has to delay the reclamation, by putting the pointer in a private memory location. To not impact the throughput of the DS the reclamation of the current pointer will be tried again when the same thread is deallocating another pointer. This doesn't give us strong guarantees on when this will happen.

#### Tracking Epochs
Aboe are mentioned some serious pointer that make up the compelling case that hazard pointers may not be the gratest solution in order to make fast memory reclamation.

Instead we can use another strategy that is very related to *Epoch Based Garbage Collectors*.

> Just to say: we can build a hazard pointer based garbage collector, using 2 shared queues as a copying collection and an epoch that for every iteration swaps the buffers. One will be used as `fromSpace` and one as `toSpace`. The main problem of this is the hazard free check where for each pointer in the queue we necessarily have to check if any thread is protecting it. Since in this case protection and clearing are swift, we may be overloading the system of a linear scan of padded cells. This is heavy. Plus the fact that we have to make a consensus algorithm on how to advance the epoch, and this requires no thread to concurrently operate on any queue, so to reference protect the epoch.

Epoch based garbage collector work in this way:
Each thread uses a SWL to announce that it's working on shared memory. We can call this operation `protect`, but in this case we protect the current epoch of the GC. Until that thread is active, and protecting that epoch, the same epoch cannot be advanced. 

Advancing an epoch corresponds in a bucket advancement. Currently we use a 4 buffer approach:

- current(e) -> threads put here memory that cannot be deallocated
- grace(e) -> additional buffer that holds memory that cannot be yet reclaimed
- free(e) -> threads can deallocate memory in this buffer
- next(e) -> empry buffer for next iteration

 > The buffers rotate modulo 4, in practice we use the epoch as an index to always find the right buffer, when the epoch advance, we don't need to copy all items from a buffer to another, so the buffer shift is implicit. A constraint is that when we operate on any buffer we have to make sure that the epoch doesn't change (or we could incoherently modify a DS).

This said the garbage collector allows for 2 operations:
- `retire` -> needs the caller to be protecting an epoch, puts the ML (memory location) in the `current bucket` for the epoch that the thread is protecting
- `reclaim` -> needs the caller not to be protecting an epoch. Tries to get a ML from the free bucket of the global epoch. If the free bucket is empty it tries to advance the epoch, until it's known that at least one ML has been retired and not yet reclaimed.

The GC (or Recycler) also uses an additional cache layer, that can be used by threads to put or get fresh ML
> This is useful since in our linked queue of $n$ threads that try to link a new queue $n-1$ will be unsuccessful.

# Bounded Recycled Queue
The main challenges we need to face when using a recycler data structure are the following:
- full condition for enqueues
- pointer comparison (since we have to handle ABA)

Let's sketch a first implementation

```cpp
//assuming
struct Recycler {
	void protect_epoch(uint64_t ticket);
	void clear_epoch(uint64_t ticket);
	T protect_epoch_and_load(uint64_t ticket, std::atomic<T>& atom);
	void retire(uint64_t ticket, T tml);
	void reclaim(uint64_t ticket, T& tml_out); //gc is supposed to always be able to hold all tmls
	bool getCache(T& tml_out);
	void putCache(T tml_in); //cache is supposed to always be able to hold all tmls 
}

//get a valid ticket for the calling thread
uint64_t get_ticket() const;
//clears a private queue and make it empty again
void clearPrivateQueue() const;


//global atomic pointers
std::atomic<Queue*> tail;
std::atomic<Queue*> head;

bool enqueue(T1 item) {
	uint64_t ticket = get_ticket();
	bool failedReclaim = false;
	while(1) {
		//spins until the currTail stabilizes
		Queue* currTail = protect_epoch_and_load(ticket, tail);
		
		//check if next was setted
		Queue* next = currTail->next.load();
		if(next != nullptr) {
			(void)(tail.compare_exchange_strong(currTail,next);
			continue; //load and protect the new pointer
		}
		
		//try to enqueue on the current tail
		if(currTail->enqueue(item)) {
			clear_epoch(ticket);
			return true;
		}
		
		//enqueue could not be placed, need a new Queue
		Queue* newQueue = nullptr;
		if(getCache(newQueue)) {
			failedReclaim = false;
			//we're still protecting the current tail
			//enqueue on our segment
			(void)newQueue->enqueue(item);
			Queue* nullNode = nullptr;
			if(tail->next.compare_exchange_strong(nullNode,newQueue)) {
				//try to update the pointer
				(void)tail.compare_exchange_strong(currTail,newQueue);
				clear_epoch(ticket);
				return true;
			} else {
				//someone else linked... put the queue back in cache
				clearPrivateQueue(newQueue);
				putCache(newQueue)
			}
		} else {
			//drop protection reclaim a segment and put it in cache
			clear_epoch(ticket);
			if(reclaim(newQueue)) {
				failedReclaim = false;
				putCache(newQueue);
			} else {
				//the queue could be full
				//check if next field was setted or the current tail was changed
				if(failedReclaim) {
					return false;
				} else {
					failedReclaim = true;
				}
			}
		}
		
	}
}

/*
Dequeue doesn't change much, we just need to add protections and retire

*/
```

Given this, the only problem we're facing is that the recycler, uses `Index` instead of pointer, in order to keep fast queues and don't rely on DCAS operations. So for everything, we cannot store raw pointers for the global shared pointers, but we need to rely on tagged pointers.

> A tag pointer consist in a 8 byte value where the high 4 bytes, define a `Version` (for aba control and safeEnqueue operations ) and the low 4 bytes identify the index of the Recycler.

So we can make 2 main methods, that allow to convert `TaggedPtrs` to `Ptrs` and viceversa:
- `encode` takes an index and a version and produces a `TaggedPtrs`
- `decode` takes a `TaggedPtr` and returns its associate raw pointer.

This can be useful to design `safeEnqueue`, since we can `thread_local` store the last view tail (`TaggedPtr`) so we just care about the version and decide to abort operations if the version hasn't changed.

As versions we have to make sure to keep a reserved version that will represent `nullptr`, to do this we decide version is always an odd number and to keep conversion simple `Version = 0` maps to `nullptr`


 

