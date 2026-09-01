#pragma once
/**
 * @file PSCQ.hpp
 * @brief PRQ's cell protocol plus SCQ's threshold counter, storing payloads directly. A benchmark comparator.
 * @ingroup algo
 */

#include <cell/SequencedCell.hpp>
#include <cell/Tagging.hpp>
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

struct PSCQOpt {
    /**
     * @brief Take the capacity exactly as asked instead of rounding up to a power of two.
     *
     * Rounding is the default so that every algorithm answers a capacity request the same way
     * and a cross-algorithm benchmark compares the same geometry. Opting out costs a division
     * where the default path masks. The physical ring stays twice the capacity either way --
     * that doubling is inherent to the algorithm, not to the rounding.
     */
    struct no_pow2 {};
    struct no_cell_padding {};

    /// Inner-loop iterations a consumer spends on one cell before stealing it. See
    /// PRQOpt::max_dequeue_retries -- this is the same cell protocol.
    template <auto N> struct max_dequeue_retries {};

    /// How often that loop re-reads the shared tail, in iterations. **Power of two.**
    template <auto N> struct tail_reload_period {};
};

/**
 * @brief PRQ's cell protocol plus SCQ's threshold counter, storing payloads directly.
 *
 * A benchmark comparator, not a segment.
 *
 * PRQ has to sweep cells to discover it is empty; SCQ avoids that with a threshold, but
 * pays for it with indirection through an index ring. This combines the two: PRQ's
 * claim/publish cell protocol, with a threshold so an empty queue answers immediately
 * and no indirection. It exists to show what the threshold alone is worth.
 *
 * The physical ring is twice the usable capacity, which is what gives the unsafe-cell
 * marking room to work.
 */
template <typename T, typename Opt = meta::EmptyOptions, typename Link = linkage::None,
          typename Tag = cell::MsbTag<T>>
    requires meta::AcceptsOnly<Opt, typename PSCQOpt::no_cell_padding, typename PSCQOpt::no_pow2,
                               meta::ValueOption<PSCQOpt::max_dequeue_retries>,
                               meta::ValueOption<PSCQOpt::tail_reload_period>> &&
             cell::ClaimingTag<Tag, T>
class PSCQ : public mem::SingleBlock<PSCQ<T, Opt, Link, Tag>> {
    using Self = PSCQ<T, Opt, Link, Tag>;

    using word = typename Tag::word;

    static constexpr bool pad_cells = !Opt::template has<typename PSCQOpt::no_cell_padding>;
    /// Index mapping is a mask rather than a modulo; see PSCQOpt::no_pow2.
    static constexpr bool pow2 = !Opt::template has<typename PSCQOpt::no_pow2>;
    static constexpr uint64_t kTailReloadPeriod = static_cast<uint64_t>(
        Opt::template get<PSCQOpt::tail_reload_period, uint64_t{1ull << 8}>);
    static_assert(kTailReloadPeriod != 0 && (kTailReloadPeriod & (kTailReloadPeriod - 1)) == 0,
                  "PSCQOpt::tail_reload_period must be a power of two: the dequeue loop tests "
                  "it as a mask");
    static constexpr uint64_t kTailSnapshotMask = kTailReloadPeriod - 1;

    static constexpr size_t kMaxRetry = static_cast<size_t>(
        Opt::template get<PSCQOpt::max_dequeue_retries, std::size_t{4 * 1024}>);

public:
    /// @name Tuning in effect
    /// @{
    static constexpr std::size_t max_dequeue_retries = kMaxRetry;
    static constexpr uint64_t tail_reload_period = kTailReloadPeriod;
    /// @}

    using tag_type = Tag;
    using cell_type = cell::SequencedCell<word, pad_cells>;
    using link_state = typename Link::template state<Self>;
    using handle_type = typename link_state::handle;

    /// Physical ring size: twice the requested capacity, which is rounded to a power of two
    /// unless PSCQOpt::no_pow2 says otherwise. The doubling is the algorithm's, not the
    /// rounding's -- an SCQ-style ring needs two entries per item to tell a lap apart.
    static constexpr std::size_t phys_size(std::size_t n) noexcept {
        const std::size_t f = n < 2 ? 2 : n;
        if constexpr (pow2) return 2 * bit::round_to_next_pow2(f);
        else return 2 * f;
    }

    /// @return the capacity a queue built with @p n will report; mirrors capacity().
    static constexpr std::size_t capacity_for(std::size_t n) noexcept { return phys_size(n) >> 1; }

