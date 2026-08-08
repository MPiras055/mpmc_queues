# As Shipped — the design that actually exists

[[Architecture Patterns]] diagnosed the pre-refactor code. [[Abstraction Map]] proposed a
replacement. This note records what was **built**, and every place the result departs from
the proposal — because a design document that quietly disagrees with the code is worse than
no document.

Read this one first. The other two remain useful: the first as a record of what was wrong
and why, the second for the UML and the reasoning behind the abstraction set. Their
*forward-looking* sections are historical.

---

## Where things live

```
meta/     OptionsPack (+ AcceptsOnly), TypeList, FixedString
util/     bit, specs, atomic/cas2, threading/{ThreadRegistry,ThreadPinner}, timing/
core/     Queue, Segment, SegmentTraits, Source, Admission, Proxy, Construction  (concepts only)
mem/      Align, Layout, SingleBlock, Handle; detail/RingSlab; source/{Hazard,Pool}
cell/     PlainCell, SequencedCell, Tagging
linkage/  None, Node<HandlePolicy>
algo/     Vyukov, VyukovNoABA, VyukovDCAS, PRQ, FAAArray, HQ, LFring, SCQ, PSCQ, Mutex
proxy/    LinkedProxy, Admission, Aliases
registry/ Registry
```

Strictly acyclic; `core/` names no implementation.

---

## Deviations from the proposal

| Proposed | Shipped | Why |
| --- | --- | --- |
| `AnyQueue<T>` type erasure at the benchmark edge | **Nothing.** Name→type dispatch is a compile-time fold in `registry::dispatch_impl` | The no-vtable constraint rules out type erasure; a hand-rolled vtable is still a vtable. The fold keeps the measured loop monomorphic. |
| `namespace link` | `namespace linkage` | `link` collides with POSIX `link()` from `<unistd.h>`. |
| `AdmissionPolicy::may_admit()` | `try_admit()` + `cancel_admit()` | Check-then-act is a race: every producer that passed the check committed. Measured overshoot at 4 producers against a bound of 256: peak 257. Item admission now *reserves*. |
| `queue::LFring` | `queue::IndexRing` | Truer name (it carries indices, not payloads), and it avoided a clash while the old header still existed. |
| `Layout` with N regions | same, and SCQ uses **five** | Two ring headers, two cell arrays, one payload buffer. |
| `SegmentSource` as specified | same, plus `recycles` | The proxy must know whether an acquired segment needs `reopen()`. |
| `segment_traits` | same, plus `needs_inflight_drain` and `CompleteSegmentTraits` | See below. |
| Policies constructed by the proxy | policies construct **themselves** via `Config` + `config()` | The proxy had to know that ItemCount wanted `segment_capacity * chunks` and SegmentCount wanted the divisor. |
| `Pool` wrapping the existing `Recycler` | `Pool` owns its reclamation; `util/hazard/` deleted | See below. |

## Added after the proposal, because the code demanded it

- **`core::DrainableSegment` + `needs_inflight_drain`.** SCQ inserts in three steps —
  claim a free index, write the payload, publish the index — so between the first and last
  the item exists but cannot be found. A proxy that drained, saw empty and unlinked during
  that window lost it: 10 runs in 15 lossy at 16-slot segments, 6/15 at 64, 0/15 at 1024.
  SCQ now counts in-flight producers and the proxy will not unlink while any remain.
- **`linkage::Linked` as a constraint.** PRQ, FAAArray and HQ are unsound standalone, so
  they are constrained rather than documented: `algo::PRQ<T, Opt, linkage::None>` is not a
  nameable type, and there is no `queue::PRQ` alias. See the README for the diagnostic.
- **`core::CompleteSegmentTraits`.** Checking one flag as a proxy for "complete" let an
  incomplete specialization compile and fail later at an unrelated use site.
  `MPMC_ASSERT_SEGMENT_TRAITS` sits beside each specialization.
- **`meta::AcceptsOnly`.** `has<>` reads an unknown tag as "not requested", so a misspelled
  or foreign option silently did nothing. Each algorithm declares its accepted tags as a
  *constraint* — deliberately not an in-class `static_assert`, which would only fire once
  something forced instantiation.
- **`core::Construction`.** `registry::Instance` had been detecting construction shape with
  inline `requires` probes, the very pattern the refactor existed to remove. `BlockAllocated`
  / `DirectConstructed` / `Joinable` / `Constructible` name it, and an ambiguous or
  unsupported shape is now a diagnosable error.

---

## `util::threading::ThreadRegistry` — who is participating

