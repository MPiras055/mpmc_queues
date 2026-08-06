#pragma once
#include <util/bit.hpp>
#include <cstdint>

namespace mem {

/**
 * @brief A pool slot index paired with a reuse counter, packed into 64 bits.
 *
 * A pooled source hands out indices rather than pointers, so a stale handle could
 * otherwise be indistinguishable from a live one after the slot is reused (ABA). The
 * version half makes reuse observable.
 *
 * @note This used to exist twice: once at global scope inside BoundedMemProxy.hpp and
 *       once in a header nothing included. One definition, here.
 * 
 * @note: this can be further specialized since BoundedMemProxy knows the size of the pool at
 * compile time. We can add a template size_t and compute the necessary masks to extract the value
 * at rutime, this dictates that the Version and Index fields become their own data types, Version has
 * to be encoded with uint64_t (since at the best/worst case the pool manages 2 segments) so the version
 * can encode 62 bit value. For the index we can stick to uint32_t so to have a hard cap on the representation
 * range of Version (less than 32 bit versions may become ineffective for ABA mitigation). 
 */
struct VersionedIndex {
    uint64_t raw = 0;

    constexpr VersionedIndex() = default;
    VersionedIndex(uint32_t version, uint32_t index) : raw(bit::merge<uint64_t>(version, index)) {}

    uint32_t version() const noexcept { return bit::keep_high<uint32_t>(raw); }
    uint32_t index() const noexcept { return bit::keep_low<uint32_t>(raw); }

    constexpr bool operator==(const VersionedIndex& o) const noexcept { return raw == o.raw; }
    constexpr bool operator!=(const VersionedIndex& o) const noexcept { return raw != o.raw; }

    /// Version 0 is reserved for the null handle, so a fresh version is never 0.
    static constexpr uint32_t next_version(uint32_t v) noexcept {
        return v + (v + 1 == 0 ? 2 : 1);
    }
};

static_assert(sizeof(VersionedIndex) == sizeof(uint64_t));

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

struct IndexHandle {
    template <typename S>
    using type = VersionedIndex;
};

} // namespace mem
