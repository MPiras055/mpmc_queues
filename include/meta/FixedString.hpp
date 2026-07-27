#pragma once
#include <cstddef>
#include <string_view>

namespace meta {

/**
 * @brief A string literal usable as a non-type template parameter (C++20).
 *
 * Lets registry entries be written as `Entry<"lprq", ...>` so an implementation
 * carries its own name in the type system. The name is matched against the CLI
 * argument by a compile-time fold, which is what keeps runtime selection free of
 * any vtable or type erasure.
 */
template <std::size_t N>
struct FixedString {
    char value[N]{};

    consteval FixedString(const char (&literal)[N]) {
        for (std::size_t i = 0; i < N; ++i) value[i] = literal[i];
    }

    /// View over the characters, excluding the trailing NUL.
    constexpr std::string_view view() const noexcept { return std::string_view{value, N - 1}; }

    constexpr operator std::string_view() const noexcept { return view(); }

    constexpr bool operator==(std::string_view other) const noexcept { return view() == other; }
};

template <std::size_t N>
FixedString(const char (&)[N]) -> FixedString<N>;

} // namespace meta
