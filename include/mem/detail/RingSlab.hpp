#pragma once
#include <algo/LFring.hpp>
#include <mem/SingleBlock.hpp>
#include <cassert>
#include <cstddef>
#include <memory>
#include <vector>

namespace mem::detail {

/**
 * @brief A fixed set of independent index rings, addressed positionally.
 *
 * The epoch recycler needs several rings of slot indices: one per epoch stage, plus an
 * optional per-thread reuse cache. This owns them.
 *
 * Replaces the earlier queue::LFringSlab, which was built on the pre-refactor LFring and
 * was the last thing keeping include/segment/ alive (now removed). Rings are allocated
 * individually through mem::SingleBlock, like every other single-block object here.
 *
 * @note algo::LFring holds atomics and a const member, so it is neither copyable nor
 *       movable; the slab therefore stores owning pointers rather than values.
 */
class RingSlab {
public:
    using Ring = algo::LFring<>;

    /**
     * @param count    number of rings
     * @param capacity logical capacity of each, rounded up to a power of two
     */
    RingSlab(std::size_t count, std::size_t capacity) : rings_(count) {
        assert(count != 0 && "RingSlab: count must be non-null");
        assert(capacity > 1 && "RingSlab: each ring needs capacity >= 2");
        for (std::size_t i = 0; i < count; ++i) rings_[i].reset(Ring::create(capacity));
    }

    RingSlab(const RingSlab&) = delete;
    RingSlab& operator=(const RingSlab&) = delete;

    Ring* get(std::size_t idx) const noexcept {
        assert(idx < rings_.size() && "RingSlab: index out of range");
        return rings_[idx].get();
    }

    std::size_t count() const noexcept { return rings_.size(); }

private:
    std::vector<mem::unique_block<Ring>> rings_;
};

} // namespace mem::detail
