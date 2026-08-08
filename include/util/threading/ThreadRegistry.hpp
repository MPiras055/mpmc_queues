#pragma once
#include <meta/OptionsPack.hpp>
#include <util/specs.hpp>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>

namespace util::threading {

/// Option tags accepted by ThreadRegistry.
struct ThreadRegistryOpt {
    /**
     * @brief Make a scan retry when a thread attaches during the walk.
     *
     * Off by default. See "Threads that attach mid-scan" in the ThreadRegistry docs for what
     * this buys and what the default relies on instead.
     */
    struct retry_scan_on_attach {};
};

/**
 * @brief An unbounded, lock-free registry of participating threads, each carrying a payload.
 *
 * Answers one question -- "which threads are participating, and what is each publishing?" --
 * for both reclamation sources. Each thread's state lives *in* the node that represents it, so
 * there is no side array and therefore no index to hand out.
 *
 * ## Structure
 *
 * Two intrusive singly linked lists over the same nodes:
 *
 *  - the **active list**, which reclamation walks. Append-only: a node is linked exactly once,
 *    when it is created, and **is never unlinked**. A node that detaches is marked inactive
 *    and stays where it is.
 *  - the **free list**, holding the nodes currently marked inactive, so a later attach can
 *    take one in O(1) instead of hunting for it.
 *
 * The registry owns every node it links, whether it allocated it or adopted it from a caller,
 * and deletes them in its destructor by walking the active list. There is no separate
 * owned-list to keep in step, because the active list already contains every node that has
 * ever existed.
 *
 * ## Nothing is scanned on the hot path
 *
 * `self()` -- called from `pin()` on every enqueue and dequeue -- is a thread-local read and
 * nothing else. Attach pops the free list; detach pushes onto it. The only members that
 * traverse anything are `reduce`, `any_of`, `all_of` and the two `for_each` forms, which is
 * exactly where a traversal is the point.
 *
 * ## Why nothing is ever unlinked
 *
 * A walk holds its predecessor as a *snapshot*. Once a node can be unlinked and immediately
 * reused, that snapshot can lead the walk into a detached node's stale successor chain, where
 * the next link looks perfectly consistent and an unlink CAS through it therefore **succeeds**
 * -- removing and freeing a node that is still live in the real list. An earlier version of
 * this file did splice, Harris-style, and it produced a double push to the free list within a
 * few thousand attach/detach cycles on eight threads.
 *
 * Splicing safely needs reclamation for the *nodes*, which is the thing this registry exists
 * to support rather than to depend on. Not unlinking removes the problem outright, and it is
 * what makes a walk a pure read: no CAS, no helping, no restarts, no possibility of being led
 * astray. The cost is that the chase is bounded by the peak number of threads that have ever
 * attached rather than by the number attached now; the functor still only runs on the latter.
 *
 * ## Threads that attach mid-scan
 *
 * A thread attaching at the head after a walk has passed it is not visited. For a hazard scan
 * that would mean reclaiming an object the new thread had just protected.
 * `ThreadRegistryOpt::retry_scan_on_attach` closes it: attaches bump a counter, and a scan
 * re-reads it afterwards and repeats if it moved. Attaches happen at thread-lifetime scale, so
 * in practice it never retries.
 *
 * It is **off by default**, and the default instead relies on the argument that a thread
 * attaching after a scan began cannot already hold a pointer to an object that was unlinked
 * before `retire()` was called on it. That argument is delicate and its failure mode is
 * silent, so turning the flag on is worth doing for any stress or TSan run:
 *
 * @code
 * using Registry = util::threading::ThreadRegistry<
 *     ThreadData, meta::OptionsPack<ThreadRegistryOpt::retry_scan_on_attach>>;
 * @endcode
 *
 * ## Payload lifetime
 *
 * `ThreadData` is default-constructed once, with the node, and is **not** reset when a node is
 * reused. A thread that inherits a node inherits its payload -- deliberately, because
 * mem::source::Hazard keeps its retire list there and a detaching thread's undestroyed
 * retirements must not be dropped. A caller needing a clean slate (Pool does, for its epoch
 * word) resets it after attaching.
 *
 * @pre Every thread that attaches must `detach()` before the registry is destroyed. The
 *      destructor deletes the nodes, and a thread that never detached is left holding a
 *      dangling entry in its thread-local chain.
 *
 * @tparam ThreadData per-thread payload. **Align it**: the registry deliberately does not
 *                    over-align `Node`, so that the link words and the payload land on
 *                    different cache lines.
 * @tparam Opt        meta::OptionsPack of ThreadRegistryOpt tags.
 */
template <typename ThreadData, typename Opt = meta::EmptyOptions>
    requires meta::AcceptsOnly<Opt, typename ThreadRegistryOpt::retry_scan_on_attach>
class ThreadRegistry {
    static constexpr bool kRetryOnAttach =
        Opt::template has<typename ThreadRegistryOpt::retry_scan_on_attach>;

public:
    struct Node;

