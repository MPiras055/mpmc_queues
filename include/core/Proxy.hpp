#pragma once
/**
 * @file Proxy.hpp
 * @brief The contract a composed, segment-linked queue satisfies.
 * @ingroup core
 */

#include <core/Queue.hpp>
#include <concepts>

namespace core {

/**
 * @brief A queue assembled from linked segments, plus thread registration.
 *
 * @note the Proxy requires all partecipating threads to hold some thread_local
 *  data (e.g., for memory reclamation purposes). The proxy define a session RAII
 *  object and a method to join the proxy for the lifetime of the session object
 */
template <typename P, typename T>
concept Proxy = Queue<P, T> && requires(P p) {
    typename P::session; ///< a scope in which this thread may use the queue
    { p.join() } -> std::same_as<typename P::session>;
};

} // namespace core
