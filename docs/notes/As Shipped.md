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
mem/      Align, Layout, SingleBlock, Handle; source/{Hazard,Pool,Payload}
cell/     PlainCell, SequencedCell, Tagging
linkage/  None, Node<HandlePolicy>
algo/     Vyukov, VyukovNoABA, VyukovDCAS, PRQ, FAAArray, HQ, LFring, SCQ, PSCQ, Mutex,
          PhasedBucket, CacheRing   (index buckets, not segments)
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

Four buckets in a rotation, with the role of each taken from the stage modulo four:

```
  current(e) = e       retirements land here
  grace(e)   = e - 1   late retirements, from a thread a stage behind
  free(e)    = e - 2   acquire pops here
  next(e)    = e + 1   about to become current

  next <-- free <-- grace <-- current <--+
    |                                    |
    +------------------------------------+
```

Four rather than three is what lets the buckets be `algo::PhasedBucket`: a single `fetch_add` at
each end, no per-cell sequence word, no padding, because a bucket is never filled and drained at
the same time. `sizeof(PhasedBucket<8>)` is 256 bytes against 1280 for a padded ring of the same
capacity.

> **Every role comes from the stage a thread *pinned* at, never the global stage.** Pins span two
> stages, so fills target `bucket(p)` for `p in {g-1, g}` and pops target `bucket(p+2)`, i.e.
> `{g+1, g+2}` — disjoint, which is the whole reason there are four. Deriving roles from the
> global stage instead looks equivalent and is not: a thread pinned at `g` would pop
> `bucket(g+1)`, and the instant the rotation moves that bucket becomes `current` and is filled.
> That trips PhasedBucket's own assertion within a few hundred milliseconds of stress.

### Three things the stress found that reasoning had not

- **The pin had a publication race.** `pin()` loaded the stage and then stored it, and in that
  window the thread is invisible to the advance scan — its state word still reads unpinned — so
  the rotation could move *twice* and the thread would publish a stage two behind. Pins then span
  three stages and the disjointness above collapses. `pin()` now publishes and re-reads, retrying
  until the stage it published is still current; the seq_cst store and the seq_cst scan are
  totally ordered, so either the scan sees the pin or the pin sees the advance.
- **The rotation had to be exclusive.** The bucket flipping from draining to filling must be
  emptied *before* the new stage is visible, or threads pinning at the new stage fill it while a
  straggling drainer is still popping. A second CAS claims the rotation; the drain and the publish
  both happen under it.
- **Without that drain the pool could starve holding free slots.** Whatever sits in `next` is
  reachable only by a thread a stage behind, and a rotation is refused while any such thread
  exists — so those slots would be stranded until the rotation came round, which was itself
  waiting on them. The rotation now sweeps them into the cache on the way past.

`discard()` does not enter the rotation. A segment that was never published has no observers, so
it goes straight to an `algo::CacheRing` and `acquire()` looks there first. The cache is genuinely
MPMC with no phase discipline, which is why it is a ring and not a fifth bucket — its single-word
ABA-safe CAS, with the lap folded into every cell, is what makes that cheap.

**The cache is read before the pin, not under it.** Everything in it is either a discard, never
published and so never reachable by another thread, or a slot the rotation swept out of the
flipping bucket — and that sweep only runs once the scan has established no pin can still name it.
Neither case is made safer by a pin, so requiring one charged the common path a thread-local lookup
and a stage read for a guarantee it was not using. A cache hit is now a single CAS, and it no
longer publishes a pin that another thread's `try_advance` would have to wait behind.
`PoolReclamationTest.ACacheHitIsServiceableWhileTheRotationIsFrozen` holds this down: with a stale
pin freezing the rotation, a discarded slot must still come back.

`try_advance` also **remembers whoever refused it last** and asks that thread before asking
everybody. A straggler stays a straggler for its whole traversal while every other thread hammers
the function, so one load usually settles it. Three things make the hint sound, and the third is
the subtle one: reading a payload directly bypasses the `is_active()` filter that
`ThreadRegistry::all_of` applies, and detached nodes keep their payload — but `~guard` stores 0
unconditionally, so a departed thread always reads *unpinned* and the hint falls through to the
real scan. A hint that does read pinned-at-an-older-stage therefore names a live pin, possibly a
different thread that has since taken the node over, which is an equally genuine blocker. The
hint can only refuse; the scan alone ever permits a rotation.
`PoolReclamationTest.AHintNamingADepartedThreadDoesNotWedgeTheRotation` holds that down — it
fails the moment `~guard` stops clearing the state byte.

