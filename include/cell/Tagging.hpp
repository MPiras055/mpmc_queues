#pragma once
#include <util/bit.hpp>
#include <atomic>
#include <concepts>
#include <cstdint>
#include <type_traits>

namespace cell {

/**
 * @brief How a cell word encodes "empty", "consumed", "claimed" and a payload.
 *
 * Three segments previously hand-rolled three mutually incompatible schemes for the
 * same problem, each with its own predicate and its own scattered reinterpret_casts:
 *
 * | segment  | empty    | consumed | claim              | predicate               |
 * |----------|----------|----------|--------------------|-------------------------|
 * | PRQ      | 0        | --       | msb(tid << 1) \| 1 | reserved = msb set      |
 * | FAAArray | msb(0)   | msb(1)   | --                 | reserved = msb set      |
 * | HQ       | 0        | 1        | --                 | reserved = (w <= 1)     |
 *
 * Note that the two "reserved" predicates spelled the same way meant *different
 * things*: for FAAArray it included the empty word, for PRQ it excluded it. Unifying on
 * a `reserved` predicate would have silently broken one of them. The contract is
 * therefore stated in terms of what a word positively *is*:
 *
 *   is_empty | is_consumed | is_claim | is_payload   -- mutually exclusive, exhaustive
 *
 * @note `can_store_null` surfaces a difference that was previously buried in a
 *       debug-only `assert(!reserved(item))` on HQ's enqueue path: HQ reserves word 0,
 *       so an HQ segment **cannot store a null payload**. FAAArray uses msb(0) for
 *       empty precisely so that it can.
 */
template <typename Tag, typename T>
concept Tagging = requires(T v, typename Tag::word w) {
    typename Tag::word;

    { Tag::empty() } noexcept -> std::same_as<typename Tag::word>;
    { Tag::consumed() } noexcept -> std::same_as<typename Tag::word>;

    { Tag::is_empty(w) } noexcept -> std::same_as<bool>;
    { Tag::is_consumed(w) } noexcept -> std::same_as<bool>;
    { Tag::is_payload(w) } noexcept -> std::same_as<bool>;

    { Tag::encode(v) } noexcept -> std::same_as<typename Tag::word>;
    { Tag::decode(w) } noexcept -> std::same_as<T>;

    { Tag::can_store_null } -> std::convertible_to<bool>;
};

/**
 * @brief A scheme that can additionally mint a per-thread claim token.
 *
 * PRQ reserves a cell in three steps: publish a token unique to this thread, fix up the
 * sequence number, then swap the real payload in. A consumer that finds a claim knows a
 * producer is mid-insert and may steal the cell. Not every scheme can express this —
 * LowTag reserves only two words and has none spare — so this is a refinement rather
 * than a member that some models would have to fake.
 */
template <typename Tag, typename T>
concept ClaimingTag = Tagging<Tag, T> && requires(typename Tag::word w) {
    { Tag::claim() } noexcept -> std::same_as<typename Tag::word>;
    { Tag::is_claim(w) } noexcept -> std::same_as<bool>;
};

namespace detail {
template <typename T, typename W>
constexpr W to_word(T v) noexcept {
    if constexpr (std::is_pointer_v<T>) return reinterpret_cast<W>(v);
    else return static_cast<W>(v);
}
template <typename T, typename W>
constexpr T from_word(W w) noexcept {
    if constexpr (std::is_pointer_v<T>) return reinterpret_cast<T>(w);
    else return static_cast<T>(w);
}
} // namespace detail

// ---------------------------------------------------------------------------
// MsbTag -- non-payload words carry the most significant bit.
// ---------------------------------------------------------------------------

/**
 * @brief Payloads never set the MSB; every reserved word does.
 *
 * Valid for pointer payloads on all mainstream 64-bit ABIs (user-space addresses leave
 * bit 63 clear) and for integer payloads below 2^63.
 *
 * Used by PRQ (which needs claims) and FAAArray (which does not).
 */
template <typename T>
struct MsbTag {
    using word = uintptr_t;

    /// A null payload encodes to 0, which has no MSB, so it is an ordinary payload.
    static constexpr bool can_store_null = true;

    static constexpr word empty() noexcept { return bit::set_msb<word>(0); }
    static constexpr word consumed() noexcept { return bit::set_msb<word>(1); }

    static constexpr bool is_empty(word w) noexcept { return w == empty(); }
    static constexpr bool is_consumed(word w) noexcept { return w == consumed(); }
    static constexpr bool is_payload(word w) noexcept { return bit::get_msb<word>(w) == 0; }
    /// Words 0 and 1 above the MSB are taken by empty/consumed; claims start at 2.
    static constexpr bool is_claim(word w) noexcept {
        return bit::get_msb<word>(w) != 0 && bit::clear_msb<word>(w) >= 2;
    }

    /// A token unique to the calling thread, distinct from empty() and consumed().
    static word claim() noexcept {
        static thread_local const word token = [] {
            static std::atomic<word> counter{0};
            return bit::set_msb<word>(counter.fetch_add(1, std::memory_order_relaxed) + 2);
        }();
        return token;
    }

    static constexpr word encode(T v) noexcept { return detail::to_word<T, word>(v); }
    static constexpr T decode(word w) noexcept { return detail::from_word<T, word>(w); }
};

// ---------------------------------------------------------------------------
// LowTag -- the two smallest words are reserved.
// ---------------------------------------------------------------------------

/**
 * @brief 0 is empty, 1 is consumed, everything else is a payload.
 *
 * Cheaper to test than MsbTag (one unsigned compare) and leaves the top bit free, at
 * the cost of not being able to represent a null payload.
 *
 * Used by HQ.
 */
template <typename T>
struct LowTag {
    using word = uintptr_t;

    /// 0 means empty, so a null item is indistinguishable from an unwritten cell.
    static constexpr bool can_store_null = false;

    static constexpr word empty() noexcept { return 0; }
    static constexpr word consumed() noexcept { return 1; }

    static constexpr bool is_empty(word w) noexcept { return w == 0; }
    static constexpr bool is_consumed(word w) noexcept { return w == 1; }
    static constexpr bool is_payload(word w) noexcept { return w > 1; }

    static constexpr word encode(T v) noexcept { return detail::to_word<T, word>(v); }
    static constexpr T decode(word w) noexcept { return detail::from_word<T, word>(w); }
};

} // namespace cell
