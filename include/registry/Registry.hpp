#pragma once
#include <algo/FAAArray.hpp>
#include <core/Construction.hpp>
#include <algo/HQ.hpp>
#include <algo/Spin.hpp>
#include <algo/LFring.hpp>
#include <algo/Mutex.hpp>
#include <algo/PRQ.hpp>
#include <algo/PSCQ.hpp>
#include <algo/SCQ.hpp>
#include <algo/Vyukov.hpp>
#include <algo/VyukovDCAS.hpp>
#include <algo/VyukovNoABA.hpp>
#include <concepts>
#include <meta/FixedString.hpp>
#include <variant>
#include <meta/TypeList.hpp>
#include <proxy/Aliases.hpp>
#include <string>
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

/// Size of every pooled entry below. Named once because it appears twice per entry -- in
/// the segment's handle policy and in the proxy alias -- and LinkedProxy static_asserts
/// that the two agree.
/**
 * @brief Segments a bounded queue may have live at once, for *every* bounded family.
 *
 * Read by the pooled entries as the pool size and by the item- and chunk-bounded ones as their
 * segment count, so the three are always the same geometry and a comparison between them is
 * measuring the algorithm rather than the layout. Previously the chunk count was a defaulted
 * constructor argument nothing passed, so it stayed at 4 while this constant moved -- and the
 * families silently diverged.
 */
inline constexpr std::size_t kPoolSize = 4;

// Handle-policy shorthands: a pooled source addresses segments by index, not pointer, and
// the index is sized by the pool so the rest of the word is ABA counter.
template <typename T, std::size_t N = kPoolSize>
using IdxVyukov = seg::Vyukov<T, meta::EmptyOptions, mem::IndexHandle<N>>;
template <typename T, std::size_t N = kPoolSize>
using IdxPRQ = seg::PRQ<T, meta::EmptyOptions, mem::IndexHandle<N>>;
template <typename T, std::size_t N = kPoolSize>
using IdxSCQ = seg::SCQ<T, meta::EmptyOptions, mem::IndexHandle<N>>;
template <typename T, std::size_t N = kPoolSize>
using IdxFAAArray = seg::FAAArray<T, meta::EmptyOptions, mem::IndexHandle<N>>;
template <typename T, std::size_t N = kPoolSize>
using IdxHQ = seg::HQ<T, meta::EmptyOptions, mem::IndexHandle<N>>;
template <typename T, std::size_t N = kPoolSize>
using IdxMutex = seg::Mutex<T, meta::EmptyOptions, mem::IndexHandle<N>>;
template <typename T, std::size_t N = kPoolSize>
using IdxSpin = seg::Spin<T, meta::EmptyOptions, mem::IndexHandle<N>>;
template <typename T, std::size_t N = kPoolSize>
using IdxPSCQ = seg::PSCQ<T, meta::EmptyOptions, mem::IndexHandle<N>>;
template <typename T, std::size_t N = kPoolSize>
using IdxDCAS = seg::VyukovDCAS<T, meta::EmptyOptions, mem::IndexHandle<N>>;
template <typename T, std::size_t N = kPoolSize>
using IdxNoABA = seg::VyukovNoABA<T, meta::EmptyOptions, mem::IndexHandle<N>>;


/**
 * @brief Standalone bounded queues. Everything here models core::Queue.
 *
 * FAAArray and HQ are deliberately absent, and cannot even be named here: both are
 * constrained on linkage::Linked. Their indices only advance, so a *standalone* one has no
 * way back to the start -- the first fill/drain works and every later enqueue is refused
 * (measured: cycle 0 accepts 8, cycles 1-2 accept 0). Reopening them is a proxy-side
 * operation: it needs the segment to be quiescent and unlinked first, which only a source
 * that recycles can guarantee. See FAAArray::reopen.
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
    Entry<"mutex", queue::Mutex<T>>,
    Entry<"spin",queue::Spin<T>>
>;

/**
 * @brief Linked queues: proxy x segment. Everything here models core::Proxy.
 *
 * The full grid -- four proxies over six segments. Kept complete rather than curated: the
 * point of the registry is that a combination costs one line, so leaving cells empty only
 * means nobody can benchmark them. `mutex` is in every family on purpose; a lock-based
 * segment under each proxy is the baseline the lock-free ones are worth measuring against.
 *
 * @note Segment spelling is not uniform (`faaarray` in three families, `faa` in the pooled
 *       one). Left alone deliberately: the names appear in saved benchmark CSVs, and a
 *       rename would silently orphan them.
 */
