#pragma once
#include <algo/FAAArray.hpp>
#include <core/Construction.hpp>
#include <algo/HQ.hpp>
#include <algo/LFring.hpp>
#include <algo/Mutex.hpp>
#include <algo/PRQ.hpp>
#include <algo/PSCQ.hpp>
#include <algo/SCQ.hpp>
#include <algo/Vyukov.hpp>
#include <algo/VyukovDCAS.hpp>
#include <algo/VyukovNoABA.hpp>
#include <meta/FixedString.hpp>
#include <meta/TypeList.hpp>
#include <proxy/Aliases.hpp>
#include <string_view>

/**
 * @file Registry.hpp
 * @brief The single place that declares which implementations exist.
 *
 * Adding an implementation used to mean editing the segment header, both proxy type
 * lists, an umbrella include-everything header, and the benchmark's switch -- and
 * accepting that proxies could not be benchmarked at all. Now it is one line here, and
 * both the typed test suites and the benchmark pick it up.
 */
namespace registry {

/// One implementation, carrying its own name in the type system.
template <meta::FixedString Name, typename Q>
struct Entry {
    static constexpr auto name = Name;
    using type = Q;
};

// Handle-policy shorthands: a pooled source addresses segments by index, not pointer.
template <typename T> using IdxVyukov = seg::Vyukov<T, meta::EmptyOptions, mem::IndexHandle>;
template <typename T> using IdxPRQ = seg::PRQ<T, meta::EmptyOptions, mem::IndexHandle>;
template <typename T> using IdxSCQ = seg::SCQ<T, meta::EmptyOptions, mem::IndexHandle>;

/**
 * @brief Standalone bounded queues. Everything here models core::Queue.
 *
 * FAAArray and HQ are deliberately absent. Their cells are write-once and their indices
 * never reset, so standalone they are single-use: the first fill/drain works and every
 * later enqueue is refused (measured: cycle 0 accepts 8, cycles 1-2 accept 0). They are
 * sound only as linked segments, where the proxy discards a drained segment rather than
 * reusing it -- which is also what segment_traits<>::recyclable == false records.
 *
 * PRQ is present and correct standalone (its dequeue drags an over-run tail back), but
 * expect poor throughput when producers spin on a full ring: the tail runs ahead on every
 * refused enqueue and consumers must keep pulling it back.
 */
template <typename T>
using Standalone = meta::TypeList<
    Entry<"vyukov", queue::Vyukov<T>>,
    Entry<"vyukov-noaba", queue::VyukovNoABA<T>>,
    Entry<"vyukov-dcas", queue::VyukovDCAS<T>>,
    Entry<"scq", queue::SCQ<T>>,
    Entry<"pscq", queue::PSCQ<T>>,
    Entry<"mutex", queue::Mutex<T>>>;

/// Linked queues: proxy x segment. Everything here models core::Proxy.
template <typename T>
using Linked = meta::TypeList<
    Entry<"u-vyukov", proxy::Unbounded<T, seg::Vyukov<T>>>,
    Entry<"u-prq", proxy::Unbounded<T, seg::PRQ<T>>>,
    Entry<"u-faaarray", proxy::Unbounded<T, seg::FAAArray<T>>>,
    Entry<"u-hq", proxy::Unbounded<T, seg::HQ<T>>>,
    Entry<"u-scq", proxy::Unbounded<T, seg::SCQ<T>>>,

    Entry<"item-vyukov", proxy::ItemBounded<T, seg::Vyukov<T>>>,
    Entry<"item-prq", proxy::ItemBounded<T, seg::PRQ<T>>>,

    Entry<"chunk-vyukov", proxy::ChunkBounded<T, seg::Vyukov<T>>>,
    Entry<"chunk-prq", proxy::ChunkBounded<T, seg::PRQ<T>>>,
    Entry<"chunk-faaarray", proxy::ChunkBounded<T, seg::FAAArray<T>>>,

