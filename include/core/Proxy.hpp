#pragma once
#include <core/Queue.hpp>
#include <concepts>

namespace core {

/**
 * @brief A queue assembled from linked segments, plus thread registration.
 *
 * Replaces base::IProxy. Note what is absent: the old interface took the segment as a
 * template template parameter and instantiated a *second*, otherwise-unused
 * specialization (`Seg<T, void, Opt, void>`) purely to run a static assert. A concept
 * constrains without instantiating.
 *
 * @note Operating on a proxy without holding a ticket is undefined. `acquire()` is
 *       idempotent per thread; `release()` is safe to call repeatedly.
 */
template <typename P, typename T>
concept Proxy = Queue<P, T> && requires(P p) {
    { p.acquire() } noexcept -> std::same_as<bool>;
    { p.release() } noexcept -> std::same_as<void>;
};

} // namespace core