    /**
     * @brief A node pointer paired with a generation, compared as one unit.
     *
     * This is what makes the free queue's compare-and-swaps safe. Nodes here are immortal,
     * which rules out use-after-free but **not** ABA: a stalled `CAS(head, A, B)` succeeds when
     * the head is A *again* rather than still A, publishing a node another thread already owns.
     * Bumping the generation on every successful link makes a stale expected value impossible
     * to match. It is Michael & Scott's counted pointer, for the reason they introduced it.
     *
     * @note `alignas(16)` is required: the double-width compare-exchange this lowers to needs
     *       the operand aligned. Where the instruction is unavailable the standard library
     *       falls back to an internal lock, which is correct and is only ever reached on
     *       attach and detach -- see @ref free_list_is_lock_free.
     */
    struct alignas(16) TaggedPtr {
        Node* ptr = nullptr;
        uint64_t gen = 0;

        bool operator==(const TaggedPtr&) const noexcept = default;

        /// The same slot one generation on, so no earlier reader's value can compare equal.
        TaggedPtr with(Node* p) const noexcept { return TaggedPtr{p, gen + 1}; }
    };

    static_assert(std::is_trivially_copyable_v<TaggedPtr>);

    /**
     * @brief Does the free queue compile to a real double-width CAS on this target?
     *
     * Reported rather than asserted: a target without the instruction still behaves correctly,
     * it just takes a lock inside `std::atomic`. Asserting would break those builds for no
     * gain, but a silent fallback on a target that *should* have it -- `-mcx16` not reaching
     * the translation unit, say -- is worth being able to see, so the test suite prints it.
     */
    static constexpr bool free_list_is_lock_free = std::atomic<TaggedPtr>::is_always_lock_free;

    /**
     * @brief One thread's registration.
     *
     * Deliberately **not** `alignas(CACHE_LINE)`. `ThreadData` carries the alignment instead,
     * which puts the link words and the payload on separate lines: after linking, `next` is
     * immutable apart from its mark bit, so a scan reads link lines that are essentially never
     * written, while payload lines are written only by their owner. Aligning the node as a
     * whole put both in one line, so every hazard-pointer store dirtied the line every scan
     * was reading. `sizeof(Node)` is larger this way; it is one allocation per participating
     * thread, so the sharing is what matters and the size is not.
     *
     * The two link fields are both needed and mean different things. A node lives in the
     * active list **for its whole life**, marked inactive when nobody holds it, and is
     * *simultaneously* queued on the free list when it is available. Collapsing them into one
     * pointer would mean a detached node leaving the active list, and a scan sitting on that
     * node would follow it into the free queue and terminate there -- silently skipping every
     * still-active thread behind it.
     */
    struct Node {
        /// Active-list successor, written exactly once when the node is linked, plus a mark in
        /// bit 0 meaning "nobody is using this node". The mark is the only mutable part.
        std::atomic<Node*> next{nullptr};
        /// Free-queue successor. Meaningful only while the node is queued.
        std::atomic<TaggedPtr> free_next{};
        /// Which registry this node belongs to; identifies it in a thread's chain.
        const void* owner{nullptr};
        /// This thread's chain across registry instances. Touched only by the owning thread.
        Node* tls_next{nullptr};

        ThreadData data{};
    };

