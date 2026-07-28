#pragma once
#include <cell/PlainCell.hpp>
#include <core/SegmentTraits.hpp>
#include <linkage/Linkage.hpp>
#include <mem/SingleBlock.hpp>
#include <meta/OptionsPack.hpp>
#include <util/bit.hpp>
#include <util/specs.hpp>
#include <atomic>
#include <cassert>

namespace algo {

struct LFringOpt {
    struct no_cell_padding {};
    /**
     * The ring holds indices into someone else's buffer rather than the payload itself.
     * Removes the fullness pre-check, because the owner guarantees an index is available.
     */
    struct indirect_store {};
};

/**
 * @brief A lock-free ring of `size_t` values, addressed by fetch-add with a threshold.
 *
 * The physical buffer is twice the logical capacity: each slot carries a cycle number in
 * its high bits, so a slot can record "empty for this lap" without a separate flag. The
 * `threshold` counter lets an empty ring answer dequeue immediately instead of sweeping.
 *
 * Used two ways:
 *  - standalone, allocated as its own block (`queue::LFring`);
 *  - embedded, two at a time, inside SCQ's single block via the (order, cells) constructor.
 *
 * @note Not a linked segment on its own. SCQ is the segment; this is its component.
 */
template <typename Opt = meta::EmptyOptions, typename Link = linkage::None>
    requires meta::AcceptsOnly<Opt, typename LFringOpt::no_cell_padding, typename LFringOpt::indirect_store>
class LFring : public mem::SingleBlock<LFring<Opt, Link>> {
    using Self = LFring<Opt, Link>;


    static constexpr bool pad_cells = !Opt::template has<typename LFringOpt::no_cell_padding>;
    static constexpr bool direct_store = !Opt::template has<typename LFringOpt::indirect_store>;
    /// Only a direct-store ring must check fullness; an index ring is bounded by its owner.
    static constexpr bool full_check = direct_store;

    static constexpr uint64_t kReloadTailMask = (1ull << 8) - 1;
    static constexpr size_t kMaxRetry = 1024 * 10;

public:
    using cell_type = cell::PlainCell<uint64_t, pad_cells>;
    using link_state = typename Link::template state<Self>;
    using handle_type = typename link_state::handle;

    /// Logical capacity for a given order.
    static constexpr size_t virtual_size(size_t order) noexcept { return 1ull << order; }
    /// Physical cell count: twice the logical capacity.
    static constexpr size_t cells_for(size_t order) noexcept { return virtual_size(order) << 1; }
    /// Order needed to hold @p n items.
    static constexpr size_t order_for(size_t n) noexcept {
        return bit::log2(bit::round_to_next_pow2(n < 2 ? 2 : n));
    }

    // -- single-block layout (standalone use) -------------------------------
    static constexpr auto plan(std::size_t n) noexcept {
        mem::LayoutBuilder b{sizeof(Self), alignof(Self)};
        mem::Plan<1> p{};
        p.regions[0] = b.add(cells_for(order_for(n)) * sizeof(cell_type), alignof(cell_type));
        p.total = b.total();
        p.block_align = b.block_align();
        return p;
    }

    LFring(std::size_t n, mem::Blocks blk) noexcept
        : LFring(order_for(n), blk.template at<cell_type>(plan(n).regions[0]), false) {}

    /**
     * @brief Embedded constructor: adopt an externally placed cell array.
     * @param order     logarithm of the logical capacity
     * @param cells     at least cells_for(order) entries
     * @param init_full start with every index present (used for SCQ's free-slot ring)
     */
    LFring(std::size_t order, cell_type* cells, bool init_full) noexcept
        : order_{order}, cells_{cells} {
        assert(order != 0 && "LFring: order must be non-null");
        init(0, init_full ? virtual_size(order_) : 0);
    }

    // -- queue ---------------------------------------------------------------

