#pragma once
#include <mem/Align.hpp>
#include <util/specs.hpp>
#include <array>
#include <cstddef>

namespace mem {

/// One trailing sub-array carved out of a single block.
struct Region {
    std::size_t offset = 0;
    std::size_t bytes = 0;

    constexpr std::size_t end() const noexcept { return offset + bytes; }
};

/**
 * @brief A computed single-block layout: N trailing regions after the header.
 *
 * N regions, not one. SCQ carves *three* out of one block — two index rings plus the
 * payload buffer — which it previously did with a bump-allocator overload that mutated
 * a running size and returned the next free address. A declarative plan replaces that,
 * and unlike the bump pointer it can be checked.
 */
template <std::size_t N>
struct Plan {
    std::array<Region, N> regions{};
    std::size_t total = 0;
    std::size_t block_align = 0;

    /**
     * @brief Does this layout actually hold together?
     *
     * Regions must start after the header, not overlap each other, and stay inside the
     * block. Because plan() is constexpr this is a compile-time check, which is what
     * turns "we hope the offset arithmetic is right" into "it does not build if it is
     * not".
     */
    constexpr bool valid(std::size_t header_bytes) const noexcept {
        std::size_t prev_end = header_bytes;
        for (const Region& r : regions) {
            if (r.offset < prev_end) return false; // overlaps the header or the previous region
            if (r.end() > total) return false;     // runs off the end of the block
            prev_end = r.end();
        }
        return block_align != 0 && is_pow2(block_align) && total % block_align == 0;
    }
};

/**
 * @brief Accumulates a layout: header first, then regions in declaration order.
 */
class LayoutBuilder {
    std::size_t cursor_;
    std::size_t align_;

public:
    constexpr LayoutBuilder(std::size_t header_bytes, std::size_t header_align) noexcept
        : cursor_{header_bytes}, align_{header_align} {}

    /// Append a region of @p bytes at alignment @p a, advancing the cursor.
    constexpr Region add(std::size_t bytes, std::size_t a) noexcept {
        cursor_ = align_up(cursor_, a);
        if (a > align_) align_ = a;
        const Region r{cursor_, bytes};
        cursor_ += bytes;
        return r;
    }

    /// Whole-block alignment: at least a cache line, more if some region demands it.
    constexpr std::size_t block_align() const noexcept {
        return align_ > CACHE_LINE ? align_ : CACHE_LINE;
    }

    /// Total block size. Rounded to block_align, which std::aligned_alloc requires.
    constexpr std::size_t total() const noexcept { return align_up(cursor_, block_align()); }
};

/// A raw block, ready to have its regions addressed.
struct Blocks {
    void* base = nullptr;

    template <typename T>
    T* at(Region r) const noexcept {
        return reinterpret_cast<T*>(static_cast<std::byte*>(base) + r.offset);
    }
};

} // namespace mem