Both sources need the same thing: the set of attached threads, and what each publishes. They
used to get it from `DynamicThreadTicket`, which handed out a dense integer from a bitset and
left each caller to keep its state in a *separate* array indexed by it.

| | `DynamicThreadTicket` | now |
| --- | --- | --- |
| thread cap | `DTT_MAX_BITS` = 1024, compile-time | none; no constructor takes one |
| live instances | `DTT_MAX_INSTANCES` = 16, threw past it | unbounded |
| scan | O(configured ceiling) | O(peak attached), functor only on active |
| per-thread state | a side array indexed by a ticket | the registry node |
| identity | a `uint32_t` slot | a reference to `ThreadData` |

Nodes are individually allocated and owned by the registry, which deletes them by walking the
active list — there is no separate owned-list, because the active list already holds every
node that has ever existed. `attach()` pops the free list and allocates only if it is empty;
`attach(Node*)` adopts a caller-allocated node and is therefore lock-free and allocation-free,
which is the way in for a caller that must not touch the allocator.

**Nothing is scanned on the hot path.** `self()` — the call `pin()` makes on every enqueue and
dequeue — walks this thread's chain of nodes comparing `owner`, which is one pointer compare
for a thread using a single instance. Only `reduce`, `any_of`, `all_of` and the two `for_each`
forms traverse the registry.

### Nothing is ever unlinked

Detach marks the node inactive and pushes it to the free list; the node keeps its place in the
active list, and attach clears the mark.

> **The physical splice was tried, and removed.** The first version unlinked detached nodes
> Harris-style to make the chase O(*currently* attached). It is not safe here. A walk holds its
> predecessor as a *snapshot*; once a node can be unlinked and immediately recycled, that
> snapshot can lead the walk into a detached node's stale successor chain, where the next link
> looks perfectly consistent and the splice CAS therefore **succeeds** — unlinking and freeing a
> node still live in the real list. It then sits on both lists at once, is popped while still
> linked, and attach closes a cycle. Measured before the fix: a double push to the free list
> within a few thousand attach/detach cycles on eight threads, and roughly half of all
> `NoNodeIsLostOverManyAttachDetachCycles` runs ending with nodes lost. Splicing safely needs
> reclamation for the *nodes* — the thing this registry exists to support rather than depend on.

### The free list is an MS queue over counted pointers

Head, tail and a permanent dummy, with detach enqueuing at the tail and attach dequeuing at the
head. Both are ordinary CAS loops — a failed CAS means another thread made progress — and the
node a dequeue hands out is the *old dummy*, its successor becoming the new one. That is what
carries payloads round intact: a node holds the data of whichever thread last owned it, so a
detaching thread's pending retirements travel with the node and are inherited rather than
dropped. Reuse is therefore **FIFO**; which node a thread gets is not part of the contract,
only that it did not have to allocate one.

> **Immortal nodes do not make a plain pointer CAS safe.** They rule out use-after-free, not
> ABA. Free list `A → B → C`; T1 reads `head = A` and `A.free_next = B`, then stalls; T2 pops A
> and attaches; T3 pops B and attaches; T2 detaches, pushing A back. T1 resumes and
> `CAS(free_head_, A, B)` **succeeds** — the head really is A — so T1 correctly takes A, but
> the head is now B, which T3 owns, and C is lost. The next popper hands B to a second thread.
> Each link is therefore a `{Node*, uint64_t}` counted pointer whose generation moves on every
> successful link, so a stale expected value can never compare equal. On x86-64 with `-mcx16`
> that is one `cmpxchg16b`; elsewhere `std::atomic` takes an internal lock, which is correct and
> only ever reached on attach and detach. `free_list_is_lock_free` reports which, rather than
> asserting — a target without the instruction should still build.

Two earlier attempts are worth recording, because both were only visible in a growth
measurement rather than a functional run:

| attempt | why it failed |
| --- | --- |
| pop by `exchange`-ing the whole stack | no lock and no tag, but it empties the list while pushing the remainder back, so every attach in that window allocates — and more nodes lengthen the window. A hint of 2 reached 309 nodes in under a second, still climbing, at every thread count. |
| serialise poppers behind a flag | correct, and the count sat exactly on the hint, but `attach()` stopped being lock-free for want of a disambiguator that the counted pointer supplies properly. |

The queue plateaus a little above the hint rather than exactly on it — threads churning past
each other can briefly all be between detach and attach — and then holds there. Monotonic
growth is the thing to watch for, and `NoNodeIsLostOverManyAttachDetachCycles` bounds it.

### `ThreadData` carries the alignment, not `Node`

