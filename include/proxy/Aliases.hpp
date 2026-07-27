#pragma once
#include <mem/source/Hazard.hpp>
#include <mem/source/Pool.hpp>
#include <proxy/Admission.hpp>
#include <proxy/LinkedProxy.hpp>

namespace proxy {

/**
 * @file Aliases.hpp
 * @brief The proxy family, as bindings of one template.
 *
 * Each of these was a separate ~250-300 line file. What distinguished them was a
 * predicate and a reclamation scheme; everything else was the same traversal.
 */

/// Grows without limit. Was UnboundedProxy.
template <typename T, typename Seg>
using Unbounded = LinkedProxy<T, Seg, admit::None, mem::source::Hazard<Seg>>;

/// Bounded by live item count. Was BoundedCounterProxy.
template <typename T, typename Seg>
using ItemBounded = LinkedProxy<T, Seg, admit::ItemCount, mem::source::Hazard<Seg>>;

/// Bounded by live segment count. Was BoundedChunkProxy.
template <typename T, typename Seg>
using ChunkBounded = LinkedProxy<T, Seg, admit::SegmentCount, mem::source::Hazard<Seg>>;

/**
 * @brief Bounded by a fixed pool of recycled segments. Was BoundedMemProxy.
 *
 * Note the admission policy: `None`. The ceiling is not a rule the proxy enforces, it is
 * the pool running dry -- which is why this needed no policy of its own, and why the
 * whole 288-line proxy collapses into an alias.
 */
template <typename T, typename Seg, std::size_t N = 4>
using MemBounded = LinkedProxy<T, Seg, admit::None, mem::source::Pool<Seg, N>>;

} // namespace proxy