    /// @brief Where the co-allocated regions go. See @ref block-construction.
    /// @param n requested capacity; the only thing the layout may depend on.
    static constexpr auto plan(std::size_t n) noexcept {
        mem::LayoutBuilder b{sizeof(Self), alignof(Self)};
        mem::Plan<1> p{};
        p.regions[0] = b.add(phys_size(n) * sizeof(cell_type), alignof(cell_type));
        p.total = b.total();
        p.block_align = b.block_align();
        return p;
    }

    PSCQ(std::size_t n, mem::Blocks blk) noexcept
        : size_{phys_size(n)}, mask_{phys_size(n) - 1},
          max_threshold_{static_cast<int64_t>(2 * phys_size(n)) - 1},
          cells_{blk.template at<cell_type>(plan(n).regions[0])} {
        for (std::size_t i = 0; i < size_; ++i) {
            cells_[i].val.store(Tag::empty(), std::memory_order_relaxed);
            cells_[i].seq.store(i, std::memory_order_relaxed);
        }
        threshold_.store(max_threshold_, std::memory_order_relaxed);
    }

    /// @brief Add an item, skipping the attempt when the caller already knows it is closed.
    /// @param closed_hint the caller believes this segment is closed; see
    ///        core::segment_traits::needs_close_hint.
    bool enqueue(T item, bool closed_hint) noexcept {
        if constexpr (Link::is_linked) {
            // Same reason PRQ takes the hint: re-entering the loop on a segment already known
            // closed drives consumers down the unsafe-cell path for nothing.
            if (closed_hint && is_closed()) return false;
        }
        return enqueue(item);
    }

    /// @brief Add an item.
    /// @return false if the queue is full, or closed.
    bool enqueue(T item) noexcept {
        {   // cheap fullness pre-check, sampled twice to avoid a torn read
            const uint64_t t = clean(tail_.load(std::memory_order_acquire));
            for (int i = 0; i < 2; ++i)
                if (t >= head_.load(std::memory_order_acquire) + size_) {
                    // Full. A linked segment closes itself so the proxy links a successor
                    // rather than retrying here; see algo::Vyukov::enqueue.
                    if constexpr (Link::is_linked) close();
                    return false;
                }
        }
        for (;;) {
            // The ticket *is* the permission: once close() has set the bit, every ticket
            // handed out from here carries it, so no producer past that point ever reaches a
            // cell. That is what makes the close enforcing rather than advisory -- a flag a
            // producer merely reads can go stale between the read and the commit, and the item
            // then lands in a segment the proxy has already drained and retired.
            const uint64_t raw = tail_.fetch_add(1, std::memory_order_acq_rel);
            if constexpr (Link::is_linked) {
                if (is_closed_ticket(raw)) return false;
            }
            const uint64_t t = clean(raw);
            cell_type& c = cells_[mod(t)];
            uint64_t seq_word = c.seq.load(std::memory_order_acquire);
            word val = c.val.load(std::memory_order_acquire);
            const bool unsafe = bit::get_msb(seq_word) != 0;

            if (Tag::is_empty(val) && bit::clear_msb(seq_word) <= t &&
                (!unsafe || head_.load(std::memory_order_acquire) <= t)) {
                const word token = Tag::claim();
                if (c.val.compare_exchange_strong(val, token, std::memory_order_acq_rel,
                                                  std::memory_order_acquire)) {
                    uint64_t expect_seq = seq_word;
                    if (c.seq.compare_exchange_strong(expect_seq, t + size_,
                                                      std::memory_order_acq_rel,
                                                      std::memory_order_acquire)) {
                        word expect_tok = token;
                        if (c.val.compare_exchange_strong(expect_tok, Tag::encode(item),
                                                          std::memory_order_acq_rel,
                                                          std::memory_order_acquire)) {
                            if (threshold_.load(std::memory_order_acquire) != max_threshold_)
                                threshold_.store(max_threshold_, std::memory_order_release);
                            return true;
                        }
                    } else {
                        word expect_tok = token;
                        (void)c.val.compare_exchange_strong(expect_tok, Tag::empty(),
                                                            std::memory_order_acq_rel,
                                                            std::memory_order_acquire);
                    }
                }
            }

            if ((t + 1) >= head_.load(std::memory_order_acquire) + size_) {
                if constexpr (Link::is_linked) close();
                return false;
            }
        }
    }

