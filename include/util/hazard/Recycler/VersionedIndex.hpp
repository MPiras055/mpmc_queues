#pragma once
#include <cstdint>
#include <cassert>
#include <bit.hpp>

namespace util::hazard::recycler::details {

/**
 * @brief A packed, atomic-compatible 64-bit structure storing an Index and a Version.
 * * This struct dynamically calculates the bit-width required for the Index based on
 * the template Capacity. The remaining bits are used for the Version (ABA prevention).
 * * Layout: [ Version (High Bits) | Index (Low Bits) ]
 * * @tparam Capacity The maximum number of valid elements.
 * Valid Indices: [0, Capacity - 1].
 * Reserved Value: Capacity.
 */
template<size_t Capacity>
struct VersionedIndex {

    using Version   = uint64_t;
    using Index     = uint64_t;

    // =========================================================================
    // Constants & Bit Calculation
    // =========================================================================

    /**
     * Calculate bits needed to represent numbers up to 'Capacity' inclusive.
     * Example: Capacity = 4. Indices 0,1,2,3. Reserved = 4 (binary 100).
     * We need to store values 0..4. detail::bit_width(4) = 3 bits.
     */
    static constexpr size_t INDEX_BITS = bit::bit_width(Capacity);
    static constexpr size_t VERSION_BITS = 64 - INDEX_BITS;

    // Sanity checks
    static_assert(INDEX_BITS > 0, "VersionedIndex: Null Capacity");
    static_assert(INDEX_BITS < 64, "VersionedIndex: Index bits overflow");

    static constexpr Index      INDEX_MASK = (1ULL << INDEX_BITS) - 1;
    static constexpr Version    VERSION_MASK = ~INDEX_MASK;

    // The specific value used to indicate "None" or "Reserved"
    static constexpr Index RESERVED_VAL = Capacity;

    // =========================================================================
    // Data
    // =========================================================================

    uint64_t raw_;

    // =========================================================================
    // Constructors
    // =========================================================================

    /**
     * @brief Default Constructor.
     * Initializes the Index to RESERVED_VAL (Capacity) and Version to 0.
     * This represents a "Null" or "Empty" state.
     */
    constexpr VersionedIndex() noexcept : raw_(RESERVED_VAL) {}

    constexpr VersionedIndex(Version ver, Index idx) noexcept {
        assert(idx <= Capacity && "VersionedIndex: Index out of range");
        assert(ver >> VERSION_BITS == 0 && "VersionedIndex: Version out of range");

        // Pack: (Version << shift) | Index
        raw_ = (ver << INDEX_BITS) | (idx & INDEX_MASK);
    }

    // Explicit conversion from raw uint64_t (useful for atomic CAS debugging)
    explicit constexpr VersionedIndex(uint64_t raw_val) noexcept : raw_(raw_val) {}

    // =========================================================================
    // Accessors
    // =========================================================================

    [[nodiscard]] constexpr Index index() const noexcept {
        return raw_ & INDEX_MASK;
    }

    [[nodiscard]] constexpr Version version() const noexcept {
        return raw_ >> INDEX_BITS;
    }

    [[nodiscard]] constexpr bool isReserved() const noexcept {
        return index() == RESERVED_VAL;
    }

    /**
     * @brief Returns the raw 64-bit representation.
     * Useful for atomic CAS operations.
     */
    [[nodiscard]] constexpr uint64_t raw() const noexcept {
        return raw_;
    }

    // =========================================================================
    // Modifiers (Mutable, Non-constexpr)
    // =========================================================================

    /**
     * @brief Sets the index, preserving the current version.
     */
    void setIndex(Index new_idx) noexcept {
        assert(new_idx <= Capacity);
        raw_ = (raw_ & VERSION_MASK) | (new_idx & INDEX_MASK);
    }

    /**
     * @brief Sets the version, preserving the current index.
     */
    void setVersion(Version new_ver) noexcept {
        assert(new_ver >> VERSION_BITS == 0 && "VersionedIndex: version out of range");
        uint64_t ver_part = (new_ver << INDEX_BITS) & VERSION_MASK;
        raw_ = ver_part | (raw_ & INDEX_MASK);
    }

    /**
     * @brief Sets index to RESERVED, preserving the current version.
     */
    void setReserved() noexcept {
        raw_ = (raw_ & VERSION_MASK) | RESERVED_VAL;
    }

    /**
     * @brief Increments the version.
     * Effectively adds 1 to the high bits.
     */
    void advanceVersion() noexcept {
        // Adding (1 << INDEX_BITS) effectively adds 1 to the upper Version part
        raw_ += (1ULL << INDEX_BITS);
    }

    // =========================================================================
    // Operators (C++17 Manual Implementation)
    // =========================================================================

    constexpr bool operator==(const VersionedIndex& other) const noexcept {
        return raw_ == other.raw_;
    }

    constexpr bool operator!=(const VersionedIndex& other) const noexcept {
        return raw_ != other.raw_;
    }
};

} // namespace util::hazard::recycler
