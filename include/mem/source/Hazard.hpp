#pragma once
/**
 * @file Hazard.hpp
 * @brief Segment source reclaiming by hazard pointers: allocate on demand, free once unobserved.
 * @ingroup mem
 */

#include <core/SegmentTraits.hpp>
#include <mem/SingleBlock.hpp>
#include <mem/source/Payload.hpp>
#include <meta/OptionsPack.hpp>
#include <util/specs.hpp>
#include <util/threading/ThreadRegistry.hpp>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdlib>
#include <optional>
#include <utility>
#include <vector>

namespace mem::source {

struct HazardOpt {
    /**
     * @brief Retired objects a thread accumulates before it attempts a reclamation scan.
     */
    template <auto N> struct retire_threshold {};
};

/**
 * @brief Segment source backed by hazard pointers: allocate on demand, free when unseen.
 *
 * Models core::SegmentSource. `acquire()` allocates and therefore never fails, so a proxy
 * over this source is unbounded unless an admission policy says otherwise.
 *
 * ## Where the per-thread state lives
 *
 * A thread participating here publishes two things of its own: the segment it is currently
 * reading (the hazard pointer) and the segments it has retired but not yet freed. Both live
 * in that thread's util::threading::ThreadRegistry node, alongside @p Payload -- whatever the
 * caller needs per thread. One node, one thread-local lookup, no side arrays indexed by a
 * ticket:
 *
 * @code
 * struct ThreadData { std::atomic<S*> hp; std::vector<S*> retired; Payload user; };
 * @endcode
 *
 * That is what makes scanning cost O(attached threads) rather than O(a configured ceiling),
 * and the scan runs once per retired object per collection pass, so the difference is not
 * academic.
 *
 * @note Reclaiming with `delete obj` is not an option here: a co-allocated segment is
 *       placement-new'd into std::aligned_alloc storage and must be released through
 *       SingleBlock::destroy. Pairing `delete` with `aligned_alloc` is undefined and breaks
 *       outright under sized or aligned deallocation.
 *
 * @note One hazard slot per thread is enough: the traversal protects the head *or* the tail
 *       at any moment, never both.
 *
 * @tparam Payload the caller's per-thread state; empty by default and elided entirely.
 * @tparam Opt HazardOpt tags; see HazardOpt::retire_threshold.
 */
template <typename S, typename Payload = NoPayload, typename Opt = meta::EmptyOptions>
    requires meta::AcceptsOnly<Opt, meta::ValueOption<HazardOpt::retire_threshold>>
class Hazard {
    
    static constexpr std::size_t kRetireThreshold = static_cast<std::size_t>(
        Opt::template get<HazardOpt::retire_threshold, std::size_t{64}>);
    static_assert(kRetireThreshold != 0,
                  "HazardOpt::retire_threshold must be non-zero, or nothing is ever reclaimed");

    /**
     * @brief One thread's published state.
     *
     * Cache-line aligned here rather than in the registry, so that a thread's stores to its
     * hazard pointer do not dirty the line another thread is walking for links.
     */
    struct alignas(CACHE_LINE) ThreadData {
        std::atomic<S*> hp{nullptr};    //hazard pointer
        std::vector<S*> retired;        //per-thread free list
        [[no_unique_address]] Payload user{};
    };

    using Registry = util::threading::ThreadRegistry<ThreadData>;
    using Node = typename Registry::Node;

public:
    /// Retirements a thread accumulates before scanning; see HazardOpt::retire_threshold.
    static constexpr std::size_t retire_threshold = kRetireThreshold;

    using handle = S*;
    using thread_payload = Payload;
    /// RAII thread registration; see join().
    using session = typename Registry::session;

    /// this source doesn't perform memory recycling
    static constexpr bool recycles = false;

    static constexpr handle nil() noexcept { return nullptr; }

    /// 0: this source allocates on demand, so nothing bounds how many segments are live.
    static constexpr std::size_t live_segments() noexcept { return 0; }

    /**
     * @brief: RAII Guard Protection Scope
     */
    class guard {
        Node* node_;

    public:
        explicit guard(Node* n) noexcept : node_{n} {}
        guard(const guard&) = delete;
        guard& operator=(const guard&) = delete;
        /*
         * @brief: guard destructor
         * clears the hazard pointer registered for the caller
         */
        ~guard() { node_->data.hp.store(nullptr, std::memory_order_release); }

        Node* node() const noexcept { return node_; }

        Payload& payload() const noexcept { return node_->data.user; }
    };