`Node` is deliberately not over-aligned. `Hazard` and `Pool` declare their own `ThreadData`
`alignas(CACHE_LINE)`, which puts the link words and the payload on separate lines: after
linking, `next` is immutable apart from its mark, so a scan reads link lines that are never
written, while payload lines are written only by their owner. Aligning the node as a whole put
both in one line, so every hazard-pointer store dirtied the line every scan was reading.
`sizeof(Node)` is larger this way; it is one allocation per participating thread.

### The proxy's per-thread state rides in the source

`core::SegmentSource` no longer has `ticket()` or `max_threads()`. It has `thread_payload` and
`guard::payload()`, and the proxy's counters live in the same node as the source's own state.

This reverses a decision the refactor made deliberately — the pre-refactor reclamation classes
each took the proxy's metadata as a template parameter, which made them non-interchangeable.
Removing the dense index left a choice between two thread-local lookups per operation or one,
and on a project whose whole point is measurement the measured path won. What keeps it
interchangeable this time is that `thread_payload` is a plain template parameter with an empty
default: the source never names a proxy type, and a source used without a proxy pays nothing
for it under `[[no_unique_address]]`.

---

## `mem::source::Pool` — reclamation, as built

The proposal had `Pool` wrapping the existing `Recycler`. It now owns reclamation outright,
and `util/hazard/` is gone. `Recycler` carried its own ticketing, per-thread metadata, a
reuse cache and a lookup-table template parameter, almost none of which the proxy used —
the proxy keeps its own per-thread array. `HazardVector` and `HazardCell` were dead library
code (only their own test included them, because `source::Hazard` reimplements the
mechanism); `StaticThreadTicket` was referenced by nothing.

What was *kept*: the lock-free index container. `algo::LFring` via `mem::detail::RingSlab`
is reused rather than rewritten, so the genuinely risky component is one that already
existed and is tested.

`Pool<S, N>` hands out `mem::VersionedIndex<N>`. The split is sized by the pool rather than
fixed at 32/32: the index takes the `log2(N)` bits it needs and the version takes the other
61-or-so, because every bit not spent addressing eight slots is ABA margin. The index is
capped at 32 bits, which is what puts a floor of 32 under the version. `N` consequently
appears twice per pooled entry — in the segment's `mem::IndexHandle<N>` and in the
`MemBounded` alias — and `LinkedProxy` static_asserts that `Segment::handle_type` is
`Source::handle`, so a mismatch is a diagnosable error rather than an index read out of the
wrong bits.

Three limbo buckets and **one separate free list**:

- `retire(h)` → `limbo[e]` for the epoch current at the time
- advancing to `e'` drains `limbo[e' + 1]` (retired two epochs ago) into the free list
- `discard(h)` → straight to the free list; a never-published segment has no observers
- `acquire()` pops the free list, and attempts one advance if it is empty

The epoch advances only when every pinned thread published the current one.

> **The free list is separate on purpose.** Using the oldest limbo bucket as the free list
> looks equivalent and is not: if the epoch moves past the moment that bucket is the free
> one — which happens readily, since `acquire()` advances the epoch itself when it finds
> nothing — its contents are stranded for a whole rotation. The first version did exactly
> that, and `PoolReclamationTest` caught a slot that never came back.

### Verification status

`PoolReclamationTest` drives the epoch machine from a single thread: a slot retired under a
live pin is not reusable; it takes two advances to be released; exhaustion is reported
rather than fabricated; handle versions change across reuse; no slot leaks over 50 cycles.

**This is not a substitute for the threaded suite.** `ConcurrencyTest` compiles and is not
run here by request. The pooled entries (`mem-vyukov`, `mem-prq`, `mem-scq`) are back in the
registry and work single-threaded, but their concurrent behaviour is **unverified**.

---

## Write-once segments became recyclable

`FAAArray::reopen()` and `HQ::reopen()` used to return false, so both were
`recyclable == false` and `mem::source::Pool` refused them. A life of either ends with the
array uniformly `consumed`, so instead of sweeping it back to `empty` each segment carries a
generation flag that **swaps which sentinel word means empty**: `consumed` in generation *g*
is a valid `empty` in generation *g+1*. Reopening is a flag flip and two index stores.

The swap is expressed through the `cell::Tagging` predicates rather than by comparing words,
so it stays inside the contract and works for both `MsbTag` (msb|0, msb|1) and `LowTag`
(0, 1). `is_payload` is untouched — the two words swapped are non-payload either way.