`try_advance` takes two early-outs ahead of its scan — the stage already moved past the caller's
pinned stage, or somebody else holds the rotation. Both are re-checked under the claim, so neither
changes which rotations are legal; they exist because the scan reads one `seq_cst` line per
registered thread and every thread in `acquire()` runs it in a loop. Instrumented at 4 producers
and 4 consumers, the pool was doing roughly **61 scans per successful acquire**, the bulk of them
after a rotation had already invalidated the caller's stage.

`LinkedProxy::enqueue` closes the loop from the other side. When the source comes up empty it
renews and restarts the traversal rather than reporting exhaustion immediately — because under an
epoch source "empty" is frequently self-inflicted: only a rotation can free a slot, and the
rotation is refused while any thread sits pinned at an older stage, this one included. Renewing
rather than unpinning is what makes it safe: `s` is used after `acquire()` returns, for
`link_next`, so protection has to stay live across the call. The retry is gated on `renew()`
reporting that it actually moved, which distinguishes a convoy from the real memory bound, and
bounded by `ProxyOpt::acquire_retries` (default 2) so a genuinely full pool still refuses.

### What this cost, measured

Counters are the honest instrument here — this machine's 400–2500 MHz range puts wall-clock
differences below the noise floor. At 2M items, 4 producers / 4 consumers, `mem-faa`:

| | full scans | retries | acquire failures |
| --- | ---: | ---: | ---: |
| baseline, cap 1024 | 433,589 | — | 125,096 |
| hint + ungated retry | 16,955 | 73,101 | 127,812 |
| **hint + gated retry** | **16,611** | **1,365** | 122,931 |
| baseline, cap 262144 | 3,750 | — | 1,046 |
| **hint + gated retry** | **71** | **39** | **84** |

**Throughput did not move.** The scan was real work but not the bottleneck. What the pooled
source actually pays for, when saturated, is the *cost of discovering it is full*: a traversal
plus a failing `acquire()`, against one counter load for `admit::ItemCount`. Interleaved
measurement (reps rotated between variants — running all of one variant's reps before the next
hands the later one the whole frequency ramp, which fabricated a 2.3× "win" until it was fixed):

| capacity | chunk-faa | mem-faa pool=4 | mem-faa pool=32 |
| --- | ---: | ---: | ---: |
| 1,024 | 6.1–7.1 | 3.5 | 2.8–3.0 |
| 262,144 | 7.0–7.4 | 6.5–7.5 | 6.7–8.0 |

So pooling costs nothing when there is room and roughly 2× when saturated, and **a bigger pool
does not help** — at a fixed total capacity more slots means smaller segments (1024/32 = 32
slots each), so links get more frequent, not less. FAAArray's O(1) `reopen()` does not show up as
a throughput win: when roomy, links are rare enough that a fresh allocation is amortised away
anyway. Pooling buys a hard memory bound and no allocator traffic, not speed.

`ThreadData::state` is a single byte: bit 7 pinned, bits 0-1 the stage. The scan only ever asks
"pinned, and at my stage?", and the pin bounds the shift to one stage, so the 64-bit epoch it used
to carry was never read. The stage itself wraps — EBR has no ABA to guard against here, because a
thread is either at the current stage or it blocks the rotation outright.

`mem::detail::RingSlab` is deleted; the buckets are compile-time-sized members, so there is no
slab and no runtime allocation for them.

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

## Accounting across threads that come and go

`LinkedProxy::size()` folds per-thread counters that live in the source's registry nodes, and a
scan visits only *attached* threads. A producer that enqueued and then detached therefore took
its count with it, and the queue under-reported permanently. Invisible in every other suite,
because they hold one session for the whole test and ask the thread that pushed.

