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
util/     bit, specs, atomic/cas2, threading/, timing/
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
  above — the SCQ in-flight fix, reservation-based admission, the new reclamation — is
  argued from deterministic tests and reasoning, not from a threaded run.
- **`util/timing/TicksWait.h`** is a 687-line header used only by the benchmark's delay
  simulation; it has not been reviewed.
- **`algo::LFring`'s `reopen()` vs `reset()`** are two similar reset paths; only `reset()`
  is used by SCQ. Worth collapsing.
- **`docs/legacy/`** holds `Buckets.hpp.txt`, `EpochCell.hpp.txt`, `VersionedIndex.hpp.txt`
  and `SegmentRecyclerTest.cpp.errored_out` — kept for reference on an earlier instruction,
  no longer compiled or included. `PhasedBucket` in particular is a bucket built for this
  exact access pattern and may be worth reviving as a cheaper limbo container.