template <typename T>
using Linked = meta::TypeList<
    Entry<"u-vyukov", proxy::Unbounded<T, seg::Vyukov<T>>>,
    Entry<"u-prq", proxy::Unbounded<T, seg::PRQ<T>>>,
    Entry<"u-faa", proxy::Unbounded<T, seg::FAAArray<T>>>,
    Entry<"u-hq", proxy::Unbounded<T, seg::HQ<T>>>,
    Entry<"u-scq", proxy::Unbounded<T, seg::SCQ<T>>>,
    Entry<"u-mutex", proxy::Unbounded<T, seg::Mutex<T>>>,
    Entry<"u-spin", proxy::Unbounded<T, seg::Spin<T>>>,
    Entry<"u-pscq", proxy::Unbounded<T, seg::PSCQ<T>>>,
    Entry<"u-dcas", proxy::Unbounded<T, seg::VyukovDCAS<T>>>,
    Entry<"u-noaba", proxy::Unbounded<T, seg::VyukovNoABA<T>>>,

    Entry<"item-vyukov", proxy::ItemBounded<T, seg::Vyukov<T>, kPoolSize>>,
    Entry<"item-prq", proxy::ItemBounded<T, seg::PRQ<T>, kPoolSize>>,
    Entry<"item-faa", proxy::ItemBounded<T, seg::FAAArray<T>, kPoolSize>>,
    Entry<"item-hq", proxy::ItemBounded<T, seg::HQ<T>, kPoolSize>>,
    Entry<"item-scq", proxy::ItemBounded<T, seg::SCQ<T>, kPoolSize>>,
    Entry<"item-mutex", proxy::ItemBounded<T, seg::Mutex<T>, kPoolSize>>,
    Entry<"item-spin", proxy::ItemBounded<T, seg::Spin<T>, kPoolSize>>,
    Entry<"item-pscq", proxy::ItemBounded<T, seg::PSCQ<T>, kPoolSize>>,
    Entry<"item-dcas", proxy::ItemBounded<T, seg::VyukovDCAS<T>, kPoolSize>>,
    Entry<"item-noaba", proxy::ItemBounded<T, seg::VyukovNoABA<T>, kPoolSize>>,

    Entry<"chunk-vyukov", proxy::ChunkBounded<T, seg::Vyukov<T>, kPoolSize>>,
    Entry<"chunk-prq", proxy::ChunkBounded<T, seg::PRQ<T>, kPoolSize>>,
    Entry<"chunk-faa", proxy::ChunkBounded<T, seg::FAAArray<T>, kPoolSize>>,
    Entry<"chunk-hq", proxy::ChunkBounded<T, seg::HQ<T>, kPoolSize>>,
    Entry<"chunk-scq", proxy::ChunkBounded<T, seg::SCQ<T>, kPoolSize>>,
    Entry<"chunk-mutex", proxy::ChunkBounded<T, seg::Mutex<T>, kPoolSize>>,
    Entry<"chunk-spin", proxy::ChunkBounded<T, seg::Spin<T>, kPoolSize>>,
    Entry<"chunk-pscq", proxy::ChunkBounded<T, seg::PSCQ<T>, kPoolSize>>,
    Entry<"chunk-dcas", proxy::ChunkBounded<T, seg::VyukovDCAS<T>, kPoolSize>>,
    Entry<"chunk-noaba", proxy::ChunkBounded<T, seg::VyukovNoABA<T>, kPoolSize>>,

    // Pooled: the bound is the pool running dry, so the admission policy is None. FAAArray
    // and HQ are here now that their reopen() flips a generation flag instead of failing;
    // before that they were not recyclable and mem::source::Pool refused them outright.
    Entry<"mem-vyukov", proxy::MemBounded<T, IdxVyukov<T>, kPoolSize>>,
    Entry<"mem-prq", proxy::MemBounded<T, IdxPRQ<T>, kPoolSize>>,
    Entry<"mem-scq", proxy::MemBounded<T, IdxSCQ<T>, kPoolSize>>,
    Entry<"mem-faa", proxy::MemBounded<T, IdxFAAArray<T>, kPoolSize>>,
    Entry<"mem-hq", proxy::MemBounded<T, IdxHQ<T>, kPoolSize>>,
    Entry<"mem-mutex", proxy::MemBounded<T, IdxMutex<T>, kPoolSize>>,
    Entry<"mem-spin", proxy::MemBounded<T, IdxSpin<T>, kPoolSize>>,
    Entry<"mem-pscq", proxy::MemBounded<T, IdxPSCQ<T>, kPoolSize>>,
    Entry<"mem-dcas", proxy::MemBounded<T, IdxDCAS<T>, kPoolSize>>,
    Entry<"mem-noaba", proxy::MemBounded<T, IdxNoABA<T>, kPoolSize>>
    >;

