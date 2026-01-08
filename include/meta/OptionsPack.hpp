#pragma once
#include <type_traits>

namespace meta {

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
         * template <size_t N> struct BufferSize { static constexpr auto value = N; };
         * constexpr size_t val = Config::get<BufferSize, 1024>;
         * @endcode
         *
         * @tparam KeyTemplate The template class to search for (e.g. BufferSize).
         * @tparam Default The value to return if the option is not found.
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

} // namespace meta
