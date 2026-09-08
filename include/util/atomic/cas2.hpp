#pragma once
/**
 * @file cas2.hpp
 * @brief Double-width compare-and-swap inline assembly code
 * @ingroup util
 */

#include "util/specs.hpp"
#include <bit>
#include <concepts>
#include <cstdint>
#include <type_traits>

namespace p_atomic {

// Any type that fits within a 64-bit machine word and can be bit_cast safely
template <typename T>
concept DcasWord = (sizeof(std::remove_cvref_t<T>) == sizeof(uint64_t)) &&
                   std::is_trivially_copyable_v<std::remove_cvref_t<T>>;

/**
 * @brief Base double-width compare-and-swap (uint64_t primitive).
 *
 * Atomically compares two consecutive machine words with expected values, 
 * and if they match, replaces them with desired values. If the CAS fails, 
 * the actual values in memory are written back into @p expected_lo and @p expected_hi.
 * 
 * @param addr Pointer to the memory location (must be 16-byte aligned on x86_64).
 * @param expected_lo [in,out] Expected low word; overwritten with actual value on failure.
 * @param expected_hi [in,out] Expected high word; overwritten with actual value on failure.
 * @param desired_lo Desired new low word.
 * @param desired_hi Desired new high word.
 * @return true if swap succeeded, false otherwise.
 */
inline bool dcas(void* addr,
                 uint64_t& expected_lo, uint64_t& expected_hi,
                 uint64_t desired_lo, uint64_t desired_hi) {
#if defined(__x86_64__)
    unsigned char result;
    uint64_t old_lo = expected_lo;
    uint64_t old_hi = expected_hi;

    __asm__ __volatile__ (
        "lock cmpxchg16b %1\n\t"
        "sete %0"
        : "=q"(result),
          "+m"(*(volatile __int128*)addr),
          "+a"(old_lo), "+d"(old_hi)
        : "b"(desired_lo), "c"(desired_hi)
        : "cc", "memory");

    if (!result) {
        expected_lo = old_lo;
        expected_hi = old_hi;
    }
    return result;

#elif defined(__aarch64__)
    unsigned int status;
    uint64_t old_lo, old_hi;
    do {
        asm volatile (
            "ldaxp  %0, %1, [%4]        \n"
            : "=&r"(old_lo), "=&r"(old_hi)
            : "r"(addr)
            : "memory");

        if (old_lo != expected_lo || old_hi != expected_hi) {
            expected_lo = old_lo;
            expected_hi = old_hi;
            return false;
        }

        asm volatile (
            "stlxp  %w0, %2, %3, [%1]   \n"
            : "=&r"(status)
            : "r"(addr), "r"(desired_lo), "r"(desired_hi)
            : "memory");
    } while (status != 0);
    return true;

#elif defined(__arm__)  // ARMv7
    unsigned int status;
    uint32_t old_lo, old_hi;
    do {
        asm volatile (
            "ldrexd %0, %1, [%3]    \n"
            : "=&r"(old_lo), "=&r"(old_hi)
            : "r"(addr)
            : "memory");

        if (old_lo != (uint32_t)expected_lo ||
            old_hi != (uint32_t)expected_hi) {
            expected_lo = old_lo;
            expected_hi = old_hi;
            return false;
        }

        asm volatile (
            "strexd %0, %2, %3, [%1]\n"
            : "=&r"(status)
            : "r"(addr), "r"(desired_lo), "r"(desired_hi)
            : "memory");
    } while (status != 0);
    return true;

#else
#   error "architecture doesn't support dcas"
#endif
}

/**
 * @brief Templated updating DCAS overload.
 *
 * Selected only when both expected arguments are non-const lvalues.
 * Handles bit-casting internally and writes back the latest memory values
 * into @p expected_lo and @p expected_hi on CAS failure.
 */
template <DcasWord ExpLo, DcasWord ExpHi, DcasWord DesLo, DcasWord DesHi>
requires (!std::is_const_v<ExpLo> && !std::is_const_v<ExpHi>)
FORCE_INLINE bool dcas(void* addr,
                       ExpLo& expected_lo, ExpHi& expected_hi,
                       const DesLo& desired_lo, const DesHi& desired_hi) {
    uint64_t lo = std::bit_cast<uint64_t>(expected_lo);
    uint64_t hi = std::bit_cast<uint64_t>(expected_hi);

    const bool success = dcas(addr, lo, hi,
                              std::bit_cast<uint64_t>(desired_lo),
                              std::bit_cast<uint64_t>(desired_hi));
    if (!success) {
        expected_lo = std::bit_cast<ExpLo>(lo);
        expected_hi = std::bit_cast<ExpHi>(hi);
    }
    return success;
}

/**
 * @brief Templated non-updating DCAS overload.
 *
 * Selected when expected arguments are passed as rvalues, temporaries,
 * literals, const references, or mixed types. Expected values are not updated.
 */
template <DcasWord ExpLo, DcasWord ExpHi, DcasWord DesLo, DcasWord DesHi>
FORCE_INLINE bool dcas(void* addr,
                       ExpLo&& expected_lo, ExpHi&& expected_hi,
                       const DesLo& desired_lo, const DesHi& desired_hi) {
    uint64_t lo = std::bit_cast<uint64_t>(expected_lo);
    uint64_t hi = std::bit_cast<uint64_t>(expected_hi);

    return dcas(addr, lo, hi,
                std::bit_cast<uint64_t>(desired_lo),
                std::bit_cast<uint64_t>(desired_hi));
}

}   // namespace p_atomic