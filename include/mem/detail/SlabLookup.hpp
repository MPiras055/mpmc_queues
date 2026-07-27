#pragma once
#include <mem/SingleBlock.hpp>
#include <cassert>
#include <cstddef>
#include <memory>
#include <vector>

namespace mem::detail {

/**
 * @brief A fixed pool of single-block segments, addressed by index.
 *
 * Drop-in replacement for util::hazard::recycler::details::ImmutablePtrLookup, which
 * cannot be used here: it does `::new (&data_[i]) T(args...)`, constructing every
 * element *inline in one flat array*. A segment allocated by mem::SingleBlock owns a
 * trailing cell array inside its own block and has no such constructor, so each one must
 * be created separately and referred to by pointer.
 *
 * Same interface the recycler relies on -- construct with (count, args...) and index
 * with operator[] -- so it substitutes without touching the epoch logic.
 */
template <typename S>
class SlabLookup {
public:
    /**
     * @param count            number of segments in the pool
     * @param segment_capacity capacity of each
     */
    SlabLookup(std::size_t count, std::size_t segment_capacity) : slots_(count) {
        assert(count != 0 && "SlabLookup: pool must be non-empty");
        assert(segment_capacity != 0);
        for (std::size_t i = 0; i < count; ++i)
            slots_[i].reset(mem::SingleBlock<S>::create(segment_capacity));
    }

    SlabLookup(const SlabLookup&) = delete;
    SlabLookup& operator=(const SlabLookup&) = delete;
    SlabLookup(SlabLookup&&) = default;
    SlabLookup& operator=(SlabLookup&&) = default;

    [[nodiscard]] S* operator[](std::size_t idx) const noexcept {
        assert(idx < slots_.size() && "SlabLookup: index out of range");
        return slots_[idx].get();
    }

    [[nodiscard]] std::size_t capacity() const noexcept { return slots_.size(); }

private:
    std::vector<mem::unique_block<S>> slots_;
};

} // namespace mem::detail
