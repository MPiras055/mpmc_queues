#pragma once
#include <cstddef>
#include <type_traits>
#include <limits>

namespace bit {

    // === Type Traits / Helpers ===

    /** * Helper to ensure T is an unsigned integer.
     * We use this in static_asserts for better error messages.
     */
    template <typename T>
    static constexpr bool is_unsigned_int_v = std::is_integral_v<T> && std::is_unsigned_v<T>;

    // === Constants (Templated) ===

    /**
     * @brief Returns the Most Significant Bit mask for type T.
     * E.g., for uint8_t: 0x80, for uint64_t: 0x8000000000000000
     */
    template <typename T>
    static constexpr T msb_mask = T(1) << (std::numeric_limits<T>::digits - 1);

    /**
     * @brief Returns the mask for all bits except the Most Significant Bit.
     */
    template <typename T>
    static constexpr T non_msb_mask = ~msb_mask<T>;

    // === General Bit Utilities ===

    /**
     * @brief Extracts the most significant bit from an integer.
     * @return The MSB only (e.g., 1000...000 if set, 0 otherwise).
     */
    template <typename T>
    [[nodiscard]] static constexpr T get_msb(T value) noexcept {
        static_assert(is_unsigned_int_v<T>, "T must be an unsigned integer");
        return value & msb_mask<T>;
    }

    /**
     * @brief Sets the most significant bit in an integer.
     */
    template <typename T>
    [[nodiscard]] static constexpr T set_msb(T value) noexcept {
        static_assert(is_unsigned_int_v<T>, "T must be an unsigned integer");
        return value | msb_mask<T>;
    }

    /**
     * @brief Clears the most significant bit in an integer.
     * Replaces clearMSB64/clearMSB32/get63LSB.
     */
    template <typename T>
    [[nodiscard]] static constexpr T clear_msb(T value) noexcept {
        static_assert(is_unsigned_int_v<T>, "T must be an unsigned integer");
        return value & non_msb_mask<T>;
    }

    // === Conversion / Truncation Utilities ===

    /**
     * @brief Casts a larger integer down to a smaller integer (keeping lower bits).
     * Usage: keep_low<uint32_t>(my_uint64);
     */
    template <typename OutT, typename InT>
    [[nodiscard]] static constexpr OutT keep_low(InT value) noexcept {
        static_assert(is_unsigned_int_v<InT> && is_unsigned_int_v<OutT>, "Types must be unsigned");
        static_assert(sizeof(OutT) <= sizeof(InT), "Output type should be smaller or equal to Input type");
        return static_cast<OutT>(value);
    }

    /**
     * @brief Keeps the upper bits of an integer by shifting and casting.
     * Usage: keep_high<uint32_t>(my_uint64);
     */
    template <typename OutT, typename InT>
    [[nodiscard]] static constexpr OutT keep_high(InT value) noexcept {
        static_assert(is_unsigned_int_v<InT> && is_unsigned_int_v<OutT>, "Types must be unsigned");
        static_assert(sizeof(OutT) < sizeof(InT), "Output type must be smaller than Input type to be useful");

        // Calculate how many bits we need to shift based on the size difference
        // or simply shift by the bit-width of the Output type if we assume strictly 2x split.
        constexpr size_t shift_amount = sizeof(OutT) * 8;
        return static_cast<OutT>(value >> shift_amount);
    }

    // === Merge & Split ===

    /**
     * @brief Merges two smaller integers into a generic larger integer.
     * @tparam OutT The resulting large type (e.g. uint64_t)
     * @param high The value for the upper bits.
     * @param low The value for the lower bits.
     */
    template <typename OutT, typename InT>
    [[nodiscard]] static constexpr OutT merge(InT high, InT low) noexcept {
        static_assert(is_unsigned_int_v<OutT> && is_unsigned_int_v<InT>, "Types must be unsigned");
        static_assert(sizeof(OutT) >= sizeof(InT) * 2, "Output type must be at least 2x larger than Input type");

        constexpr size_t shift = sizeof(InT) * 8;
        return (static_cast<OutT>(high) << shift) | static_cast<OutT>(low);
    }

    /**
     * @brief Splits a generic large integer into two smaller integers.
     * @param value The large input value.
     * @param high [out] The upper bits.
     * @param low [out] The lower bits.
     */
    template <typename InT, typename OutT>
    static constexpr void split(InT value, OutT& high, OutT& low) noexcept {
        static_assert(is_unsigned_int_v<OutT> && is_unsigned_int_v<InT>, "Types must be unsigned");

        low = static_cast<OutT>(value);

        constexpr size_t shift = sizeof(OutT) * 8;
        // Check to prevent shifting by width of type (UB) if InT == OutT
        if constexpr (shift < std::numeric_limits<InT>::digits) {
            high = static_cast<OutT>(value >> shift);
        } else {
            high = 0;
        }
    }

    // === Power of 2 Utilities ===

    template <typename T>
    [[nodiscard]] static constexpr bool is_pow2(T n) noexcept {
        static_assert(is_unsigned_int_v<T>, "T must be unsigned integral type");
        return n != 0 && ((n & (n - 1)) == 0);
    }

    /**
     * @brief Computes the next power of 2 (e.g., 3 -> 4, 5 -> 8).
     * Note: In C++20, consider using std::bit_ceil().
     */
    template <typename T>
    [[nodiscard]] static constexpr T next_pow2(T n) noexcept {
        static_assert(is_unsigned_int_v<T>, "T must be unsigned integral type");

        if (n == 0) return 1;
        if (is_pow2(n)) return n;

        n--; // Decrement to handle exact powers of 2 correctly

        // Unrolled loop logic generically for any type width
        constexpr size_t digits = std::numeric_limits<T>::digits;

        for (size_t shift = 1; shift < digits; shift <<= 1) {
            n |= n >> shift;
        }

        return n + 1;
    }

    /**
     * @brief Integer base-2 logarithm (floor).
     * Note: In C++20, consider using std::bit_width() - 1.
     */
    template<typename T>
    [[nodiscard]] static constexpr T log2(T n) noexcept {
        static_assert(is_unsigned_int_v<T>, "T must be unsigned integral type");
        T r = 0;
        while (n >>= 1) {
            ++r;
        }
        return r;
    }

    // C++17 compatible bit_width calculation (replaces std::bit_width from C++20)
    constexpr size_t bit_width(size_t n) {
        if (n == 0) return 0;
        size_t bits = 0;
        while (n > 0) {
            n >>= 1;
            bits++;
        }
        return bits;
    }

} // namespace bit
