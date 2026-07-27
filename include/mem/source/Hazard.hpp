#pragma once
#include <core/SegmentTraits.hpp>
#include <mem/SingleBlock.hpp>
#include <util/specs.hpp>
#include <util/threading/DynamicThreadTicket.hpp>
#include <atomic>
#include <cassert>
#include <cstddef>
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
 * @note This does not reuse util::hazard::HazardVector, for a concrete reason: that
 *       class reclaims with `delete obj`, but a co-allocated segment is placement-new'd
 *       into std::aligned_alloc storage and must be released through
 *       SingleBlock::destroy. Pairing `delete` with `aligned_alloc` is undefined, and it
 *       would break outright under sized or aligned deallocation.
 *
 * @note One hazard slot per thread is enough: the traversal protects the head *or* the
 *       tail at any moment, never both.
 */
template <typename S>
class Hazard {
    using Ticketing = util::threading::DynamicThreadTicket;

    struct ALIGNED_CACHE Slot {
        std::atomic<S*> hp{nullptr};
        CACHE_PAD_TYPES(std::atomic<S*>);
    };

    struct ALIGNED_CACHE Retired {
        std::vector<S*> list;
        CACHE_PAD_TYPES(std::vector<S*>);
    };

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
        Hazard* owner_;
        std::size_t tid_;

    public:
        guard(Hazard* o, std::size_t tid) noexcept : owner_{o}, tid_{tid} {}
        guard(const guard&) = delete;
        guard& operator=(const guard&) = delete;
        ~guard() { owner_->slots_[tid_].hp.store(nullptr, std::memory_order_release); }

        std::size_t tid() const noexcept { return tid_; }
    };

    /**
     * @param max_threads      concurrent participants
     * @param segment_capacity capacity handed to each segment this source creates
     */
    Hazard(std::size_t max_threads, std::size_t segment_capacity)
        : max_threads_{max_threads}, seg_capacity_{segment_capacity}, ticketing_{max_threads},
          slots_(max_threads), retired_(max_threads) {
        assert(max_threads != 0);
        assert(segment_capacity != 0);
    }

    Hazard(const Hazard&) = delete;
    Hazard& operator=(const Hazard&) = delete;

    ~Hazard() {
        for (auto& r : retired_)
            for (S* p : r.list) mem::SingleBlock<S>::destroy(p);
    }

    guard pin() noexcept { return guard{this, ticket()}; }

    /// Publish @p h as protected. The caller must re-validate before dereferencing.
    handle protect(guard& g, handle h) noexcept {
        slots_[g.tid()].hp.store(h, std::memory_order_seq_cst);
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
        auto& mine = retired_[ticket()].list;
        mine.push_back(h);
        if (mine.size() >= HAZARD_RETIRE_THRESHOLD) collect();
    }

    [[nodiscard]] bool register_thread() noexcept {
        std::size_t t;
        return ticketing_.acquire(t);
    }

    void unregister_thread() noexcept { ticketing_.release(); }

    std::size_t ticket() noexcept {
        std::size_t t = 0;
        const bool ok = ticketing_.acquire(t);
        assert(ok && "Hazard: no ticket available; did the thread call acquire()?");
        if (!ok) std::abort();
        return t;
    }

    std::size_t max_threads() const noexcept { return max_threads_; }

private:
    bool is_protected(S* p) const noexcept {
        for (std::size_t i = 0; i < max_threads_; ++i)
            if (slots_[i].hp.load(std::memory_order_acquire) == p) return true;
        return false;
    }

    void collect() noexcept {
        auto& mine = retired_[ticket()].list;
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

    std::size_t max_threads_;
    std::size_t seg_capacity_;
    Ticketing ticketing_;
    std::vector<Slot> slots_;
    std::vector<Retired> retired_;
};

} // namespace mem::source
