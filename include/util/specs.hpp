#pragma once
/**
 * @file specs.hpp
 * @brief Compiler directives: inlining and the spin hint.
 * @ingroup util
 *
 * Only things that steer code generation. Cache-line size, alignment arithmetic and padding
 * live in util/align.hpp, which describes layout rather than codegen.
 */

/// Force inlining where the compiler supports it; the hot paths rely on it.
#if defined(_MSC_VER)
    // Microsoft Visual C++
    #define FORCE_INLINE __forceinline
#elif defined(__GNUC__) || defined(__clang__)
    // GCC and Clang
    #define FORCE_INLINE inline __attribute__((always_inline))
#else
    // Fallback to standard hint
    #define FORCE_INLINE inline
#endif

/**
 * A hint that this thread is spinning, not working.
 *
 * On x86 `pause` keeps the pipeline from filling with speculative loads that a store from
 * another core is about to invalidate; on ARM `yield` does the analogous thing. Neither
 * sleeps or reschedules, which matters where the spinner holds something -- an epoch pin,
 * say -- that the thread it is waiting on needs released.
 */
#if defined(__x86_64__) || defined(__i386__)
    #define SPIN_HINT() __builtin_ia32_pause()
#elif defined(__aarch64__) || defined(__arm__)
    #define SPIN_HINT() __asm__ __volatile__("yield" ::: "memory")
#else
    #define SPIN_HINT() ((void)0)
#endif
