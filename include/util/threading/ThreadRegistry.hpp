#pragma once
#include <meta/OptionsPack.hpp>
#include <util/bit.hpp>
#include <util/specs.hpp>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

namespace util::threading {

/// Option tags accepted by ThreadRegistry.
struct ThreadRegistryOpt {
    /**
     * @brief Make for_each_active retry when a thread attaches during the walk.
     *
     * Off by default. See "Threads that attach mid-scan" in the ThreadRegistry docs for
     * what this buys and what the default relies on instead.
     */
    struct retry_scan_on_attach {};
};

namespace detail {

/**
 * @brief A node index, an activity mark and a reuse version in one CAS-able word.
 *
 * Both list heads and both per-node link fields are one of these, so every structural
 * change is a single 64-bit compare-exchange. Nothing here needs a double-width CAS, so
 * none of it depends on `-mcx16`.
 *
 * ```
 *  63                    32 31   30                     0
 * +------------------------+-----+-----------------------+
 * |        version         | mark|         index         |
 * +------------------------+-----+-----------------------+
 * ```
 *
 * @note The version is what stops a stale compare-exchange from *succeeding* against a
 *       node that has been detached and re-attached in the meantime. Immortal nodes rule
 *       out use-after-free, but they do not rule out ABA, and on the free list ABA is a
 *       lost node rather than a crash: T1 reads `head = A, A.free_next = B` and stalls;
 *       T2 pops A; T3 pops B; T2 pushes A back; T1's `CAS(head, A -> B)` then succeeds and
 *       hands out a node T3 is already using. Same 32/32 split as mem::VersionedIndex.
 *
 * @note Wraparound needs 2^32 updates to one node's link. Links change only on attach and
 *       detach -- thread-lifecycle events -- so it is not reachable in practice.
 */
struct Link {
    /// No successor. Also the largest representable index, so it doubles as the cap.
    static constexpr uint32_t kNil = 0x7FFF'FFFFu;
    /// Set while the node is *not* attached. Cleared by attach, set by detach.
    static constexpr uint32_t kMark = 0x8000'0000u;

    uint64_t raw = bit::merge<uint64_t, uint32_t>(0u, kNil);

    constexpr Link() noexcept = default;
    explicit constexpr Link(uint64_t r) noexcept : raw{r} {}

    uint32_t version() const noexcept { return bit::keep_high<uint32_t>(raw); }
    uint32_t index() const noexcept { return bit::keep_low<uint32_t>(raw) & ~kMark; }
    bool marked() const noexcept { return (bit::keep_low<uint32_t>(raw) & kMark) != 0u; }
    bool nil() const noexcept { return index() == kNil; }

    /// The value to publish next: one version on, so no earlier reader's word can match.
    Link next_gen(uint32_t idx, bool mark = false) const noexcept {
        return Link{bit::merge<uint64_t, uint32_t>(version() + 1u, idx | (mark ? kMark : 0u))};
    }

    /// Same successor, mark flipped to @p mark, one version on.
    Link remark(bool mark) const noexcept { return next_gen(index(), mark); }

