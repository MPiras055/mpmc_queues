#pragma once

extern "C" {
#include "lfring.h"
#include "lfring_cas1.h"
}

#pragma once
#include <cassert>
#include <specs.hpp>            //  padding and compatibility def
#include <IQueue.hpp>           //  base queue interface
#include <ILinkedSegment.hpp>   //  base linked segment interface
#include <SequencedCell.hpp>    //  cell definition
#include <bit.hpp>              //  bit manipulation utilities
#include <OptionsPack.hpp>      //  options

namespace queue {

struct SCQOption {
    struct DisableAutoClose{};
};

template<typename T, typename Opt = meta::EmptyOptions, typename Derived = void>
class SCQueue: public base::IQueue<T> {
    //SCQueue doesn't require pointer items
    using Effective = std::conditional_t<std::is_void_v<Derived>,SCQueue,Derived>;
    static constexpr bool AUTO_CLOSE = !Opt::template has<SCQOption::DisableAutoClose> &&
        base::is_linked_segment_v<Effective>;

protected:
    struct alignas(CACHE_LINE) Legacy {
        const size_t scq_order; //size power of 2
        char *aq_;         //acquired slots
        char *fq_;         //free slots
        T* underlying;    //underlying storage
        CACHE_PAD_TYPES(char*,char*,T*,size_t);

        template<typename V>
        static V* init_storage(size_t size) {
            assert(size != 0 && "Legacy Init Storage: Size must be non-null");
            size_t bytes = sizeof(T) * size;
            if(bytes % CACHE_LINE != 0)
                bytes += CACHE_LINE - (bytes % CACHE_LINE);
            V* buffer = static_cast<V*>(std::aligned_alloc(CACHE_LINE,bytes));
            assert(buffer != nullptr && "Failed aligned_alloc");
            return buffer;
        }

        inline struct lfring* aq() const {
            return reinterpret_cast<struct lfring*>(aq_);
        }

        inline struct lfring* fq() const {
            return reinterpret_cast<struct lfring*>(fq_);
        }

        Legacy() = delete;

        explicit Legacy(size_t size):
            scq_order{bit::log2(size) >= LFRING_MIN_ORDER? bit::log2(size) : 0},
            aq_{init_storage<char>(LFRING_SIZE(scq_order))},
            fq_{init_storage<char>(LFRING_SIZE(scq_order))},
            underlying{init_storage<T>(1u << scq_order)}
        {
            assert(bit::log2(size) >= LFRING_MIN_ORDER && "Size < LFRING_MIN_ORDER");
            lfring_init_empty(aq(), scq_order);
            lfring_init_full(fq(),  scq_order);
        }

        ~Legacy() {
            if(aq_ != nullptr) std::free(aq_);
            if(fq_ != nullptr) std::free(fq_);
            if(underlying != nullptr) std::free(underlying);
        }

        // 1. Delete Copy Constructor and Assignment to prevent double-frees
        Legacy(const Legacy&) = delete;
        Legacy& operator=(const Legacy&) = delete;

        // 2. Implement Move Constructor to transfer ownership
        Legacy(Legacy&& other) noexcept
            : aq_(other.aq_)
            , fq_(other.fq_)
            , underlying(other.underlying)
            , scq_order(other.scq_order)
        {
            // Null out the source pointers so the other destructor doesn't free them
            other.aq_ = nullptr;
            other.fq_ = nullptr;
            other.underlying = nullptr;
        }

        // 3. Delete Move Assignment (optional, but required here because scq_order is const)
        Legacy& operator=(Legacy&&) = delete;
    };

public:

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


    explicit SCQueue(size_t size, uint64_t start = 0):
        lf(size),
        offset(start) {};

    explicit SCQueue(T item, size_t size, uint64_t start = 0): lf(size), offset(start) {
        enqueue(item);
    };

    size_t offset;
    Legacy lf;

    size_t capacity() const noexcept override {
        return 1u << lf.scq_order;
    }

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
    friend Base;
    friend Proxy;   ///< Proxy class can access private methods

    //proxies require segments to be on AutoClose mode
    static_assert(!Opt::template has<SCQOption::DisableAutoClose>,"LinkedSCQ: AutoClose disabled");

public:
    static constexpr bool info_required = true;
    /**
     * @brief Constructs a linked SCQ segment.
     *
     * @param size Capacity of this segment.
     * @param start Initial sequence number (defaults to 0).
     */
    LinkedSCQ(size_t size, uint64_t start = 0): Base(size,start) {}
    LinkedSCQ(T item, size_t size, uint64_t start = 0): Base(item,size,start) {}

    /// @brief Defaulted destructor.
    ~LinkedSCQ() override = default;

private:
    inline Next getNext() const noexcept override {
        return next_.load(std::memory_order_acquire);
    }

    static inline bool is_closed_(uint64_t val) {
        uint64_t mask = ~__LFRING_CLOSED;
        //check if the closed bit is set
        return !(val & mask);
    }

    inline bool isClosed() const noexcept final override {
        return is_closed_(lfring_get_tail(Base::lf.fq()));
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
        return Base::enqueue(item);
    }

    /// @brief dequeue with additional info
    inline bool dequeue(T& item, [[maybe_unused]] bool info = true) noexcept final override {
        return Base::dequeue(item);
    }

    /// @brief Reset threshold after observing a new queue linked
    void prepareDequeueAfterNextLinked() {
        lfring_reset_threshold(Base::lf.aq(), Base::lf.scq_order);
    }

    ALIGNED_CACHE std::atomic<Next> next_{};
    CACHE_PAD_TYPES(std::atomic<Next>);
};


}   //namespace segment

}   //namespace queue
