#pragma once
/**
 * @file VyukovNoABA.hpp
 * @brief Vyukov's ring with the lap folded into the empty cell, removing the separate sequence word.
 * @ingroup algo
 */

#include <cell/PlainCell.hpp>
#include <core/SegmentTraits.hpp>
#include <linkage/Linkage.hpp>
#include <mem/SingleBlock.hpp>
#include <meta/OptionsPack.hpp>
#include <util/bit.hpp>
#include <util/align.hpp>
#include <util/specs.hpp>
#include <atomic>
#include <cassert>

namespace algo {

struct VyukovNoABAOpt {
    /**
     * @brief Take the capacity exactly as asked instead of rounding up to a power of two.
     *
     * Rounding is the default so that every algorithm answers a capacity request the same way.
     * Opting out is more expensive here than elsewhere: this ring derives the *lap number* from
     * the ticket as well as the index, so the modulo path pays a division **and** a quotient per
     * operation where the default pays a mask and a shift. A comparator run with this set is no
     * longer measuring the same thing as one without it.
     */
    struct no_pow2 {};
    struct no_cell_padding {};
};

/**
 * @brief Vyukov's ring with the lap number folded into the empty cell itself.
 *
 * A benchmark comparator, not a segment.
 *
 * The plain Vyukov ring needs a separate sequence word per cell. This variant exploits
 * the fact that a valid user-space pointer has both its top and bottom bits clear: a
 * word with bit 63 *and* bit 0 set can never be a payload, so an empty cell can carry
 * its lap number in the middle 62 bits and the cell collapses to a single word.
 *
 * The trade is that enqueue and dequeue become plain single-word CAS, which on x86_64 is
 * **not** ABA-safe in general -- the lap number in the cell is what closes that hole. It
 * is the comparison point for VyukovDCAS, which instead pays for a real double-width CAS.
 */
template <typename T, typename Opt = meta::EmptyOptions, typename Link = linkage::None>
    requires meta::AcceptsOnly<Opt, typename VyukovNoABAOpt::no_cell_padding, typename VyukovNoABAOpt::no_pow2>
class VyukovNoABA : public mem::SingleBlock<VyukovNoABA<T, Opt, Link>> {
    using Self = VyukovNoABA<T, Opt, Link>;

    static_assert(sizeof(T) == sizeof(uintptr_t), "VyukovNoABA: T must be pointer-sized");

    static constexpr bool pad_cells = !Opt::template has<typename VyukovNoABAOpt::no_cell_padding>;

    /// Both ends set: unreachable for any real pointer, so it marks a non-payload word.
    static constexpr uintptr_t kMask = bit::set_msb<uintptr_t>(0) | uintptr_t{1};

    static constexpr uintptr_t lap_word(uint64_t lap) noexcept {
        return (static_cast<uintptr_t>(lap) << 1) | kMask;
    }
    static constexpr bool is_lap_word(uintptr_t w) noexcept { return (w & kMask) == kMask; }

    /// Index mapping is a mask and the lap a shift, rather than a division; see
    /// VyukovNoABAOpt::no_pow2.
    static constexpr bool pow2 = !Opt::template has<typename VyukovNoABAOpt::no_pow2>;

public:
    using cell_type = cell::PlainCell<uintptr_t, pad_cells>;
    using link_state = typename Link::template state<Self>;
    using handle_type = typename link_state::handle;

    static constexpr std::size_t round_size(std::size_t n) noexcept {
        // Floored at 2: at capacity 1 the lap number changes on every ticket, so the ring can
        // never distinguish a full cell from a stale one.
        const std::size_t f = n < 2 ? 2 : n;
        if constexpr (pow2) return bit::round_to_next_pow2(f);
        else return f;
    }

    /// @return the capacity a queue built with @p n will report.
    static constexpr std::size_t capacity_for(std::size_t n) noexcept { return round_size(n); }

    /// @brief Where the co-allocated regions go. See @ref block-construction.
    /// @param n requested capacity; the only thing the layout may depend on.
    static constexpr auto plan(std::size_t n) noexcept {
        mem::LayoutBuilder b{sizeof(Self), alignof(Self)};
        mem::Plan<1> p{};
        p.regions[0] = b.add(round_size(n) * sizeof(cell_type), alignof(cell_type));
        p.total = b.total();
        p.block_align = b.block_align();
        return p;
    }

    VyukovNoABA(std::size_t n, mem::Blocks blk) noexcept
        : capacity_{round_size(n)}, shift_{pow2 ? bit::log2(round_size(n)) : 0},
          cells_{blk.template at<cell_type>(plan(n).regions[0])} {
        for (std::size_t i = 0; i < capacity_; ++i)
            cells_[i].val.store(lap_word(lap(i)), std::memory_order_relaxed);
    }

