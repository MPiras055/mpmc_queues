#pragma once
#include <IQueue.hpp>

namespace meta {

/// @brief Marker type used to detect whether a queue segment is linked.
/// 
/// Segments that inherit from `LinkedTag<T>` can be recognized at compile time
/// as part of a linked-segment structure.
template <typename T>
struct LinkedTag {};

/// @brief Helper trait to check whether a given type `T`
///        is a linked segment (derives from `LinkedTag`).
/// 
/// This can be used in `static_assert` or `if constexpr` checks to conditionally
/// enable logic for linked vs. non-linked queue segments.
/// 
/// @tparam T Candidate type to check.
/// @retval true  if `T` is derived from `LinkedTag`.
/// @retval false otherwise.
template <typename T>
constexpr bool is_linked_segment_v =
    std::is_base_of_v<LinkedTag<typename T::value_type>, T>;


// ==========================
// Helper macro
// ==========================

#define IS_LINKED is_linked_segment_v<std::decay_t<decltype(*this)>>


// ==========================
// Linked Segment Interface
// ==========================

/// @brief Interface for a queue segment that can be linked with others
///        to form an unbounded queue structure.
/// 
/// Extends the base `IQueue` contract with next-pointer management,
/// allowing multiple bounded segments to be chained together.
/// 
/// @tparam T       Value type stored in the queue (must satisfy `IQueue` contract).
/// @tparam Derived The concrete linked segment type (CRTP pattern).
template <typename T, typename Derived>
class ILinkedSegment : public IQueue<T>, public LinkedTag<T> {
public:
    /// @brief Retrieves the next segment in the chain.
    /// 
    /// @return Pointer to the next segment, or `nullptr` if this is the last segment.
    virtual Derived* getNext() const = 0;
};

}   //namespace meta
