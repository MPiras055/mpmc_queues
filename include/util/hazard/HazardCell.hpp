#pragma once
#include <atomic>
#include <specs.hpp>    // for cache line
#include <type_traits>  // for class type evaluation

namespace util::hazard {
    /**
     * @brief HazardCell with additional per-thread metadata
     */
    template<typename Data, typename Meta>
    struct alignas(CACHE_LINE) HazardCell {
        Data data{};
        Meta meta{};

        inline Meta& getMetadata() {
            return meta; 
        }

        inline Data& getData() {
            return data;
        }

        inline const Meta& get_metadata_ronly_() const {
            //get a const reference to the metdata
            return meta;
        }

    private:

        static constexpr size_t used = sizeof(Data) + sizeof(Meta);
        static_assert(used < CACHE_LINE, "HazardCell requires the whole cell to fit in CACHE_LINE");
        static constexpr size_t pad = CACHE_LINE - used;
        char _pad[pad];
    };

    /**
     * @brief HazardPtr cell with no additional metadata
     */
    template<typename Data> 
    struct alignas(CACHE_LINE) HazardCell<Data,void> {
        Data data{};
        inline Data& getData() {
            return data;
        }

    private:
        static constexpr size_t pad = CACHE_LINE - sizeof(Data);
        static_assert(pad < CACHE_LINE, "HazardCell requires the whole cell to fit in CACHE_LINE");
        char _pad[pad];
    };



}