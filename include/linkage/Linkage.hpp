#pragma once
/**
 * @file Linkage.hpp
 * @brief Whether an algorithm carries a successor handle, and of what type.
 * @ingroup linkage
 */

#include <mem/Handle.hpp>
#include <util/align.hpp>
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
 * The handle type comes from the *source* (e.g., a pointer source hands out `S*`, a pooled
 * one hands out VersionedIndex), which is why it is a policy here rather than a fourth
 * template parameter every proxy had to remember to pass in the right slot.
 *
 * @tparam HandlePolicy (see mem::PtrHandle or mem::IndexHandle)
 */
template <typename HandlePolicy>
struct Node {
    static constexpr bool is_linked = true;

    template <typename S>
    struct state {
        // templated handle type
        using handle = typename HandlePolicy::template type<S>;

        // null handle
        static constexpr handle nil() noexcept { return handle{}; }

        // getter method for the next field
        handle next() const noexcept { return next_.load(std::memory_order_acquire); }

        /**
         * @brief CAS nil -> @p h. Succeeds for exactly one caller.
         *
         * @param h       the successor to install.
         * @param current always set to the successor that is installed when this returns:
         *                @p h on success, the winner's handle on failure.
         * @return true for the one caller that installed it.
         *
         * The out-parameter is free. A failing `compare_exchange_strong` writes the value it
         * actually found into `expected`, so the loser already holds the winner's handle and
         * needs no second acquire load of `next_` -- which it would otherwise do on exactly
         * the path where producers are colliding and that line is contended.
         *
         * Set on the success path too, rather than only on failure: one store on a path that
         * runs once per segment, against a parameter whose meaning would otherwise depend on
         * the return value.
         */

        /**
         * @brief link a handle to the current one
         * @param h     successor handle to install
         * @param c     reference which is updated with the current
         *  `next` field if the method fails
         * @returns: true if the link was successful, false otherwise
         * setting the reference 
         */
        bool link_next(handle h, handle& c) noexcept {
            assert(c == nil() && "Provided handle reference is not nil()");
            return (next_.compare_exchange_strong(c, h, std::memory_order_acq_rel,
                                              std::memory_order_acquire));
        }

        /**
         * @brief: unlink the successor handle
         */
        void unlink() noexcept { next_.store(nil(), std::memory_order_relaxed); }

    private:
        CACHE_LINE_MEMBER(std::atomic<handle>, next_, {handle{}});
    };
};

} // namespace linkage
