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

/**
 * @brief The source a hazard-reclaimed proxy is built on.
 *
 * Spelled once here rather than at each alias: the source carries the proxy's per-thread
 * state, so every alias has to hand it `ThreadMeta<handle>` and the handle of a hazard source
 * is just `Seg*`. LinkedProxy static_asserts the two agree, so getting this wrong is a
 * diagnosable error rather than a silent layout mismatch.
 */
template <typename Seg>
using HazardSource = mem::source::Hazard<Seg, ThreadMeta<Seg*>>;

/// Grows without limit. Was UnboundedProxy.
template <typename T, typename Seg>
using Unbounded = LinkedProxy<T, Seg, admit::None, HazardSource<Seg>>;

/// Bounded by live item count. Was BoundedCounterProxy.
template <typename T, typename Seg>
using ItemBounded = LinkedProxy<T, Seg, admit::ItemCount, HazardSource<Seg>>;

/// Bounded by live segment count. Was BoundedChunkProxy.
template <typename T, typename Seg>
using ChunkBounded = LinkedProxy<T, Seg, admit::SegmentCount, HazardSource<Seg>>;

/**
 * @brief Bounded by a fixed pool of recycled segments. Was BoundedMemProxy.
 *
 * Note the admission policy: `None`. The ceiling is not a rule the proxy enforces, it is
 * the pool running dry -- which is why this needed no policy of its own, and why the
 * whole 288-line proxy collapses into an alias.
 */
template <typename T, typename Seg, std::size_t N = 4>
using MemBounded =
    LinkedProxy<T, Seg, admit::None,
                mem::source::Pool<Seg, N, ThreadMeta<mem::VersionedIndex<N>>>>;

} // namespace proxy
