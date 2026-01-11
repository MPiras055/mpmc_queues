//

#pragma once

extern "C" {
#include "lfring.h"
#include "lfring_cas1.h"
}

#include <cassert>
#include <cstdlib>      // aligned_alloc, free
#include <new>          // placement new
#include <specs.hpp>
#include <IQueue.hpp>
#include <ILinkedSegment.hpp>
#include <SequencedCell.hpp>
#include <bit.hpp>
#include <OptionsPack.hpp>

namespace queue {

struct SCQOption {
    struct DisableAutoClose{};
};

template<typename T, typename Opt = meta::EmptyOptions, typename Derived = void>
class SCQueue: public base::IQueue<T> {
    using Effective = std::conditional_t<std::is_void_v<Derived>,SCQueue,Derived>;
    static constexpr bool AUTO_CLOSE = !Opt::template has<SCQOption::DisableAutoClose> &&
        base::is_linked_segment_v<Effective>;

protected:
    // =========================================================================
    // LEGACY STRUCT (Internal Memory Manager)
    // =========================================================================
    struct alignas(CACHE_LINE) Legacy {
        const size_t scq_order;

        // Pointers into the allocated block
        char *aq_;          // Acquired slots ring
        char *fq_;          // Free slots ring
        T* underlying;      // Data buffer

        // If true, this struct allocated the block and must free it.
        // If false, the block is part of a larger allocation (LinkedSCQ) and we don't own it.
        bool owns_memory_;
        void* memory_block_; // Pointer to the start of the allocated block

        CACHE_PAD_TYPES(char*,char*,T*,size_t, bool, void*);

        // --- Helper to calculate size and offsets ---
        static size_t align_size(size_t s) {
            if(s % CACHE_LINE != 0) return s + CACHE_LINE - (s % CACHE_LINE);
            return s;
        }

        static size_t ring_bytes(size_t order) {
            return align_size(LFRING_SIZE(order));
        }

        static size_t buffer_bytes(size_t order) {
            return align_size(sizeof(T) * (1ull << order));
        }

    public:
        // Helper to calculate total bytes needed for the rings + buffer
        static size_t total_bytes_needed(size_t order) {
            // 2 Rings + 1 Data Buffer
            return (ring_bytes(order) * 2) + buffer_bytes(order);
        }

        // --- Layout Initializer ---
        // Sets up aq_, fq_, underlying based on a base pointer
        void setup_pointers(void* base, size_t order) {
            char* ptr = static_cast<char*>(base);
            size_t r_sz = ring_bytes(order);

            aq_ = ptr;
            ptr += r_sz;

            fq_ = ptr;
            ptr += r_sz;

            underlying = reinterpret_cast<T*>(ptr);

            // Initialize the C-rings
            lfring_init_empty(aq(), order);
            lfring_init_full(fq(),  order);
        }

        inline struct lfring* aq() const { return reinterpret_cast<struct lfring*>(aq_); }
        inline struct lfring* fq() const { return reinterpret_cast<struct lfring*>(fq_); }

        Legacy() = delete;

        // 1. STANDARD CONSTRUCTOR (Allocates its own single block)
        explicit Legacy(size_t size):
            scq_order{bit::log2(size) >= LFRING_MIN_ORDER? bit::log2(size) : LFRING_MIN_ORDER},
            owns_memory_{true}
        {
            assert(bit::log2(size) >= LFRING_MIN_ORDER && "Size < LFRING_MIN_ORDER");

            size_t bytes = total_bytes_needed(scq_order);
            memory_block_ = std::aligned_alloc(CACHE_LINE, bytes);
            if(!memory_block_) throw std::bad_alloc();

            setup_pointers(memory_block_, scq_order);
        }

        // 2. INJECTED CONSTRUCTOR (Uses external block from LinkedSCQ::create)
        Legacy(size_t size, void* external_block) :
             scq_order{bit::log2(size) >= LFRING_MIN_ORDER? bit::log2(size) : LFRING_MIN_ORDER},
             owns_memory_{false},
             memory_block_{external_block}
        {
            assert(external_block != nullptr);
            setup_pointers(memory_block_, scq_order);
        }

        ~Legacy() {
            if(owns_memory_ && memory_block_) {
                std::free(memory_block_);
            }
        }

        // Move Semantics
        Legacy(Legacy&& other) noexcept
            : scq_order(other.scq_order)
            , aq_(other.aq_), fq_(other.fq_), underlying(other.underlying)
            , owns_memory_(other.owns_memory_), memory_block_(other.memory_block_)
        {
            other.owns_memory_ = false;
            other.memory_block_ = nullptr;
            other.aq_ = nullptr;
            other.fq_ = nullptr;
            other.underlying = nullptr;
        }

        Legacy(const Legacy&) = delete;
        Legacy& operator=(const Legacy&) = delete;
        Legacy& operator=(Legacy&&) = delete;
    };

public:
    // ... [Enqueue / Dequeue logic UNCHANGED] ...

    bool enqueue(T item) noexcept override {
        size_t eidx = lfring_dequeue(lf.fq(), lf.scq_order, false);
        if (eidx == LFRING_EMPTY) {
            if constexpr (AUTO_CLOSE) {
                lfring_close(lf.aq());
            }
            return false;
        }

        lf.underlying[eidx] = item;
        if (lfring_enqueue(lf.aq(), lf.scq_order, eidx, false))
            return true;

        (void) lfring_enqueue(lf.fq(), lf.scq_order, eidx, false);
        return false;
    }

    bool dequeue(T& out) noexcept override {
        size_t eidx = lfring_dequeue(lf.aq(), lf.scq_order, false);
        if (eidx == LFRING_EMPTY)
            return false;
        T val = lf.underlying[eidx];
        lfring_enqueue(lf.fq(), lf.scq_order, eidx, false);
        out = val;
        return true;
    }

