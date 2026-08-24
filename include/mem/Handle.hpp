#pragma once
/**
 * @file Handle.hpp
 * @brief How a segment refers to its successor: raw pointer, or pool index with a reuse counter.
 * @ingroup mem
 */

#include <util/bit.hpp>
#include <cstddef>
#include <cstdint>
#include <cassert>

namespace mem {

/**
 * @brief A pool slot index paired with a reuse counter, packed into 64 bits.
 * 
 * A pooled source which cannot avoid ABA internally (e.g., Hazard Pointers resolve this) cannot
 * safely refer to segments using pointers. It manages segments internally, using indeces to deref
 * to the data. Allows to pack a version tags next in the index word
 *
 * ABA is not solved using version tags only mitigated. Probability of ABA is proportional to the 
 * range which the version_tag can represent. Since pooled sources expose the size of the pool (considering 
 * a static pool of segments) as compile time property, its possible to compute the maximum number of version
 * tags while still addressing N indeces.
 *
 * ```
 *  63                                 index_bits              0
 * +--------------------------------------+---------------------+
 * |               version                |        index        |
 * +--------------------------------------+---------------------+
 * ```
 *
 * @note The index is capped at 32 bits, which is what puts a **floor** of 32 bits under
 *       the version.
 *
 * @note Index in the low bits and version above, so a default-constructed handle is
 *       `raw == 0` -- version 0, index 0 -- and version 0 is reserved for exactly that
 *       null handle. A live handle therefore never compares equal to nil.
 *
 * @tparam N: number of indeces in the range [0,N-1] that the struct is able to represent
 */
template <std::size_t N = 32>
struct VersionedIndex {
    static_assert(N >= 2, "a pool needs at least two slots to make progress");

    /// Bits needed to address `PoolSize` slots; at least one, so the layout is never empty.
    static constexpr unsigned index_bits =
        static_cast<unsigned>(bit::bit_width(N - 1) < 1 ? 1 : bit::bit_width(N - 1));

    static_assert(index_bits <= 32,
                  "the index must fit in 32 bits, so that the version keeps at least 32 and "
                  "stays an effective ABA guard");

    static constexpr unsigned version_bits = 64 - index_bits;

    /// Wide: version can be up to 62 bits 
    using version_type = uint64_t;
    /// Capped: index must be up to 32 bits
    using index_type = uint32_t;

    static constexpr uint64_t index_mask = (uint64_t{1} << index_bits) - 1;
    static constexpr version_type max_version = (~uint64_t{0}) >> index_bits;

    uint64_t raw = 0;

    /**
     * @brief: default constructor
     * @returns: VersionedIndex which resolves to version: 0 index: 0
     */
    constexpr VersionedIndex() = default;

    /**
     * @brief: constructor
     * @returns: VersionedIndex object which packs a (possible truncated, though consistent) 
     *  provided version and index
     */
    constexpr VersionedIndex(version_type version, index_type index) noexcept
        : raw{(static_cast<uint64_t>(version) << index_bits) |
              (static_cast<uint64_t>(index) & index_mask)} {
            assert((index & index_mask) == index && "VersionedIndex: index truncation");
        }

    /**
     * @brief: unpack the version field
     */
    constexpr version_type version() const noexcept { return raw >> index_bits; }

    /**
     * @brief: unpack the index field
     */
    constexpr index_type index() const noexcept {
        return static_cast<index_type>(raw & index_mask);
    }

    /// Comparator methods
    constexpr bool operator==(const VersionedIndex& o) const noexcept { return raw == o.raw; }
    constexpr bool operator!=(const VersionedIndex& o) const noexcept { return raw != o.raw; }

    /**
     * @brief Fold a free-running counter into a usable version.
     * 
     * @warning: to be an effective ABA prevention, the counter should (generally) be monotonic
     *
     * Truncates the counter to version bits skipping zero to have a reserved value
     */
    static constexpr version_type to_version(uint64_t counter) noexcept {
        const version_type v = counter & max_version;
        return v | (v == 0);
    }

    /**
     * @brief: get the version which logically comes after the one provided
     */
    static constexpr version_type next_version(version_type v) noexcept {
        return to_version(v + 1);
    }
};

/**
 * @brief: Direct Successor Handle
 * 
 * @note: the current node handle packs a pointer to the next node
 */
struct PtrHandle {
    template <typename S>
    using type = S*;
};

/**
 * @brief Address segments by pool slot rather than by pointer.
 * 
 * @note: the current node handle packs a VersionedIndex which indirectly matches
 * to the next node
 *
 * Parameterised by the pooled source static size
 */
template <std::size_t N>
struct IndexHandle {
    template <typename S>
    using type = VersionedIndex<N>;
};

} // namespace mem