    /// @brief Add an item, skipping the attempt when the caller already knows it is closed.
    /// @param closed_hint the caller believes this segment is closed; see
    ///        core::segment_traits::needs_close_hint.
    bool enqueue(T item, bool /*closed_hint*/) noexcept { return enqueue(item); }

    /// @brief Add an item.
    /// @return false if the queue is full, or closed.
    bool enqueue(T item) noexcept {
        assert(!is_lap_word(reinterpret_cast<uintptr_t>(item)) &&
               "VyukovNoABA: item collides with the reserved encoding");
        for (;;) {
            const uint64_t raw = tail_.load(std::memory_order_relaxed);
            if constexpr (Link::is_linked) {
                if (is_closed_ticket(raw)) return false;
            }
            const uint64_t t = clean(raw);
            const uint64_t h = head_.load(std::memory_order_acquire);
            if (t == h + capacity_) {
                // Ring is full. A linked segment closes itself so the proxy stops retrying
                // here and links a successor instead; see algo::Vyukov::enqueue.
                if constexpr (Link::is_linked) close();
                return false;
            }

            // Reserve the ticket, then publish. Two things depend on the reservation being
            // mandatory rather than the best-effort advance this used to do:
            //
            //  * close() sets the top bit of the tail, so this CAS is what it invalidates.
            //    Without that the cell CAS below decided success on its own, and a producer
            //    could publish into a segment the proxy had already drained and retired.
            //  * The tail used to advance even when the cell CAS *failed*, so a ticket could
            //    be skipped and its cell left unfilled behind a head that had moved past it.
            uint64_t expect_tail = raw;
            if (!tail_.compare_exchange_weak(expect_tail, raw + 1, std::memory_order_acq_rel,
                                             std::memory_order_acquire))
                continue;

            // Ticket t is ours alone, and the full check above means the cell has been
            // consumed, so it carries exactly the lap word this ticket expects.
            uintptr_t expect = lap_word(lap(t));
            const bool won = cells_[mod(t)].val.compare_exchange_strong(
                expect, reinterpret_cast<uintptr_t>(item), std::memory_order_acq_rel,
                std::memory_order_relaxed);
            // Returned rather than asserted-and-ignored: with NDEBUG an assert compiles away,
            // and reporting success without having stored anything would lose the item
            // silently. It should be unreachable -- the ticket is ours and the full check
            // guarantees the cell has been consumed back to this lap.
            assert(won && "VyukovNoABA: a reserved cell was not at its expected lap");
            return won;
        }
    }

    /// @brief Take the oldest item.
    /// @return false if the queue is empty.
    bool dequeue(T& out) noexcept {
        for (;;) {
            // clean(): a closed segment carries the marker in its tail, and consumers must
            // still be able to compare it against the head to answer "empty". Reading it raw
            // makes `t == h` unreachable and the consumer spins for ever.
            const uint64_t t = clean(tail_.load(std::memory_order_relaxed));
            uint64_t h = head_.load(std::memory_order_relaxed);
            cell_type& c = cells_[mod(h)];
            uintptr_t val = c.val.load(std::memory_order_acquire);
            if (h != head_.load(std::memory_order_acquire)) continue;
            if (t == h) return false; // empty

            const uintptr_t next_lap = lap_word(lap(h) + 1);
            if (is_lap_word(val)) { // already taken; help the head along
                if (val == next_lap) {
                    uint64_t hh = h;
                    (void)head_.compare_exchange_weak(hh, h + 1, std::memory_order_relaxed);
                }
                continue;
            }

            const bool won = c.val.compare_exchange_weak(val, next_lap, std::memory_order_acq_rel,
                                                         std::memory_order_relaxed);
            uint64_t hh = h;
            (void)head_.compare_exchange_weak(hh, h + 1, std::memory_order_relaxed);
            if (won) {
                out = reinterpret_cast<T>(val);
                return true;
            }
        }
    }


    /// @copydoc core::Queue::try_enqueue
    /// Never blocks, so this is the same operation as enqueue().
    bool try_enqueue(T item) noexcept { return enqueue(item); }
    /// @copydoc core::Queue::try_dequeue
    /// Never blocks, so this is the same operation as dequeue().
    bool try_dequeue(T& out) noexcept { return dequeue(out); }

    /// @return Items currently held. Approximate under concurrency, exact when quiescent.
    std::size_t size() const noexcept {
        const uint64_t t = clean(tail_.load(std::memory_order_acquire));
        const uint64_t h = head_.load(std::memory_order_acquire);
        return t < h ? 0 : static_cast<std::size_t>(t - h);
    }

    /// @return Items this queue can hold.
    std::size_t capacity() const noexcept { return capacity_; }

    // -- linkage (only when linked) -----------------------------------------

    /**
     * @brief Refuse all further enqueues, permanently.
     *
     * The marker rides in the top bit of the tail, as PRQ and Vyukov do, so it invalidates
     * every outstanding reservation rather than merely being visible to whoever reads it next.
     * The lap number is derived from the ticket, so every arithmetic use goes through clean().
     */
    void close() noexcept
        requires(Link::is_linked)
    {
        tail_.fetch_or(bit::set_msb<uint64_t>(0), std::memory_order_release);
    }

