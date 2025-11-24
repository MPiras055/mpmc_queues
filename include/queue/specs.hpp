#pragma once

#include <type_traits>
#include <utility>
#include <cstddef>
#include <atomic>

#ifndef CACHE_LINE
#define CACHE_LINE 128ul
#endif

// Use a namespace to avoid global pollution
namespace align {

    // Get size from object or integral value
    template<typename T>
    constexpr std::size_t get_size(T&& arg) {
        if constexpr (std::is_integral_v<T>) {
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

    // Unified padding computation: mix of sizes and/or identifiers
    template <typename... Args>
    constexpr std::size_t padding(Args&&... args) {
        constexpr std::size_t size = total_size(std::forward<Args>(args)...);
        static_assert(size <= CACHE_LINE, "Total size exceeds cache line size");
        return padding_for(size);
    }

    // Overload for type-based padding
    template <typename... Ts>
    constexpr std::size_t padding_for_types() {
        constexpr std::size_t size = (sizeof(Ts) + ... + 0);
        static_assert(size <= CACHE_LINE, "Total type size exceeds cache line size");
        return padding_for(size);
    }

} // namespace cache_align

// Macro to generate a unique padding name using __COUNTER__ for safety
#define CONCAT_IMPL(x, y) x##y
#define CONCAT(x, y) CONCAT_IMPL(x, y)
#define UNIQUE_NAME(base) CONCAT(base, __COUNTER__)

// Macro: Add padding using objects or sizes
#define CACHE_PAD(...) \
    char UNIQUE_NAME(_pad)[align::padding(__VA_ARGS__)]

// Macro: Add padding using types
#define CACHE_PAD_TYPES(...) \
    char UNIQUE_NAME(_pad)[align::padding_for_types<__VA_ARGS__>()]

// Optional: shorthand for aligning a struct
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