    FORCE_INLINE bool enqueue(size_t item) noexcept {
        const size_t half = virtual_size(order_), n = half << 1;
        item ^= (n - 1); // fold the cycle bits in

        if constexpr (direct_store) {
            if (tail_.load(std::memory_order_acquire) >=
                head_.load(std::memory_order_acquire) + n)
                return false;
        }

        for (;;) {
            const uint64_t t = tail_.fetch_add(1, std::memory_order_acq_rel);
            if (bit::get_msb(t)) return false; // closed
            const uint64_t tcycle = (t << 1) | (2 * n - 1);
            cell_type& c = cells_[t & (n - 1)];
            uint64_t entry = c.val.load(std::memory_order_acquire);

            for (;;) {
                const uint64_t ecycle = entry | (2 * n - 1);
                const bool older = static_cast<int64_t>(ecycle - tcycle) < 0;
                const bool usable =
                    (entry == ecycle) ||
                    ((entry == (ecycle ^ n)) &&
                     static_cast<int64_t>(head_.load(std::memory_order_acquire) - t) <= 0);
                if (!(older && usable)) break;
                if (c.val.compare_exchange_weak(entry, tcycle ^ item, std::memory_order_acq_rel,
                                                std::memory_order_acquire)) {
                    const int64_t want = max_threshold(half, n);
                    if (threshold_.load(std::memory_order_seq_cst) != want)
                        threshold_.store(want, std::memory_order_seq_cst);
                    return true;
                }
            }

            if constexpr (full_check) {
                if ((t + 1) >= head_.load(std::memory_order_acquire) + n) return false;
            }
        }
    }

    FORCE_INLINE bool dequeue(size_t& out) noexcept {
        const size_t half = virtual_size(order_), n = half << 1;
        if (deq_blocked_.load(std::memory_order_acquire)) return false;   // acquisition closed
        if (threshold_.load(std::memory_order_seq_cst) < 0) return false; // known empty

        for (;;) {
            const uint64_t h = head_.fetch_add(1, std::memory_order_acq_rel);
            const uint64_t hcycle = (h << 1) | (2 * n - 1);
            cell_type& c = cells_[h & (n - 1)];
            size_t attempt = 0;
            uint64_t tail_snap = 0, tail_idx = 0;
            bool closed = false;

            uint64_t entry = c.val.load(std::memory_order_acquire);
            uint64_t entry_new = 0;
            for (;;) {
                const uint64_t ecycle = entry | (2 * n - 1);
                if (ecycle == hcycle) { // ours
                    c.val.fetch_or(n - 1, std::memory_order_release);
                    out = static_cast<size_t>(entry & (n - 1));
                    return true;
                }
                if ((entry | n) != ecycle) { // mark the slot unsafe for this lap
                    entry_new = entry & ~static_cast<uint64_t>(n);
                    if (entry == entry_new) break;
                } else { // empty: wait a bounded while, then advance the slot
                    if ((attempt & kReloadTailMask) == 0) {
                        tail_snap = tail_.load(std::memory_order_acquire);
                        closed = bit::get_msb(tail_snap) != 0;
                        tail_idx = bit::clear_msb(tail_snap);
                    }
                    if (++attempt <= kMaxRetry && !closed &&
                        static_cast<int64_t>(tail_idx - (h + 1)) >= 0) {
                        entry = c.val.load(std::memory_order_acquire);
                        continue;
                    }
                    entry_new = hcycle ^ ((~entry) & n);
                }
                if (static_cast<int64_t>(ecycle - hcycle) >= 0) break;
                if (c.val.compare_exchange_weak(entry, entry_new, std::memory_order_acq_rel,
                                                std::memory_order_acquire))
                    break;
            }

            tail_snap = tail_.load(std::memory_order_acquire);
            tail_idx = bit::clear_msb(tail_snap);
            uint64_t head_snap = h + 1;
            if (tail_idx <= head_snap) { // consumers have caught the producers
                while (tail_idx < head_snap &&
                       !tail_.compare_exchange_weak(tail_idx, head_snap,
                                                    std::memory_order_acq_rel,
                                                    std::memory_order_acquire))
                    head_snap = head_.load(std::memory_order_acquire);
                (void)threshold_.fetch_sub(1, std::memory_order_release);
                return false;
            }
            if (threshold_.fetch_sub(1, std::memory_order_acq_rel) <= 0) return false;
        }
    }

    std::size_t size() const noexcept {
        const uint64_t t = bit::clear_msb(tail_.load(std::memory_order_acquire));
        const uint64_t h = head_.load(std::memory_order_acquire);
        return t <= h ? 0 : static_cast<std::size_t>(t - h);
    }

    std::size_t capacity() const noexcept { return virtual_size(order_); }