    bool operator==(const Link&) const noexcept = default;
};

static_assert(sizeof(Link) == sizeof(uint64_t));
static_assert(std::is_trivially_copyable_v<Link>);

} // namespace detail

/**
 * @brief A dynamic, lock-free registry of participating threads, each carrying a payload.
 *
 * Replaces util::threading::DynamicThreadTicket. That handed out a dense integer from an
 * atomic bitset and left the caller to keep its per-thread state in a separate array
 * indexed by it, which had three consequences:
 *
 *  - the caps were compile-time (`DTT_MAX_BITS` threads, `DTT_MAX_INSTANCES` live
 *    instances), because the per-thread cache was a fixed `thread_local std::array`;
 *  - every reclamation scan was O(max_threads) whether or not those threads existed --
 *    and `Hazard::is_protected` runs that scan once per retired object per pass;
 *  - identity and state were separate, so detaching a thread could not also release
 *    whatever the thread had been publishing.
 *
 * Here the payload lives *in* the node that represents the thread, the scan only reaches
 * nodes that have actually been used, and detaching is O(1).
 *
 * ## Structure
 *
 * One immortal node array and two lock-free singly linked lists threaded through it:
 *
 *  - the **active list** (`active_head_`), walked by reclamation;
 *  - the **free list** (`free_head_`), a Treiber stack of nodes available for reuse.
 *
 * `max_threads` nodes are allocated once in the constructor and never allocated or freed
 * again. Attach therefore never calls the allocator -- it pops an index -- and `attach()`
 * failing when the free list is empty is exactly the old `register_thread()` returning
 * false. What was a *compile-time* cap is now purely a runtime argument.
 *
 * ## Detaching, and why nothing is ever unlinked
 *
 * Detach is one CAS: the owning thread sets the mark bit in its own link, and pushes the
 * node onto the free list. The node **stays where it is in the active list**; attach pops
 * it and clears the mark in place. A node is appended to the active list at most once
 * ever, on its first use, and is never removed.
 *
 * The consequence is that the active list is **append-only and acyclic**, and its nodes
 * never move. A walk is therefore a pure read -- no CAS, no helping, no restarts, and no
 * possibility of wandering. `for_each_active` invokes the functor only on unmarked nodes,
 * so the functor still runs O(attached threads) times, while the pointer chase is bounded
 * by the *peak* number of threads that have ever attached rather than by `max_threads`.
 * For a queue built for 128 threads and used by 4, that is a 4-node walk.
 *
 * @note An earlier version of this did physically splice detached nodes out, Harris-style,
 *       to make the chase O(currently attached). **It is not safe here and was removed.**
 *       A walk holds `prev` as a *snapshot*; once a node can be unlinked and immediately
 *       recycled, that snapshot can lead the walk into a detached node's stale successor
 *       chain, where the next link looks perfectly consistent and the splice CAS succeeds
 *       -- unlinking and freeing a node that is still live in the real list. It then sits
 *       on both lists at once, gets popped while still linked, and attach closes a cycle.
 *       Measured before the fix: a double push to the free list within a few thousand
 *       attach/detach cycles on 8 threads, and roughly half of all runs ending with nodes
 *       lost. Splicing safely needs reclamation for the *nodes*, which is the thing this
 *       registry exists to support rather than to depend on. Not unlinking removes the
 *       problem outright, and costs only that the chase is bounded by peak rather than
 *       current concurrency.
 *
 * ## Threads that attach mid-scan
 *
 * A thread attaching at the head after the walk has passed it is not visited. For a hazard
 * scan that would mean reclaiming an object the new thread had just protected.
 * `ThreadRegistryOpt::retry_scan_on_attach` closes this outright: attaches bump a counter,
 * and `for_each_active` re-reads it after the walk and repeats if it moved. Attaches happen
 * at thread-lifetime scale, so in practice it never retries.
 *
 * It is **off by default**, and the default instead relies on the argument that a thread
 * attaching after a scan began cannot already hold a pointer to an object that was
 * unlinked before `retire()` was called on it. That argument is delicate and its failure
 * mode is silent, so turning the flag on is worth doing for any stress or TSan run: it is a
 * one-token change at the source alias, e.g.
 *
 * @code
 * using Registry = util::threading::ThreadRegistry<
 *     ThreadData, meta::OptionsPack<ThreadRegistryOpt::retry_scan_on_attach>>;
 * @endcode
 *
 * ## Payload lifetime
 *
 * `ThreadData` is default-constructed once, with the node, and is **not** reset when a node
 * is reused. A thread that inherits a node inherits its payload. That is deliberate:
 * mem::source::Hazard keeps its retire list there, and a detaching thread's undestroyed
 * retirements must not be dropped on the floor. A caller that needs a clean slate (Pool
 * does, for its epoch word) resets it after `attach()`.
 *
 * @tparam ThreadData per-thread payload; stored inline in the node, cache-line isolated.
 * @tparam Opt        meta::OptionsPack of ThreadRegistryOpt tags.
 */
template <typename ThreadData, typename Opt = meta::EmptyOptions>
    requires meta::AcceptsOnly<Opt, typename ThreadRegistryOpt::retry_scan_on_attach>
class ThreadRegistry {
    using Link = detail::Link;

