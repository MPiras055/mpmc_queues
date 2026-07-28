#pragma once
#include <cell/SequencedCell.hpp>
#include <cell/Tagging.hpp>
#include <core/SegmentTraits.hpp>
#include <linkage/Linkage.hpp>
#include <mem/SingleBlock.hpp>
#include <meta/OptionsPack.hpp>
#include <util/bit.hpp>
#include <util/specs.hpp>
#include <atomic>
#include <cassert>

namespace algo {

struct PRQOpt {
    struct force_pow2 {};
    struct no_cell_padding {};
};

/**
 * @brief Ring with per-cell sequence numbers and an "unsafe" bit, indexed by fetch-add.
 *
 * Producers and consumers each fetch-add their own ticket, so neither side spins on a
 * shared CAS. A consumer that reaches a cell before its producer marks the cell
 * *unsafe* (the MSB of the sequence word) and moves on; the producer then knows to skip
 * it. That is what makes the algorithm obstruction-free rather than lock-free.
 *
 * A producer reserves in three steps -- publish a per-thread claim token, fix the
 * sequence, then swap the payload in -- so the tagging policy must be able to mint
 * claims.
 *
 * @note **Linked-only.** PRQ closes itself when producers overshoot and then relies on a proxy to link a
 * successor. Standalone there is nobody to link one, so producers spin on a full
 * ring while consumers drag the over-run tail back -- correct, but pathologically
 * slow (measured ~21k ops/s against ~12M for a plain Vyukov ring).
 *       The class template is constrained on linkage::Linked, so
 *       `algo::PRQ<T, Opt, linkage::None>` is not a nameable type.
 *
 * @tparam Tag cell::ClaimingTag policy
 */
template <typename T, typename Opt, typename Link,
          typename Tag = cell::MsbTag<T>>
    requires meta::AcceptsOnly<Opt, typename PRQOpt::force_pow2, typename PRQOpt::no_cell_padding> && linkage::Linked<Link> && cell::ClaimingTag<Tag, T>
class PRQ : public mem::SingleBlock<PRQ<T, Opt, Link, Tag>> {
    using Self = PRQ<T, Opt, Link, Tag>;

    using word = typename Tag::word;

    static constexpr bool pad_cells = !Opt::template has<typename PRQOpt::no_cell_padding>;
    static constexpr bool force_pow2 = Opt::template has<typename PRQOpt::force_pow2>;

    static constexpr uint64_t kMaxRetryDeq = 4 * 1024;
    static constexpr uint64_t kReloadTailMask = (1u << 8) - 1;

public:
    using tag_type = Tag;
    using cell_type = cell::SequencedCell<word, pad_cells>;
    using link_state = typename Link::template state<Self>;
    using handle_type = typename link_state::handle;

    static constexpr std::size_t round_size(std::size_t n) noexcept {
        if constexpr (force_pow2) return bit::round_to_next_pow2(n > 1 ? n : 2);
        else return n;
    }

    static constexpr auto plan(std::size_t n) noexcept {
        mem::LayoutBuilder b{sizeof(Self), alignof(Self)};
        mem::Plan<1> p{};
        p.regions[0] = b.add(round_size(n) * sizeof(cell_type), alignof(cell_type));
        p.total = b.total();
        p.block_align = b.block_align();
        return p;
    }

    PRQ(std::size_t n, mem::Blocks blk) noexcept
        : capacity_{round_size(n)}, cells_{blk.template at<cell_type>(plan(n).regions[0])} {
        assert(capacity_ != 0 && "PRQ: capacity must be non-null");
        for (std::size_t i = 0; i < capacity_; ++i) {
            cells_[i].val.store(Tag::empty(), std::memory_order_relaxed);
            cells_[i].seq.store(i, std::memory_order_relaxed);
        }
    }

