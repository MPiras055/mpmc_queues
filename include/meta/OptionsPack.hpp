#pragma once
#include <type_traits>

namespace meta {

    /**
     * @brief A compile-time immutable container for option types.
     *
     * OptionsPack allows you to build configurations using types (tag dispatching)
     * rather than runtime boolean flags. It supports method chaining to build
     * complex configurations at compile-time.
     *
     * @section usage_guide Usage Guide
     *
     * @subsection step1 1. Define Options
     * Options are simply empty structs (tags).
     * @code
     * struct Logging {};
     * struct Encryption {};
     * struct FastPath {};
     * @endcode
     *
     * @subsection step2 2. Build the Pack (Basic & Conditional)
     * Start with an empty pack and chain calls. Since these are types,
     * every call returns a **new** type.
     * @code
     * constexpr bool is_debug = true;
     *
     * using Config = meta::EmptyOptions
     * ::add<Logging>                    // Basic Add
     * ::add_if<is_debug, Encryption>    // Conditional Add
     * ::add_if<!is_debug, FastPath>;    // Conditional Add
     * @endcode
     *
     * @subsection step3 3. Merging Packs
     * You can combine existing packs into a master configuration.
     * @code
     * using NetworkOpts = meta::OptionsPack<struct TCP, struct UDP>;
     * using FileOpts    = meta::OptionsPack<struct ReadOnly>;
     *
     * // Merge a pack into the current one
     * using FullSystem = NetworkOpts::merge_pack<FileOpts>;
     * @endcode
     *
     * @subsection step4 4. Checking Options
     * Use `has` in `if constexpr` blocks to compile code conditionally.
     * @code
     * template <typename Config>
     * void run() {
     * if constexpr (Config::template has<Logging>) {
     * // This code is compiled out if Logging is missing
     * std::cout << "Log start";
     * }
     * }
     * @endcode
     *
     * @tparam Options The variadic list of option types contained in this pack.
     */
    template <typename... Options>
    struct OptionsPack {

        // =============================================================
        // Status Checks
        // =============================================================

        /**
         * @brief Checks if a specific option type exists in this pack.
         *
         * @tparam QueryOpt The option type to search for.
         * @return true if QueryOpt is present in the pack, false otherwise.
         */
        template <typename QueryOpt>
        static constexpr bool has = ((std::is_same_v<QueryOpt, Options>) || ...);

        /**
         * @brief The number of options currently in the pack.
         */
        static constexpr auto size = sizeof...(Options);


        // =============================================================
        // Modifiers (Type Generators)
        // =============================================================

        /**
         * @brief Appends a new option to the pack.
         *
         * @tparam NewOpt The option type to add.
         * @return A new OptionsPack type containing the previous options plus NewOpt.
         */
        template <typename NewOpt>
        using add = OptionsPack<Options..., NewOpt>;

        /**
         * @brief Conditionally appends a new option to the pack.
         *
         * If `Condition` is true, returns a pack with `NewOpt` added.
         * If `Condition` is false, returns the current pack type unchanged.
         *
         * @tparam Condition A compile-time boolean constant.
         * @tparam NewOpt The option type to potentially add.
         */
        template <bool Condition, typename NewOpt>
        using add_if = std::conditional_t<
            Condition,
            OptionsPack<Options..., NewOpt>,
            OptionsPack<Options...>
        >;

        /**
         * @brief Merges a raw list of types into this pack.
         *
         * @tparam OtherOptions... A variadic list of types to append.
         * @return A new OptionsPack containing the original options followed by OtherOptions.
         */
        template <typename... OtherOptions>
        using merge = OptionsPack<Options..., OtherOptions...>;

        // Helper to unwrap another OptionsPack for merging
        template <typename OtherPack>
        struct merger;

        // Specialization to extract types from the other pack
        template <typename... OtherOptions>
        struct merger<OptionsPack<OtherOptions...>> {
            using type = OptionsPack<Options..., OtherOptions...>;
        };

        /**
         * @brief Merges another existing OptionsPack into this one.
         *
         * @tparam OtherPack Must be an instantiation of OptionsPack<...>.
         * @return A new combined OptionsPack.
         */
        template <typename OtherPack>
        using merge_pack = typename merger<OtherPack>::type;
    };

    /**
     * @brief A convenience alias for starting a new configuration chain.
     */
    using EmptyOptions = OptionsPack<>;

} // namespace meta
