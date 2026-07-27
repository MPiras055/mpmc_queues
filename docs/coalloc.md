#pragma once
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <new>
#include <utility>
#include <cassert>

// ============================================================================
// 1. OWNERSHIP POLICIES
// ============================================================================
// Strategy A: Standard Heap Allocation (For Fixed Queues)
template <typename T>
struct HeapOwner {
static T* allocate(size_t n) { return new T[n]; }
static void deallocate(T* p) { delete[] p; }
};

// Strategy B: No Ownership (For Co-Allocated Segments)
// The buffer is part of the segment's memory block, so we do nothing.
template <typename T>
struct NullOwner {
static T* allocate(size_t) { return nullptr; }
static void deallocate(T*) { /_ No-op: we don't own the buffer _/ }
};

// ============================================================================
// 2. THE CO-ALLOC MIXIN (The Magic)
// ============================================================================
// This handles the complex single-block allocation logic.
// Inherit from this to get ::create() and ::destroy() methods.
template <typename Derived, typename T>
class CoAlloc {
public:
// Factory Method: Allocates Header + Buffer in one contiguous block
static Derived* create(size_t capacity, uint64_t start_seq = 0) {
// 1. Calculate Layout
// Round up header size to ensure proper alignment for T[]
constexpr size_t header_size = (sizeof(Derived) + alignof(T) - 1) & ~(alignof(T) - 1);
size_t total_bytes = header_size + (capacity * sizeof(T));

        // 2. Allocate Raw Memory
        // Ensure the whole block is aligned to the stricter of the two types
        constexpr size_t block_align = alignof(Derived) > alignof(T) ? alignof(Derived) : alignof(T);
        void* raw_mem = std::aligned_alloc(block_align, total_bytes);
        if (!raw_mem) throw std::bad_alloc();

        // 3. Calculate Buffer Start Address
        T* buffer_ptr = reinterpret_cast<T*>(static_cast<char*>(raw_mem) + header_size);

        // 4. Construct the Object in-place
        // We pass the calculated buffer_ptr to the constructor
        return new (raw_mem) Derived(capacity, start_seq, buffer_ptr);
    }

    // Custom Destroyer
    static void destroy(Derived* ptr) {
        if (!ptr) return;
        ptr->~Derived();  // 1. Call Destructor manually
        std::free(ptr);   // 2. Free the single block
    }

};

// ============================================================================
// 3. THE FIXED SEGMENT BASE (Zero Pollution)
// ============================================================================
// Defines the Logic. Uses Policy to handle memory.
// Note: No 'bool owns*buffer' flag. No runtime overhead.
template <typename T, typename OwnerPolicy = HeapOwner<T>>
class FixedSegment {
protected:
// Core Queue State
std::atomic<uint64_t> head*{0};
std::atomic<uint64*t> tail*{0};
T\* buffer*;  
 const size_t capacity*;

public:
// Constructor 1: Standard (Allocates its own buffer)
FixedSegment(size*t size, uint64_t start = 0)
: head*(start), tail*(start)
, buffer*(OwnerPolicy::allocate(size)) // Policy decides allocation
, capacity\_(size)
{
// If OwnerPolicy::allocate returned nullptr (e.g. NullOwner),
// we expect the Derived class to have handled it (see Constructor 2).
}

    // Constructor 2: Injection (Accepts external buffer)
    // Used by Co-Allocated segments to inject their memory.
    FixedSegment(size_t size, uint64_t start, T* injected_buffer)
        : head_(start), tail_(start)
        , buffer_(injected_buffer)
        , capacity_(size)
    {}

    virtual ~FixedSegment() {
        // Policy handles cleanup.
        // For NullOwner, this compiles to literally nothing.
        OwnerPolicy::deallocate(buffer_);
    }

    // --- LOGIC (Shared by all) ---
    bool enqueue(T val) {
        // ... CAS Loop ...
        // Uses buffer_[index]
        return true;
    }

};

// ============================================================================
// 4. THE OPTIMIZED LINKED SEGMENT
// ============================================================================
// Inherits Logic from FixedSegment.
// Inherits Factory from CoAlloc.
class LinkedSegment :
public FixedSegment<int, NullOwner<int>>, // <--- Logic (No Ownership)
public CoAlloc<LinkedSegment, int> // <--- Factory
{
// Allow CoAlloc to access our private constructor
friend class CoAlloc<LinkedSegment, int>;

    std::atomic<LinkedSegment*> next_{nullptr};

    // Private Constructor
    // Receives the 'buffer_ptr' calculated by CoAlloc
    LinkedSegment(size_t size, uint64_t start, int* buffer_ptr)
        : FixedSegment(size, start, buffer_ptr) // Inject into Base
    {
        // Initialize Linked-specific state
    }

public:
// Destructor is default.
// Base class destructor does nothing (NullOwner).
// CoAlloc::destroy handles the 'free'.
~LinkedSegment() override = default;

    void link_next(LinkedSegment* next) {
        next_.store(next);
    }

};

// ============================================================================
// 5. USAGE EXAMPLE
// ============================================================================
/\*
int main() {
// 1. Standard Fixed Queue
// Uses HeapOwner (default). Allocates buffer internally via 'new'.
FixedSegment<int> simple_queue(1024);
simple_queue.enqueue(42);

    // 2. Optimized Linked Segment
    // Uses CoAlloc to create a single contiguous block of memory.
    // FixedSegment acts as a "View" over this memory.
    auto* segment = LinkedSegment::create(1024);

    segment->enqueue(100);

    // Cleanup
    LinkedSegment::destroy(segment);

}
\*/
