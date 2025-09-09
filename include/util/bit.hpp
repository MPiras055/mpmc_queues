#pragma once
#include <cstdint>
#include <iostream> 

namespace bit {

    // === Constants ===

    /// Most Significant Bit mask for 64-bit unsigned integer
    static constexpr uint64_t MSB64 = 1ull << 63;

    /// Mask to keep only the least significant 63 bits in a 64-bit unsigned integer
    static constexpr uint64_t LSB63_MASK = ~MSB64;

    /// Most Significant Bit mask for 32-bit unsigned integer
    static constexpr uint32_t MSB32 = 1ul << 31;

    /// Mask to keep only the least significant 31 bits in a 32-bit unsigned integer
    static constexpr uint32_t LSB31_MASK = ~MSB32;

    // === 64-bit Utilities ===

    /**
     * @brief Extracts the most significant bit from a 64-bit unsigned integer.
     * @param value The 64-bit unsigned integer.
     * @return The MSB only (0x8000000000000000 if set, 0 otherwise).
     */
    static inline uint64_t getMSB64(uint64_t value) noexcept {
        return value & MSB64;
    }

    /**
     * @brief Sets the most significant bit in a 64-bit unsigned integer.
     * @param value The 64-bit unsigned integer.
     * @return The value with the MSB set.
     */
    static inline uint64_t setMSB64(uint64_t value) noexcept {
        return value | MSB64;
    }

    /**
     * @brief Clears the most significant bit in a 64-bit unsigned integer.
     * @param value The 64-bit unsigned integer.
     * @return The value with the MSB cleared.
     */
    static inline uint64_t clearMSB64(uint64_t value) noexcept {
        return value & LSB63_MASK;
    }

    /**
     * @brief Extracts the least significant 63 bits of a 64-bit unsigned integer.
     * @param value The 64-bit unsigned integer.
     * @return The value with the MSB cleared.
     */
    static inline uint64_t get63LSB(uint64_t value) noexcept {
        return value & LSB63_MASK;
    }

    // === 32-bit Utilities ===

    /**
     * @brief Extracts the most significant bit from a 32-bit unsigned integer.
     * @param value The 32-bit unsigned integer.
     * @return The MSB only (0x80000000 if set, 0 otherwise).
     */
    static inline uint32_t getMSB32(uint32_t value) noexcept {
        return value & MSB32;
    }

    /**
     * @brief Sets the most significant bit in a 32-bit unsigned integer.
     * @param value The 32-bit unsigned integer.
     * @return The value with the MSB set.
     */
    static inline uint32_t setMSB32(uint32_t value) noexcept {
        return value | MSB32;
    }

    /**
     * @brief Clears the most significant bit in a 32-bit unsigned integer.
     * @param value The 32-bit unsigned integer.
     * @return The value with the MSB cleared.
     */
    static inline uint32_t clearMSB32(uint32_t value) noexcept {
        return value & LSB31_MASK;
    }

    /**
     * @brief Extracts the least significant 31 bits of a 32-bit unsigned integer.
     * @param value The 32-bit unsigned integer.
     * @return The value with the MSB cleared.
     */
    static inline uint32_t get31LSB(uint32_t value) noexcept {
        return value & LSB31_MASK;
    }

    // === Conversion Utilities ===

    /**
     * @brief Keeps the lower 32 bits of a 64-bit unsigned integer.
     * @param value The 64-bit unsigned integer.
     * @return The least significant 32 bits as a 32-bit unsigned integer.
     */
    static inline uint32_t keep_low(uint64_t value) noexcept {
        return static_cast<uint32_t>(value);
    }

    /**
     * @brief Keeps the upper 32 bits of a 64-bit unsigned integer.
     * @param value The 64-bit unsigned integer.
     * @return The most significant 32 bits as a 32-bit unsigned integer.
     */
    static inline uint32_t keep_high(uint64_t value) noexcept {
        return static_cast<uint32_t>(value >> 32);
    }

    // === Merge & Split ===

    /**
     * @brief Merges two 32-bit unsigned integers into a single 64-bit value.
     * @param high The value to place in the upper 32 bits.
     * @param low The value to place in the lower 32 bits.
     * @return A 64-bit unsigned integer with 'high' in bits 32-63 and 'low' in bits 0-31.
     */
    static inline uint64_t merge(uint32_t high, uint32_t low) noexcept {
        return (static_cast<uint64_t>(high) << 32) | static_cast<uint64_t>(low);
    }

    /**
     * @brief Splits a 64-bit unsigned integer in 2 32_bit signed integers
     */
    static inline void split(uint64_t value, uint32_t& high, uint32_t& low) noexcept {
        low     = static_cast<uint32_t>(value);
        high    = value >> 32;
    }

    template <typename T>
    constexpr bool is_power_of_two(T n) {
        static_assert(std::is_unsigned_v<T>, "T must be unsigned integral type");
        return n != 0 && ( (n & (n - 1)) == 0 );
    }

    template <typename T>
    constexpr T next_power_of_two(T n) {
        static_assert(std::is_unsigned_v<T>, "T must be unsigned integral type");

        if (n == 0) return 1;

        if (is_power_of_two(n)) return n;

        // Compute next power of 2
        n--;
        
        // Number of bits in T
        constexpr unsigned bits = sizeof(T) * 8;

        // The bit-shifts below should cover all bits of T,
        // so we do a loop or unroll for powers of two <= bits
        for (unsigned shift = 1; shift < bits; shift <<= 1) {
            n |= n >> shift;
        }

        n++;
        return n;
    }

    template<typename T>
    constexpr T log2(T n) {
        static_assert(std::is_unsigned_v<T>,"T must be unsigned integral type");
        T r = 0;
        while (n >>= 1) ++r;
        return r;
    }


} // namespace bit
