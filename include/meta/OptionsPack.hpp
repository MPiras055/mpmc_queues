#pragma once
/**
 * @file OptionsPack.hpp
 * @brief Compile-time option packs: flag tags and value options, with validation.
 * @ingroup meta
 */

#include <type_traits>

namespace meta {

/**
 * @brief Names a *value* option in an accepts-list.
 *
 * Flag options are plain tags, so an accepts-list can name them directly. A value option is a
 * template -- `patience<2>` and `patience<8>` are different types -- and an accepts-list has no
 * way to say "any instantiation of this one". This marker is that way:
 *
 * @code
 * requires meta::AcceptsOnly<Opt,
 *              typename HQOpt::force_cell_padding,        // a flag, named directly
 *              meta::ValueOption<HQOpt::patience>>        // a value, named as a template
 * @endcode
 *
 * @tparam K the option template, which must take a single `auto` non-type parameter so it can
 *           also be handed to `OptionsPack::get`.
 */
template <template <auto> class K>
struct ValueOption {};

namespace detail {
/**
 * @brief Does option @p O satisfy accepts-list entry @p A?
 *
 * The primary template is plain type equality, which is what a flag needs. The partial
 * specialization is what admits value options: `K<V>` matches `ValueOption<K>` for any V.
 */
template <typename O, typename A>
struct option_matches : std::is_same<O, A> {};

template <template <auto> class K, auto V>
struct option_matches<K<V>, ValueOption<K>> : std::true_type {};

/// Is @p T admitted by any entry of @p Us? Factored out so `accepts` folds over one pack,
/// not two nested.
template <typename T, typename... Us>
inline constexpr bool is_one_of_v = (option_matches<T, Us>::value || ...);
} // namespace detail

    /**
     * @brief A compile-time immutable container for option types and values.
     *
     * OptionsPack allows you to build configurations using types (tag dispatching)
     * and value wrappers. It supports method chaining to build complex configurations
     * at compile-time and strictly encapsulates its helper logic.
     *
     * @tparam Options The variadic list of option types contained in this pack.
     */
    template <typename... Options>
    struct OptionsPack {

        // =============================================================
        // 1. Status Checks
        // =============================================================

        /**
         * @brief Checks if a specific option type exists in this pack.
         * @tparam QueryOpt The option type to search for.
         */
        template <typename QueryOpt>
        static constexpr bool has = ((std::is_same_v<QueryOpt, Options>) || ...);

        /**
         * @brief The number of options currently in the pack.
         */
        static constexpr auto size = sizeof...(Options);

        /**
         * @brief Is every option in this pack one of @p Accepted?
         *
         * `has<>` answers false for anything it does not recognise, which means a
         * misspelled tag, or one belonging to a different algorithm, reads as "not
         * requested" and quietly does nothing. `OptionsPack<VyukovOpt::no_pow2>` handed
         * to PRQ is the concrete case: PRQ has its own `no_pow2` and would ignore
         * Vyukov's.
         *
         * An algorithm declares what it accepts and asserts this, so an unrecognised tag
         * becomes an error at the point of instantiation.
         *
         * Entries are either a flag tag, named directly, or `meta::ValueOption<K>`, which
         * admits every instantiation of the option template `K`.
         */
        template <typename... Accepted>
        static constexpr bool accepts = (detail::is_one_of_v<Options, Accepted...> && ...);


        // =============================================================
        // 2. Value Extraction
        // =============================================================

    private:
        /* * Internal Recursive Search for Value Extraction.
         * Detects if a type in the pack matches the pattern KeyTemplate<Value>.
         */

        // Base Case: List is empty or no match found -> Return Default
        template <template <auto> class KeyTemplate, auto Default, typename... Remaining>
        struct ValueFinder {
            static constexpr auto value = Default;
        };

        // Recursive Step 1: Head matches KeyTemplate<V> -> Return V
        // This specialization takes precedence if the Head type matches the template pattern.
        template <template <auto> class KeyTemplate, auto Default, auto V, typename... Tail>
        struct ValueFinder<KeyTemplate, Default, KeyTemplate<V>, Tail...> {
            static constexpr auto value = V;
        };

        // Recursive Step 2: Head does NOT match -> Discard Head and continue searching Tail
        template <template <auto> class KeyTemplate, auto Default, typename Head, typename... Tail>
        struct ValueFinder<KeyTemplate, Default, Head, Tail...> {
            static constexpr auto value = ValueFinder<KeyTemplate, Default, Tail...>::value;
        };

    public:

        /**
         * @brief Extract a value from a template option in the pack.
         *
         * Usage:
         * @code
         * struct RingOpt { template <auto N> struct buffer_size {}; };
         * constexpr std::size_t n = Config::get<RingOpt::buffer_size, std::size_t{1024}>;
         * @endcode
         *
         * @note The option template must take a single **`auto`** parameter. A
         *       `template <std::size_t N>` is less general than the `template <auto> class`
         *       parameter here and will not bind.
         *
         * @warning **The result type is not stable.** When the option is absent the type comes
         *          from @p Default; when it is present it comes from however the caller spelled
         *          the value, so `buffer_size<1024>` yields `int` and `buffer_size<1024u>`
         *          yields `unsigned`. Cast at the use site rather than relying on it:
         *          `static_cast<std::size_t>(Opt::template get<K, std::size_t{2}>)`.
         *
         * @tparam KeyTemplate The option template to search for.
         * @tparam Default The value to return if the option is not present.
         */
        template <template <auto> class KeyTemplate, auto Default>
        static constexpr auto get = ValueFinder<KeyTemplate, Default, Options...>::value;


        // =============================================================
        // 3. Modifiers (Type Generators)
        // =============================================================

        /**
         * @brief Appends a new option to the pack.
         * @return A new OptionsPack type.
         */
        template <typename NewOpt>
        using add = OptionsPack<Options..., NewOpt>;

        /**
         * @brief Conditionally appends a new option.
         */
        template <bool Condition, typename NewOpt>
        using add_if = std::conditional_t<
            Condition,
            OptionsPack<Options..., NewOpt>,
            OptionsPack<Options...>
        >;

        /**
         * @brief Merges a raw list of types into this pack.
         */
        template <typename... OtherOptions>
        using merge = OptionsPack<Options..., OtherOptions...>;

    private:
        /*
         * Internal Helper for Pack Merging.
         * Unpacks 'OtherPack' to extract its variadic types.
         */
        template <typename OtherPack>
        struct PackMerger;

        template <typename... OtherOptions>
        struct PackMerger<OptionsPack<OtherOptions...>> {
            using type = OptionsPack<Options..., OtherOptions...>;
        };

    public:

        /**
         * @brief Merges another existing OptionsPack into this one.
         * @tparam OtherPack Must be an instantiation of OptionsPack<...>.
         */
        template <typename OtherPack>
        using merge_pack = typename PackMerger<OtherPack>::type;
    };

    /**
     * @brief A convenience alias for starting a new configuration chain.
     */
    using EmptyOptions = OptionsPack<>;

    /**
     * @brief Constrain a template so it only accepts option tags it understands.
     *
     * Used as a requires-clause rather than an in-class static_assert on purpose: a
     * constraint is checked when the specialization is *named*, so
     * `Bad* p = nullptr;` is already an error. An in-class assertion only fires once
     * something forces instantiation, which lets a wrong configuration travel a long
     * way before it is caught.
     */
    template <typename Opt, typename... Accepted>
    concept AcceptsOnly = Opt::template accepts<Accepted...>;

} // namespace meta