    /// @return true once closed; a closed segment still drains.
    bool is_closed() const noexcept
        requires(Link::is_linked)
    {
        return is_closed_ticket(tail_.load(std::memory_order_acquire));
    }

    /**
     * @brief Reset a drained segment for reuse.
     * @return true -- no cell rewrite is needed. Tickets stay absolute, so a cell drained at
     *         ticket `t - capacity` already holds `lap_word(lap(t))`, which is exactly what an
     *         enqueue at `t` expects to find.
     * @warning Not MT-safe; the segment must already be drained and unreachable.
     */
    bool reopen() noexcept
        requires(Link::is_linked)
    {
        link_.unlink();
        // Realigning on the head clears the closed marker along with it.
        tail_.store(head_.load(std::memory_order_acquire), std::memory_order_release);
        return true;
    }

    /// @brief Is a producer holding a reserved ticket it has not published yet?
    /// @see algo::VyukovDCAS::has_inflight -- identical reasoning, identical two-step publish.
    bool has_inflight() const noexcept
        requires(Link::is_linked)
    {
        return clean(tail_.load(std::memory_order_acquire)) !=
               head_.load(std::memory_order_acquire);
    }

    /// @return The successor handle, or nil if this is the tail.
    handle_type next() const noexcept
        requires(Link::is_linked)
    {
        return link_.next();
    }

    /// @brief Publish @p h as the successor.
    /// @param current set to the successor now installed -- @p h if we won, the winner's
    ///        handle if we lost, so a losing caller need not re-read next().
    /// @return true for exactly one caller; the loser must discard its segment.
    bool link_next(handle_type h, handle_type& current) noexcept
        requires(Link::is_linked)
    {
        return link_.link_next(h, current);
    }

private:
    /// The closed marker lives in the top bit of the tail, so every arithmetic use of a ticket
    /// drops it first -- and here that includes the *lap*, not just the index.
    static constexpr uint64_t clean(uint64_t t) noexcept { return bit::clear_msb(t); }
    static constexpr bool is_closed_ticket(uint64_t t) noexcept { return bit::get_msb(t) != 0; }

    FORCE_INLINE std::size_t mod(uint64_t i) const noexcept {
        if constexpr (pow2) return clean(i) & (capacity_ - 1);
        else return clean(i) % capacity_;
    }

    /// Which time around the ring ticket @p i is. The cell carries this when empty, which is
    /// what removes the separate sequence word.
    FORCE_INLINE uint64_t lap(uint64_t i) const noexcept {
        if constexpr (pow2) return clean(i) >> shift_;
        else return clean(i) / capacity_;
    }

    [[no_unique_address]] link_state link_{};
    const std::size_t capacity_;
    /// Only meaningful on the pow2 path; zero otherwise, where lap() divides instead.
    const std::size_t shift_;
    cell_type* const cells_;
    CACHE_LINE_MEMBER(std::atomic<uint64_t>, tail_, {0});
    CACHE_LINE_MEMBER(std::atomic<uint64_t>, head_, {0});
};

} // namespace algo

/// @brief Capabilities of algo::VyukovNoABA as a linked segment. Every field is mandatory:
/// core::segment_traits has no primary definition, so omitting one is a compile error.
template <typename T, typename Opt, typename Link>
struct core::segment_traits<algo::VyukovNoABA<T, Opt, Link>> {
    /// The closed flag is re-read at the top of every enqueue loop.
    static constexpr bool needs_close_hint = false;
    static constexpr bool needs_dequeue_prepare = false;
    /// One CAS swaps the lap word for the payload, so no *cell* is half-written -- but the
    /// tail reservation happens first, and that gap is what this covers. See has_inflight.
    static constexpr bool needs_inflight_drain = true;
    /// reopen() is tail realignment only; see there for why the cells need no rewrite.
    static constexpr bool recyclable = true;
    /// A null payload is not a lap word -- those need both the top and bottom bits set -- so it
    /// round-trips as an ordinary value.
    static constexpr bool can_store_null = true;
};
MPMC_ASSERT_SEGMENT_TRAITS(algo::VyukovNoABA<int*, meta::EmptyOptions, linkage::None>);

namespace queue {
template <typename T, typename Opt = meta::EmptyOptions>
/// Standalone ring with the lap folded into the empty cell; no separate sequence word.
using VyukovNoABA = algo::VyukovNoABA<T, Opt, linkage::None>;
}

namespace seg {
template <typename T, typename Opt = meta::EmptyOptions, typename HP = mem::PtrHandle>
/// The single-word lap ring as a linked segment.
using VyukovNoABA = algo::VyukovNoABA<T, Opt, linkage::Node<HP>>;
}
