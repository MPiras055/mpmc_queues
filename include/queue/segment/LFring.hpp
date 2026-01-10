#pragma once
#include <IQueue.hpp>
#include <bit.hpp>
#include <type_traits>
#include <cstdlib>
#include <cassert>

extern "C" {
#include "lfring.h"
#include "lfring_cas1.h"
}

/**
* (Zero cost) compatibility layer for lfring direct usage
* compliant with IQueue interface
*
*
*/
namespace queue {
template<typename T>
class LFring: public base::IQueue<T> {
    public:
    static_assert(std::is_convertible_v<T,size_t>, "T must be convertible to size_t");
    using lfring_legacy = struct lfring;

    /**
     * Computes the size in bytes of s LFring
     * @returns true if the provided size is
     */
    static size_t size_in_bytes(size_t initial_size) {
        return LFRING_SIZE(order_(initial_size));
    }

    LFring(size_t size):
        scq_order{order_(size)},
        ring_{reinterpret_cast<lfring_legacy*>(new char[LFRING_SIZE(scq_order)])}
    {
        lfring_init_empty(ring_, scq_order);
    }

    ~LFring() {
        delete[] reinterpret_cast<char*>(ring_);
    }

    bool enqueue(size_t item) noexcept final override {
        assert(bit::keep_high<uint32_t>(item) == 0);
        return lfring_enqueue(ring_,scq_order,item,false);
    }

    bool dequeue(size_t& item) noexcept final override {
        size_t i = lfring_dequeue(ring_,scq_order,false);
        if(i == LFRING_EMPTY) return false;
        item = i;
        return true;
    }

    size_t capacity() const noexcept final override {
        return LFRING_SIZE(scq_order);
    }

    private:
    static size_t order_(size_t s) {
        size_t o = bit::log2(s);
        return o >= LFRING_MIN_ORDER?
            o : LFRING_MIN_ORDER;
    }
    const size_t scq_order;
    lfring_legacy *ring_;
};


} //namespace queue