> **The flip alone was not enough, and the test caught it.** `LinkedProxy` reopens *every*
> segment it acquires from a recycling source, and only one of the three states a segment can
> be in is uniformly `consumed`. A pristine pool slot is uniformly `empty`, so flipping it
> made every cell read as `consumed` and the segment was born full; the proxy's
> `assert(placed)` is compiled out in Release, so it linked an empty segment and lost the
> item. `RegistryConformanceTest` showed it precisely: 71 pushed, 64 popped — seven lost, one
> for each of the seven pool slots that are not the sentinel. `reopen()` now dispatches on
> `head_`: pristine (`head == tail == 0`, nothing to do), fully drained (`head >= capacity`,
> flip), or partially used from the `discard` path (sweep). Only the last is O(capacity), and
> only the loser of a link race reaches it.

`recyclable` is now true for both, `mem-faa` and `mem-hq` are registered, and the tree has no
non-recyclable segment left — so the `Pool` guard on `recyclable` can no longer be exercised
positively, only asserted to exist.

## `Hazard::collect` — one registry walk, not one per pointer

`collect` used to ask `is_protected(p)` once per retired pointer, and each of those walked the
whole thread registry: *R* traversals of a pointer-chased list to answer a question that takes
one. The two collections are asymmetric — the retire list is a contiguous vector, the registry
is not — so the registry is now scanned once and each node scans the vector, partitioning it
in place:

```
[0, k)      protected: some thread published this pointer
[k, size)   not matched by any node seen so far
```

A node whose hazard pointer hits the unclassified region swaps that entry down to `k`, so each
node only scans what is still in question and that region shrinks. At the end `[k, size)` is
exactly what nobody is reading: `pop_back` and destroy. No temporary container, and every
vector operation is O(1).

The functor is not idempotent — it advances `k` — which is sound only because
`ThreadRegistry`'s walk visits each node exactly once. That is true of the append-only,
never-restarting walk described above, and the coupling is noted at both ends.

---

## Registration is a scope, not a pair

`register_thread()`/`unregister_thread()` on the sources and `acquire()`/`release()` on the
proxy are gone. `join()` returns a move-only `session` whose destructor detaches:

```cpp
auto s = q.join();      // or registry::Instance<Q>::session(q) in generic code
...                     // every exit path releases
```

The session carries two pieces of state, and keeping them apart is what makes a **nested**
join safe: `attached_` answers "is this thread attached", which an inner join must report
truthfully, while `owner_` answers "does this session owe the detach", which only the join
that actually attached may hold. An inner session therefore converts to `true` and does
nothing on destruction.

This was not cosmetic. `ConcurrencyTest`'s producer had a `leave()` on its livelock bail-out
path, duplicated from the normal exit — exactly the failure mode that made `pin()` a guard in
the first place. It also un-collides two names: `Source::acquire()` hands out a *segment* and
returns `optional<handle>`, while `LinkedProxy::acquire()` used to mean "register this thread"
and return `bool`.

`core::Ticketed` is renamed `core::Joinable`, since there have been no tickets for some time,
and `registry::Instance::join`/`leave` collapse into one `session(q)` that returns
`std::monostate` for standalone queues, which have no notion of participation.

> **`core::DirectConstructed` narrowed to one `std::size_t`.** Dropping the thread count left
> `LinkedProxy(segment_capacity, chunks = 4)`, which is *still*
> `constructible_from<size_t, size_t>` — so generic code would have kept compiling and quietly
> bound the old thread count to `chunks`. Narrowing the concept turns a stale two-argument
> construction into a compile error instead of a change of meaning. Standalone queues take
> `(capacity, mem::Blocks)` and still fail it, so `Constructible` keeps discriminating.

---

## What is still open

- **`ConcurrencyTest` has not been run** against any of this. Everything concurrency-related
  above — the SCQ in-flight fix, reservation-based admission, the new reclamation, the
  thread registry — is argued from deterministic tests and reasoning, not from a threaded run
  of the queues themselves.
- **`retry_scan_on_attach` is off**, so `for_each_active` currently rests on an ordering
  argument rather than on a counter. Turning it on is a one-token change; the first threaded
  runs are worth doing both ways.
- **`util/timing/TicksWait.h`** is a 687-line header used only by the benchmark's delay
  simulation; it has not been reviewed.
- **`algo::LFring`'s `reopen()` vs `reset()`** are two similar reset paths; only `reset()`
  is used by SCQ. Worth collapsing.
- **`docs/legacy/`** holds `Buckets.hpp.txt`, `EpochCell.hpp.txt`, `VersionedIndex.hpp.txt`
  and `SegmentRecyclerTest.cpp.errored_out` — kept for reference on an earlier instruction,
  no longer compiled or included. `PhasedBucket` in particular is a bucket built for this
  exact access pattern and may be worth reviving as a cheaper limbo container.