    /// @brief Take the oldest item.
    /// @return false if the queue is empty.
    bool dequeue(T& out) noexcept {
        for (;;) {
            if (threshold_.load(std::memory_order_acquire) <= 0) return false; // known empty

            const uint64_t h = head_.fetch_add(1, std::memory_order_acq_rel);
            cell_type& c = cells_[mod(h)];
            size_t retry = 0;
            uint64_t tail_snap = 0;

            for (;;) {
                const uint64_t seq_word = c.seq.load(std::memory_order_relaxed);
                const bool unsafe = bit::get_msb(seq_word) != 0;
                const uint64_t seq = bit::clear_msb(seq_word);
                const word val = c.val.load(std::memory_order_relaxed);

                if (seq > h + size_) break;
                if (seq_word != c.seq.load(std::memory_order_acquire)) continue;

                if (Tag::is_payload(val)) {
                    if (seq == h + size_) {
                        c.val.store(Tag::empty(), std::memory_order_release);
                        out = Tag::decode(val);
                        return true;
                    }
                    if (unsafe) {
                        if (c.seq.load(std::memory_order_acquire) == seq_word) break;
                    } else {
                        uint64_t expect = seq_word;
                        if (c.seq.compare_exchange_strong(expect, bit::set_msb(seq_word),
                                                          std::memory_order_release,
                                                          std::memory_order_relaxed))
                            break;
                    }
                } else {
                    if ((retry & kTailSnapshotMask) == 0)
                        tail_snap = clean(tail_.load(std::memory_order_acquire));
                    if (unsafe || tail_snap < h + 1 || retry > kMaxRetry) {
                        if (Tag::is_claim(val)) {
                            word expect = val;
                            if (!c.val.compare_exchange_strong(expect, Tag::empty(),
                                                               std::memory_order_acq_rel,
                                                               std::memory_order_acquire))
                                continue;
                        }
                        uint64_t expect = seq_word;
                        const uint64_t desired =
                            (unsafe ? bit::set_msb<uint64_t>(0) : 0) | (h + size_);
                        if (c.seq.compare_exchange_strong(expect, desired,
                                                          std::memory_order_acq_rel,
                                                          std::memory_order_acquire))
                            break;
                    }
                    ++retry;
                }
            }

            tail_snap = clean(tail_.load(std::memory_order_acquire));
            if (tail_snap <= h + 1) {
                fix_state(tail_snap, h + 1);
                threshold_.fetch_sub(1, std::memory_order_release);
                return false;
            }
            if (threshold_.fetch_sub(1, std::memory_order_acq_rel) <= 0) return false;
        }
    }

    /// Usable capacity: half the physical ring.
    std::size_t capacity() const noexcept { return size_ >> 1; }


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

    // -- linkage (only when linked) -----------------------------------------

    /**
     * @brief Restore the threshold before the proxy re-reads this segment after a link.
     *
     * Same reason as SCQ: an empty dequeue *spends* threshold, so a segment that has been
     * declared empty has none left. When a successor is linked the proxy comes back for one
     * more drain, and without this that drain reports empty regardless of what is in the ring.
     * @see core::segment_traits::needs_dequeue_prepare
     */
    void prepare_dequeue_after_link() noexcept
        requires(Link::is_linked)
    {
        threshold_.store(max_threshold_, std::memory_order_release);
    }

    /**
     * @brief Is any producer part-way through an insert?
     *
     * Unlike PRQ, which publishes its payload in the same CAS that claims the cell, a producer
     * here claims with a token, fixes the sequence word, and only then swaps the payload in --
     * three steps, so a cell can be seen holding a claim that is going to become an item. A
     * segment must not be retired while one is outstanding or that item is lost.
     *
     * O(ring), and deliberately so: it runs once per retirement, not per operation.
     * @see core::segment_traits::needs_inflight_drain
     */
    bool has_inflight() const noexcept
        requires(Link::is_linked)
    {
        for (std::size_t i = 0; i < size_; ++i)
            if (Tag::is_claim(cells_[i].val.load(std::memory_order_acquire))) return true;
        return false;
    }

