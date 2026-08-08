#pragma once
#include <core/SegmentTraits.hpp>
#include <mem/SingleBlock.hpp>
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

#ifndef HAZARD_RETIRE_THRESHOLD
/// Retired objects per thread before a reclamation scan is attempted.
#define HAZARD_RETIRE_THRESHOLD 64
#endif

/// Default payload for a source used without a proxy: costs nothing.
struct NoPayload {};

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
 */
template <typename S, typename Payload = NoPayload>
class Hazard {
    /**
     * @brief One thread's published state.
     *
     * Cache-line aligned here rather than in the registry, so that a thread's stores to its
     * hazard pointer do not dirty the line another thread is walking for links.
     *
     * `retired` is deliberately **not** cleared when a node is recycled. A thread that
     * detaches with retirements still pending has not leaked them: the next thread to inherit
     * the node inherits the list and will collect it. Clearing here would drop segments.
     */
    struct alignas(CACHE_LINE) ThreadData {
        std::atomic<S*> hp{nullptr};
        std::vector<S*> retired;
        [[no_unique_address]] Payload user{};
    };

    /**
     * Flip to `meta::OptionsPack<util::threading::ThreadRegistryOpt::retry_scan_on_attach>`
     * to make a scan that races an attaching thread retry rather than rely on the ordering
     * argument in the ThreadRegistry docs. Worth doing for stress and TSan runs.
     */
    using Registry = util::threading::ThreadRegistry<ThreadData>;
    using Node = typename Registry::Node;

public:
    using handle = S*;
    using thread_payload = Payload;
    /// RAII thread registration; see join().
    using session = typename Registry::session;

    /// Every acquire() allocates a fresh segment, so none ever needs reopening.
    static constexpr bool recycles = false;

    static constexpr handle nil() noexcept { return nullptr; }

    /**
     * @brief RAII protection scope.
     *
     * Replaces manual protect/clear pairing. The old proxies cleared by hand on every exit
     * path -- three of them in dequeue alone -- so correctness depended on every future
     * `return` remembering.
     */
    class guard {
        Node* node_;

    public:
        explicit guard(Node* n) noexcept : node_{n} {}
        guard(const guard&) = delete;
        guard& operator=(const guard&) = delete;
        ~guard() { node_->data.hp.store(nullptr, std::memory_order_release); }

        Node* node() const noexcept { return node_; }

        /// This thread's caller-owned state. No second lookup: the pin already found it.
        Payload& payload() const noexcept { return node_->data.user; }
    };

    /// @param segment_capacity capacity handed to each segment this source creates
    explicit Hazard(std::size_t segment_capacity) : seg_capacity_{segment_capacity} {
        assert(segment_capacity != 0);
    }

    Hazard(const Hazard&) = delete;
    Hazard& operator=(const Hazard&) = delete;

    /// Drains every node's retire list, attached or not -- a detached thread's pending
    /// retirements are still ours to destroy.
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

    S* deref(handle h) const noexcept { return h; }

    /// Allocates. Never returns nullopt; throws only if the allocator does.
    std::optional<handle> acquire() {
        return std::optional<handle>{mem::SingleBlock<S>::create(seg_capacity_)};
    }

    /// Never published to another thread, so no scan is needed.
    void discard(handle h) noexcept { mem::SingleBlock<S>::destroy(h); }

    /// Was reachable by other threads: defer until no hazard slot names it.
    void retire(handle h) noexcept {
        if (!h) return;
        auto& mine = node()->data.retired;
        mine.push_back(h);
        if (mine.size() >= HAZARD_RETIRE_THRESHOLD) collect(mine);
    }

    /**
     * @brief Fold @p op over every attached thread's caller-owned payload.
     *
     * The payload lives in this source's registry, so anything that wants to aggregate it --
     * `LinkedProxy::size()` summing per-thread operation counts -- has to come through here.
     */
    template <typename Acc, typename Op>
    Acc reduce_payloads(Acc init, Op op) const noexcept {
        return registry_.reduce(std::move(init), [&op](Acc a, const ThreadData& d) {
            return op(std::move(a), d.user);
        });
    }

    /**
     * @brief Attach the calling thread for the duration of the returned scope.
     *
     * The allocating path, taken once per thread; every later `pin()` is a thread-local
     * read. Holding the session is what keeps that true, and its destructor is what returns
     * the node -- there is no unregister to forget.
     */
    [[nodiscard]] session join() { return registry_.join(); }

    /// Non-allocating join, for a caller that must not touch the allocator.
    [[nodiscard]] session try_join() noexcept { return registry_.try_join(); }

private:
    /**
     * @brief This thread's registry node, attaching on first use.
     *
     * Three tiers, cheapest first: a thread-local read; then a lock-free claim of a node that
     * is already free; then, only if the registry has none, an allocation. The last tier can
     * throw, and this is reached from `pin()`, which is noexcept -- so an allocation failure
     * terminates. That is the same bargain `retire()` already makes by pushing onto a vector
     * in a noexcept function, and it is why `join()` exists: holding a session for the life
     * of the thread keeps every later operation on the first tier.
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
     * cache miss per hop. Asking "is this pointer protected?" once per retired pointer walked
     * that chain R times, when the whole question can be answered in **one** walk.
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
     * A node whose hazard pointer hits an entry in the unclassified region swaps it down to
     * @c k and widens the prefix, so each node only ever scans the region still in question,
     * and that region shrinks. When the walk ends, `[k, size)` is exactly the set nobody is
     * reading: pop and destroy it. No temporary container, and every vector operation is
     * O(1) -- `swap`, `size`, `back`, `pop_back`.
     *
     * @note The inner `break` relies on a pointer appearing **at most once** in the retire
     *       list, which holds because the proxy retires a handle once, on the unlink that made
     *       it unreachable. A duplicate would be a double free here, but it would equally have
     *       been one before this change.
     *
     * @note Carrying @c k across nodes is sound because ThreadRegistry's walk visits each node
     *       exactly once: the active list is append-only and the walk is a pure read, with no
     *       restarts and no helping. If that ever changes, this has to become two passes.
     *
     * @note Reading each hazard slot once per pass is the ordinary hazard-pointer scan. A
     *       retired object was unlinked before `retire()` was called, so no thread can newly
     *       acquire it, and anyone still holding it has it published.
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

    std::size_t seg_capacity_;
    Registry registry_;
};

} // namespace mem::source
