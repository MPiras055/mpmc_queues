#pragma once
#include <cstdint>
#include <type_traits>
namespace base {

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
///
/// @note: @tparam Derived should have `IQueue<T>` as indirect or direct base class

template <typename T, typename Derived>
class ILinkedSegment {
public:
    /// @brief Retrieves the next segment in the chain.
    ///
    /// @return Pointer to the next segment, or `nullptr` if this is the last segment.
    virtual Derived* getNext() const = 0;

    virtual uint64_t getNextStartIndex() const = 0;

    // ==========================
    // Lifecycle Control
    // ==========================

    /// @brief Marks the queue as closed.
    ///
    /// A closed queue will not accept new items (`enqueue` fails),
    /// but may still allow `dequeue` of remaining items.
    ///
    /// @return true if successfully closed, false otherwise.
    virtual bool close() = 0;

    /// @brief Marks the queue as open.
    ///
    /// An open queue can accept new items (`enqueue` succeeds if not full).
    ///
    /// @return true if successfully opened, false otherwise.
    virtual bool open() = 0;

    /// @brief Checks if the queue has been closed.
    ///
    /// @return true if closed, false otherwise.
    virtual bool isClosed() const = 0;

    /// @brief Checks if the queue is currently open.
    ///
    /// @return true if open, false otherwise.
    virtual bool isOpened() const = 0;

    using linked_segment_tag = void;
};

// ===========================================================
// Helper trait to detect whether a segment is linked or not
// ===========================================================

template<typename, typename = void>
struct is_linked_segment : std::false_type {};

template<typename T>
struct is_linked_segment<T, std::void_t<typename T::linked_segment_tag>> : std::true_type {};

/// @brief Helper trait to check whether a given type `T`
///        is a linked segment (derives from `LinkedTag`).
///
/// This can be used in `static_assert` or `if constexpr` checks to conditionally
/// enable logic for linked vs. non-linked queue segments.
///
/// @tparam T Candidate type to check.
/// @retval true  if `T` is derived from `LinkedTag`.
/// @retval false otherwise.
template<typename T>
inline constexpr bool is_linked_segment_v = is_linked_segment<T>::value;

}   //namespace meta