    /**
     * @param closed_hint the caller believes this segment is already closed. Skipping
     *        the fetch-add in that case is not just an optimisation: re-entering the
     *        loop on a closed segment drives consumers down the unsafe-cell path, and
     *        under a bounded proxy that can livelock producers and consumers on the same
     *        segment. See segment_traits::needs_close_hint.
     */
    FORCE_INLINE bool enqueue(T item, bool closed_hint = false) noexcept {
        if constexpr (Link::is_linked) {
            if (closed_hint && is_closed()) return false;
        }
        for (;;) {
            const uint64_t t = tail_.fetch_add(1, std::memory_order_acq_rel);
            if constexpr (Link::is_linked) {
                if (is_closed_ticket(t)) return false;
            }
            cell_type& c = cells_[mod(t)];
            const uint64_t safe_seq = c.seq.load(std::memory_order_acquire);
            word val = c.val.load(std::memory_order_acquire);
            const bool unsafe = bit::get_msb(safe_seq) != 0;
            const uint64_t seq = bit::clear_msb(safe_seq);

            if (Tag::is_empty(val) && seq <= t &&
                (!unsafe || head_.load(std::memory_order_acquire) <= t)) {
                const word token = Tag::claim();
                if (c.val.compare_exchange_strong(val, token, std::memory_order_acq_rel,
                                                  std::memory_order_acquire)) {
                    uint64_t expect_seq = safe_seq;
                    if (c.seq.compare_exchange_strong(expect_seq, t + capacity_,
                                                      std::memory_order_acq_rel,
                                                      std::memory_order_acquire)) {
                        word expect_tok = token;
                        if (c.val.compare_exchange_strong(expect_tok, Tag::encode(item),
                                                          std::memory_order_acq_rel,
                                                          std::memory_order_acquire))
                            return true;
                    } else {
                        word expect_tok = token;
                        (void)c.val.compare_exchange_strong(expect_tok, Tag::empty(),
                                                            std::memory_order_acq_rel,
                                                            std::memory_order_acquire);
                    }
                }
            }

            if (t >= head_.load(std::memory_order_relaxed) + capacity_) {
                if constexpr (Link::is_linked) close();
                return false;
            }
        }
    }

    FORCE_INLINE bool dequeue(T& out) noexcept {
        for (;;) {
            const uint64_t h = head_.fetch_add(1, std::memory_order_acq_rel);
            cell_type& c = cells_[mod(h)];
            uint64_t retry = 0;
            uint64_t tail_snap = 0;

            for (;;) {
                const uint64_t safe_seq = c.seq.load(std::memory_order_acquire);
                const bool unsafe = bit::get_msb(safe_seq) != 0;
                const uint64_t seq = bit::clear_msb(safe_seq);
                const word val = c.val.load(std::memory_order_acquire);

                if (seq > h + capacity_) break; // cell belongs to a later lap
                if (safe_seq != c.seq.load(std::memory_order_acquire)) continue; // torn read

                if (Tag::is_payload(val)) {
                    if (seq == h + capacity_) { // ours
                        c.val.store(Tag::empty(), std::memory_order_relaxed);
                        out = Tag::decode(val);
                        return true;
                    }
                    // Payload from a different lap: mark unsafe so producers skip it.
                    if (unsafe) {
                        if (c.seq.load(std::memory_order_acquire) == safe_seq) break;
                    } else {
                        uint64_t expect = safe_seq;
                        if (c.seq.compare_exchange_strong(expect, bit::set_msb(seq),
                                                          std::memory_order_acq_rel,
                                                          std::memory_order_acquire))
                            break;
                    }
                } else { // empty or claimed: consider stealing it from the producer
                    if ((retry & kReloadTailMask) == 0)
                        tail_snap = tail_.load(std::memory_order_acquire);
                    const uint64_t tail_idx = bit::clear_msb(tail_snap);
                    const bool closed = bit::get_msb(tail_snap) != 0;

                    if (unsafe || tail_idx < h + 1 || closed || retry > kMaxRetryDeq) {
                        if (Tag::is_claim(val)) {
                            word expect = val;
                            if (!c.val.compare_exchange_strong(expect, Tag::empty(),
                                                               std::memory_order_acq_rel,
                                                               std::memory_order_acquire))
                                continue;
                        }
                        uint64_t expect = safe_seq;
                        const uint64_t desired =
                            (unsafe ? bit::set_msb<uint64_t>(0) : 0) | (h + capacity_);
                        if (c.seq.compare_exchange_strong(expect, desired,
                                                          std::memory_order_acq_rel,
                                                          std::memory_order_acquire))
                            break;
                    }
                    ++retry;
                }
            }

            // We advanced past an empty cell. If consumers have overshot producers the
            // segment is empty and the tail must be dragged back up to the head.
            tail_snap = tail_.load(std::memory_order_acquire);
            if (bit::clear_msb(tail_snap) <= h + 1) {
                uint64_t head_snap;
                // The guard below compares the RAW word on purpose. A closed segment
                // carries the MSB, making tail_snap enormous, so the condition is false
                // and the tail is left alone. Masking the MSB off here would let the CAS
                // write a plain head value over the closed marker -- reopening a segment
                // that has already been unlinked, so anything enqueued into it afterwards
                // is silently lost.
                do {
                    head_snap = head_.load(std::memory_order_acquire);
                } while (tail_snap < h &&
                         !tail_.compare_exchange_strong(tail_snap, head_snap,
                                                        std::memory_order_acq_rel,
                                                        std::memory_order_acquire));
                return false;
            }
        }
    }

