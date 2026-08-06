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
  / `DirectConstructed` / `Ticketed` / `Constructible` name it, and an ambiguous or
  unsupported shape is now a diagnosable error.

---

## `util::threading::ThreadRegistry` — who is participating

Both sources need the same thing: the set of attached threads, and what each is publishing.
Both used to get it from `DynamicThreadTicket`, which handed out a dense integer from an
atomic bitset and left the caller to keep its state in a *separate* array indexed by it —
`Hazard` had `slots_` and `retired_`, `Pool` had `threads_`. Three costs followed:

| | before | now |
| --- | --- | --- |
| thread cap | `DTT_MAX_BITS` = 1024, **compile-time** | constructor argument |
| live instances | `DTT_MAX_INSTANCES` = 16, threw past it | unbounded |
| reclamation scan | O(`max_threads`) | O(peak threads ever attached), functor only on active |
| per-thread state | separate array, indexed by ticket | the registry node itself |

`ThreadRegistry<ThreadData, Opt>` pre-allocates `max_threads` cache-line-isolated nodes once
and threads two lock-free lists through them: an **active list** that reclamation walks, and
a **free list** of recyclable nodes. Nodes are immortal — never allocated or freed while the
registry lives — so `attach()` never calls the allocator and is genuinely lock-free, and
`attach()` returning false when the free list is empty is exactly the old `register_thread()`
returning false. `ticket()` is now just the node's stable `slot`, so
`core::SegmentSource` is unchanged and `LinkedProxy` needed no edit.

Detach is one CAS and no traversal: the owning thread sets the mark bit in its own link and
pushes the node onto the free list. The node **keeps its place in the active list**; attach
pops it and clears the mark in place. A node is appended to the active list at most once
ever, on first use, and is never removed — so the list is append-only and acyclic, and a walk
is a pure read with no CAS, no helping and no restarts.

> **The physical splice was tried, and removed.** The first version did unlink detached nodes
> Harris-style, to make the chase O(*currently* attached). It is not safe here. A walk holds
> its predecessor as a *snapshot*; once a node can be unlinked and immediately recycled, that
> snapshot can lead the walk into a detached node's stale successor chain, where the next
> link looks perfectly consistent and the splice CAS therefore **succeeds** — unlinking and
> freeing a node that is still live in the real list. It then sits on both lists at once, is
> popped while still linked, and attach closes a cycle. Measured before the fix: a double
> push to the free list within a few thousand attach/detach cycles on 8 threads, and roughly
> half of all `NoNodeIsLostOverManyAttachDetachCycles` runs ending with nodes lost. Versioned
> links stop ABA on an individual CAS; they cannot stop a traversal anchoring on a node that
> has left the list. Splicing safely needs reclamation for the *nodes* — the very thing this
> registry exists to support rather than depend on. Not unlinking removes the problem
> outright and costs only that the chase is bounded by peak rather than current concurrency.

> **Every link carries a version, and that is not optional.** Immortal nodes remove
> use-after-free, which makes it tempting to conclude bare indices suffice. They do not.
> On the free list, ABA is a *lost node*: T1 reads `head = A, A.free_next = B` and stalls;
> T2 pops A; T3 pops B; T2 pushes A back; T1's `CAS(head, A -> B)` then succeeds and hands
> out a node T3 is already using. Each link is
> `{version:32 | mark:1 | index:31}` in one word — the same 32/32 split as
> `mem::VersionedIndex` — so every publication produces a word no earlier reader can have
> observed and a stale CAS always fails. One 64-bit CAS throughout; no `cas2`, no dependence
> on `-mcx16`.

Because nothing is ever unlinked and a node's successor is written exactly once, a walk
cannot be led astray: there is no helping, no restart loop and no visit budget. It skips
marked nodes and invokes the functor on the rest.

**The one hole, and the knob for it.** A thread attaching at the head *after* the walk passed
it is not visited; for a hazard scan that would mean freeing an object it had just protected.
`ThreadRegistryOpt::retry_scan_on_attach` closes it — attaches bump a counter,
`for_each_active` re-reads it and repeats if it moved. It is **off by default**, so the
default relies instead on the argument that a thread attaching after a scan began cannot
already hold a pointer to something unlinked before `retire()`. That argument is delicate and
fails silently, so the flag is worth setting for stress and TSan runs; it is one token at the
`using Registry = ...` line in `Hazard.hpp` / `Pool.hpp`.

A side effect worth naming: a thread that dies while pinned no longer wedges `Pool`'s epoch
forever. Its node leaves the active list and stops being consulted, where the old fixed slot
array kept its stale pinned word visible for the life of the pool.

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
