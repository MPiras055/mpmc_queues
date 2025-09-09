To recycle segments in the MPMC linked queues, the idea was to implement a core system that could interface with the queue internals as a sobstitute of an allocator while also providing strong guarantees about not violating any capacity precondition upon insertion.

> Making queues memory free would allow us to reuse old segments without needing to implicitly call the constructor during allocation, this could save a lot of resources since all cells (padded so lots of cache-misses) of a segment have to be initialized, even if the segment doesn't even get added to the queue.

# Why Epochs - Why not integrating Hazard Pointers?

While hazard pointers protection can be quite good and lightweight to prevent `use_after_free` and other related memory access problems, it mostly relies on the allocator performance to keep low latencies in data structure.

Adding a linked proxy to a queue means adding overhead, which we would want to minimize. The fast path of said queues is very short, while the slow path can be more tedious (especially in starvation prone enviroments), so the time a pointer is being `hazard_protected` may vary.

Designing a recycler using hazard pointers at its core means that for every pointer we ought to recycle, we have to iterate on all threads SWL (Single Writer Location) checking if it's not protected. It's hard to quantify the overhead added due to this, but since protection can be cleared right after we checked for it, and the fact this work has to be done for all pointers look like wasted computation.

> Using a epoch based recycler, each thread uses SWL to sync with a global monotonic epoch. This syncing is relatable (NOT equal) to `hazard_ptr.protect()`.

# General Idea
In general to make an epoch based recycler we need the following:
1. **Epoch**: an atomic monotonic counter (`uint64_t` to prevent ABA)
2. **SWLs**: single writer locations, one for each thread each one being `struct{bool active,uint64_t epoch}` or `uint64_t` with the `bool` being the MSB
3. **Recycle Buckets**: 4 buckets that contains object that will be recycled
4. **Cache Bucket**: a bucket for fast access (not essential)

## Epoch
A monotonic counter that is used to synchronize the processes. When operating on shared memory, each process protects the current epoch by writing it in its SWL. This prevents the epoch from advancing.

## Recycle Buckets
As previously said, we can track reachable memory implicitly comparing the global epoch with each thread epoch. A tracked memory location (TML) is not reachable when each process its at least 2 epochs in advance in respect to the the TML last protection epoch.

To do this we use a series of 4 buffers (FAA queues) to orchestrate this.

- `Current Bucket` -> bucket where garbage is put
- `Grace Bucket` -> bucket that contains TML that we can't know if they're still reachable
- `Free Bucket` -> bucket that contains TML that aren't reachable
- `Next Bucket` -> bucket for the next epoch
  
All the buckets are bounded (by the total number of TML defined in initialization) and rotate modulo 4 based on the epoch value.

> This means that given an epoch `e` at epoch `e+1` the current bucket at epoch `e` will become the grace bucket and at epoch `e+2` the current bucket at epoch `e` will have become the free bucket.

Processes only touch 2 of these buffers for any given epoch:
- `Current Bucket`: processes can insert there TML that are up for recycling
- `Free Bucket`: process extract TML not reachable for future reuse.

### Advancement Invariant
A strong invariant of this system is that the current epoch `e` can only be advanced when the `Free Bucket` is found empty. Then the free bucket will become the `Next bucket` at epoch `e+1`;

## Thread SWL (Single Writer Locations)
They're used to determine if the current epoch can be advanced.

Each SWL consists in:
```cpp
//thread single writer
struct TSW{
	std::atomic<uint64_t> epoch{0};
	std::atomic<bool> active{false};
}

//thread single writer
struct TSW{
	//used to set protection
	void setActiveAndEpoch(uint64_t epoch) {
		assert(((epoch & MSB_MASK) == 0) && "Epoch MSB cannot be set");
		activeAndEpoch.store((epoch | MSB_MASK, std::memory_order_release);
	}
	
	//used to clear protection
	void clearActive() {
		activeAndEpoch.fetch_and(LSB63_MASK,std::memory_order_release);
	}
	
	//used for strong assertions
	bool isActive {
		(activeAndEpoch.load(std::memory_order_acquire) & MSB_MASK) != 0;
	}
	
	void getActiveAndEpoch(bool& active, uint64_t& epoch) {
		uint64_t current = activeAndEpoch.load(std::memory_order_acquire);
		active = (current & MSB_MASK) != 0;
		epoch  = current & LSB63_MASK;
	}
private:
	static constexpr uint64_t MSB_MASK = 1ull<<63;
	static constexpr uint64_t LSB63_MASK = ~MSB_MASK;
	std::atomic<uint64_t> activeAndEpoch{0};
}
```

