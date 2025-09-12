#pragma once
#include <IQueue.hpp>
#include <ILinkedSegment.hpp>

namespace base {

/// @brief Base interface for proxy queues that manage linked segments.
///
/// A proxy acts as the user-facing queue, hiding the underlying segments.
/// Only linked segments are allowed as the underlying storage for proxies.
///
/// @tparam T            Type of elements stored in the queue (must satisfy `IQueue` contract).
/// @tparam DerivedProxy The derived proxy class (CRTP style).
/// @tparam SegmentType  The linked segment template (template template parameter)
template <  typename T, template<typename,typename,bool,bool> class SegmentType>
class IProxy : public IQueue<T> {
    // -------------------------------------------------------------------------
    // Static assertion to ensure SegmentType is a linked segment
    // -------------------------------------------------------------------------
    static_assert(is_linked_segment_v<SegmentType<T,void,true,true>>,
                  "Proxy interfaces only allow Linked Segments");

public:
    using proxy_tag = void;

    /// @brief Virtual destructor to ensure proper cleanup of derived proxies.
    virtual ~IProxy() = default;

    /// @brief books a slot for the current thread
    /// @returns true if the thread successfuly acquired a slot false otherwise
    ///
    /// @warning if no slot can be acquired using the data structure results in undefined
    /// behaviour
    ///
    virtual bool acquire() = 0;

    /// @brief clears a booked slot
    /// @returns true (always successful)
    ///
    /// @note the method must be idenpotent (calling it multiple times results
    /// in no side effects)
    ///
    virtual void release() = 0;

protected:
    // -------------------------------------------------------------------------
    // Disable lifecycle methods for proxies
    // These operations are managed by the underlying segments
    // -------------------------------------------------------------------------

    /// @brief Disabled for proxies. Segments control the lifecycle.
    /// @return false always
    bool open() final override { return false; }

    /// @brief Disabled for proxies. Segments control the lifecycle.
    /// @return false always
    bool close() final override { return false; }

    /// @brief Disabled for proxies. Segments control the lifecycle.
    /// @return true always
    bool isOpened() const final override { return true; }

    /// @brief Disabled for proxies. Segments control the lifecycle.
    /// @return false always
    bool isClosed() const final override { return false; }
};

// ===========================================================
// Helper trait to detect whether an instance is a Proxy
// ===========================================================

template<typename, typename = void>
struct is_proxy : std::false_type {};

template<typename T>
struct is_proxy<T, std::void_t<typename T::proxy_tag>> : std::true_type {};

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
inline constexpr bool is_proxy_v = is_proxy<T>::value;



} // namespace meta