    /**
     * @param expected_threads how many nodes to pre-create. A **hint**, not a cap: attaching
     *                         past it simply allocates more.
     */
    explicit ThreadRegistry(std::size_t expected_threads = 0) {
        // The free queue is never empty: it always holds a dummy, whose successor is the
        // first real element. `node_count()` therefore reports one more than the hint.
        Node* dummy = link_new_node();
        free_head_.store(TaggedPtr{dummy, 0}, std::memory_order_relaxed);
        free_tail_.store(TaggedPtr{dummy, 0}, std::memory_order_relaxed);
        for (std::size_t i = 0; i < expected_threads; ++i) enqueue_free(*link_new_node());
    }

    ThreadRegistry(const ThreadRegistry&) = delete;
    ThreadRegistry& operator=(const ThreadRegistry&) = delete;

    /// Deletes every node. All threads must have detached; see the class precondition.
    ~ThreadRegistry() {
        detach(); // whoever is destroying it may still be attached
        Node* n = unmarked(active_head_.load(std::memory_order_acquire));
        while (n) {
            Node* next = unmarked(n->next.load(std::memory_order_relaxed));
            delete n;
            n = next;
        }
    }

    // ===== attach / detach =====

    /**
     * @brief A scope in which the calling thread is attached. Detaches on destruction.
     *
     * The same reasoning as the `pin()` guard in the sources: a manual
     * register/unregister pair is only correct if every future `return` remembers, and one
     * had already grown an early exit that skipped it.
     *
     * @note The two members answer different questions, and keeping them apart is what makes
     *       a **nested** join safe. `attached_` is "this thread is attached", which an inner
     *       join must report truthfully; `owner_` is "this session owes the detach", which
     *       only the join that actually attached may hold. An inner session therefore
     *       converts to `true` and does nothing on destruction, leaving the outer scope in
     *       charge.
     */
    class session {
        ThreadRegistry* owner_ = nullptr;
        bool attached_ = false;

    public:
        session() noexcept = default;
        session(ThreadRegistry* owner, bool attached) noexcept
            : owner_{owner}, attached_{attached} {}

        session(const session&) = delete;
        session& operator=(const session&) = delete;

        session(session&& o) noexcept
            : owner_{std::exchange(o.owner_, nullptr)}, attached_{std::exchange(o.attached_, false)} {}

        session& operator=(session&& o) noexcept {
            if (this != &o) {
                reset();
                owner_ = std::exchange(o.owner_, nullptr);
                attached_ = std::exchange(o.attached_, false);
            }
            return *this;
        }

        ~session() { reset(); }

        /// Detach now, if this session owes it. Idempotent.
        void reset() noexcept {
            if (owner_) {
                owner_->detach();
                owner_ = nullptr;
            }
            attached_ = false;
        }

        /// @return whether the calling thread is attached -- not whether this session owns it.
        explicit operator bool() const noexcept { return attached_; }
    };

    /**
     * @brief Attach for the duration of the returned scope.
     * @throws whatever `operator new` throws, if no node is free.
     */
    [[nodiscard]] session join() {
        if (self() != nullptr) return session{nullptr, true}; // nested: do not own the detach
        attach();
        return session{this, true};
    }

    /// Non-allocating join. The session reports false if no node was free.
    [[nodiscard]] session try_join() noexcept {
        if (self() != nullptr) return session{nullptr, true};
        if (!try_attach()) return session{};
        return session{this, true};
    }

    /**
     * @brief Attach using a node that is already free. Never allocates.
     * @return false if no node is free, in which case nothing happened.
     *
     * The lock-free half of attaching: pair it with `attach(Node*)` on a path that must not
     * call the allocator.
     */
    [[nodiscard]] bool try_attach() noexcept {
        if (self() != nullptr) return true; // already attached; idempotent
        Node* n = dequeue_free();
        if (n == nullptr) return false;
        activate(*n);
        return true;
    }

    /**
     * @brief Attach the calling thread, allocating a node if none is free.
     * @throws whatever `operator new` throws. Not lock-free -- see `attach(Node*)`.
     */
    void attach() {
        if (try_attach()) return;
        // The queue had nothing: this is the only path that allocates.
        activate(*link_new_node());
    }