`size()` is now a departed-thread total plus a fold over the attached. The wrapper session
returned by `LinkedProxy::join()` folds a thread's `ops` into that total on the way out and
clears its `last_seen`, so the node goes back for reuse already clean — the alternative,
inheriting the previous owner's tally, double-counts, and inheriting its close hint makes the
next thread skip a segment it never saw closed. The reset happens at **detach** rather than at
attach, so it is done by the only thread that can still name the payload and needs no
source-side hook. `Hazard`'s retire list is deliberately left alone; that inheritance is
load-bearing.

`ProxyAccountingTest` is the regression suite, and it fails on the old code in four places.

## Reopening belongs to the source

`mem::source::Pool::acquire()` reopens the segment before handing it out, and `LinkedProxy`
lost its `if constexpr (Source::recycles)` branch. A source that returns a segment which has
already held items is the one that knows it needs resetting; making the caller ask first put
that knowledge in the wrong place. `recycles` stays, because the proxy still static_asserts on
it against `segment_traits<>::recyclable`.

`acquire()` and `discard()` now assert the caller holds a pin — reclamation there is
epoch-based, so doing either outside one is meaningless. `LinkedProxy`'s constructor and
destructor were adjusted to satisfy it rather than the assertion being weakened.

> **`Pool::pin()` is not re-entrant.** The guard's destructor stores 0 unconditionally, so an
> inner guard ending clears the epoch the outer one published and leaves it running unpinned,
> with nothing to report it. The proxy's destructor is written around this: its pin covers the
> discard loop only, after the drain, because `dequeue()` pins internally.

## Capacity means one thing now

Every algorithm rounds a capacity request **up to the next power of two by default**, so
"capacity 1000" is the same 1024 cells whichever one you pick. Before this the tree disagreed
three ways: `LFring`, `SCQ`, `PSCQ` and the two Vyukov comparators always rounded; `Vyukov` and
`PRQ` rounded only when asked; `FAAArray`, `HQ` and `Mutex` never did. A benchmark across
families was therefore comparing geometries as much as algorithms.

The opt-out is spelled `no_pow2` in every options struct that offers it, and `PSCQ`,
`VyukovDCAS` and `VyukovNoABA` grew modulo fallbacks so it is real for them rather than
decorative — `VyukovNoABA` most awkwardly, since it derives its *lap number* from the ticket and
so trades a shift for a division as well as a mask for a modulo.

`LFring` and `SCQ` deliberately offer no opt-out: `LFring` stores its size as an *order* and
`SCQ` is built on two of them, so a non-power-of-two is not expressible there rather than merely
slower. Passing them the tag is a compile error through `meta::AcceptsOnly`.

`RegistryConformanceTest.CapacityRoundsUpToAPowerOfTwo` checks the property across all 30
registered entries at sizes 3, 100 and 1000 — chosen because they are *not* powers of two.
Everything else in the tree builds queues at 64 or 8, which satisfies the property without
exercising any rounding at all, and is why the old suite could not have caught this.

> **Benchmark rows recorded before this are not comparable** for `vyukov`, `prq`, `faa`, `hq`
> and `mutex`, whose reported capacity now rounds up where it previously did not.

### Known deviation: PSCQ does not honour its own capacity()

`PSCQ::enqueue` tests fullness against `size_`, the physical ring, while `capacity()` reports
`size_ >> 1` — so it advertises 128 and accepts 256. Its own header states that "the physical
ring is twice the usable capacity", which makes the fullness check the wrong one of the two.

**Pre-existing and unrelated to the rounding**: it behaves identically with the default. It
surfaced only because the new capacity tests are the first to compare items admitted against
`capacity()` — `RefusesBeyondCapacityThenDrainsExactly` checks that a queue drains what it
accepted, never that it accepted what it advertised.

Left as-is deliberately. Correcting the check would halve `pscq`'s effective capacity and
invalidate every benchmark number recorded for it, which is a bigger change than the
inconsistency warrants. Two consequences worth knowing:

- **`pscq` benchmark rows are taken at twice their stated capacity.** Comparing `pscq` against
  another algorithm at "capacity N" is comparing N against 2N.
- `PSCQ` is verified correct *up to* its advertised capacity — the new tests fill it to
  `capacity()`, drain it in FIFO order, and wrap a 3-slot ring 100 times. Whether filling it to
  the full `size_` preserves FIFO and the threshold invariant is **untested and unknown**, so the
  extra headroom should not be relied on.

