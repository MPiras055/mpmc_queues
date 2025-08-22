#pragma once
#include <ILinkedSegment.hpp>

namespace meta {

/// @brief Base interface for proxy queues that manage linked segments.
///
/// A proxy acts as the user-facing queue, hiding the underlying segments.
/// Only linked segments are allowed as the underlying storage for proxies.
/// 
/// @tparam T            Type of elements stored in the queue (must satisfy `IQueue` contract).
/// @tparam SegmentType  The linked segment type managed by this proxy (must be a linked segment).
template <typename T, typename SegmentType>
class IProxy : public IQueue<T> {
    // Ensure the proxy only wraps linked segments
    static_assert(is_linked_segment_v<SegmentType>,
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
    bool isOpened() final override const { return true; }

    /// @brief Disabled for proxies. Segments control the lifecycle.
    /// @return false always
    bool isClosed() final override const { return false; }
};

} //namespace meta