    static constexpr bool kRetryOnAttach =
        Opt::template has<typename ThreadRegistryOpt::retry_scan_on_attach>;

public:
    /**
     * @brief One thread's registration.
     *
     * Cache-line isolated: `next` is read by every reclamation scan while `data` is written
     * by its owner, and both live here, so without the alignment one thread publishing a
     * hazard pointer would invalidate the line another thread is walking.
     */
    struct ALIGNED_CACHE Node {
        /// Active-list successor, plus the mark meaning "nobody is using this node".
        /// The successor half is written exactly once, when the node is first linked.
        std::atomic<Link> next{};
        /// Free-list successor. Meaningful only while the node is on the free list.
        std::atomic<Link> free_next{};
        /// Stable index of this node, and the value `ticket()` hands back. Never changes.
        uint32_t slot = 0;
        /// Has this node been appended to the active list? Only ever touched by the thread
        /// that popped it, which holds it exclusively, and set once for the node's life.
        std::atomic<bool> linked{false};
        ThreadData data{};
    };

    /// Largest addressable thread count. Runtime, unlike DTT_MAX_BITS.
    static constexpr std::size_t max_capacity = Link::kNil - 1;

    /**
     * @param max_threads number of concurrently attached threads this registry can hold
     */
    explicit ThreadRegistry(std::size_t max_threads)
        : id_{next_id_.fetch_add(1, std::memory_order_relaxed)},
          capacity_{max_threads},
          nodes_{std::make_unique<Node[]>(max_threads)} {
        assert(max_threads != 0 && "ThreadRegistry: max_threads must be non-null");
        assert(max_threads <= max_capacity && "ThreadRegistry: max_threads not representable");

        // Every node starts free and unlinked, chained in index order so the first
        // attachers get ascending slots -- which keeps the old "smallest ticket" feel.
        for (std::size_t i = 0; i < max_threads; ++i) {
            nodes_[i].slot = static_cast<uint32_t>(i);
            const uint32_t succ =
                (i + 1 == max_threads) ? Link::kNil : static_cast<uint32_t>(i + 1);
            nodes_[i].free_next.store(Link{}.next_gen(succ), std::memory_order_relaxed);
            // Marked: "not attached" is what for_each_active filters on.
            nodes_[i].next.store(Link{}.next_gen(Link::kNil, true), std::memory_order_relaxed);
        }
        free_head_.store(Link{}.next_gen(0), std::memory_order_relaxed);
    }

    ThreadRegistry(const ThreadRegistry&) = delete;
    ThreadRegistry& operator=(const ThreadRegistry&) = delete;

    /**
     * @brief Destructor.
     *
     * Only the destroying thread's TLS association can be purged from here. A thread that
     * attached and never detached leaves a dead entry behind, which is harmless: registry
     * ids are monotonic and never reused, so no future registry can match it.
     */
    ~ThreadRegistry() { tls_erase(id_); }

    /**
     * @brief Attach the calling thread, if it is not attached already.
     * @return false only when every node is already handed out.
     */
    [[nodiscard]] bool attach() noexcept { return self_or_attach() != nullptr; }

    /**
     * @brief Detach the calling thread, releasing its node for reuse.
     *
     * Two writes and no traversal: mark the node inactive, then push it onto the free
     * list. The node keeps its place in the active list, so scans in flight are entirely
     * undisturbed -- they simply stop invoking the functor on it.
     *
     * Safe to call when not attached, and safe to call repeatedly.
     */
    void detach() noexcept {
        Node* n = tls_lookup(id_);
        if (n == nullptr) return;
        tls_erase(id_);

        // Only the owner marks, so this contends with nothing and settles immediately.
        Link cur = n->next.load(std::memory_order_acquire);
        while (!cur.marked()) {
            if (n->next.compare_exchange_weak(cur, cur.remark(true), std::memory_order_acq_rel,
                                              std::memory_order_acquire))
                break;
        }
        push_free(*n);
    }