    /**
     * @brief Attach using a caller-supplied node.
     *
     * Always adopts @p n rather than checking the free list first, so this is branch-free,
     * allocation-free and genuinely lock-free, and the caller never has a node left over to
     * manage. The registry owns @p n from here and will delete it.
     *
     * @param n a fresh, heap-allocated node. Must not already belong to a registry.
     * @return true if @p n was adopted. **False means the calling thread was already
     *         attached and @p n is still the caller's to free** -- it is not silently leaked.
     */
    bool attach(Node* n) noexcept {
        if (self() != nullptr) return false;
        link(*n);
        activate(*n);
        return true;
    }

    /**
     * @brief Detach the calling thread.
     *
     * Marks the node inactive and pushes it onto the free list. The node keeps its place in
     * the active list, so scans in flight are entirely undisturbed -- they simply stop
     * invoking the functor on it. Safe when not attached, and safe to repeat.
     */
    void detach() noexcept {
        Node* n = self();
        if (n == nullptr) return;
        tls_unlink(*n);
        // Release, so the payload this thread wrote is visible to whoever takes the node next.
        n->next.store(marked(unmarked(n->next.load(std::memory_order_relaxed))),
                      std::memory_order_release);
        enqueue_free(*n);
    }

    /// @return this thread's node, or nullptr if it is not attached. A TLS read; never walks.
    Node* self() noexcept {
        for (Node* n = tls_head_; n != nullptr; n = n->tls_next)
            if (n->owner == this) return n;
        return nullptr;
    }

    /// @return this thread's payload, attaching first if necessary.
    ThreadData& payload() {
        if (Node* n = self()) return n->data;
        attach();
        return self()->data;
    }

    // ===== traversal: the only members that walk anything =====

    /**
     * @brief Fold @p op over the payload of every attached thread.
     *
     * @param op invoked as `op(Acc, const ThreadData&) -> Acc`.
     *
     * @note @p op may keep external state (`Hazard::collect` accumulates an index this way).
     *       That is sound because the walk visits each node exactly once -- the active list is
     *       append-only and the walk is a pure read, with no restarts and no helping.
     */
    template <typename Acc, typename Op>
    Acc reduce(Acc init, Op op) const noexcept {
        for (;;) {
            const uint64_t before = attach_generation();
            Acc acc = init;
            for (const Node* n = first(); n != nullptr; n = advance(n))
                if (is_active(n)) acc = op(std::move(acc), std::as_const(n->data));
            if (!scan_was_disturbed(before)) return acc;
        }
    }

    /// True if any attached thread satisfies @p p. Stops at the first one.
    template <typename Pred>
    bool any_of(Pred p) const noexcept {
        return short_circuit(/*stop_on=*/true, p);
    }

    /// True if every attached thread satisfies @p p. Stops at the first that does not.
    template <typename Pred>
    bool all_of(Pred p) const noexcept {
        return !short_circuit(/*stop_on=*/false, p);
    }

    /// Mutable visit of every attached thread's payload -- for resetting published state.
    template <typename Fn>
    void for_each_active(Fn fn) {
        for (Node* n = first(); n != nullptr; n = advance(n))
            if (is_active(n)) fn(n->data);
    }

    /// Every node, attached or not. Detached nodes keep their payload, so teardown needs this.
    template <typename Fn>
    void for_each_node(Fn fn) {
        for (Node* n = first(); n != nullptr; n = advance(n)) fn(n->data);
    }

    /// Attached threads right now. For tests and diagnostics.
    std::size_t active_count() const noexcept {
        return reduce(std::size_t{0}, [](std::size_t n, const ThreadData&) { return n + 1; });
    }

    /// Nodes ever created. For tests: a registry that leaks nodes grows without bound.
    std::size_t node_count() const noexcept {
        std::size_t n = 0;
        for (const Node* p = first(); p != nullptr; p = advance(p)) ++n;
        return n;
    }

private:
    // ===== mark encoding =====
    //
    // Bit 0 of `next` means "this node is not in use". Nodes come from `new`, so the low bits
    // are always free.