    // Pooled: the bound is the pool running dry, so the admission policy is None.
    // FAAArray and HQ cannot appear here -- segment_traits says they are not
    // recyclable, and mem::source::Pool static_asserts on exactly that.
    Entry<"mem-vyukov", proxy::MemBounded<T, IdxVyukov<T>, 8>>,
    Entry<"mem-prq", proxy::MemBounded<T, IdxPRQ<T>, 8>>,
    Entry<"mem-scq", proxy::MemBounded<T, IdxSCQ<T>, 8>>>;

/// Everything, for the benchmark.
template <typename T>
using All = meta::concat<Standalone<T>, Linked<T>>;

/**
 * @brief Owns one instance of any registered queue, however it happens to be built.
 *
 * A standalone queue is a single block and comes from Q::create(capacity); a proxy is an
 * ordinary object taking (segment_capacity, max_threads). This hides that difference so
 * one benchmark body can drive both -- which is what the old
 * `Benchmark<template<typename> typename>` could not express, and why no proxy was ever
 * benchmarkable.
 */
template <typename Q>
class Instance {
    // Declared, not probed. `Constructible` holds only when exactly one shape applies, so
    // a queue that supports both, or neither, is a diagnosable error here rather than a
    // silent wrong branch followed by a confusing constructor failure elsewhere.
    static_assert(core::BlockAllocated<Q> || core::DirectConstructed<Q>,
                  "registry entry is neither block-allocated (Q::create(capacity)) nor "
                  "directly constructible from (segment_capacity, max_threads)");
    static_assert(core::Constructible<Q>,
                  "registry entry satisfies both construction shapes; which one applies "
                  "is ambiguous");

public:
    Instance(std::size_t capacity, std::size_t max_threads) {
        if constexpr (core::BlockAllocated<Q>) {
            (void)max_threads;
            block_.reset(Q::create(capacity));
        } else {
            owned_ = std::make_unique<Q>(capacity, max_threads);
        }
    }

    Q& get() noexcept {
        if constexpr (core::BlockAllocated<Q>) return *block_;
        else return *owned_;
    }

    /// Proxies require a per-thread ticket; standalone queues have no such notion.
    static void join(Q& q) noexcept {
        if constexpr (core::Ticketed<Q>) (void)q.acquire();
    }
    static void leave(Q& q) noexcept {
        if constexpr (core::Ticketed<Q>) q.release();
    }

private:
    mem::unique_block<Q> block_{};
    std::unique_ptr<Q> owned_{};
};

namespace detail {
template <typename List>
struct ToTypes;
template <typename... Es>
struct ToTypes<meta::TypeList<Es...>> {
    template <template <typename...> class F>
    using apply = F<typename Es::type...>;
};
} // namespace detail

/// Expand a registry list into a gtest type list: registry::AsTypes<List>::apply<::testing::Types>
template <typename List>
using AsTypes = detail::ToTypes<List>;

/**
 * @brief Call @p f with the entry whose name matches @p name.
 *
 * A short-circuiting fold over the list. No vtable, no std::variant, no std::function:
 * the compiler emits one branch per entry and the selected body is instantiated with the
 * concrete type, so the measured loop stays monomorphic. Selection happens once at
 * startup, outside anything being timed.
 *
 * @return true if a name matched.
 */
template <typename List, typename F>
bool dispatch(std::string_view name, F&& f);

template <typename... Es, typename F>
bool dispatch_impl(meta::TypeList<Es...>, std::string_view name, F&& f) {
    return ([&]() -> bool {
        if (Es::name == name) {
            f.template operator()<typename Es::type>();
            return true;
        }
        return false;
    }() || ...);
}

/// Names in a list, for a usage message.
template <typename... Es, typename F>
void for_each_name(meta::TypeList<Es...>, F&& f) {
    (f(std::string_view{Es::name}), ...);
}

} // namespace registry
