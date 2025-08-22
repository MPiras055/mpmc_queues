#pragma once
#include <cstdint>


namespace p_atomic {

/**
 * @brief Double-width compare-and-swap with expected update.
 * 
 * Atomically compares two consecutive machine words with expected values, 
 * and if they match, replaces them with desired values. If the CAS fails, 
 * the actual values in memory are written back into the @p expected_lo 
 * and @p expected_hi parameters.
 * 
 * @param addr Pointer to the memory location (must be 16-byte aligned on x86_64).
 * @param expected_lo [in,out] Expected low word; overwritten with actual value on failure.
 * @param expected_hi [in,out] Expected high word; overwritten with actual value on failure.
 * @param desired_lo Desired new low word.
 * @param desired_hi Desired new high word.
 * @return true if swap succeeded, false otherwise.
 */
inline bool cas2(void* addr,
                 uint64_t& expected_lo, uint64_t& expected_hi,
                 uint64_t desired_lo, uint64_t desired_hi) {
#if defined(__x86_64__)
    unsigned char result;
    uint64_t old_lo = expected_lo;
    uint64_t old_hi = expected_hi;

    __asm__ __volatile__ (
        "lock cmpxchg16b %1\n"
        "sete %0\n"
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
#   error "cas2 not supported on this architecture"
#endif
}

}   // namespace portable atomic
