# mpmc_queues

A testing ground for concurrent MPMC queues: several bounded ring algorithms, and a way to
compose them into unbounded or memory-bounded queues through a linked list of segments.

The point of the project is measurement — what thread synchronisation actually costs under
different contention profiles — so the design rule is that **nothing pays for abstraction on
the measured path**. There is no virtual dispatch anywhere in a queue operation: contracts
are C++20 concepts, composition is by template parameter, and per-implementation
capabilities are `if constexpr` over a traits block.

---

## Build

```bash
cmake -S . -B build          # clang is preferred when present; -DUSE_CLANG=OFF for gcc
cmake --build build -j
ctest --test-dir build       # unit tests
```

Requires C++20. On x86_64 the build passes `-mcx16` for the double-width CAS one comparator
needs. `compile_commands.json` and a `.clangd` are generated at configure time.

| Option | Default | |
| --- | --- | --- |
| `USE_CLANG` | ON | prefer clang so the compile database matches a clangd editor |
| `NATIVE_ARCH` | ON | `-march=native` |
| `BUILD_BENCHMARK` | ON | |
| `BUILD_TESTING_MPMC` | ON | |
| `SANITIZE_ADDR` / `SANITIZE_THREAD` | OFF | build the tests under ASan / TSan |

## Run

```bash
./build/benchmark --list
./build/benchmark u-prq 4 4 10000000 1024 pin
```

`<name> <producers> <consumers> <items> <capacity> [pin] [prod_ticks amp] [cons_ticks amp]`,
printing items/second. For sweeps and plots see [`python/README.md`](python/README.md).

`<capacity>` is the queue's **total**, split across the segments that will exist — divided by
the chunk count for a bounded proxy, by the pool size for a pooled one. Each segment is floored
at two slots and then rounded up by its own algorithm, so `capacity()` reports what the queue
can actually reach, which may exceed what was asked for.

---

## Architecture

Six layers, each depending only on the ones above it.

```
meta/   util/        option packs, type lists, bit and cache-line helpers
core/                the contracts: concepts only, no implementation
mem/    cell/        single-block allocation, reclamation sources, cell layout and tagging
linkage/             None, or Node<HandlePolicy>
algo/                the queue algorithms
proxy/  registry/    composition, and the list of what exists
```

### Contracts are concepts

`core::Queue`, `core::LinkedSegment`, `core::Proxy`, `core::SegmentSource`,
`core::AdmissionPolicy`, `cell::Tagging` — plus `core::segment_traits<S>`, which has **no
primary definition**, so a segment that forgets to declare its capabilities fails to compile
rather than silently inheriting defaults.

### One algorithm, two shapes

Each algorithm is written once and parameterised by a linkage policy. `linkage::None` is an
empty struct under `[[no_unique_address]]`, so a standalone queue pays nothing for being
linkable — measurably: `sizeof(queue::Vyukov<T*>)` is 384 bytes against 512 for
`seg::Vyukov<T*>`, exactly one padded next-pointer apart.

```cpp
queue::Vyukov<Item*>   q = ...;   // standalone bounded ring
seg::Vyukov<Item*>     s = ...;   // the same algorithm, as a linked segment
```

### Some algorithms cannot stand alone, and the compiler enforces it

PRQ, FAAArray and HQ are only correct as linked segments:

- **FAAArray** and **HQ** write each cell once and their indices only advance. Standalone
  they are single-use — the first fill/drain works and every later enqueue is refused.
  Recovering capacity means reopening, and reopening is only sound on a segment that is
  quiescent and unlinked, which only a proxy over a recycling source can arrange.
- **PRQ** closes itself when producers overshoot and then depends on a proxy linking a
  successor. Standalone there is nobody to link one.

These are constrained on `linkage::Linked`, so the unsound configuration **is not a nameable
type**:

```cpp
algo::PRQ<Item*, meta::EmptyOptions, linkage::None>  // does not compile
//  error: constraints not satisfied for class template 'PRQ'
//  note: because 'linkage::None' does not satisfy 'Linked'
//  note: because 'None::is_linked' evaluated to false
```

There is deliberately no `queue::PRQ`, `queue::FAAArray` or `queue::HQ` alias. The guarantee
is asserted in `SegmentLifecycleTest.cpp`, which checks both that the standalone spelling is
unnameable and that the linked one still is.

### One proxy, three policies

A single `proxy::LinkedProxy<T, Segment, Admit, Source>` carries the Michael–Scott
traversal. The four proxies it replaces differed only in an admission rule and a
reclamation scheme:

| Alias | Admission | Source | Bound |
| --- | --- | --- | --- |
| `proxy::Unbounded` | `admit::None` | `source::Hazard` | none |
| `proxy::ItemBounded` | `admit::ItemCount` | `source::Hazard` | live items (hard) |
| `proxy::ChunkBounded` | `admit::SegmentCount` | `source::Hazard` | live segments |
| `proxy::MemBounded` | `admit::None` | `source::Pool` | pool size |

`MemBounded` needs no admission policy of its own: its ceiling is the segment pool running
dry, which is a *source that runs out* rather than a rule the proxy enforces. That
observation is what collapses four proxies into one.

