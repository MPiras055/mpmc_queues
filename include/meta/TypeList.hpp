#pragma once
/**
 * @file TypeList.hpp
 * @brief A type list and the folds over it that the registry is built on.
 * @ingroup meta
 */

#include <cstddef>
#include <type_traits>

namespace meta {

/**
 * @brief A compile-time list of types.
 *
 * Used by the registry to hold the (proxy x segment x options) matrix, and to
 * feed the same list to both the typed test suites and the benchmark's
 * compile-time dispatch. There is deliberately no runtime representation.
 */
template <typename... Ts>
struct TypeList {
    static constexpr std::size_t size = sizeof...(Ts);

    /// Expand the list into another variadic template, e.g. ::testing::Types.
    template <template <typename...> class F>
    using apply = F<Ts...>;
};

// ---------------------------------------------------------------------------
// concat
// ---------------------------------------------------------------------------
namespace detail {

template <typename...>
struct ConcatImpl;

template <>
struct ConcatImpl<> {
    using type = TypeList<>;
};

template <typename... Ts>
struct ConcatImpl<TypeList<Ts...>> {
    using type = TypeList<Ts...>;
};

template <typename... As, typename... Bs, typename... Rest>
struct ConcatImpl<TypeList<As...>, TypeList<Bs...>, Rest...> {
    using type = typename ConcatImpl<TypeList<As..., Bs...>, Rest...>::type;
};

} // namespace detail

/// Concatenate any number of TypeLists into one.
template <typename... Lists>
using concat = typename detail::ConcatImpl<Lists...>::type;

// ---------------------------------------------------------------------------
// map
// ---------------------------------------------------------------------------
namespace detail {

template <template <typename> class F, typename List>
struct MapImpl;

template <template <typename> class F, typename... Ts>
struct MapImpl<F, TypeList<Ts...>> {
    using type = TypeList<F<Ts>...>;
};

} // namespace detail

/// Apply a unary alias template to every element.
template <template <typename> class F, typename List>
using map = typename detail::MapImpl<F, List>::type;

// ---------------------------------------------------------------------------
// for_each: invoke f.template operator()<T>() for each T, in order.
// ---------------------------------------------------------------------------

/// Invoke @p f once per element. Used to build the registry's dispatch fold.
template <typename... Ts, typename F>
constexpr void for_each(TypeList<Ts...>, F&& f) {
    (f.template operator()<Ts>(), ...);
}

/**
 * @brief Invoke @p f on elements until one returns true.
 * @return true if some element's invocation returned true.
 *
 * This is the primitive behind name-based dispatch: the fold short-circuits, so
 * exactly one implementation runs and no indirect call is involved.
 */
template <typename... Ts, typename F>
constexpr bool any_of(TypeList<Ts...>, F&& f) {
    return (f.template operator()<Ts>() || ...);
}

} // namespace meta
