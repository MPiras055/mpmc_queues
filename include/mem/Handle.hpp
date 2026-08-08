#pragma once
#include <util/bit.hpp>
#include <cstddef>
#include <cstdint>

namespace mem {

/**
 * @brief A pool slot index paired with a reuse counter, packed into 64 bits.
 *
 * A pooled source hands out indices rather than pointers, so a stale handle would
 * otherwise be indistinguishable from a live one once the slot is reused (ABA). The
 * version half makes reuse observable: every hand-out of a slot carries a counter no
 * earlier holder can have seen.
 *
 * ## Why the split is sized by the pool
 *
 * The pool size is a compile-time property of `mem::source::Pool<S, N>`, so the number of
 * bits the index actually needs is known here too, and every bit not spent on the index is
 * a bit of ABA margin. A fixed 32/32 split spent half the word addressing eight slots:
 *
 * | pool | index bits | version bits |
 * |------|-----------:|-------------:|
 * | 2    | 1          | 63           |
 * | 8    | 3          | 61           |
 * | 1024 | 10         | 54           |
 * | 2^32 | 32         | 32           |
 *
 * ```
 *  63                                 index_bits              0
 * +--------------------------------------+---------------------+
 * |               version                |        index        |
 * +--------------------------------------+---------------------+
 * ```
 *
 * @note The index is capped at 32 bits, which is what puts a **floor** of 32 bits under
 *       the version. That is the useful direction of the constraint: a version much
 *       narrower than 32 bits stops being an effective ABA guard, because the counter can
 *       wrap within the lifetime of one stale reader. Capping the index is how the version
 *       is prevented from being squeezed below that.
 *
 * @note Index in the low bits and version above, so a default-constructed handle is
 *       `raw == 0` -- version 0, index 0 -- and version 0 is reserved for exactly that
 *       null handle. A live handle therefore never compares equal to nil.
 *
 * @tparam PoolSize number of slots the handle must address.
 */
template <std::size_t PoolSize>
struct VersionedIndex {
    static_assert(PoolSize >= 2, "a pool needs at least two slots to make progress");

    /// Bits needed to address `PoolSize` slots; at least one, so the layout is never empty.
    static constexpr unsigned index_bits =
        static_cast<unsigned>(bit::bit_width(PoolSize - 1) < 1 ? 1 : bit::bit_width(PoolSize - 1));

    static_assert(index_bits <= 32,
                  "the index must fit in 32 bits, so that the version keeps at least 32 and "
                  "stays an effective ABA guard");

    static constexpr unsigned version_bits = 64 - index_bits;

    /// Wide on purpose: a small pool leaves nearly the whole word to the counter.
    using version_type = uint64_t;
    /// Capped on purpose: this is what floors `version_bits` at 32.
    using index_type = uint32_t;

    static constexpr uint64_t index_mask = (uint64_t{1} << index_bits) - 1;
    static constexpr version_type max_version = (~uint64_t{0}) >> index_bits;

    uint64_t raw = 0;

    constexpr VersionedIndex() = default;

    constexpr VersionedIndex(version_type version, index_type index) noexcept
        : raw{(static_cast<uint64_t>(version) << index_bits) |
              (static_cast<uint64_t>(index) & index_mask)} {}

    constexpr version_type version() const noexcept { return raw >> index_bits; }
    constexpr index_type index() const noexcept {
        return static_cast<index_type>(raw & index_mask);
    }

    constexpr bool operator==(const VersionedIndex& o) const noexcept { return raw == o.raw; }
    constexpr bool operator!=(const VersionedIndex& o) const noexcept { return raw != o.raw; }

    /**
     * @brief Fold a free-running counter into a usable version.
     *
     * Truncates to `version_bits` and skips 0, which belongs to the null handle. Callers
     * keep an ordinary monotonic counter per slot and pass it through here rather than
     * having to know the layout.
     */
    static constexpr version_type to_version(uint64_t counter) noexcept {
        const version_type v = counter & max_version;
        return v == 0 ? 1 : v;
    }

    /// The version after @p v, skipping 0.
    static constexpr version_type next_version(version_type v) noexcept {
        return to_version(v + 1);
    }
};

/**
 * @brief How a segment refers to its successor.
 *
 * The `next` field's type depends on the *source*, not on the segment — which is why
 * the old segments carried a fourth `NextT` template parameter that every proxy had to
 * pass correctly. A handle policy moves the choice to the source, where it belongs.
 * `S*` naming its own enclosing incomplete type is legal.
 */
struct PtrHandle {
    template <typename S>
    using type = S*;
};

/**
 * @brief Address segments by pool slot rather than by pointer.
 *
 * Parameterised by the pool size because VersionedIndex is: a segment built for
 * `IndexHandle<8>` stores an eight-slot handle, and handing it to a `Pool<S, 16>` would
 * pair mismatched layouts. `proxy::LinkedProxy` static_asserts that the segment's
 * `handle_type` is the source's `handle`, so that mismatch is a diagnosable error rather
 * than an index silently read out of the wrong bits.
 */
template <std::size_t PoolSize>
struct IndexHandle {
    template <typename S>
    using type = VersionedIndex<PoolSize>;
};

} // namespace mem
