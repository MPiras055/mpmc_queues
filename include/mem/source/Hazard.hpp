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
#include <vector>

namespace mem::source {

#ifndef HAZARD_RETIRE_THRESHOLD
/// Retired objects per thread before a reclamation scan is attempted.
#define HAZARD_RETIRE_THRESHOLD 64
#endif

/**
 * @brief Segment source backed by hazard pointers: allocate on demand, free when unseen.
 *
 * Models core::SegmentSource. `acquire()` allocates and therefore never fails, so a
 * proxy over this source is unbounded unless an admission policy says otherwise.
 *
 * ## Where the per-thread state lives
 *
 * A thread participating here publishes two things: the segment it is currently reading
 * (the hazard pointer) and the segments it has retired but not yet freed. Both are its
 * own, both must survive exactly as long as the thread is registered, and both are needed
 * by whoever is scanning. So both are the payload of that thread's
 * util::threading::ThreadRegistry node, and there is no separate array indexed by a
 * ticket:
 *
 * @code
 * struct ThreadData { std::atomic<S*> hp; std::vector<S*> retired; };
 * @endcode
 *
 * That is what makes `is_protected()` cost O(attached threads) instead of
 * O(max_threads) -- it walks the registry's active list rather than a fixed slot array
 * sized for a thread count that may never materialise. The scan runs once per retired
 * object per collection pass, so the difference is not academic.
 *
 * @note One hazard slot per thread is enough: the traversal protects the head *or* the
 *       tail at any moment, never both.
 *
 * @note This owns its reclamation rather than delegating it. Reclaiming with `delete obj`
 *       is not an option here: a co-allocated segment is placement-new'd into
 *       std::aligned_alloc storage and must be released through SingleBlock::destroy.
 *       Pairing `delete` with `aligned_alloc` is undefined and breaks outright under sized
 *       or aligned deallocation.
 */
template <typename S>
class Hazard {
    /**
     * @brief One thread's published state.
     *
     * `retired` is deliberately *not* cleared when a registry node is recycled. A thread
     * that detaches with retirements still pending has not leaked them: the next thread to
     * inherit the node inherits the list and will collect it. Clearing here would drop
     * segments on the floor.
     */
    struct ThreadData {
        std::atomic<S*> hp{nullptr};
        std::vector<S*> retired;
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

    /// Every acquire() allocates a fresh segment, so none ever needs reopening.
    static constexpr bool recycles = false;

    static constexpr handle nil() noexcept { return nullptr; }

    /**
     * @brief RAII protection scope.
     *
     * Replaces manual protect/clear pairing. The old proxies cleared by hand on every
     * exit path -- three of them in dequeue alone -- so correctness depended on every
     * future `return` remembering.
     */
    class guard {
        Node* node_;

    public:
        explicit guard(Node* n) noexcept : node_{n} {}
        guard(const guard&) = delete;
        guard& operator=(const guard&) = delete;
        ~guard() { node_->data.hp.store(nullptr, std::memory_order_release); }

        Node* node() const noexcept { return node_; }
        std::size_t tid() const noexcept { return node_->slot; }
    };

    /**
     * @param max_threads      concurrent participants
     * @param segment_capacity capacity handed to each segment this source creates
     */
    Hazard(std::size_t max_threads, std::size_t segment_capacity)
        : seg_capacity_{segment_capacity}, registry_{max_threads} {
        assert(max_threads != 0);
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

    [[nodiscard]] bool register_thread() noexcept { return registry_.attach(); }

    void unregister_thread() noexcept { registry_.detach(); }

    std::size_t ticket() noexcept { return node()->slot; }

    std::size_t max_threads() const noexcept { return registry_.max_threads(); }

private:
    /// This thread's registry node, attaching on first use. Aborts if the registry is full,
    /// which means more threads reached the queue than it was constructed for.
    Node* node() noexcept {
        Node* n = registry_.self_or_attach();
        assert(n && "Hazard: registry full; more threads than max_threads reached the queue");
        if (!n) std::abort();
        return n;
    }

    /**
     * @brief Is any attached thread currently reading @p p?
     *
     * Walks only the threads that have actually attached. The functor returns false to cut
     * the walk short on the first match, and is idempotent, which ThreadRegistry requires
     * because a node can be visited more than once when the list changes under the scan.
     */
    bool is_protected(S* p) noexcept {
        bool found = false;
        registry_.for_each_active([&](ThreadData& d) noexcept {
            if (d.hp.load(std::memory_order_acquire) == p) {
                found = true;
                return false; // stop
            }
            return true;
        });
        return found;
    }

    /**
     * @brief Free everything in @p mine that no attached thread is protecting.
     *
     * Swap-with-back compaction, so surviving entries stay in the list at O(1) each and
     * the scan is proportional to the retire list, not to its square.
     */
    void collect(std::vector<S*>& mine) noexcept {
        for (std::size_t i = 0; i < mine.size();) {
            S* p = mine[i];
            if (!is_protected(p)) {
                std::swap(mine[i], mine.back());
                mine.pop_back();
                mem::SingleBlock<S>::destroy(p);
                // do not advance: the swapped-in entry still needs checking
            } else {
                ++i;
            }
        }
    }

    std::size_t seg_capacity_;
    Registry registry_;
};

} // namespace mem::source
