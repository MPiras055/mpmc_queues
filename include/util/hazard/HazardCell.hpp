#pragma once
#include <cstddef>
#include <type_traits>
#include <specs.hpp>

namespace util::hazard {
    /**
     * @brief HazardCell with additional per-thread metadata
     */
    template<typename Data, typename Meta>
    struct ALIGNED_CACHE HazardCell {
        inline Meta& metadata() {
            return meta_;
        }
        inline const Meta& metadata() const {
            return meta_;
        };
        inline Data& data() {
            return data_;
        }
        inline const Data& data() const {
            return data_;
        }
    private:
        Data data_{};
        Meta meta_{};

        static_assert(std::is_default_constructible_v<Data>,"HazardCell: Data not default constructible");
        static_assert(std::is_default_constructible_v<Meta>,"HazardCell: Meta is not default constructible");
        static constexpr size_t used = sizeof(Data) + sizeof(Meta);
        static_assert(used < CACHE_LINE, "HazardCell requires the whole cell to fit in CACHE_LINE");
        char _pad[CACHE_LINE - used];
    };

    /**
     * @brief HazardPtr cell with no additional metadata
     */
    template<typename Data>
    struct ALIGNED_CACHE HazardCell<Data,void> {
        inline Data& getData() {
            return data_;
        }
        inline const Data& getData() const {
            return data_;
        }

    private:
        Data data_{};
        static_assert(std::is_default_constructible_v<Data>,"HazardCell: Data not default constructible");
        static constexpr size_t pad = CACHE_LINE - sizeof(Data);
        static_assert(pad < CACHE_LINE, "HazardCell requires the whole cell to fit in CACHE_LINE");
        char _pad[pad];
    };

}  //namespace util::hazard