    // Standard Constructor
    explicit SCQueue(size_t size, uint64_t start = 0):
        lf(size), offset(start) {};

    // Standard with Item
    explicit SCQueue(T item, size_t size, uint64_t start = 0): lf(size), offset(start) {
        enqueue(item);
    };

protected:
    // Protected Constructor for Co-Allocation
    SCQueue(size_t size, uint64_t start, void* external_buffer) :
        lf(size, external_buffer), offset(start)
    {}

public:
    Legacy lf;
    size_t offset;

    size_t capacity() const noexcept override { return 1u << lf.scq_order; }

    size_t size() const noexcept override {
        uint64_t h = lfring_get_head(lf.aq());
        uint64_t t = lfring_get_tail(lf.aq());
        return h > t? 0 : t - h;
    }
};

namespace segment {

template<typename T, typename Proxy, typename Opt = meta::EmptyOptions, typename NextT = void>
class LinkedSCQ:
    public SCQueue<
        T,Opt,LinkedSCQ<T,Proxy,Opt,NextT>
    >,
    public base::ILinkedSegment<
        T,std::conditional_t<
            std::is_void_v<NextT>,
            LinkedSCQ<T,Proxy,Opt,NextT>*,
            NextT
        >
    >
{
    using Base = SCQueue<T,Opt,LinkedSCQ<T,Proxy,Opt,NextT>>;
    using Next = std::conditional_t<std::is_void_v<NextT>,LinkedSCQ<T,Proxy,Opt,NextT>*,NextT>;
    static constexpr bool optimized_alloc = true;
    friend Base;
    friend Proxy;

    static_assert(!Opt::template has<SCQOption::DisableAutoClose>,"LinkedSCQ: AutoClose disabled");

    // =========================================================================
    // CO-ALLOCATION MECHANICS
    // =========================================================================
    struct CoAllocTag {};

    static void* compute_buffer_addr(void* self) {
        // Calculate where the arrays start: Immediately after this object
        // NOTE: We must ensure this offset respects alignment.
        // aligned_alloc guarantees the base is aligned.
        // sizeof(LinkedSCQ) should be padded to alignment by the compiler if struct alignas is used.
        // To be safe, we manually align the offset calculation.

        uintptr_t addr = reinterpret_cast<uintptr_t>(self) + sizeof(LinkedSCQ);
        if(addr % CACHE_LINE != 0) {
            addr += CACHE_LINE - (addr % CACHE_LINE);
        }
        return reinterpret_cast<void*>(addr);
    }

public:
    static constexpr bool info_required = true;

    // =========================================================================
    // FACTORY METHOD
    // =========================================================================
    static LinkedSCQ* create(size_t s, uint64_t start = 0) {
        // 1. Calculate Order
        size_t order = bit::log2(s) >= LFRING_MIN_ORDER ? bit::log2(s) : LFRING_MIN_ORDER;

        // 2. Calculate Header Size (Aligned)
        size_t header_size = sizeof(LinkedSCQ);
        if(header_size % CACHE_LINE != 0) header_size += CACHE_LINE - (header_size % CACHE_LINE);

        // 3. Calculate Payload Size
        size_t legacy_payload_size = Base::Legacy::total_bytes_needed(order);

        size_t total_bytes = header_size + legacy_payload_size;

        // 4. Allocate
        void* mem = std::aligned_alloc(alignof(LinkedSCQ), total_bytes);
        if(!mem) throw std::bad_alloc();

        // 5. Construct
        return ::new (mem) LinkedSCQ(CoAllocTag{}, s, start);
    }

    // =========================================================================
    // MEMORY OPERATORS
    // =========================================================================
    static void operator delete(void* ptr) { std::free(ptr); }
    static void* operator new(size_t size) {
        void* ptr = std::aligned_alloc(alignof(LinkedSCQ), size);
        if(!ptr) throw std::bad_alloc();
        return ptr;
    }

    // =========================================================================
    // CONSTRUCTORS
    // =========================================================================

    // Standard
    LinkedSCQ(size_t size, uint64_t start = 0): Base(size,start) {}
    LinkedSCQ(T item, size_t size, uint64_t start = 0): Base(item,size,start) {}

    ~LinkedSCQ() override = default;

private:
    // Optimized
    LinkedSCQ(CoAllocTag, size_t size, uint64_t start)
        : Base(size, start, compute_buffer_addr(this))
    {}

public:
    // ... [Linked Segment Logic UNCHANGED] ...

    inline Next getNext() const noexcept override {
        return next_.load(std::memory_order_acquire);
    }

    inline bool isClosed() const noexcept final override {
        return lfring_is_closed(Base::lf.fq());
    }

    inline bool isOpened() const noexcept final override {
        return !isClosed();
    }

    inline bool close() noexcept final override {
        lfring_close(Base::lf.fq());
        return true;
    }

    inline bool open() noexcept final override {
        lfring_open(Base::lf.fq());
        lfring_reset_threshold(Base::lf.fq(),Base::lf.scq_order);
        return true;
    }

    inline bool enqueue(T item, [[maybe_unused]] bool info = false) noexcept final override {
        return info && isClosed()? false : Base::enqueue(item);
    }

    inline bool dequeue(T& item, [[maybe_unused]] bool info = true) noexcept final override {
        return Base::dequeue(item);
    }

    void prepareDequeueAfterNextLinked() {
        lfring_reset_threshold(Base::lf.aq(), Base::lf.scq_order);
    }

    ALIGNED_CACHE std::atomic<Next> next_{};
    CACHE_PAD_TYPES(std::atomic<Next>);
};

} //namespace segment
} //namespace queue
