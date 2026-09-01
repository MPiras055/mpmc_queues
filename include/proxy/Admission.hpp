#pragma once
/**
 * @file Admission.hpp
 * @brief The admission policies: unbounded, item-bounded, segment-bounded.
 * @ingroup proxy
 */

#include <core/Admission.hpp>
#include <util/align.hpp>
#include <util/specs.hpp>
#include <atomic>
#include <cstddef>
#include <type_traits>

namespace proxy::admit {

/**
 * @brief No bound: admit everything.
 *
 * An empty struct apart from its (empty) config, so under [[no_unique_address]] an
 * unbounded proxy pays nothing for the policy hook -- no counters, no cache lines, no
 * atomics on the hot path.
 */
struct None {
    struct Config {};
    static constexpr Config config(std::size_t /*segment*/) noexcept { return {}; }

    /// Whether this policy can ever refuse. `if constexpr`-tested, so an unbounded proxy
    /// emits no admission check at all.
    static constexpr bool bounded = false;
    /// Never asked, since `bounded` is false; a value is still required by the concept.
    static constexpr core::AdmitPoint admit_point = core::AdmitPoint::Enqueue;

    /// 0: unbounded, so there is no segment count to split a capacity across. The proxy falls
    /// back on the source's own limit, which is the pool size for a pooled source and 1 for an
    /// allocating one.
    static constexpr std::size_t live_segments() noexcept { return 0; }

    constexpr explicit None(Config) noexcept {}

    /// @brief Ask, and reserve where the policy can.
    /// @return false to refuse the enqueue.
    constexpr bool try_admit() noexcept { return true; }
    /// Give back a reservation try_admit() took, when the enqueue then failed anyway.
    constexpr void cancel_admit() noexcept {}
    /// @return The ceiling, in whatever unit this policy counts.
    constexpr std::size_t bound() const noexcept { return 0; } ///< 0 == unbounded
    /**
     * @brief 0, meaning *no opinion* -- ask the structure instead.
     *
     * Not "zero capacity". This policy imposes no ceiling, so the real one comes from how many
     * segments can be live at once, which only the proxy can work out (it is the smaller of this
     * policy's limit and the source's). Answering with one segment's worth here, as this used
     * to, was wrong for a pooled source and forced `LinkedProxy::capacity()` to special-case it.
     */
    constexpr std::size_t capacity(std::size_t /*segment*/) const noexcept { return 0; }

    /// @name Traversal hooks. Called by LinkedProxy as it observes each event.
    /// @{
    constexpr void on_enqueue() noexcept {}
    constexpr void on_dequeue() noexcept {}
    constexpr void on_segment_linked() noexcept {}
    constexpr void on_segment_retired() noexcept {}
    /// @}
};

/**
 * @brief Bound the number of live items.
 *
 * Two counters rather than one, so producers and consumers touch different cache lines;
 * the difference is the occupancy. Replaces BoundedCounterProxy.
 */
template <std::size_t Chunks = 4>
class ItemCount {
public:
    static_assert(Chunks != 0, "admit::ItemCount: a queue spread over zero segments holds nothing");

    struct Config {
        std::size_t items;
    };
    /// The segment count times what a segment actually holds is how many items fit. Taken from
    /// the *rounded* segment capacity, so the bound is a figure the queue can really reach.
    static constexpr Config config(std::size_t segment) noexcept { return {segment * Chunks}; }

    /// Whether this policy can ever refuse. `if constexpr`-tested, so an unbounded proxy
    /// emits no admission check at all.
    static constexpr bool bounded = true;
    /**
     * Up front, and it has to be. This policy *reserves*: the ticket is taken before the
     * traversal commits, so concurrent producers cannot each pass a test and then all commit
     * past the ceiling. Measured on the check-then-act version, 4 producers against a bound
     * of 256: peak occupancy 257.
     */
    static constexpr core::AdmitPoint admit_point = core::AdmitPoint::Enqueue;

    /// The bound is in items, but the segments it is spread over are still the divisor.
    static constexpr std::size_t live_segments() noexcept { return Chunks; }

    explicit ItemCount(Config c) noexcept : bound_{c.items} {}

    /**
     * Reserve a slot, or refuse.
     *
     * The ticket comes from the fetch_add, so no two producers can claim the same one; a
     * losing claim is rolled back immediately. `popped_` only ever grows, so reading a
     * stale (smaller) value overstates occupancy and errs towards refusing -- the safe
     * direction. Together those give a hard ceiling rather than one that leaks by the
     * number of concurrent producers.
     */
    bool try_admit() noexcept {
        const uint64_t ticket = pushed_.fetch_add(1, std::memory_order_acq_rel);
        if (ticket - popped_.load(std::memory_order_acquire) >= bound_) {
            pushed_.fetch_sub(1, std::memory_order_release);
            return false;
        }
        return true;
    }

