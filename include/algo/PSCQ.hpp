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

struct PSCQOpt {
    struct no_cell_padding {};
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
    requires cell::ClaimingTag<Tag, T>
class PSCQ : public mem::SingleBlock<PSCQ<T, Opt, Link, Tag>> {
    using Self = PSCQ<T, Opt, Link, Tag>;
    using word = typename Tag::word;
    static_assert(Link::is_linked == false, "PSCQ is a standalone comparator only");

    static constexpr bool pad_cells = !Opt::template has<typename PSCQOpt::no_cell_padding>;
    static constexpr uint64_t kTailSnapshotMask = (1ull << 8) - 1;
    static constexpr size_t kMaxRetry = 4 * 1024;

public:
    using tag_type = Tag;
    using cell_type = cell::SequencedCell<word, pad_cells>;
    using link_state = typename Link::template state<Self>;
    using handle_type = typename link_state::handle;

    /// Physical ring size: twice the requested capacity, rounded to a power of two.
    static constexpr std::size_t phys_size(std::size_t n) noexcept {
        return 2 * bit::round_to_next_pow2(n < 2 ? 2 : n);
    }

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

    bool enqueue(T item) noexcept {
        {   // cheap fullness pre-check, sampled twice to avoid a torn read
            const uint64_t t = tail_.load(std::memory_order_acquire);
            for (int i = 0; i < 2; ++i)
                if (t >= head_.load(std::memory_order_acquire) + size_) return false;
        }
        for (;;) {
            const uint64_t t = tail_.fetch_add(1, std::memory_order_acq_rel);
            cell_type& c = cells_[t & mask_];
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

            if ((t + 1) >= head_.load(std::memory_order_acquire) + size_) return false;
        }
    }

    bool dequeue(T& out) noexcept {
        for (;;) {
            if (threshold_.load(std::memory_order_acquire) <= 0) return false; // known empty

            const uint64_t h = head_.fetch_add(1, std::memory_order_acq_rel);
            cell_type& c = cells_[h & mask_];
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
                        tail_snap = tail_.load(std::memory_order_acquire);
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

            tail_snap = tail_.load(std::memory_order_acquire);
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

    std::size_t size() const noexcept {
        const uint64_t t = tail_.load(std::memory_order_acquire);
        const uint64_t h = head_.load(std::memory_order_acquire);
        return t < h ? 0 : static_cast<std::size_t>(t - h);
    }

private:
    /// Drag the tail back up when consumers have overshot the producers.
    void fix_state(uint64_t t, uint64_t h) noexcept {
        while (h > t && !tail_.compare_exchange_strong(t, h, std::memory_order_acq_rel,
                                                       std::memory_order_acquire))
            h = head_.load(std::memory_order_relaxed);
    }

    const std::size_t size_;
    const std::size_t mask_;
    const int64_t max_threshold_;
    cell_type* const cells_;
    ALIGNED_CACHE std::atomic<uint64_t> tail_{0};
    CACHE_PAD_TYPES(std::atomic<uint64_t>);
    ALIGNED_CACHE std::atomic<uint64_t> head_{0};
    CACHE_PAD_TYPES(std::atomic<uint64_t>);
    ALIGNED_CACHE std::atomic<int64_t> threshold_{0};
    CACHE_PAD_TYPES(std::atomic<int64_t>);
};

} // namespace algo

namespace queue {
template <typename T, typename Opt = meta::EmptyOptions>
using PSCQ = algo::PSCQ<T, Opt, linkage::None>;
}