Reopening a recycled segment is the **source's** job, not the traversal's: `source::Pool`
reopens inside `acquire()`, so the proxy never asks whether its source recycles. And when the
pool comes up empty the proxy looks once more for a successor another producer may have just
linked before reporting the bound — losing that race is not the same as being full.

`source::Pool` owns its epoch-based reclamation and `static_assert`s on
`segment_traits<S>::recyclable`. Its handles are `mem::VersionedIndex<N>`, sized by the pool: the
index gets the `log2(N)` bits it needs and the version gets the rest, so a pool of 8 has a 61-bit
ABA counter rather than the 32 a fixed split left it. `N` therefore appears both in the segment's
`IndexHandle<N>` and in the proxy alias, and `LinkedProxy` static_asserts that the two agree.

Reclamation is **four buckets in a rotation**, roles taken from the stage modulo four:
`current` receives retirements, `grace` catches late ones from a thread a stage behind, `free` is
what `acquire()` pops, and `next` is about to become `current`. Four rather than three is what
lets the buckets be `algo::PhasedBucket` — a single `fetch_add` at each end, no per-cell sequence
word, no padding — because a bucket is then never filled and drained at the same time. Every role
is derived from the stage a thread **pinned** at, never the global one: pins span two stages, so
fills land in `{g-1, g}` and pops come from `{g+1, g+2}`, and those sets are disjoint.

Discards skip the rotation entirely. A segment nobody ever saw owes no grace period, so it goes
straight to an `algo::CacheRing` — genuinely MPMC, one word per cell, ABA-safe on a single CAS —
and `acquire()` looks there first.

Both sources find their participating threads through `util::threading::ThreadRegistry`, an
unbounded lock-free registry. Each thread's state — a hazard pointer and its retire list, or an
epoch word, **plus the proxy's own per-thread counters** — lives in one node, so `pin()`'s
thread-local lookup serves both and an operation never does a second one. There is no thread
cap and no index handed out: a thread gets a reference to its `ThreadData`. Nothing takes a
thread count any more — the registry sizes itself, so a participant limit would be a number
the queue does not need and could only get wrong.

Threads join for a scope rather than pairing calls: `q.join()` returns a move-only session
whose destructor detaches. That replaced an `acquire()`/`release()` pair which had already
grown an early-return path that skipped the release — the same reason `pin()` is a guard. A
nested `join()` reports the thread as attached while owing no detach, so the outermost scope
stays in charge.

The active list is append-only. Detach marks the node inactive and pushes it to a free list;
the node keeps its place, and attach clears the mark. Nothing is ever unlinked, so a walk is a
pure read — no CAS, no helping, no restarts. That is a deliberate retreat from physically
splicing detached nodes out: a walk holds its predecessor as a snapshot, and once a node can be
unlinked and immediately reused, that snapshot can lead the walk into a detached node's stale
successor chain, where the splice CAS succeeds and frees a node that is still live. Splicing
safely needs reclamation for the *nodes*, which is the thing this registry exists to support.

Only `reduce`, `any_of`, `all_of` and the two `for_each` forms traverse anything; `self()` is a
thread-local read. The free list is a Michael–Scott queue — head, tail and a dummy — over
**counted pointers**: a `{Node*, generation}` pair compared as a unit, which is what stops a
stalled `CAS(head, A, B)` from succeeding when the head is A *again* rather than still A and
publishing a node another thread owns. Immortal nodes rule out use-after-free but not that.
On x86-64 with `-mcx16` the pair is one `cmpxchg16b`, so attach and detach are both lock-free
CAS loops; `ThreadRegistry::free_list_is_lock_free` reports whether that held on the target.

`size()` is two parts: a running total for threads that have departed, plus a fold over those
still attached. A per-thread counter stops being reachable the moment its thread detaches, so
without the first part a producer that enqueued and then left took its count with it and the
queue under-reported for the rest of its life. A departing thread folds its tally in and clears
its payload, so whoever inherits the recycled node starts from zero rather than from the last
owner's tally and stale close hint. `ProxyAccountingTest` covers both halves.

Item admission *reserves* rather than testing-then-acting, so the bound is hard: a plain
check followed by an enqueue let every producer that passed the check commit, overshooting
by up to one item per producer.

### Mistakes the compiler catches

| Mistake | Caught by |
| --- | --- |
| a linked-only algorithm used standalone | `linkage::Linked` constraint |
| a pooled source over a non-recyclable segment | `segment_traits<S>::recyclable` assert |
| a segment whose handle is sized for a different pool | `Segment::handle_type` vs `Source::handle` assert |
| a source not given the proxy's per-thread payload | `Source::thread_payload` vs `ThreadMeta<H>` assert |
| a `segment_traits` specialization missing a flag | `core::CompleteSegmentTraits` |
| an option tag that is misspelled or belongs to another algorithm | `meta::AcceptsOnly` |
| a registry entry with an ambiguous construction shape | `core::Constructible` |

Each of these used to be silent: an unrecognised option read as "not requested", an
incomplete traits block failed at some unrelated use site, and a queue that lost its
`create()` simply took the other branch.