    /// @brief Refuse all further enqueues, permanently.
    ///
    /// A separate flag rather than a spare bit of the tail: the tail here is advanced with
    /// fetch_add and the sequence words already use their top bit as the "unsafe" marker, so
    /// there is no bit going spare.
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
     * @brief Reset a drained segment for reuse, by rebuilding it.
     * @return true, always.
     *
     * **O(capacity), unlike every other segment here**, which recycles by realigning indices.
     * The cheap trick does not obviously hold for this algorithm: a drained cell can carry the
     * `unsafe` bit into the next lap, and the threshold counter is consumed by empty dequeues
     * rather than by position, so neither is a function of the head alone. Rewriting the ring
     * restores exactly the constructor's state, which is the one state the algorithm is
     * demonstrably correct from.
     *
     * @warning Not MT-safe; the segment must already be drained and unreachable.
     */
    bool reopen() noexcept
        requires(Link::is_linked)
    {
        link_.unlink();
        for (std::size_t i = 0; i < size_; ++i) {
            cells_[i].val.store(Tag::empty(), std::memory_order_relaxed);
            cells_[i].seq.store(i, std::memory_order_relaxed);
        }
        head_.store(0, std::memory_order_relaxed);
        tail_.store(0, std::memory_order_relaxed);   // clears the closed marker with it
        threshold_.store(max_threshold_, std::memory_order_release);
        return true;
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
    /// Drag the tail back up when consumers have overshot the producers.
    void fix_state(uint64_t t, uint64_t h) noexcept {
        while (h > t) {
            uint64_t expect = t;
            if (tail_.compare_exchange_strong(expect, h, std::memory_order_acq_rel,
                                              std::memory_order_acquire))
                return;
            // A closed tail carries the marker, so the CAS will never match a clean value:
            // stop rather than spin. There is nothing to fix on a segment that has stopped
            // accepting anyway.
            if constexpr (Link::is_linked) {
                if (is_closed_ticket(expect)) return;
            }
            t = clean(expect);
            h = head_.load(std::memory_order_relaxed);
        }
    }

    /// The closed marker lives in the top bit of the tail, so every arithmetic use of a
    /// ticket has to drop it first. A missed one is a wrong index rather than a crash, which
    /// is why it goes through a single helper.
    static constexpr uint64_t clean(uint64_t t) noexcept { return bit::clear_msb(t); }
    static constexpr bool is_closed_ticket(uint64_t t) noexcept { return bit::get_msb(t) != 0; }

    FORCE_INLINE std::size_t mod(uint64_t i) const noexcept {
        if constexpr (pow2) return clean(i) & mask_;
        else return clean(i) % size_;
    }

    [[no_unique_address]] link_state link_{};
    const std::size_t size_;
    /// Only meaningful on the pow2 path; mod() divides instead when it is off.
    const std::size_t mask_;
    const int64_t max_threshold_;
    cell_type* const cells_;
    CACHE_LINE_MEMBER(std::atomic<uint64_t>, tail_, {0});
    CACHE_LINE_MEMBER(std::atomic<uint64_t>, head_, {0});
    CACHE_LINE_MEMBER(std::atomic<int64_t>, threshold_, {0});
};

} // namespace algo

/// @brief Capabilities of algo::PSCQ as a linked segment. Every field is mandatory:
/// core::segment_traits has no primary definition, so omitting one is a compile error.
template <typename T, typename Opt, typename Link, typename Tag>
struct core::segment_traits<algo::PSCQ<T, Opt, Link, Tag>> {
    /// The closed flag is re-read at the top of every enqueue.
    /// As PRQ: re-entering the enqueue loop on a known-closed segment is what livelocks.
    static constexpr bool needs_close_hint = true;
    /// The threshold has to be restored before a post-link drain retry, exactly as for SCQ --
    /// an empty dequeue consumes it, and a segment that is about to be re-read needs it back.
    static constexpr bool needs_dequeue_prepare = true;
    /// A producer claims the cell with a token and publishes the payload in a *later* CAS, so
    /// a cell can be seen mid-insert and a drain has to wait it out.
    static constexpr bool needs_inflight_drain = true;
    /// reopen() rebuilds the ring; see there for why the O(1) trick does not apply here.
    static constexpr bool recyclable = true;
    /// The tag reserves an encoding for the empty cell, so a null payload is representable.
    static constexpr bool can_store_null = Tag::can_store_null;
};
MPMC_ASSERT_SEGMENT_TRAITS(algo::PSCQ<int*, meta::EmptyOptions, linkage::None>);

namespace queue {
template <typename T, typename Opt = meta::EmptyOptions>
/// Standalone PSCQ: PRQ's cell protocol with SCQ's threshold. A comparator.
using PSCQ = algo::PSCQ<T, Opt, linkage::None>;
}

namespace seg {
template <typename T, typename Opt = meta::EmptyOptions, typename HP = mem::PtrHandle>
/// PSCQ as a linked segment.
using PSCQ = algo::PSCQ<T, Opt, linkage::Node<HP>>;
}