    static Node* marked(Node* p) noexcept {
        return reinterpret_cast<Node*>(reinterpret_cast<uintptr_t>(p) | uintptr_t{1});
    }
    static Node* unmarked(Node* p) noexcept {
        return reinterpret_cast<Node*>(reinterpret_cast<uintptr_t>(p) & ~uintptr_t{1});
    }
    static bool is_marked(Node* p) noexcept {
        return (reinterpret_cast<uintptr_t>(p) & uintptr_t{1}) != 0;
    }
    static bool is_active(const Node* n) noexcept {
        return !is_marked(n->next.load(std::memory_order_acquire));
    }

    const Node* first() const noexcept {
        return unmarked(active_head_.load(std::memory_order_acquire));
    }
    Node* first() noexcept { return unmarked(active_head_.load(std::memory_order_acquire)); }

    static const Node* advance(const Node* n) noexcept {
        return unmarked(n->next.load(std::memory_order_acquire));
    }
    static Node* advance(Node* n) noexcept {
        return unmarked(n->next.load(std::memory_order_acquire));
    }

    // ===== list construction =====

    /// Allocate a node and append it to the active list, marked inactive.
    Node* link_new_node() {
        Node* n = new Node();
        link(*n);
        return n;
    }

    /// Publish @p n at the head of the active list, inactive. Its successor never changes again.
    void link(Node& n) noexcept {
        n.owner = this;
        Node* head = active_head_.load(std::memory_order_acquire);
        for (;;) {
            n.next.store(marked(unmarked(head)), std::memory_order_relaxed);
            if (active_head_.compare_exchange_weak(head, &n, std::memory_order_release,
                                                   std::memory_order_acquire))
                return;
        }
    }

    /// Claim an inactive node for this thread: clear the mark and record it in the TLS chain.
    /// A plain store, not a CAS: the queue already handed this node to exactly one thread.
    void activate(Node& n) noexcept {
        n.next.store(unmarked(n.next.load(std::memory_order_relaxed)),
                     std::memory_order_release);
        tls_link(n);
        if constexpr (kRetryOnAttach)
            attaches_.v.fetch_add(1, std::memory_order_release);
    }

    // ===== free list =====

    // ===== free queue: Michael-Scott, over counted pointers =====
    //
    // A FIFO rather than a stack, with head, tail and a permanent dummy. Both ends are
    // ordinary CAS loops -- a failed CAS means some other thread made progress, which is
    // exactly lock-freedom -- and the generation in TaggedPtr is what stops a stalled thread's
    // compare from matching a head that has come back round to the same node.
    //
    // The node a dequeue hands out is the **old dummy**, and its successor becomes the new
    // dummy. That is what carries payloads round the queue intact: every node holds the data
    // of whichever thread last owned it, so a detaching thread's pending retirements travel
    // with the node and are inherited rather than dropped.

    /// Append @p n to the tail. Called by detach.
    void enqueue_free(Node& n) noexcept {
        n.free_next.store(TaggedPtr{}, std::memory_order_relaxed);

        TaggedPtr tail;
        for (;;) {
            tail = free_tail_.load(std::memory_order_acquire);
            const TaggedPtr next = tail.ptr->free_next.load(std::memory_order_acquire);
            if (tail != free_tail_.load(std::memory_order_acquire)) continue; // tail moved

            if (next.ptr == nullptr) {
                TaggedPtr expect = next;
                if (tail.ptr->free_next.compare_exchange_weak(expect, next.with(&n),
                                                              std::memory_order_release,
                                                              std::memory_order_relaxed))
                    break;
            } else {
                // The tail is lagging behind a completed link; help it along rather than wait.
                TaggedPtr expect = tail;
                (void)free_tail_.compare_exchange_weak(expect, tail.with(next.ptr),
                                                       std::memory_order_release,
                                                       std::memory_order_relaxed);
            }
        }
        // Swing the tail to the node we linked. Failing is fine: somebody helped.
        TaggedPtr expect = tail;
        (void)free_tail_.compare_exchange_strong(expect, tail.with(&n),
                                                 std::memory_order_release,
                                                 std::memory_order_relaxed);
    }

