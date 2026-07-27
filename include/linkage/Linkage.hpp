#pragma once
#include <mem/Handle.hpp>
#include <util/specs.hpp>
#include <atomic>

namespace linkage {

/**
 * @brief Does this policy give the algorithm a successor pointer?
 *
 * Constrain an algorithm on this when it is only *correct* as a linked segment. Some of
 * these algorithms have no way to recover capacity on their own -- FAAArray and HQ write
 * each cell once and never reset the indices, so standalone they accept exactly one
 * fill/drain cycle and refuse everything afterwards; PRQ closes itself on overshoot and
 * relies on a proxy to link a successor rather than spin. Expressing that as a constraint
 * rather than a comment means the unsound configuration cannot be named at all.
 */
template <typename Link>
concept Linked = Link::is_linked;

/// The inverse: an algorithm that is only meaningful as a standalone queue.
template <typename Link>
concept Standalone = !Link::is_linked;

/**
 * @brief Not linked: a standalone bounded queue.
 *
 * `state` is empty, so under [[no_unique_address]] the algorithm pays nothing at all
 * for being linkable — no next pointer, no cache line, and every `if constexpr
 * (Link::is_linked)` branch compiles out.
 */
struct None {
    static constexpr bool is_linked = false;

    template <typename S>
    struct state {
        using handle = void*; ///< never used; present only so the alias is well-formed
    };
};

/**
 * @brief Linked: the segment carries a successor handle.
 *
 * The handle type comes from the *source* (a pointer source hands out `S*`, a pooled
 * one hands out VersionedIndex), which is why it is a policy here rather than a fourth
 * template parameter every proxy had to remember to pass in the right slot.
 *
 * @tparam HandlePolicy mem::PtrHandle or mem::IndexHandle
 */
template <typename HandlePolicy = mem::PtrHandle>
struct Node {
    static constexpr bool is_linked = true;

    template <typename S>
    struct state {
        using handle = typename HandlePolicy::template type<S>;

        static constexpr handle nil() noexcept { return handle{}; }

        handle next() const noexcept { return next_.load(std::memory_order_acquire); }

        /// CAS nil -> h. Succeeds for exactly one caller.
        bool link_next(handle h) noexcept {
            handle expected = nil();
            return next_.compare_exchange_strong(expected, h, std::memory_order_acq_rel,
                                                 std::memory_order_acquire);
        }

        /// Detach, so a recycled segment does not carry a stale successor.
        void unlink() noexcept { next_.store(nil(), std::memory_order_relaxed); }

    private:
        ALIGNED_CACHE std::atomic<handle> next_{handle{}};
        CACHE_PAD_TYPES(std::atomic<handle>);
    };
};

} // namespace linkage