/**
 * @brief Tuning variants, deliberately **outside** `All`.
 *
 * Two things live here that must not be in the headline sweep:
 *
 *  - **Instrumented entries.** `ProxyOpt::segment_stats` puts atomics on the link path, so a
 *    counter run and a throughput run have to be separate passes. Keeping them in a separate
 *    list is what stops them being mixed by accident.
 *  - **The backoff grid.** `patience` is a template parameter, so every value is a distinct
 *    type and needs its own entry. Sweeping them from `All` would multiply the cost of every
 *    full run for a question only two algorithms are being asked.
 *
 * Names carry the value (`u-faa-p0`) so a CSV row is self-describing.
 * @see docs/"Benchmark Notes - Concurrent Queue Implementations.md"
 */
template <typename T>
using Stats = meta::OptionsPack<proxy::ProxyOpt::segment_stats>;

template <typename T, std::size_t P>
using FAAp = seg::FAAArray<T, meta::OptionsPack<algo::FAAArrayOpt::patience<P>>>;
template <typename T, std::size_t P>
using HQp = seg::HQ<T, meta::OptionsPack<algo::HQOpt::patience<P>>>;

/// Instrumented: same shape as the `All` entry, with the segment counters switched on. These are
/// what the slot-efficiency formulas need -- W = S*n - i needs S, and nothing else does.
template <typename T>
using Instrumented = meta::TypeList<
    Entry<"i-u-faa", proxy::Unbounded<T, seg::FAAArray<T>, meta::EmptyOptions, Stats<T>>>,
    Entry<"i-u-hq", proxy::Unbounded<T, seg::HQ<T>, meta::EmptyOptions, Stats<T>>>,
    Entry<"i-u-prq", proxy::Unbounded<T, seg::PRQ<T>, meta::EmptyOptions, Stats<T>>>,
    Entry<"i-u-pscq", proxy::Unbounded<T, seg::PSCQ<T>, meta::EmptyOptions, Stats<T>>>,
    Entry<"i-u-scq", proxy::Unbounded<T, seg::SCQ<T>, meta::EmptyOptions, Stats<T>>>,
    Entry<"i-u-vyukov", proxy::Unbounded<T, seg::Vyukov<T>, meta::EmptyOptions, Stats<T>>>,
    Entry<"i-u-noaba", proxy::Unbounded<T, seg::VyukovNoABA<T>, meta::EmptyOptions, Stats<T>>>>;

/// The backoff grid. `p0` is the no-backoff case the notes expect to fail pathologically.
template <typename T>
using Backoff = meta::TypeList<
    Entry<"u-faa-p0", proxy::Unbounded<T, FAAp<T, 0>, meta::EmptyOptions, Stats<T>>>,
    Entry<"u-faa-p16", proxy::Unbounded<T, FAAp<T, 16>, meta::EmptyOptions, Stats<T>>>,
    Entry<"u-faa-p64", proxy::Unbounded<T, FAAp<T, 64>, meta::EmptyOptions, Stats<T>>>,
    Entry<"u-faa-p256", proxy::Unbounded<T, FAAp<T, 256>, meta::EmptyOptions, Stats<T>>>,
    Entry<"u-faa-p1024", proxy::Unbounded<T, FAAp<T, 1024>, meta::EmptyOptions, Stats<T>>>,
    Entry<"u-faa-p4096", proxy::Unbounded<T, FAAp<T, 4096>, meta::EmptyOptions, Stats<T>>>,
    Entry<"u-hq-p0", proxy::Unbounded<T, HQp<T, 0>, meta::EmptyOptions, Stats<T>>>,
    Entry<"u-hq-p16", proxy::Unbounded<T, HQp<T, 16>, meta::EmptyOptions, Stats<T>>>,
    Entry<"u-hq-p64", proxy::Unbounded<T, HQp<T, 64>, meta::EmptyOptions, Stats<T>>>,
    Entry<"u-hq-p256", proxy::Unbounded<T, HQp<T, 256>, meta::EmptyOptions, Stats<T>>>,
    Entry<"u-hq-p1024", proxy::Unbounded<T, HQp<T, 1024>, meta::EmptyOptions, Stats<T>>>,
    Entry<"u-hq-p4096", proxy::Unbounded<T, HQp<T, 4096>, meta::EmptyOptions, Stats<T>>>>;

