#pragma once
/**
 * @file Strategy.hpp
 * @brief How a block handle picks which of its D buffers to try next.
 * @ingroup block
 *
 * Both policies honour the one obligation that matters:
 *
 * > `enqueue` may report full **only** when all D buffers refused, and `dequeue` may report
 * > empty only when all D were empty.
 *
 * Anything weaker breaks the benchmark's final drain (`while (q.try_dequeue(out)) {}`), which
 * would stop early and be reported as lost items -- a silent wrong answer, not a crash.
 *
 * The cursor they move lives in the handle, not in the block, so it is thread-private by
 * construction and a strategy costs nothing in coherence traffic. The common case in both is one
 * buffer touched; the O(D) walk happens only on the refusal that is about to report anyway.
 */

#include <util/specs.hpp>
#include <cstddef>

namespace block {

/// Advance after every attempt, successful or not: spreads items evenly across the buffers.
struct RoundRobin {
    template <typename Attempt>
    static FORCE_INLINE bool run(std::size_t& cursor, std::size_t degree, Attempt&& attempt) noexcept {
        for (std::size_t n = 0; n < degree; ++n) {
            const bool ok = attempt(cursor);
            if (++cursor == degree) cursor = 0;
            if (ok) return true;
        }
        return false;
    }
};

/// Prioritised: keep using the current buffer while it accepts, move on only when it refuses.
struct Sticky {
    template <typename Attempt>
    static FORCE_INLINE bool run(std::size_t& cursor, std::size_t degree, Attempt&& attempt) noexcept {
        for (std::size_t n = 0; n < degree; ++n) {
            if (attempt(cursor)) return true;
            if (++cursor == degree) cursor = 0;
        }
        return false;
    }
};

} // namespace block