    /// @return this thread's node, or nullptr if it is not attached. No side effects.
    Node* self() noexcept { return tls_lookup(id_); }

    /**
     * @brief This thread's node, attaching it on first use.
     * @return nullptr if the registry is full.
     */
    Node* self_or_attach() noexcept {
        if (Node* n = tls_lookup(id_)) return n;

        Node* n = pop_free();
        if (n == nullptr) return nullptr;

        // We hold this node exclusively -- it came off the free list -- so the only
        // concurrency here is with scans reading `next`.
        if (!n->linked.load(std::memory_order_relaxed)) {
            // First use: append at the head, unmarked, so it becomes visible already
            // active. This is the only time a node's successor is ever written.
            Link head = active_head_.load(std::memory_order_acquire);
            for (;;) {
                n->next.store(n->next.load(std::memory_order_relaxed).next_gen(head.index()),
                              std::memory_order_release);
                if (active_head_.compare_exchange_weak(head, head.next_gen(n->slot),
                                                       std::memory_order_acq_rel,
                                                       std::memory_order_acquire))
                    break;
            }
            n->linked.store(true, std::memory_order_relaxed);
        } else {
            // Already in the list: just clear the mark, in place.
            Link cur = n->next.load(std::memory_order_acquire);
            while (cur.marked()) {
                if (n->next.compare_exchange_weak(cur, cur.remark(false),
                                                  std::memory_order_acq_rel,
                                                  std::memory_order_acquire))
                    break;
            }
        }

        if constexpr (kRetryOnAttach)
            attaches_.v.fetch_add(1, std::memory_order_release);

        tls_insert(id_, n);
        return n;
    }

    /**
     * @brief Visit the payload of every attached thread.
     *
     * @param fn invoked as `fn(ThreadData&)`. Returning `bool` stops the walk on `false`;
     *           returning `void` always continues.
     */
    template <typename Fn>
    void for_each_active(Fn&& fn) noexcept {
        if constexpr (kRetryOnAttach) {
            for (;;) {
                const uint64_t before = attaches_.v.load(std::memory_order_acquire);
                if (!walk(fn)) return; // stopped early: fn already has its answer
                if (attaches_.v.load(std::memory_order_acquire) == before) return;
                // Somebody attached behind us and may have been missed. Go again.
            }
        } else {
            (void)walk(fn);
        }
    }

    /// Maximum concurrently attached threads.
    std::size_t max_threads() const noexcept { return capacity_; }

    /// Attached threads right now. For tests and diagnostics.
    std::size_t active_count() noexcept {
        std::size_t n = 0;
        for_each_active([&](ThreadData&) noexcept { ++n; });
        return n;
    }

    /// Every node, attached or not, in index order. For teardown -- only meaningful when
    /// no other thread is using the registry.
    template <typename Fn>
    void for_each_node(Fn&& fn) {
        for (std::size_t i = 0; i < capacity_; ++i) fn(nodes_[i].data);
    }

private:
    // ===== traversal =====

    /**
     * @brief Read-only walk of the active list.
     *
     * Nodes are never unlinked and a node's successor is written exactly once, so this
     * chases a chain that is append-only and acyclic. There is nothing to help with, no
     * CAS, and no restart: the walk simply cannot be led astray, which is the whole reason
     * detach marks instead of splicing.
     *
     * @return false if @p fn asked to stop, true if the walk reached the end.
     */
    template <typename Fn>
    bool walk(Fn& fn) noexcept {
        Link cur = active_head_.load(std::memory_order_acquire);
        while (!cur.nil()) {
            Node& n = nodes_[cur.index()];
            const Link nx = n.next.load(std::memory_order_acquire);
            if (!nx.marked() && !invoke(fn, n.data)) return false;
            cur = nx;
        }
        return true;
    }

    /// Accept both `bool(ThreadData&)` and `void(ThreadData&)` functors.
    template <typename Fn>
    static bool invoke(Fn& fn, ThreadData& d) noexcept {
        if constexpr (std::is_invocable_r_v<bool, Fn&, ThreadData&>) return fn(d);
        else {
            fn(d);
            return true;
        }
    }