> SWLs are to be padded to cache line, we manage this packing them in `HazardCell<thread_storage_,Meta>` that also allows us to store useful metadata (see below) for each individual thread

Notice we don't need to clear the current epoch when clearing protection, the reason will become apparent in the section **Epoch Advancement**.

## Retiring a TML
Retiring a TML, means inserting its related pointer (or a mapped 1:1 index, see below on **ABA prevention**) in the `Current Bucket` for the given epoch.

To ensure retiring doesn't interfere with the system, a retirement can be made only if the calling thread is protecting the current epoch. This prevents epoch advancements while TML the correct bucket is being selected.

```cpp
//assuming
std::atomic<uint64_t> epoch;
HazardCell<TSW> SingleWriter[MAX_THREADS];
Bucket<TML*> Buckets[4];

Bucket<TML*>& CurrentBucket(uint64_t epoch) {
	return Buckets[(epoch) % 4]; //equal to (epoch) & 3;
}


void retire(TML* mem, uint64_t ticket) {
	assert(ticket < MAX_THREADS && "Single Writer index out of range");
	bool active;
	uint64_t epoch;
	SingleWriter[ticket].getActiveAndEpoch(active,epoch);
	if(!active) {
		// STRONG ENFORCEMENT
		// assert(false && "Retire: called without epoch stall");
		
		//note the -1 [prevents the epoch from advancing]
		SingleWriter[ticket].setActiveAndEpoch(true,epoch.load()-1);
	}
	//epoch of the active thread not global
	bool s = CurrentBucket(epoch).enqueue(mem);
	assert(s && "FreeBucket shoudn't be full");
	if(!active) SingleWriter[ticket].clearActive();
}
```

> Provided 2 different versions of `retire`, with one that automatically blocks the epoch from advancing to get a consistent view of the current space. In practice we can expect most `retire` calls to satisfy the strong assertion. This happens because, logically, to decide whether to recycle or keep using a TML, the object must be observed in a specified state, therefore protected before observing.


### Cache Fast-Path
A successful recycle of a TML happens after at most 2 epoch transition. After that the TML is up to reuse. The epoch advancement though is strongly dependant on the process coordination and consensus. A stall of one process (not in all cases) may cause the stall of the entire apparatus.

Since this might happen, using a fast-path cache in order to keep TML acquired from the recycled, but not published to the rest of the processes is preferrable.
> As this comes any TML can be put into cache, and putting incoherent TMLs or arbitrary pointers will result in undefined behaviour. The most likely, that are recorded in the system are cache overflow phoenomena.


## Epoch Protection
With this said let's start by defining in practice how epochs are protected and cleared:
```cpp
//assuming
std::atomic<uint64_t> epoch;
HazardCell<TSW> SingleWriter[MAX_THREADS];

void protectEpoch(uint64_t ticket) {
	assert(ticket < MAX_THREADS && "Single writer index out of range");
	SingleWriter[ticket].setActiveAndEpoch(
		epoch.load(std::memory_order_acquire); //synch with the current epoch
	);
}

void clearEpoch(uint64_t ticket) {
	assert(ticket < MAX_THREADS && "Single writer index out of range");
	SingleWriter[ticket].clearActive();
}

// Update epoch protection until an atomic value stabilizes
// The atomic value should account for ABA
template<T>
T protectEpoch(uint64_t ticket, std::atomic<T>& atom) {
	while(true) {
		//cycle - overwrite protection until the value stabilizes
		protectEpoch(ticket);
		T tmp = atom.load(std::memory_order_acquire);
		if(atom.load(std::memory_order_acquire) == tmp) {
			return tmp;
		}
	}
}
```

> Epoch protection should always be acquired before attempting to load a shared value. This in order to avoid dangling references (references to retired TMLs)


## Epoch Advancement
Epoch advancement is as necessary as delicate. The epoch defines the step of the recycler, and any stall or desynchronization may (will) stall it.

> The epoch is defined as an atomic monotonic counter