    /**
     * @brief Restore the threshold so a drained-looking ring is swept again.
     *
     * The threshold is what lets dequeue answer "empty" without scanning. Once a
     * successor segment exists the head segment must be drained exhaustively, so the
     * shortcut has to be disarmed first or items still present are reported as absent.
     */
    FORCE_INLINE void reset_threshold() noexcept {
        const size_t half = virtual_size(order_);
        threshold_.store(static_cast<int64_t>(half + (half << 1) - 1), std::memory_order_release);
    }

    void close() noexcept { tail_.fetch_or(bit::set_msb<uint64_t>(0), std::memory_order_release); }

    bool is_closed() const noexcept {
        return bit::get_msb(tail_.load(std::memory_order_acquire)) != 0;
    }

    /**
     * @brief Stop handing values out, while still accepting values back.
     *
     * SCQ's free ring is consumed to acquire a slot and produced to return one, so
     * "closed" for it means *dequeue* must stop -- enqueue must keep working or a
     * consumer cannot give a drained slot back.
     *
     * close() is the wrong tool for that: it sets the MSB of the tail, which both blocks
     * enqueue and corrupts the tail's index arithmetic. Hence a separate flag.
     */
    void block_dequeue() noexcept { deq_blocked_.store(true, std::memory_order_release); }

    bool dequeue_blocked() const noexcept { return deq_blocked_.load(std::memory_order_acquire); }

    void unblock_dequeue() noexcept { deq_blocked_.store(false, std::memory_order_release); }

    bool reopen() noexcept {
        deq_blocked_.store(false, std::memory_order_release);
        threshold_.store(0, std::memory_order_release);
        tail_.store(head_.load(std::memory_order_acquire), std::memory_order_release);
        return true;
    }

    /**
     * @brief Return the ring to its freshly constructed state.
     *
     * @param init_full start holding every index (a free-slot ring) or empty (a data ring)
     *
     * Costs O(cells). Needed because a dequeue can report empty while entries remain --
     * the threshold is a heuristic, not a count -- so a ring handed back for reuse may
     * still hold stranded indices. Merely clearing flags leaves them there to be handed
     * out again in the segment's next life.
     *
     * @warning Not MT-safe. The owner must be unreachable to other threads.
     */
    void reset(bool init_full) noexcept {
        deq_blocked_.store(false, std::memory_order_relaxed);
        init(0, init_full ? virtual_size(order_) : 0);
    }

private:
    static constexpr int64_t max_threshold(size_t half, size_t n) noexcept {
        if constexpr (direct_store) return static_cast<int64_t>(2 * n) - 1;
        else return static_cast<int64_t>(half + n) - 1;
    }

    void init(size_t start, size_t end) noexcept {
        const auto rlx = std::memory_order_relaxed;
        const size_t half = virtual_size(order_), n = half << 1;
        size_t i = 0;
        for (; i < start; ++i) cells_[i & (n - 1)].val.store(2 * n - 1, rlx);
        for (; i < end; ++i) cells_[i & (n - 1)].val.store(n + i, rlx);
        for (; i < n; ++i) cells_[i & (n - 1)].val.store(static_cast<uint64_t>(-1), rlx);
        head_.store(start, rlx);
        tail_.store(end, rlx);
        threshold_.store(end != start ? max_threshold(half, n) : -1, rlx);
    }

    const size_t order_;
    cell_type* const cells_;
    ALIGNED_CACHE std::atomic<uint64_t> head_{0};
    CACHE_PAD_TYPES(std::atomic<uint64_t>);
    ALIGNED_CACHE std::atomic<uint64_t> tail_{0};
    CACHE_PAD_TYPES(std::atomic<uint64_t>);
    ALIGNED_CACHE std::atomic<int64_t> threshold_{0};
    CACHE_PAD_TYPES(std::atomic<int64_t>);
    ALIGNED_CACHE std::atomic<bool> deq_blocked_{false};
    CACHE_PAD_TYPES(std::atomic<bool>);
};

} // namespace algo

namespace queue {
/// Standalone index ring.
/// Named IndexRing rather than LFring: it carries indices, not user payloads, and the
/// name LFring is still taken by the legacy header for as long as that tree exists.
template <typename Opt = meta::EmptyOptions>
using IndexRing = algo::LFring<Opt, linkage::None>;
} // namespace queue