    // ===== free list (Treiber stack over versioned indices) =====

    Node* pop_free() noexcept {
        Link head = free_head_.load(std::memory_order_acquire);
        for (;;) {
            if (head.nil()) return nullptr; // every node is handed out
            Node& n = nodes_[head.index()];
            const Link succ = n.free_next.load(std::memory_order_acquire);
            if (free_head_.compare_exchange_weak(head, head.next_gen(succ.index()),
                                                 std::memory_order_acq_rel,
                                                 std::memory_order_acquire))
                return &n;
        }
    }

    void push_free(Node& n) noexcept {
        Link head = free_head_.load(std::memory_order_acquire);
        for (;;) {
            n.free_next.store(n.free_next.load(std::memory_order_relaxed).next_gen(head.index()),
                              std::memory_order_release);
            if (free_head_.compare_exchange_weak(head, head.next_gen(n.slot),
                                                 std::memory_order_acq_rel,
                                                 std::memory_order_acquire))
                return;
        }
    }

    // ===== per-thread association =====

    /**
     * @brief This thread's registry associations.
     *
     * Keyed by a monotonic registry id rather than by the registry's address: an address
     * can be reused by a later registry allocated in the same storage, which would make a
     * stale entry match and hand a thread a node belonging to a destroyed instance.
     *
     * One inline entry covers the overwhelmingly common case of a thread using a single
     * registry -- the fast path is one 64-bit compare, cheaper than the array index it
     * replaces -- and `others` absorbs the rest with no cap, which is what removes
     * DTT_MAX_INSTANCES.
     */
    struct Tls {
        uint64_t cached_id = 0; ///< 0 means empty; ids start at 1
        Node* cached_node = nullptr;
        std::vector<std::pair<uint64_t, Node*>> others;
    };

    static Tls& tls() noexcept {
        static thread_local Tls t;
        return t;
    }

    static Node* tls_lookup(uint64_t id) noexcept {
        Tls& t = tls();
        if (t.cached_id == id) return t.cached_node;
        for (const auto& e : t.others)
            if (e.first == id) return e.second;
        return nullptr;
    }

    /// @note Allocates when a thread uses more than one registry. Declared noexcept to
    ///       satisfy core::SegmentSource; an allocation failure here terminates, as it
    ///       already does in Hazard::retire.
    static void tls_insert(uint64_t id, Node* n) noexcept {
        Tls& t = tls();
        if (t.cached_id == 0) {
            t.cached_id = id;
            t.cached_node = n;
            return;
        }
        t.others.emplace_back(id, n);
    }

    static void tls_erase(uint64_t id) noexcept {
        Tls& t = tls();
        if (t.cached_id == id) {
            if (t.others.empty()) {
                t.cached_id = 0;
                t.cached_node = nullptr;
            } else { // promote one, so the inline slot stays occupied
                t.cached_id = t.others.back().first;
                t.cached_node = t.others.back().second;
                t.others.pop_back();
            }
            return;
        }
        for (std::size_t i = 0; i < t.others.size(); ++i) {
            if (t.others[i].first == id) {
                t.others[i] = t.others.back();
                t.others.pop_back();
                return;
            }
        }
    }

    // ===== data =====

    /// Present only when retry_scan_on_attach is set, so the default configuration pays
    /// neither the counter's cache line nor its increment.
    struct AttachCounter {
        ALIGNED_CACHE std::atomic<uint64_t> v{0};
        CACHE_PAD_TYPES(std::atomic<uint64_t>);
    };
    struct NoCounter {};

    static inline std::atomic<uint64_t> next_id_{1};

    const uint64_t id_;
    const std::size_t capacity_;
    std::unique_ptr<Node[]> nodes_;

    ALIGNED_CACHE std::atomic<Link> active_head_{};
    CACHE_PAD_TYPES(std::atomic<Link>);
    ALIGNED_CACHE std::atomic<Link> free_head_{};
    CACHE_PAD_TYPES(std::atomic<Link>);

    [[no_unique_address]] std::conditional_t<kRetryOnAttach, AttachCounter, NoCounter> attaches_{};
};

} // namespace util::threading