Starting by reasoning, an epoch may be advanced if only if all active threads (threads that acquired protection over an epoch) are on the current one. We can discard inactive processes (processes that aren't currently holding protection of the epoch), since at their activation they will sync on the current one.

Considering active threads has to happen because the recycling storage is shared between all threads (to prevent throughput loss). The epoch cannot be advanced if a thread is stuck on a prior epoch.
> Being stuck on a prior epoch logically means that a thread is still working on a TML. If the epoch advances and another thread decides to put that same TML up for recycling, the TML can actually be recycled while the thread is working on it.

The epoch advancement is a best-effort operation, while done strictly on-demand. Epoch advancement, if successful enables a process to reclaim retired TMLs for reuse. 
> The epoch value is cache-hot so we advance it only when necessary

We enforce the on-demand update by hardcoding the advancement in the `reclaim - get` operation, making sure to reclaim all the objects in the current epoch before advancing to the next one.

```cpp
//assuming
/*
	atomic incremented by the retire function and decremented by the 
	reclaim function.
*/
std::atomic<uint64_t> retiredObjects; 
std::atomic<uint64_t> epoch;
Bucket<TML*> Buckets[4];
HazardCell<TSW> SingleWriter[MAX_THREADS];

Bucket<TML*>& FreeBucket(uint64_t epoch) {
	return Buckets[(epoch + 2) % 4];
}

/*
	Returns the number of objects currently retired
	and not yet reclaimed
*/
uint64_t available() {
	return retiredOjects.load(std::memory_order_acquire);
}

void markReclaimed() {
	retiredObjects.fetch_sub(std::memory_order_release);
}

bool canAdvanceEpoch(uint64_t epoch) {
	bool t_active;
	uint64_t t_epoch;
	
	for(size_t i = 0; i < MAX_THREADS; i++) {
	SingleWriter[i].getActiveAndEpoch(t_active,t_epoch);
		if(t_active && (t_epoch != epoch)) {
			return false; //active thread stuck on older epoch
		}
	}
	return true;
}

bool reclaim(uint64_t ticket, TML*& out) {
    assert(ticket < MAX_THREADS && "Single Writer index out of range");
    assert(!SingleWriter[ticket].isActive() && "Reclamation cannot be attempted while active");

	bool got = false;
	
    for(;;) {
	    /*
		    if no available objects then we don't spin
	    */
	    if(!available()) 
		    break;
		/*
			Protect the epoch to get a coherent snapshot
		*/
		uint64_t localEpoch = protectEpoch(ticket,epoch);
		// drain the free bucket
		if(got = FreeBucket(localEpoch).dequeue(out); got) {
			markReclaimed();
			break;
		}
		
		/*
			at this point:
			either Epoch == localEpoch (nobody advanced it)
			or Epoch == localEpoch + 1 (somebody advanced it after we got it)
			
		*/
		
		if(canAdvanceEpoch(localEpoch)) {
			/*
				the current epoch e is protected, so the epoch advancement
				either has already happened (epoch is now e+1) (and cannot
				happen again since e is protected (by us)), or it hasnt happen.
				
				So if CAS = true we're advancing the epoch by one, else 
				somebody already has. Epoch stays consistent
				
			*/
			uint64_t nextEpoch = localEpoch + 1;
			(void)(epoch.compare_exchange_strong(localEpoch, nextEpoch)) 
			
			
			//our epoch is still protected so either this advancement happened
			//or someone advanced before us (no problem we're locking now)
			if(got = FreeBucket(localEpoch).dequeue(out); got) {
				markReclaimed();
				break;
			}
		}
    }
    
    clearEpoch(ticket);
    return got; 
}

```
> The reclaim function spins only until there are TMLs that can be reclaimed. This process can be integrated with more consistent cache lookup which we can consider cheap since they dont impact other processes view of the recycler's epoch.

By protecting the epoch it makes sure that epoch snapshots are consistent.

## Indexing TMLs
Hazard pointers avoid ABA by protecting **individual nodes explicitly**: a thread announces the exact pointer it accesses, preventing that node’s immediate reuse while protected.

Epoch-Based Reclamation (EBR), instead, works at a coarser granularity by protecting **global epochs**, delaying reclamation until all threads move past an epoch. This can still allow ABA within that epoch window because individual pointers are not explicitly protected.
**In short:**
- Hazard pointers prevent ABA by **locking specific pointers against reuse while in use**.
- EBR prevents premature reclamation **only by delaying free operations based on global epoch progress**, not by protecting individual pointers.

This makes hazard pointers stronger against ABA but more complex, while EBR offers simpler, batched protection with some residual ABA risk within epochs.

This said, make a program that interfaces with the Recycler deal with raw pointers is a limitation in several ways:
1. The recycler cannot implement any watchdog guard (i.e. retiring the same item twice or retiring an unregistered item)
2. The recycler doesn't support reallocation of TMLs after construction, can be good but can mine optimal NUMA locality
3. Pointers are exactly 1 word of memory, so dealing with them in CAS operations makes hard to exclude ABA
4. Bucket queues need to specifically handle (virtualize DCAS) for fast access operation due the pointer item requirement, (i.e. packing 32 bit indexes enables us to use single CAS operations while enabling us to track 4 billion TMLs concurrently).

These are the principal reasons while it would be beneficial to introduce a layer of abstraction, that maps a 32bit integer to a pointer that we will refer to as **Access Table**. The full API of the recycler has to be reworked in order to support this new scheme.
> We also suppose the **Access Table** if fairly stable after initialization, meaning that reallocation of TMLs in order to prioritize NUMA locality is supported but infrequent.

```cpp
template<typename T>
struct AccessStorage {
	static_assert(std::is_pointer_t<T>,"AccessStorage requires T to be a pointer");
	T ptr{nullptr};
}
```

> We stick to a minimal version now, to support watchdog for double retire, we should add an `atomic_flag`;


1. `reclaim` -> puts an object up for recycling by addressing its index
2. `recycle` -> returns the specified index of a free TML
3. **new method** -> `T* get_ptr(uint32_t index)` returns the associated translation for a specified index.
4. **rework all queues** to use `uin32_t` instead of pointers. We can adapt the PRQ design to work with `uint64_t` epoch and values, this would semplify a lot the design not needing the `DCAS emulation`. Even tho PRQueues can be prone to livelock the use we do in this recycler excludes this, since whenever we find a queue empty we drop it, to reuse it when the epoch wrap around. 
5. **rework cache** to use `uint32_t` instead of pointers. The design doesn't change much, from the fact that we can store indexes and values in the same cell, we have to also relax the pointer type assertion but shoudn't be a problem.

## Flow
Follows a description of all methods and structures the recycler must implement

### Struct `HeapStorage<T>`
ALternative to `std::vector`, methods to implement are:
- `construction` -> constructs a container for an item of type `T`
> As for right now supports construction and initialization for both trivial and non-trivial types with argument forwarding to the constructor. Most of the times the type `T` reflects the type of cell we want (can be padded so `aligned` to cache line);
- `index access` -> overload
- `index assignment` -> overload
- `destruction` 

### Struct `HazardCell<T,Metadata>`
padded cell to be used as single writer storage, useful when making thread announcement vectors. In this case the struct internally necessitate padding, so it's aligned to cache line (or see `std::hardware_interference_size`). Regarding the padding, we can choose to encode certain types of metadata which still stays per thread.

### Struct `ThreadSW`
Single writer struct for each thread.
> Implementation already above

#### Methods
- `void setActiveEpoch(uint64_t epoch) noexcept`
- `void clearActive() noexcept`
- `bool isActive() const noexcept`
- `void getEpochActive(uint64_t& epoch, bool& active) const noexcept`

> Also add wrappers in the recycler for metadata set & get

## Recycler Methods
- `void protectEpoch(uint64_t ticket)` marks the thread as active on the current global epoch in its TSW
- `T protectEpoch(uint64_t ticket, std::atomic<T>& atom)` repeatily calls protect until an atomic value stabilizes. Returns the value
- `void clearEpoch(uint64_t ticket)` marks the thread as inactive in its TSW
- `bool canAdvanceEpoch(uint64_t epoch) const` checks if the the current epoch can be advanced on the condition that all active threads must reside on the parameter epoch
- `bool getCache(uint32_t& ref)` fast path for getting a clear value from cache. The value got is an `uint32_t` index that can be converted to the type `T` via lookup on the `AccessStorage` 
- `void putCache(const uint32_t value)` puts the index directly in cache. Used on vlaues that are not shared
- `void retire(const uint32 idx, ticket)` puts the specified index up for retirement