/// What `mpmc_tune` sweeps.
template <typename T>
using Tuning = meta::concat<Instrumented<T>, Backoff<T>>;

/// Everything, for the benchmark.
template <typename T>
using All = meta::concat<Standalone<T>, Linked<T>>;

/**
 * @brief Owns one instance of any registered queue, however it happens to be built.
 *
 * A standalone queue is a single block and comes from Q::create(capacity); a proxy is an
 * ordinary object taking (segment_capacity). This hides that difference so
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
                  "directly constructible from (segment_capacity)");
    static_assert(core::Constructible<Q>,
                  "registry entry satisfies both construction shapes; which one applies "
                  "is ambiguous");

public:
    explicit Instance(std::size_t capacity) {
        if constexpr (core::BlockAllocated<Q>) block_.reset(Q::create(capacity));
        else owned_ = std::make_unique<Q>(capacity);
    }

    Q& get() noexcept {
        if constexpr (core::BlockAllocated<Q>) return *block_;
        else return *owned_;
    }

    /**
     * @brief A scope in which this thread may use @p q.
     *
     * Empty for standalone queues, which have no notion of participation. Hold it for as
     * long as the thread uses the queue; there is nothing to release by hand, which is what
     * makes an early return safe.
     */
    [[nodiscard]] static auto session(Q& q) {
        if constexpr (core::Joinable<Q>) return q.join();
        else {
            (void)q;
            return std::monostate{};
        }
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

namespace detail {
template <typename Q, typename... Es>
constexpr std::string_view lookup_name(meta::TypeList<Es...>) noexcept {
    std::string_view found{};
    // Short-circuits on the match; `found` stays empty for a type that is not registered.
    //
    // The fold's own `bool` is only the short-circuit mechanism -- the answer is `found` -- so
    // it is discarded, and the cast says so explicitly. Without it clang reports an unused
    // expression result once per instantiation, and each report reproduces the entire expanded
    // registry type: sixty warnings, several kilobytes each.
    (void)((std::same_as<Q, typename Es::type> ? (found = Es::name.view(), true) : false) || ...);
    return found;
}
} // namespace detail

/// The registry name of @p Q, or an empty view when @p Q is not in @p List.
template <typename List, typename Q>
inline constexpr std::string_view name_of = detail::lookup_name<Q>(List{});

/**
 * @brief gtest type-parameter names, taken from the registry.
 *
 * Without this a typed suite over the registry identifies its cases by index, and a failure
 * reports the type by its fully expanded template-id -- for a pooled entry that is a
 * `LinkedProxy<Data*, algo::Mutex<Data*, OptionsPack<>, Node<IndexHandle<8>>>, admit::None,
 * Pool<...same segment again..., 8, ThreadMeta<VersionedIndex<8>>>>`, which says nothing a
 * reader did not already know and cannot be typed into `--gtest_filter`.
 *
 * The names are already in the registry, so this just reads them back:
 *
 * ```cpp
 * TYPED_TEST_SUITE(Mpmc, AllTypes, registry::TestNames<registry::All<Item>>);
 * ```
 *
 * turns `Mpmc/24.Foo` into `Mpmc/mem_mutex.Foo`.
 *
 * @note Hyphens become underscores: gtest requires the suffix to be alphanumeric, and a
 *       name it rejects is a run-time abort rather than a compile error.
 */
template <typename List>
struct TestNames {
    template <typename Q>
    static std::string GetName(int i) {
        const std::string_view n = name_of<List, Q>;
        if (n.empty()) return std::to_string(i); // unregistered: fall back to gtest's index
        std::string out{n};
        for (char& c : out)
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')))
                c = '_';
        return out;
    }
};

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