    /// @brief: Hazard Constructor
    /// @param segment_capacity capacity handed to each segment this source creates
    explicit Hazard(std::size_t segment_capacity) : seg_capacity_{segment_capacity} {
        assert(segment_capacity != 0);
    }

    Hazard(const Hazard&) = delete;
    Hazard& operator=(const Hazard&) = delete;

    /**
     * @brief: Hazard Destructor
     * 
     * Drains every node's retire list, attached or not
     */
    ~Hazard() {
        registry_.for_each_node([](ThreadData& d) {
            for (S* p : d.retired) mem::SingleBlock<S>::destroy(p);
            d.retired.clear();
        });
    }

    guard pin() noexcept { return guard{node()}; }

    /// Publish @p h as protected. The caller must re-validate before dereferencing.
    handle protect(guard& g, handle h) noexcept {
        g.node()->data.hp.store(h, std::memory_order_seq_cst);
        return h;
    }

    /**
     * @brief No-op: hazard protection is per-handle and holds nothing back.
     *
     * The concept requires this so a proxy can call it without asking which source it has.
     * There is no epoch here to advance -- a thread's hazard slot names one segment and
     * delays only that segment's reclamation, never anyone else's progress.
     */
    /// @return always false: there is no epoch here, so nothing ever moves and every handle
    ///         the caller already holds stays protected.
    constexpr bool renew(guard&) const noexcept { return false; }

    S* deref(handle h) const noexcept { return h; }

    /// @brief: acquires a new segment from the source
    /// 
    /// @returns: optional handle (never nullopt)
    /// 
    /// allocates a new segment with the capacity given to Hazard constructor
    std::optional<handle> acquire() {
        return std::optional<handle>{mem::SingleBlock<S>::create(seg_capacity_)};
    }

    /// @brief: acquires a new segment from the source
    /// 
    /// @note: behaves the same as `::acquire()`
    template <typename Retry>
    std::optional<handle> acquire(Retry&&) {
        return acquire();
    }

    /**
     * @brief: discards a memory region which was not shared
     */
    void discard(handle h) noexcept { mem::SingleBlock<S>::destroy(h); }

    /**
     * @brief: retires a memory region which was shared and possibly still
     * be referenced
     */
    void retire(handle h) noexcept {
        if (!h) return;
        auto& mine = node()->data.retired;
        mine.push_back(h);
        if (mine.size() >= kRetireThreshold) collect(mine);
    }

    /**
     * @brief Fold @p op over every attached thread's caller-owned payload.
     */
    template <typename Acc, typename Op>
    Acc reduce_payloads(Acc init, Op op) const noexcept {
        return registry_.reduce(std::move(init), [&op](Acc a, const ThreadData& d) {
            return op(std::move(a), d.user);
        });
    }

    /**
     * @brief: Attach the calling thread for the duration of the returned scope.
     */
    [[nodiscard]] session join() { return registry_.join(); }

    /**
     * @brief: Try to attach a calling thread
     */
    [[nodiscard]] session try_join() noexcept { return registry_.try_join(); }

private:
    /**
     * @brief This thread's registry node, attaching on first use.
     */
    Node* node() {
        if (Node* n = registry_.self()) return n;
        if (registry_.try_attach()) return registry_.self();
        registry_.attach();
        return registry_.self();
    }

    /**
     * @brief Free everything in @p mine that no attached thread is protecting.
     *
     * The two collections being matched are asymmetric, and the loop nesting has to respect
     * that. The retire list is a contiguous vector; the registry is a chain of nodes, one
     * cache miss per hop.
     *
     * So the registry is reduced over once, and each node scans the vector. The vector is
     * partitioned in place as we go, with @c k the number of entries already known to be
     * protected:
     *
     * @code
     *   [0, k)          protected -- somebody published this pointer
     *   [k, size)       not yet matched by any node seen so far
     * @endcode
     *
     */
    void collect(std::vector<S*>& mine) noexcept {
        const std::size_t k =
            registry_.reduce(std::size_t{0}, [&mine](std::size_t k, const ThreadData& d) {
                if (k == mine.size()) return k; // all protected: nothing left to learn
                S* const hp = d.hp.load(std::memory_order_acquire);
                if (hp == nullptr) return k;    // attached but reading nothing
                for (std::size_t i = k; i < mine.size(); ++i) {
                    if (mine[i] == hp) {
                        std::swap(mine[i], mine[k]);
                        return k + 1;
                    }
                }
                return k;
            });

        while (mine.size() > k) {
            S* p = mine.back();
            mine.pop_back();
            mem::SingleBlock<S>::destroy(p);
        }
    }

    const std::size_t seg_capacity_;
    Registry registry_;
};

} // namespace mem::source
