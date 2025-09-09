#pragma once

#include <utility>
#include <tuple>
#include <cstddef>

namespace util::meta {

// Wrapper for template templates
template <template <typename> typename Template>
struct Wrapper {
    template <typename T>
    using Type = Template<T>;
};

// TemplateSet that directly handles templates and wraps them internally
template <template <typename> typename... Templates>
struct TemplateSet {
    // Internal wrappers for templates
    using WrappedTemplates = std::tuple<Wrapper<Templates>...>;

    // Retrieve a template by index (returns the Wrapper for the template)
    template <size_t i>
    using Get = typename std::tuple_element_t<i, WrappedTemplates>;

    // Append another set of templates
    template <template <typename> typename... OtherTemplates>
    using Append = TemplateSet<Templates..., OtherTemplates...>;

    // Concatenate with another TemplateSet
    template <typename OtherSet>
    struct CatImpl;

    template <template <typename> typename... OtherTemplates>
    struct CatImpl<TemplateSet<OtherTemplates...>> {
        using Type = TemplateSet<Templates..., OtherTemplates...>;
    };

    template <typename OtherSet>
    using Cat = typename CatImpl<OtherSet>::Type;

    template <typename F>
    static constexpr void foreach(F&& func) {
        // Use a fold expression to iterate over the Templates
        (func.template operator()<Templates>(), ...);
    }

    template <template <typename> typename Query>
    struct ContainsHelper {
        // Helper to check if the Wrapper matches Query
        template <typename T>
        static constexpr bool is_match() {
            return std::is_same_v<Wrapper<Query>, T>;
        }

        // Use std::tuple to iterate over WrappedTemplates
        static constexpr bool value = [] {
            bool result = false;
            std::apply([&result](auto... elements) {
                ((result = result || is_match<decltype(elements)>()), ...);
            }, WrappedTemplates{});
            return result;
        }();
    };

    template <template <typename> typename Query>
    static constexpr bool Contains = ContainsHelper<Query>::value;
};

} //namespace util::meta