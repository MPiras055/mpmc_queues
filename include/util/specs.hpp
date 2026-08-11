#pragma once

#include <type_traits>
#include <utility>
#include <cstddef>
#include <atomic>

#ifndef CACHE_LINE
#define CACHE_LINE 128ul
#endif

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

namespace align {

    // Get size from object or integral value
    template<typename T>
    constexpr std::size_t get_size(T&& arg) {
        if constexpr (std::is_integral_v<std::remove_reference_t<T>>) {
            return static_cast<std::size_t>(arg);
        } else {
            return sizeof(arg);
        }
    }

    // Sum sizes of any number of arguments
    template <typename... Args>
    constexpr std::size_t total_size(Args&&... args) {
        return (get_size(std::forward<Args>(args)) + ... + 0);
    }

    // Compute padding needed to align size to cache line
    constexpr std::size_t padding_for(std::size_t size) {
        return (CACHE_LINE - (size % CACHE_LINE)) % CACHE_LINE;
    }

    // Unified padding computation
    template <typename... Args>
    constexpr std::size_t padding(Args&&... args) {
        return padding_for(total_size(std::forward<Args>(args)...));
    }

    // Overload for type-based padding
    template <typename... Ts>
    constexpr std::size_t padding_for_types() {
        return padding_for((sizeof(Ts) + ... + 0));
    }

    /**
     * Helper struct to handle zero-size padding safely.
     * Standard C++ arrays cannot be size 0.
     */
    template <std::size_t N>
    struct Padding {
        char _buffer[N];
    };

    // Specialization for zero padding: occupies no space
    template <>
    struct Padding<0> {};

} // namespace align

// Macro helpers
#define CONCAT_IMPL(x, y) x##y
#define CONCAT(x, y) CONCAT_IMPL(x, y)
#define UNIQUE_NAME(base) CONCAT(base, __COUNTER__)

/**
 * CACHE_PAD uses [[no_unique_address]] (C++20).
 * If the padding is 0, this member disappears.
 * If > 0, it creates a buffer of the exact size needed.
 */
#define CACHE_PAD(...) [[maybe_unused]] [[no_unique_address]] align::Padding<align::padding(__VA_ARGS__)> UNIQUE_NAME(_pad)

#define CACHE_PAD_TYPES(...) [[maybe_unused]] [[no_unique_address]] align::Padding<align::padding_for_types<__VA_ARGS__>()> UNIQUE_NAME(_pad)

#define ALIGNED_CACHE alignas(CACHE_LINE)

namespace detail {

template <typename T, bool IsTC>
struct is_lock_free_impl {
    static constexpr bool value = false;
};

template <typename T>
struct is_lock_free_impl<T, true> {
    static constexpr bool value = std::atomic<T>::is_always_lock_free;
};

template <typename T>
static constexpr bool atomic_compatible_v =
    std::is_trivially_copyable_v<T> &&
    is_lock_free_impl<T, std::is_trivially_copyable_v<T>>::value;

} // namespace detail