    std::size_t size() const noexcept {
        const uint64_t t = bit::clear_msb(tail_.load(std::memory_order_acquire));
        const uint64_t h = head_.load(std::memory_order_acquire);
        return t > h ? static_cast<std::size_t>(t - h) : 0;
    }

    std::size_t capacity() const noexcept { return capacity_; }

    void close() noexcept
        requires(Link::is_linked)
    {
        tail_.fetch_or(bit::set_msb<uint64_t>(0), std::memory_order_release);
    }

    bool is_closed() const noexcept
        requires(Link::is_linked)
    {
        return is_closed_ticket(tail_.load(std::memory_order_acquire));
    }

    /// Index realignment only; cells carry their own lap numbers.
    bool reopen() noexcept
        requires(Link::is_linked)
    {
        link_.unlink();
        tail_.store(head_.load(std::memory_order_acquire), std::memory_order_release);
        return true;
    }

    handle_type next() const noexcept
        requires(Link::is_linked)
    {
        return link_.next();
    }

    bool link_next(handle_type h) noexcept
        requires(Link::is_linked)
    {
        return link_.link_next(h);
    }

private:
    static constexpr bool is_closed_ticket(uint64_t t) noexcept { return bit::get_msb(t) != 0; }

    FORCE_INLINE std::size_t mod(uint64_t i) const noexcept {
        if constexpr (force_pow2) return bit::clear_msb(i) & (capacity_ - 1);
        else return bit::clear_msb(i) % capacity_;
    }

    ALIGNED_CACHE std::atomic<uint64_t> tail_{0};
    CACHE_PAD_TYPES(std::atomic<uint64_t>);
    ALIGNED_CACHE std::atomic<uint64_t> head_{0};
    CACHE_PAD_TYPES(std::atomic<uint64_t>);
    [[no_unique_address]] link_state link_{};
    const std::size_t capacity_;
    cell_type* const cells_;
};

} // namespace algo

template <typename T, typename Opt, typename Link, typename Tag>
struct core::segment_traits<algo::PRQ<T, Opt, Link, Tag>> {
    /**
     * PRQ closes itself on overshoot and lets consumers steal cells from producers.
     * Re-entering the enqueue loop on an already-closed segment forces consumers down
     * the unsafe-cell path; under a bounded proxy, where a fresh segment may not be
     * obtainable, that livelocks. Measured on the pooled proxy at 4P/4C x 100k items:
     * 4 of 12 runs stalled permanently without the hint, 0 of 42 with it.
     */
    static constexpr bool needs_close_hint = true;
    static constexpr bool needs_dequeue_prepare = false;
    /// A single atomic step publishes the item; nothing can be mid-insert.
    static constexpr bool needs_inflight_drain = false;
    static constexpr bool recyclable = true;
    static constexpr bool can_store_null = Tag::can_store_null;
};
MPMC_ASSERT_SEGMENT_TRAITS(algo::PRQ<int*, meta::EmptyOptions, linkage::Node<mem::PtrHandle>>);

namespace seg {
template <typename T, typename Opt = meta::EmptyOptions, typename HP = mem::PtrHandle>
using PRQ = algo::PRQ<T, Opt, linkage::Node<HP>>;
}