`OptionsTest`'s `exact_size_round_trips` call site carries the same explanation, next to the one
assertion it skips for `PSCQ`.

---

## Two lock-based controls, and four more linked segments

`algo::Mutex` was the only control, and it answered one question badly: it measured
`std::mutex` acquisition *and* the try-and-fail loop a caller does when the queue is full,
mixed together. There are now two controls, differing in exactly one thing:

| | lock | on full / empty |
| --- | --- | --- |
| `algo::Mutex` | `std::mutex` | parks on one of two condition variables |
| `algo::Spin` | three-state futex spinlock | refuses immediately |

**`Mutex` gained two condition variables and a single mutex.** `not_empty_` and `not_full_`,
signalled with `notify_one()` — one item can be taken by exactly one consumer, so waking the
rest only to have them find an empty queue is the thundering herd the split exists to avoid.
The notify happens after the lock is released, since a woken thread would otherwise immediately
block on it.

The wait is **bounded** (`MutexOpt::wait_micros`, default 50) and that bound is load-bearing
rather than a tuning choice:

- `dequeue()` must still answer `false` on an empty queue. Every drain in the tree — the
  benchmark's, `ConcurrencyTest`'s — ends on `while (q.dequeue(out))`, so an unbounded wait
  would hang rather than fail.
- A **linked** segment must still refuse the instant it is full, because the proxy reads that
  refusal as "link a successor". Waiting there waits for room this segment is never going to
  have. So parking is disabled outright when `Link::is_linked`, exposed as
  `parks_when_blocked` and asserted in `OptionsTest.OnlyAStandaloneMutexParks` — a
  static_assert, so a regression is a build failure rather than a stall.

**`algo::Spin`** is the same ring with a spinlock that bounds its spinning
(`SpinOpt::spins_before_park`, default 64) and then parks on `std::atomic::wait`. The lock is
the three-state futex — free / held / held-and-contended — so an uncontended unlock is a plain
store with no `notify_one()`. It never waits on the queue's *state*, only on the lock, which is
what makes the pair separable: whatever `Spin` differs from `Mutex` by is blocking policy, not
lock cost.

### PSCQ, VyukovDCAS and VyukovNoABA are segments now

All three were standalone comparators. They now carry the full linkage surface and appear in all
four proxy families, so the registry is **47 entries**. Three details worth keeping:

- **How each closes.** `VyukovDCAS` steals the top bit of its tail ticket, as `Vyukov` does.
  The other two cannot: `VyukovNoABA` derives its *lap number* from the ticket, and `PSCQ`
  advances its tail with `fetch_add` while the sequence words already use their top bit as the
  `unsafe` marker. Both use a separate flag, read only on the linked path.
- **`PSCQ::reopen()` is O(ring)**, alone among the segments, which recycle by realigning
  indices. A drained PSCQ cell can carry the `unsafe` bit into the next lap and its threshold is
  spent by empty dequeues rather than by position, so neither is a function of the head alone.
  Rebuilding restores the one state the algorithm is demonstrably correct from.
- **`PSCQ` needs both `prepare_dequeue_after_link()` and `has_inflight()`.** The first for the
  same reason as `SCQ` — an empty dequeue spends the threshold, and a post-link retry needs it
  back. The second because it claims a cell with a token and publishes the payload in a *later*
  CAS, so unlike `PRQ` a cell can genuinely be seen mid-insert.

`LFring` and `CacheRing` and `PhasedBucket` are deliberately untouched: the first is a ring of
*indices* with an order-based size, and the other two are internals of the pooled source rather
than queues in their own right.

---

## A close has to live in the tail, not in a flag

`ConcurrencyTest` found seven failures in the segments that had just been made linkable. The
cause was one mistake, made twice: `PSCQ` and `VyukovNoABA` were given a `std::atomic<bool>` as
their closed flag.

`LinkedProxy::dequeue` unlinks and **retires** a segment on nothing more than *dequeue said empty
twice and a successor exists*. So the design rests on an invariant:

> once a segment refuses an enqueue, it must refuse for ever.

