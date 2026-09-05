#pragma once
/**
 * @file Adapters.hpp
 * @brief Bridges that let a queue with a non-standard interface join the benchmark harness.
 * @ingroup registry
 */

#include <algo/LFring.hpp>
#include <mem/SingleBlock.hpp>
#include <meta/OptionsPack.hpp>
#include <bit>
#include <cstddef>
#include <cstdint>

namespace queue {

/**
 * @brief `algo::LFring` behind the pointer-shaped interface the benchmark harness expects.
 *
 * Exists for one measurement: **CAS1 against emulated CAS2**, LFring's single-word
 * compare-exchange against PSCQ's three-step 128-bit transaction, at identical ring geometry.
 * Both allocate `2n` cells for `n` items and both report the same `capacity()`, so the only
 * thing that differs is the cell protocol.
 *
 * ### What this adapter does *not* do
 *
 * It does not transport a payload, because LFring cannot. `LFring::enqueue` folds the value into
 * the cell with `tcycle ^ item`, where the cycle counter occupies everything above
 * `log2(n)` bits -- so a value only survives the round trip while it stays inside `[0, n)`.
 * LFring is an *index* ring: `algo::SCQ` is what it looks like with payload transport added back,
 * which is why "PSCQ vs SCQ" is a separate comparison from this one.
 *
 * The harness therefore feeds **both** queues in this comparison the same even values drawn from
 * `[2, n)` (see the `MPMC_BENCH_CAS` branch of `Benchmark::encode`). Even, because PSCQ reserves
 * the top bit and the tag schemes reserve the low encodings; and repeats across laps are
 * harmless because both algorithms are ABA-free, which is the property that makes a small
 * recycled value space legitimate here.
 *
 * Read the resulting numbers as "cost of the cell protocol at equal geometry", never as
 * end-to-end throughput of a queue carrying user data.
 */
template <typename T, typename Opt = meta::EmptyOptions>
class LFringQueue : public mem::SingleBlock<LFringQueue<T, Opt>> {
    using Self = LFringQueue<T, Opt>;
    using Ring = algo::LFring<Opt, linkage::None>;
    using ring_cell = typename Ring::cell_type;

public:
    using cell_type = T;

    /// @return the capacity a queue built with @p n will report; matches PSCQ's rounding.
    static constexpr std::size_t capacity_for(std::size_t n) noexcept {
        return Ring::virtual_size(Ring::order_for(n));
    }

    /// @brief Where the co-allocated regions go. See @ref block-construction.
    /// Laid out the way algo::SCQ places its rings: a region for the header, one for the cells.
    static constexpr auto plan(std::size_t n) noexcept {
        mem::LayoutBuilder b{sizeof(Self), alignof(Self)};
        mem::Plan<2> p{};
        p.regions[0] = b.add(sizeof(Ring), alignof(Ring));
        p.regions[1] = b.add(Ring::cells_for(Ring::order_for(n)) * sizeof(ring_cell),
                             alignof(ring_cell));
        p.total = b.total();
        p.block_align = b.block_align();
        return p;
    }

    LFringQueue(std::size_t n, mem::Blocks blk) noexcept {
        const auto p = plan(n);
        ring_ = ::new (blk.template at<void>(p.regions[0]))
            Ring(Ring::order_for(n), blk.template at<ring_cell>(p.regions[1]), /*init_full=*/false);
    }

    LFringQueue(const LFringQueue&) = delete;
    LFringQueue& operator=(const LFringQueue&) = delete;

    /// @brief Add an item.
    /// @return false if the ring is full.
    /// @pre @p item is an even value inside `[2, 2 * capacity())`; see the class note. A value
    ///      outside that range does not round-trip -- the high bits belong to the cycle counter.
    bool try_enqueue(T item) noexcept {
        const auto raw = static_cast<std::size_t>(std::bit_cast<std::uintptr_t>(item));
        assert(raw < 2 * capacity() && "LFringQueue: value outside the index range");
        return ring_->enqueue(raw);
    }
    bool enqueue(T item) noexcept { return try_enqueue(item); }

    /// @brief Take the oldest item.
    /// @return false if the ring is empty.
    bool try_dequeue(T& out) noexcept {
        std::size_t v = 0;
        if (!ring_->dequeue(v)) return false;
        out = std::bit_cast<T>(static_cast<std::uintptr_t>(v));
        return true;
    }
    bool dequeue(T& out) noexcept { return try_dequeue(out); }

    std::size_t size() const noexcept { return ring_->size(); }
    std::size_t capacity() const noexcept { return ring_->capacity(); }

private:
    Ring* ring_;
};

} // namespace queue
