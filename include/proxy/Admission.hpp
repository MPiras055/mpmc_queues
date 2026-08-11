#pragma once
#include <core/Admission.hpp>
#include <util/specs.hpp>
#include <atomic>
#include <cstddef>

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
    static constexpr Config config(std::size_t /*segment*/, std::size_t /*chunks*/) noexcept {
        return {};
    }

    static constexpr bool bounded = false;

    constexpr explicit None(Config) noexcept {}

    constexpr bool try_admit() noexcept { return true; }
    constexpr void cancel_admit() noexcept {}
    constexpr std::size_t bound() const noexcept { return 0; } ///< 0 == unbounded
    /// Nothing caps the total, so the useful number is what one segment holds.
    constexpr std::size_t capacity(std::size_t segment) const noexcept { return segment; }

    constexpr void on_enqueue() noexcept {}
    constexpr void on_dequeue() noexcept {}
    constexpr void on_segment_linked() noexcept {}
    constexpr void on_segment_retired() noexcept {}
};

/**
 * @brief Bound the number of live items.
 *
 * Two counters rather than one, so producers and consumers touch different cache lines;
 * the difference is the occupancy. Replaces BoundedCounterProxy.
 */
class ItemCount {
public:
    struct Config {
        std::size_t items;
    };
    /// A chunk count times the segment capacity is how many items fit.
    static constexpr Config config(std::size_t segment, std::size_t chunks) noexcept {
        return {segment * chunks};
    }

    static constexpr bool bounded = true;

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

    void cancel_admit() noexcept { pushed_.fetch_sub(1, std::memory_order_release); }

    std::size_t bound() const noexcept { return bound_; }
    std::size_t capacity(std::size_t /*segment*/) const noexcept { return bound_; }

    /// The reservation in try_admit() already counted this item.
    void on_enqueue() noexcept {}
    void on_dequeue() noexcept { popped_.fetch_add(1, std::memory_order_release); }
    void on_segment_linked() noexcept {}
    void on_segment_retired() noexcept {}

private:
    //Two separate counters because of the frequency of the ops
    ALIGNED_CACHE std::atomic<uint64_t> pushed_{0};
    CACHE_PAD_TYPES(std::atomic<uint64_t>);
    ALIGNED_CACHE std::atomic<uint64_t> popped_{0};
    CACHE_PAD_TYPES(std::atomic<uint64_t>);
    const std::size_t bound_;
};

/**
 * @brief Bound the number of live segments.
 *
 * Coarser than ItemCount -- the real ceiling is bound() * segment_capacity -- but the
 * counters move once per segment rather than once per item, so the hot path is untouched.
 * Replaces BoundedChunkProxy.
 */
class SegmentCount {
public:
    struct Config {
        std::size_t segments;
    };
    /// This policy counts segments, so the chunk count *is* the bound.
    static constexpr Config config(std::size_t /*segment*/, std::size_t chunks) noexcept {
        return {chunks};
    }

    static constexpr bool bounded = true;

    explicit SegmentCount(Config c) noexcept : bound_{c.segments ? c.segments : 1} {}

    /**
     * Tests rather than reserves.
     *
     * Unlike an item count there is nothing to reserve here: whether this enqueue causes
     * a segment to be linked is not known until the current tail refuses it, and most
     * calls link nothing. The ceiling is therefore approximate to within the number of
     * producers that link concurrently -- which bounds memory, the property this policy
     * exists for, without pretending to an exactness it cannot provide.
     */
    bool try_admit() noexcept {
        const uint64_t linked = linked_.load(std::memory_order_relaxed);
        return (linked + 1) < bound_;
    }

    void cancel_admit() noexcept {}

    std::size_t bound() const noexcept { return bound_; }
    std::size_t capacity(std::size_t segment) const noexcept { return bound_ * segment; }

    void on_enqueue() noexcept {}
    void on_dequeue() noexcept {}
    void on_segment_linked() noexcept { linked_.fetch_add(1, std::memory_order_release); }
    void on_segment_retired() noexcept { linked_.fetch_sub(1, std::memory_order_release); }

private:
    const std::size_t bound_;
    //single counter due to the low frequency of updates
    ALIGNED_CACHE std::atomic<uint64_t> linked_{0};
    CACHE_PAD_TYPES(std::atomic<uint64_t>);
};

static_assert(core::AdmissionPolicy<None>);
static_assert(core::AdmissionPolicy<ItemCount>);
static_assert(core::AdmissionPolicy<SegmentCount>);

} // namespace proxy::admit