A flag cannot provide it. A producer reads `closed_ == false`, is descheduled, and commits its
cell CAS arbitrarily later — after the drain, the unlink and the retire. The item is counted as
enqueued and never traversed again. What the healthy segments do instead is put the marker **in
the tail counter**, so it invalidates tickets that are already in flight:

| | close() | the ticket is the permission |
| --- | --- | --- |
| `Vyukov` | MSB of the tail; commit *is* the tail CAS | ✅ |
| `PRQ` | MSB of the tail; `fetch_add` ticket | ✅ |
| `FAAArray` | `tail_ = capacity_` | ✅ |
| `HQ` | `tail_ += capacity_` | ✅ |
| `Mutex`, `Spin` | set under the lock that guards the insert | ✅ |

`PSCQ` is now a direct port of PRQ's scheme: `fetch_or` the top bit, check it on the ticket the
`fetch_add` returns, and route every arithmetic use of a ticket through one `clean()` helper —
`mod()`, the sequence CAS, both fullness comparisons, `size()` and `fix_state()`. It also takes
`needs_close_hint`, as PRQ does, since it inherits the same unsafe-cell path.

### Two things the harness found that the failure list did not

**`VyukovDCAS` had the same bug and was passing.** Its close already set the tail MSB, but the
double-CAS on the cell decided success while the tail CAS was best-effort — so the close
invalidated nothing. At 4 producers / 4 consumers on 16-slot segments it lost items in **12 of
25 trials**; it simply never tripped in the run that reported the seven failures. Fixing it
preemptively was the right call.

**Reserving the ticket is necessary but not sufficient.** With the reservation in place `DCAS`
still lost items in 4 of 25 trials, because reserving and publishing are two steps: in between,
the cell still reads as the previous lap, a consumer calls the segment empty, and the proxy
retires it under the reservation. That is what `has_inflight()` is for, and both `VyukovDCAS`
and `VyukovNoABA` now declare `needs_inflight_drain` and answer it in O(1) — `head != tail`
means a publish is outstanding, which is exact now that every reserved ticket is published.

`VyukovNoABA` additionally had a **pre-existing hole**: its tail advanced even when the cell CAS
failed, so a ticket could be skipped and its cell left unfilled behind a head that had moved
past it. The mandatory reservation closes that too. The cost is that its commit protocol is now
Vyukov's; what stays distinctive is the single-word cell with the lap folded in, which is what it
exists to measure. **Its benchmark numbers are not comparable with earlier ones.**

### Measured, before and after

4P/4C, capacity 16 (4-slot segments), 20k items, 25 trials, counting trials that lost items:

| | before | after |
| --- | ---: | ---: |
| `item-pscq` | 13/25 | **0/25** |
| `chunk-noaba` | 20/25 | **0/25** |
| `item-dcas` | 12/25 | **0/25** |
| `item-vyukov` (control) | 0/25 | 0/25 |

All fifteen proxy configurations, including the untouched controls, are clean afterwards, and
clean again under TSan with no races reported.

`SegmentLifecycleTest.AClosedSegmentRefusesEveryEnqueue` pins the single-threaded half of the
contract across all ten segment types. It is deliberately weak — an advisory flag passes it too
— and says so, because the enforcing half only shows under contention.

### Still worth knowing

`Vyukov` reserves its ticket and *then* writes the cell and sequence, which is the same two-step
shape, yet it declares `needs_inflight_drain = false` on the grounds that "a single atomic step
publishes the item". That claim does not hold literally. It shows no losses in any harness here,
so it has been left alone, but it is the next thing to look at if a loss ever appears in the
Vyukov family.

---

## Still open, and left as notes in the source

`Pool` carries `@debug` notes for a larger rework that wants measurement rather than a
refactor: four stages with `next <- free <- grace <- current`, `PhasedBucket`
(`docs/legacy/Buckets.hpp.txt`) in place of `LFring` for the limbo rings, `ThreadData::state`
narrowed to a `uint8_t` stage, the epoch kept modulo `kStages`, `segments_` on a slab, and a
`VyukovNoABA` reuse cache. `RingSlab` and `Pool` are now parameterised on the bucket ring, so
that swap can happen without touching the epoch machine.

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
