#pragma once
#include <cstddef>

namespace mem {

/**
 * @brief Round @p n up to a multiple of @p a. @p a must be a power of two.
 *
 * This is the only align-up in the tree, and it is the correct one.
 *
 * Three separate hand-rolled copies previously wrote `& (~a - 1)` instead of
 * `& ~(a - 1)`. That does not align up at all — it clears bit 0 and bit log2(a).
 * Measured on the real types: LinkedPRQ's header came out at 638 against a
 * sizeof(Self) of 640, so the co-allocated cell array began two bytes *inside* the
 * object, overlapping its own head/tail counters; LinkedFAAArray's came out at 390,
 * which clears the header but is not 8-aligned, giving a misaligned atomic array.
 *
 * Centralising it is what makes that class of bug unrepresentable rather than merely
 * fixed.
 */
constexpr std::size_t align_up(std::size_t n, std::size_t a) noexcept {
    return (n + a - 1) & ~(a - 1);
}

constexpr bool is_pow2(std::size_t n) noexcept { return n != 0 && (n & (n - 1)) == 0; }

} // namespace mem
