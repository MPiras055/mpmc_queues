#pragma once
#include <core/SegmentTraits.hpp>
#include <mem/Handle.hpp>
#include <mem/detail/SlabLookup.hpp>
#include <meta/OptionsPack.hpp>
#include <util/hazard/Recycler/Recycler.hpp>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <optional>
#include <vector>

namespace mem::source {

/// Re-exported so callers can disable the per-thread reuse cache.
using PoolOpt = util::hazard::recycler::RecyclerOpt;

/**
 * @brief Segment source backed by a fixed, epoch-recycled pool.
 *
 * Models core::SegmentSource. Segments are pre-allocated once and reused; `acquire()`
 * returns nullopt when none is free, and **that is the memory bound**. This is the whole
 * reason BoundedMemProxy no longer needs to exist as a separate proxy: its bound was
 * never an admission rule, it was a source that runs out.
 *
 * Handles are VersionedIndex rather than pointers. A pool slot is reused, so a bare
 * index would let a stale `next` value alias a live segment (ABA); the version half
 * makes reuse observable.
 *
 * @tparam N pool size, in segments. The item ceiling is N * segment_capacity.
 */
template <typename S, std::size_t N, typename Opt = meta::EmptyOptions>
class Pool {
    /**
     * A pooled segment is handed back out after it has held items, so it must be
     * resettable. Without this, pairing the pool with a write-once segment such as
     * FAAArray or HQ would compile and then fail at run time on the first reuse -- which
     * is exactly how the old `assert(false && "TODO")` in open() behaved.
     */
    static_assert(core::segment_traits<S>::recyclable,
                  "mem::source::Pool reuses segments, but segment_traits<S>::recyclable is "
                  "false: this segment cannot be reopened after being drained");

    using Rec = util::hazard::recycler::Recycler<S, N, Opt, void, mem::detail::SlabLookup<S>>;

public:
    using handle = mem::VersionedIndex;

    /// Segments come back from the pool dirty; the proxy must reopen() before reuse.
    static constexpr bool recycles = true;

    static constexpr handle nil() noexcept { return handle{}; }

    /**
     * @brief RAII epoch pin.
     *
     * Protection here is epoch-wide rather than per-object, so `protect()` has nothing
     * to publish -- holding the pin is what keeps every slot alive.
     */
    class guard {
        Pool* owner_;
        std::size_t tid_;

    public:
        guard(Pool* o, std::size_t tid) noexcept : owner_{o}, tid_{tid} {}
        guard(const guard&) = delete;
        guard& operator=(const guard&) = delete;
        ~guard() { owner_->rec_.clear_epoch(); }

        std::size_t tid() const noexcept { return tid_; }
    };

    Pool(std::size_t max_threads, std::size_t segment_capacity)
        : rec_{max_threads, segment_capacity}, versions_(N) {
        assert(max_threads != 0);
        assert(segment_capacity != 0);
        for (auto& v : versions_) v.store(0, std::memory_order_relaxed);
    }

    Pool(const Pool&) = delete;
    Pool& operator=(const Pool&) = delete;

    guard pin() noexcept {
        rec_.protect_epoch();
        return guard{this, rec_.ticket()};
    }

    /// No-op: the epoch pin already covers every slot.
    handle protect(guard&, handle h) noexcept { return h; }

    S* deref(handle h) const noexcept { return rec_.decode(h.index()); }

    /// @return nullopt when the pool is exhausted -- i.e. the memory bound is reached.
    std::optional<handle> acquire() {
        std::size_t idx = 0;
        if (!rec_.get_from_cache(idx) && !rec_.reclaim(idx)) return std::nullopt;
        const uint32_t v = versions_[idx].fetch_add(1, std::memory_order_relaxed) + 1;
        return handle{handle::next_version(v), static_cast<uint32_t>(idx)};
    }

    /// Never published, so it can go straight back without waiting for an epoch.
    void discard(handle h) noexcept { rec_.retire(h.index()); }

    /// Was published: the epoch machinery holds it until no pin can still observe it.
    void retire(handle h) noexcept { rec_.retire(h.index()); }

    [[nodiscard]] bool register_thread() noexcept { return rec_.register_thread(); }
    void unregister_thread() noexcept { rec_.unregister_thread(); }

    std::size_t ticket() noexcept { return rec_.ticket(); }
    std::size_t max_threads() const noexcept { return rec_.max_threads(); }

    static constexpr std::size_t pool_size() noexcept { return N; }

private:
    mutable Rec rec_;
    std::vector<std::atomic<uint32_t>> versions_;
};

} // namespace mem::source