### Write-once segments are still recyclable

FAAArray and HQ write each cell once and never return it to `empty`, so a life ends with the
whole array uniformly `consumed`. Rather than sweep it, each segment carries a generation flag
that **swaps which sentinel word means empty** — `consumed` in generation *g* is a perfectly
good `empty` in generation *g+1* — so `reopen()` is a flag flip and two index stores instead
of O(capacity). That is what lets them be pooled (`mem-faa`, `mem-hq`).

The flip is only valid from the fully-drained state, and the proxy reopens *every* segment it
acquires, so `reopen()` dispatches on which of three states the segment is in: pristine
(nothing to do), fully drained (flip), or partially used from the `discard` path (sweep). Only
the last is O(capacity), and only the link-race loser reaches it.

### Adding an implementation

Write the algorithm, specialise `core::segment_traits`, add the `queue::`/`seg::` aliases
appropriate to it, and add one line to `include/registry/Registry.hpp`. The registry feeds
the typed test suites and the benchmark's name dispatch, so nothing else needs touching —
including the Python tooling, which asks the binary what exists.

---

## Implementations

| | standalone (`queue::`) | linked (`seg::`) | notes |
| --- | :-: | :-: | --- |
| Vyukov | ✅ | ✅ | sequence-per-cell CAS ring |
| VyukovNoABA | ✅ | — | lap number folded into the empty cell; comparator |
| VyukovDCAS | ✅ | — | value+sequence swapped by one 16-byte CAS; comparator |
| SCQ | ✅ | ✅ | two index rings + payload buffer; the only arbitrary-sized `T` |
| PSCQ | ✅ | — | PRQ's cell protocol plus SCQ's threshold; comparator |
| Mutex | ✅ | — | lock-based baseline |
| PRQ | — | ✅ | fetch-add ring with an unsafe bit; obstruction-free |
| FAAArray | — | ✅ | linear write-once array; reopened by a generation flip |
| HQ | — | ✅ | FAAArray with a non-destructive slow path |

## Tests

| Suite | Covers |
| --- | --- |
| `RegistryConformanceTest` | contracts and sequential behaviour, every registry entry |
| `SegmentLifecycleTest` | close/reopen/link per segment; the linked-only guarantee |
| `TaggingTest` | cell tagging policies, `can_store_null`, claim tokens |
| `AdmissionTest` | bounds hold, including concurrently |
| `MemoryLayoutTest` | single-block layout arithmetic |
| `PoolReclamationTest` | the pooled source's epoch machine, driven deterministically |
| `ThreadRegistryTest` | attach/detach/recycle, and that a scan never misses a stable thread |
| `ThreadPinnerTest` | core placement rule and topology parsing |
| `ProxyAccountingTest` | `size()` across threads that join and leave |
| `BucketTest` | the phased and cache index buckets the pool is built from |
| `ConcurrencyTest` | loss, duplication and per-producer FIFO across thread shapes |

Concurrency defects here are intermittent — a lost-item bug reproduced in 3 runs of 8, a
livelock in 4 of 12 — so `ConcurrencyTest` repeats each shape (`-DMPMC_REPEATS=N` to raise
it) and treats a stall as a failure rather than hanging.

## Documentation

Start at [`docs/README.md`](docs/README.md), which indexes everything. The essentials:

- [`docs/Extending.md`](docs/Extending.md) — **how to add** an algorithm, a policy, a source,
  or an object with co-allocated arrays, with a worked example each and the obligations that
  the concepts cannot state
- [`docs/Testing.md`](docs/Testing.md) — the suites, the four build configurations, and the
  isolation-harness technique the concurrency bugs were found with
- [`docs/notes/As Shipped.md`](docs/notes/As%20Shipped.md) — **the design that exists**,
  every deviation from the plan, how the pooled reclamation works, and what is still open
- [`docs/notes/Assessment.md`](docs/notes/Assessment.md) — code review, known defects, and the
  optimisation backlog
- [`python/README.md`](python/README.md) — running sweeps and plotting

Generated API documentation:

```bash
cmake --build build --target docs     # -> build/docs/html/index.html
```

Superseded designs live in [`docs/legacy/`](docs/legacy), each headed with what replaced it.

# Benchmarks
- `FAAArrayQueue VS HybridQueue`: set the patience to a really small number: heuristics in an imbalanced (low prod - high cons) FAAArrayQueue should quickly waste all the cells while HybridQueue benefits a lot from the asymmetric-slow dequeue until the full segment has been published

- Memory Usage For Linked Segments: hard to estimate from the allocator because of its caching,
  so the proxy counts instead. `proxy::ProxyOpt::segment_stats` — off by default and free when
  off — turns on `segments_linked()`, `segments_retired()` and `segments_discarded()`; multiply
  by the segment size for the memory estimate, or divide by the pool size for the reuse factor:

  ```cpp
  using Q = proxy::MemBounded<Item, IdxHQ<Item>, 8, meta::EmptyOptions,
                              meta::OptionsPack<proxy::ProxyOpt::segment_stats>>;
  ```

