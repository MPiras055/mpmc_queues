#pragma once
#include <IQueue.hpp>
#include <ILinkedSegment.hpp>

namespace meta {

/// @brief Base interface for proxy queues that manage linked segments.
///
/// A proxy acts as the user-facing queue, hiding the underlying segments.
/// Only linked segments are allowed as the underlying storage for proxies.
/// 
/// @tparam T            Type of elements stored in the queue (must satisfy `IQueue` contract).
/// @tparam DerivedProxy The derived proxy class (CRTP style).
/// @tparam SegmentType  The linked segment template (template template parameter)
template <  typename T, template<typename,typename> class SegmentType>
class IProxy : public IQueue<T> {
    // -------------------------------------------------------------------------
    // Static assertion to ensure SegmentType is a linked segment
    // -------------------------------------------------------------------------
    static_assert(is_linked_segment_v<SegmentType<T,void>>,
                  "Proxy interfaces only allow Linked Segments");

public:
    /// @brief Virtual destructor to ensure proper cleanup of derived proxies.
    virtual ~IProxy() = default;

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

} // namespace meta
