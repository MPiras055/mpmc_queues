//
// NOT CURRENTLY USED IN ANY SEGMENT IMPLEMENTATION
//
// #pragma once
// #include <atomic>
// #include <specs.hpp>
// #include <bit.hpp>

// /**
//  * @brief A compact cell that packs:
//  *        - a 32-bit value,
//  *        - a 31-bit sequence number,
//  *        - and a 1-bit "safe" flag
//  *        into a single 64-bit atomic field.
//  *
//  * Layout of the 64-bit word:
//  *
//  *  +-----------------+--------+-----------------+
//  *  |   seq (31 bits) | safe   | value (32 bits) |
//  *  +-----------------+--------+-----------------+
//  *
//  *     bits 63..33      bit 32     bits 31..0
//  *
//  * - `value`: payload stored in the lower 32 bits.
//  * - `seq`:   sequence number stored in bits 63..33.
//  * - `safe`:  1-bit flag stored in bit 32.
//  */
// struct
// #ifndef NCELL_PAD
// alignas(CACHE_LINE)
// #else
// alignas(16)
// #endif
// PackedSequencedCell {

//     /// Atomically packed 64-bit field containing value, sequence, and safe flag.
//     std::atomic<uint64_t> pack_{0};

//     /**
//      * @brief Unpacks the 64-bit state into components.
//      * @param[out] seq   The 31-bit sequence number (bits 63..33).
//      * @param[out] safe  The 1-bit safe flag (bit 32).
//      * @param[out] value The 32-bit payload (bits 31..0).
//      */
//     static void unpack(uint64_t state, uint32_t& seq, bool& safe, uint32_t& value) noexcept {
//         uint32_t seqSafe;
//         bit::split(state, seqSafe, value); // high 32 → seqSafe, low 32 → value
//         seq  = seqSafe >> 1;
//         safe = static_cast<bool>(seqSafe & 1);
//     }

//     /**
//      * @brief Packs the value, sequence number, and safe flag into a 64-bit word.
//      *
//      * @param seq   31-bit sequence number (stored in bits 63..33).
//      * @param safe  Boolean flag (stored in bit 32).
//      * @param value 32-bit value (stored in bits 31..0).
//      * @return Packed 64-bit representation.
//      */
//     static uint64_t pack(uint32_t seq, bool safe, uint32_t value) noexcept {
//         uint32_t seqSafe = (seq << 1) | static_cast<uint32_t>(safe);
//         return bit::merge<uint64_t>(seqSafe, value); // high 32 = seqSafe, low 32 = value
//     }

// #ifndef NCELL_PAD
// private:
//     /// Padding to avoid false sharing, keeping struct size == one cache line.
//     [[no_unique_address]]char pad_[CACHE_LINE - sizeof(std::atomic<uint64_t>)];
// #endif
// };