    /// Take a node from the head, or nullptr if only the dummy is left. Called by attach.
    Node* dequeue_free() noexcept {
        for (;;) {
            const TaggedPtr head = free_head_.load(std::memory_order_acquire);
            const TaggedPtr tail = free_tail_.load(std::memory_order_acquire);
            const TaggedPtr next = head.ptr->free_next.load(std::memory_order_acquire);
            if (head != free_head_.load(std::memory_order_acquire)) continue; // head moved

            if (head.ptr == tail.ptr) {
                if (next.ptr == nullptr) return nullptr; // genuinely empty
                // Tail lagging: help, then retry.
                TaggedPtr expect = tail;
                (void)free_tail_.compare_exchange_weak(expect, tail.with(next.ptr),
                                                       std::memory_order_release,
                                                       std::memory_order_relaxed);
                continue;
            }

            TaggedPtr expect = head;
            if (free_head_.compare_exchange_weak(expect, head.with(next.ptr),
                                                 std::memory_order_acq_rel,
                                                 std::memory_order_relaxed))
                return head.ptr; // the old dummy is ours; next.ptr is the new dummy
        }
    }

    // ===== per-thread association =====
    //
    // One thread_local head per ThreadRegistry<ThreadData, Opt> instantiation, and the nodes
    // chain themselves. A thread using several instances of the same registry type has several
    // entries, told apart by `owner` -- so "a thread can hold one node per instance" is a
    // property of the nodes rather than of a side map keyed by an id. `tls_next` and `owner`
    // are read and written only by the thread that currently owns the node, and ownership is
    // exclusive, so neither needs to be atomic.

    static inline thread_local Node* tls_head_ = nullptr;

    static void tls_link(Node& n) noexcept {
        n.tls_next = tls_head_;
        tls_head_ = &n;
    }

    static void tls_unlink(Node& n) noexcept {
        Node** link = &tls_head_;
        while (*link != nullptr) {
            if (*link == &n) {
                *link = n.tls_next;
                n.tls_next = nullptr;
                return;
            }
            link = &(*link)->tls_next;
        }
    }

    // ===== scan helpers =====

    /// @param stop_on stop and report as soon as @p p returns this.
    template <typename Pred>
    bool short_circuit(bool stop_on, Pred& p) const noexcept {
        for (;;) {
            const uint64_t before = attach_generation();
            bool hit = false;
            for (const Node* n = first(); n != nullptr; n = advance(n)) {
                if (!is_active(n)) continue;
                if (static_cast<bool>(p(std::as_const(n->data))) == stop_on) {
                    hit = true;
                    break;
                }
            }
            // A hit is already decisive: a thread attaching behind us cannot unmake it.
            if (hit || !scan_was_disturbed(before)) return hit;
        }
    }

    uint64_t attach_generation() const noexcept {
        if constexpr (kRetryOnAttach) return attaches_.v.load(std::memory_order_acquire);
        else return 0;
    }

    bool scan_was_disturbed(uint64_t before) const noexcept {
        if constexpr (kRetryOnAttach) return attaches_.v.load(std::memory_order_acquire) != before;
        else {
            (void)before;
            return false;
        }
    }

    // ===== data =====

    /// Present only when retry_scan_on_attach is set, so the default pays neither the counter's
    /// cache line nor its increment.
    struct AttachCounter {
        ALIGNED_CACHE std::atomic<uint64_t> v{0};
        CACHE_PAD_TYPES(std::atomic<uint64_t>);
    };
    struct NoCounter {};

    ALIGNED_CACHE std::atomic<Node*> active_head_{nullptr};
    CACHE_PAD_TYPES(std::atomic<Node*>);
    ALIGNED_CACHE std::atomic<TaggedPtr> free_head_{};
    CACHE_PAD_TYPES(std::atomic<TaggedPtr>);
    ALIGNED_CACHE std::atomic<TaggedPtr> free_tail_{};
    CACHE_PAD_TYPES(std::atomic<TaggedPtr>);

    [[no_unique_address]] std::conditional_t<kRetryOnAttach, AttachCounter, NoCounter> attaches_{};
};

} // namespace util::threading
