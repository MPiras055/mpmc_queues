#pragma once
/**
 * @file align.hpp
 * @brief Cache-line size, alignment arithmetic, and padding a member out to a full line.
 * @ingroup util
 *
 * Everything about *where an object sits* and *what fills the space after it*. Previously split
 * between `mem/Align.hpp` (the arithmetic) and `util/specs.hpp` (the padding), with
 * `mem/Layout.hpp` including both.
 *
 * It lives at the util level rather than under `mem/` because `util::threading::ThreadRegistry`
 * pads its own members, and `util/` sits below `mem/` in the layering -- putting this in `mem/`
 * would invert that edge.
 */

#include <cstddef>

/**
 * Bytes assumed to share a cache line.
 *
 * 128 rather than 64: modern x86 prefetches in pairs, so the destructive interference size is
 * two lines. Override with `-DCACHE_LINE=`.
 */
#ifndef CACHE_LINE
#define CACHE_LINE 128ul
#endif

namespace align {

/// @copydoc CACHE_LINE
inline constexpr std::size_t cache_line = CACHE_LINE;

/**
 * @brief Round @p n up to a multiple of @p a. @p a must be a power of two.
 */
constexpr std::size_t align_up(std::size_t n, std::size_t a) noexcept {
    return (n + a - 1) & ~(a - 1);
}


/// @return Bytes left in the current cache line after @p bytes.
constexpr std::size_t pad_to_line(std::size_t bytes) noexcept {
    return (cache_line - (bytes % cache_line)) % cache_line;
}

/// Bytes needed after members of these types to reach the end of the line.
template <typename... Ts>
inline constexpr std::size_t pad_after = pad_to_line((sizeof(Ts) + ... + 0));

/**
 * @brief Filler occupying exactly @p N bytes.
 */
template <std::size_t N>
struct Padding {
    char raw[N];
};

/// @copydoc Padding
template <>
struct Padding<0> {};

} // namespace align

/// @cond INTERNAL
#define MPMC_CONCAT_IMPL(x, y) x##y
#define MPMC_CONCAT(x, y) MPMC_CONCAT_IMPL(x, y)
#define MPMC_UNIQUE(base) MPMC_CONCAT(base, __COUNTER__)
/// @endcond

/// Start the next member on its own cache line.
#define CACHE_ALIGN alignas(align::cache_line)

/**
 * Fill the rest of the cache line after members of the given types.
 *
 * `[[no_unique_address]]` is what makes the already-aligned case free: when the types happen to
 * fill the line exactly, `align::Padding<0>` is empty and the member disappears.
 */
#define CACHE_PAD(...)                                                                          \
    [[maybe_unused]] [[no_unique_address]]                                                      \
    align::Padding<align::pad_after<__VA_ARGS__>> MPMC_UNIQUE(_pad)

/**
 * @brief One member alone on a cache line: aligned to the start, padded to the end.
 *
 * The type is named once. Written out, the pair was the most repeated three lines in the tree:
 *
 * ```cpp
 * CACHE_ALIGN std::atomic<uint64_t> tail_{0};
 * CACHE_PAD(std::atomic<uint64_t>);              // the same type, again
 * ```
 *
 * becomes
 *
 * ```cpp
 * CACHE_LINE_MEMBER(std::atomic<uint64_t>, tail_, {0});
 * ```
 *
 * @warning @p Type may not contain a top-level comma -- `std::pair<int, int>` would split into
 *          two macro arguments. Use `CACHE_ALIGN` and `CACHE_PAD` directly for such a type, and
 *          for a run of several members sharing one line.
 */
#define CACHE_LINE_MEMBER(Type, name, ...)                                                      \
    CACHE_ALIGN Type name __VA_OPT__(=) __VA_ARGS__;                                            \
    CACHE_PAD(Type)