    /// Give back a reservation try_admit() took, when the enqueue then failed anyway.
    void cancel_admit() noexcept { pushed_.fetch_sub(1, std::memory_order_release); }

    /// @return The ceiling, in whatever unit this policy counts.
    std::size_t bound() const noexcept { return bound_; }
    std::size_t capacity(std::size_t /*segment*/) const noexcept { return bound_; }

    /// The reservation in try_admit() already counted this item.
    void on_enqueue() noexcept {}
    void on_dequeue() noexcept { popped_.fetch_add(1, std::memory_order_release); }
    void on_segment_linked() noexcept {}
    void on_segment_retired() noexcept {}
    /// @}

private:
    //Two separate counters because of the frequency of the ops
    CACHE_LINE_MEMBER(std::atomic<uint64_t>, pushed_, {0});
    CACHE_LINE_MEMBER(std::atomic<uint64_t>, popped_, {0});
    const std::size_t bound_;
};

/**
 * @brief Bound the number of live segments.
 *
 * Coarser than ItemCount -- the real ceiling is bound() * segment_capacity -- but the
 * counters move once per segment rather than once per item, so the hot path is untouched.
 * Replaces BoundedChunkProxy.
 */
template <std::size_t Chunks = 4>
class SegmentCount {
public:
    static_assert(Chunks != 0, "admit::SegmentCount: a bound of zero segments admits nothing");

    struct Config {
        std::size_t segments;
    };
    /// This policy counts segments, so the segment count *is* the bound.
    static constexpr Config config(std::size_t /*segment*/) noexcept { return {Chunks}; }

    /// Whether this policy can ever refuse. `if constexpr`-tested, so an unbounded proxy
    /// emits no admission check at all.
    static constexpr bool bounded = true;

    /**
     * At the link, which is what makes this policy structurally different from ItemCount
     * rather than merely differently parameterised.
     *
     * Asked at `Enqueue` it answered the wrong question. Whether an enqueue causes a segment
     * to be linked is not known until the tail refuses the item, and most enqueues link
     * nothing -- so an up-front check refuses while the tail still has free slots. Measured
     * on 64-slot segments: `chunks = 8` held 449 of an advertised 512, and `chunks = 1` held
     * **zero** of 64, because one segment's worth of bound left no room to link even the
     * first successor. Asked here, all of them reach their stated capacity.
     *
     * It is also strictly less work: at the ceiling the proxy skips the whole
     * acquire/reopen/discard round trip instead of performing it and undoing it.
     */
    static constexpr core::AdmitPoint admit_point = core::AdmitPoint::SegmentLink;

    /// This policy counts segments, so the count is both the bound and the divisor.
    static constexpr std::size_t live_segments() noexcept { return Chunks; }

    explicit SegmentCount(Config c) noexcept : bound_{c.segments ? c.segments : 1} {}

    /**
     * @brief May one more segment be linked?
     * @return false once the segment ceiling is reached.
     *
     * Tests rather than reserves, and at this call site that is sound: the proxy asks
     * immediately before acquiring, and if it loses the `link_next` race it discards and
     * asks again. The ceiling is therefore approximate to within the number of producers
     * linking concurrently -- which bounds memory, the property this policy exists for,
     * without pretending to an exactness it cannot provide.
     *
     * Live segments are `linked_ + 1` counting the sentinel, so linking one more makes
     * `linked_ + 2`, and keeping that within `bound_` is exactly the expression below.
     */
    bool try_admit() noexcept {
        const uint64_t linked = linked_.load(std::memory_order_relaxed);
        return (linked + 1) < bound_;
    }

    /// Nothing to give back: try_admit() reserves nothing.
    void cancel_admit() noexcept {}

    /// @return The ceiling, in whatever unit this policy counts.
    std::size_t bound() const noexcept { return bound_; }
    std::size_t capacity(std::size_t segment) const noexcept { return bound_ * segment; }

    /// @name Traversal hooks. Called by LinkedProxy as it observes each event.
    /// @{
    void on_enqueue() noexcept {}
    void on_dequeue() noexcept {}
    void on_segment_linked() noexcept { linked_.fetch_add(1, std::memory_order_release); }
    void on_segment_retired() noexcept { linked_.fetch_sub(1, std::memory_order_release); }
    /// @}

private:
    const std::size_t bound_;
    //single counter due to the low frequency of updates
    CACHE_LINE_MEMBER(std::atomic<uint64_t>, linked_, {0});
};

static_assert(core::AdmissionPolicy<None>);
static_assert(core::AdmissionPolicy<ItemCount<>>);
static_assert(core::AdmissionPolicy<SegmentCount<>>);
/// Empty on purpose: [[no_unique_address]] then erases the policy hook entirely from an
/// unbounded or pooled proxy, where the ceiling is the source's business rather than a rule.
static_assert(std::is_empty_v<None>);

} // namespace proxy::admit
