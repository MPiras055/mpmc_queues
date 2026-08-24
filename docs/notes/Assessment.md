# Assessment

A review of the tree as it stands, and a backlog. Written after a session that fixed four
correctness bugs, so it is as much a record of what those bugs had in common as a critique.

Every claim below is marked:

- **[code]** — established by reading the source. No build required to check it.
- **[measured]** — observed by running something. The numbers are quoted.

That distinction matters more here than in most codebases. Three of the four bugs fixed this
session were invisible to reasoning and obvious to measurement, and one hypothesis that looked
airtight on paper (`needs_inflight_drain` as the cause of PRQ's duplication) was simply wrong.

---

## 1. Scale

| area | lines | files |
| --- | ---: | ---: |
| `include/algo` | 2 702 | 12 |
| `include/mem` | 1 221 | 7 |
| `include/util` | 1 363 | 6 |
| `include/proxy` | 632 | 3 |
| `include/core` | 407 | 7 |
| `include/cell` | 276 | 3 |
| `include/meta` | 296 | 3 |
| `include/registry` | 277 | 1 |
| `include/linkage` | 78 | 1 |
| `src/test` | 2 356 | 11 |
| `src/bench` | 196 | 1 |
| `python` | 2 266 | 20 |

30 registered implementations: 6 standalone, 24 linked (4 proxies × 6 segments).

## 2. What the design gets right

**The abstraction actually pays for itself.** The stated goal was to remove vtable
polymorphism, and it is gone from the operation path entirely: concepts constrain, policies
compose, and `registry::dispatch` is a compile-time fold, so the benchmarked loop is
monomorphic. The usual cost of that — a combinatorial explosion of hand-written aliases — is
avoided because the registry is a type list rather than a switch.

**Empty policies are genuinely free.** `linkage::None`, `admit::None` and `NoPayload` are empty
structs behind `[[no_unique_address]]`. A standalone Vyukov ring carries no successor pointer,
no admission counter and no per-thread payload — not "an optimiser probably removes it", but
nothing to remove.

**Capability dispatch is static.** `core::segment_traits` plus `if constexpr` in `LinkedProxy`
means a segment that needs no close hint compiles no branch for one. The traits also carry
their own conformance assertion (`MPMC_ASSERT_SEGMENT_TRAITS`), so a new segment that forgets
one is a compile error rather than a silent default.

**One line adds an implementation.** `Entry<"mem-mutex", ...>` in `registry/Registry.hpp` is
picked up by the benchmark's `--list`, the typed conformance suite and the concurrency matrix
simultaneously. This is the single best structural decision in the tree, and it is why filling
the 4×6 grid this session was a seven-line change.

**`VersionedIndex` is sized by the pool.** Bits not spent addressing slots become ABA margin,
with a 32-bit floor under the version. An eight-slot pool gets 61 version bits where a fixed
32/32 split would have wasted 29.

**The comments record measurements, not intentions.** `SCQ::reopen` says *"Measured before this
fix: 20004 items consumed against 20000 produced."* `segment_traits<PRQ>` says *"4 of 12 runs
stalled permanently without the hint, 0 of 42 with it."* This is rare and it is load-bearing:
the PRQ duplication bug was traceable this session precisely because SCQ's comment described
the same failure signature.

## 3. What the four bugs had in common

Worth stating plainly, because it predicts where the next one is.

| bug | shape |
| --- | --- |
| `LinkedProxy` discarded a segment with an item still in it | an invariant held by **three of four** implementations, so the fourth looked fine |
| `algo::Mutex` never closed itself when full | a contract every other segment satisfied, never written down or tested |
| `algo::HQ` burned cells with nothing queued behind them | a mechanism (the destructive exchange) applied more widely than its justification |
| `Pool` retired into the pinned rather than the global stage | an off-by-one in a role calculation, invisible except under a debug assertion |

Three of four are **unwritten contracts between components**. The concepts capture *signatures*
(`has enqueue(T, bool)`) but not *obligations* ("a linked segment must refuse permanently once
full"). That is the gap: `core::segment_traits` says what a segment *can* do, and nothing says
what it *must*.

`SegmentLifecycleTest.FillingToCapacityClosesTheSegment` is the one place an obligation is
written as a test — and `seg::Mutex` was not in its type list, which is exactly why that bug
shipped.

## 4. Defects found

### 4.1 `admit::SegmentCount` never reaches its stated capacity — **FIXED**

`proxy/Admission.hpp`. `try_admit()` was called **unconditionally at the top of every enqueue**,
before the tail was even loaded, so the segment count gated every enqueue rather than only the
ones that link. At `linked_ == bound_ - 1` the queue refused while the tail segment still had
free slots, and a **one-chunk queue held zero items**.

Fixed by giving each policy a `core::AdmitPoint`. `SegmentCount` is now asked at `SegmentLink` —
once the tail has refused and immediately before a segment is acquired — which is the only point
at which a segment-counting policy can answer. The predicate itself was already correct for that
question; only the call site was wrong. As a side effect the ceiling now costs less: at the bound
the proxy skips the acquire/reopen/discard round trip instead of performing and undoing it.

| segment | chunks | `capacity()` | before | after |
| ---: | ---: | ---: | ---: | ---: |
| 8 | 1 | 8 | **0** | 8 |
| 8 | 2 | 16 | 9 | 16 |
| 8 | 4 | 32 | 25 | 32 |
| 8 | 8 | 64 | 57 | 64 |
| 16 | 4 | 64 | 49 | 64 |

Guarded by `AdmissionTest.ChunkBoundedReachesItsStatedCapacity`, which closes the test gap that
hid this: every other case asserted only `placed <= capacity()`, which a bound admitting nothing
satisfies.

### 4.2 `admit::ItemCount::try_admit` can underflow — **[code]**

```cpp
if (ticket - popped_.load(std::memory_order_acquire) >= bound_) {   // Admission.hpp:69
```

Both counters are `uint64_t` and `ticket` is this producer's slot, taken possibly long ago. A
producer that stalls after its `fetch_add` can resume to find `popped_` has overtaken its
ticket; the subtraction wraps to near-`UINT64_MAX`, the comparison succeeds, and the enqueue is
spuriously refused and rolled back. Not a ceiling violation — the caller retries — but a false
"full" under exactly the load where it is least welcome.

### 4.3 `util/timing/` is pre-refactor legacy — **[code]**

`AdditionalWork.hpp` and `TicksWait.h` predate everything else and were never brought along:
`static std::random_device` plus two `thread_local` distributions at namespace scope in a
header (a copy per translation unit), a dead `random_work(double)` overload, an
`asm volatile("nop")` delay loop, and a `.h` extension nothing else uses. Reached only by
`src/bench/main.cpp`.

### 4.4 Two mechanisms for the same instruction — **[code]**

`util/atomic/cas2.hpp` is hand-written `cmpxchg16b` inline asm, used only by `VyukovDCAS`.
`ThreadRegistry` gets the identical instruction from `std::atomic<TaggedPtr>` under `-mcx16`.
One of them should go, and the compiler intrinsic is the one with the ABI guarantees.

### 4.5 Smaller things — **[code]**

- ~~`HAZARD_RETIRE_THRESHOLD` is a `#define`~~ — **fixed**, now
  `mem::source::HazardOpt::retire_threshold<N>`. A `-D` on the command line no longer does
  anything.
- `docs/todo.md` still lists "rework hazard pointers" and "rework unbounded proxy", both long
  since done.
- No `install()` or `export()` targets: the library cannot be consumed by another CMake
  project despite being header-only, which is the easy case.
- No CI. Nothing runs the suites but a person remembering to.

## 5. Missed optimisation opportunities

Ordered by expected value.

1. **No bulk operations.** The fetch-add segments (FAAArray, HQ, PRQ) have exactly the shape
   that makes batching cheap: one `fetch_add` claims *k* consecutive slots, and the per-item
   cost collapses to a store. This is the largest throughput win available, and the trait
   system already provides somewhere to declare it (`segment_traits::supports_bulk`) so
   `LinkedProxy` can fall back to a loop for segments that do not.

2. **Payloads are restricted to a machine word.** `cell::Tagging` encodes to `uintptr_t`, so
   `T` must be a pointer or a small integer — no arbitrary types, no move semantics, no
   `emplace`. `can_store_null` already exposes one corner of this. Lifting it is a genuine
   design fork (a parallel storage array indexed by cell, versus staying word-sized and
   documenting the restriction), and worth deciding deliberately rather than by default.

3. **No backoff anywhere on the contended paths.** `Vyukov`, `PRQ` and `PSCQ` retry their CAS
   loops immediately. `SPIN_HINT()` now exists in `util/specs.hpp` — added this session for
   HQ — and none of them use it.

4. **`Pool::try_advance` scans the whole registry per failed acquire.** O(attached threads) per
   attempt, up to `kMaxAdvances` attempts per `acquire()`, on the path taken precisely when the
   pool is under pressure and threads are numerous. A counter of "threads that published a
   stale stage" would let the common case short-circuit without touching the list.

5. **Magic constants nobody has measured.** Now **declarable** rather than hard-coded — every
   one is an option on its owner's pack (`HQOpt::patience<N>`, `PRQOpt::max_dequeue_retries<N>`,
   `PoolOpt::max_acquire_spins<N>`, …), with the old literal as the default and
   `OptionsTest.OptionDefaults` asserting that nothing was retuned in the move. Each also reads
   back as a public constant, so an instantiation can be asked what tuning it is running.
   **Still unmeasured**, which was the substance of the complaint: the benchmark harness exists
   and none of these has been swept with it. Per-algorithm cell-padding defaults still differ
   between FAAArray (off) and PRQ (on) with no recorded justification.

6. **The benchmark measures one number.** Throughput only — no latency distribution, no
   fairness across producers, no cache-miss or contention counters. `mpmc_bench`'s schema and
   plotting are built around ops/s alone, so adding a second metric is a schema change, not a
   plot change.

   Half-addressed: `LinkedProxy` can now count segments. `ProxyOpt::segment_stats` — off by
   default, and free when off — turns on `segments_linked()`, `segments_retired()` and
   `segments_discarded()`, from which a reuse factor falls out:

   ```cpp
   using Q = proxy::MemBounded<Item, IdxHQ<Item>, 8, meta::EmptyOptions,
                               meta::OptionsPack<proxy::ProxyOpt::segment_stats>>;
   const double reuse = double(q.segments_linked()) / 8.0;
   ```

   The benchmark itself still reports only ops/s; wiring these into the schema is the
   outstanding half.

7. **No NUMA-aware or huge-page allocation.** `ThreadPinner` places threads carefully;
   `SingleBlock::create` then calls plain `std::aligned_alloc`, so segments land on whichever
   node the creating thread happened to be on. On a multi-socket box that silently undoes the
   pinning work for every remote consumer.

8. **No branch hints** on the hot paths, despite the fast/slow split being explicit in several
   algorithms.

## 6. Test gaps

- ~~`AdmissionTest` never asserts a bounded proxy reaches its capacity~~ — **closed**, and it
  immediately caught §4.1. Both `ChunkBounded` and `ItemBounded` are now asserted to reach
  exactly `capacity()`, over several chunk counts including 1.
- **Obligations are not tested as a family.** `SegmentLifecycleTest` is the right place and its
  type list must include every `seg::` alias — `seg::Mutex` was missing, and that is why §4.1's
  sibling bug shipped. A `static_assert` cannot express "closes permanently when full", but a
  typed test can, and every linked segment should be in it by construction.
- `PSCQ`, `LFring`, `VyukovDCAS`, `VyukovNoABA` have no coverage beyond generic conformance.
- No test for `VersionedIndex` version wrap-around, which is the whole point of the type.
- ~~`meta::OptionsPack`'s value-option half had no coverage at all~~ — **closed** by
  `OptionsTest.cpp`, which also pins the defaults so the constants-to-options move cannot
  silently retune anything.
- `ConcurrencyTest` is the only threaded suite and takes minutes, so it gets skipped. A fast
  smoke subset — one shape, 10k items, all 30 entries — would be run.

## 7. Landed this session

Kept here because several are invariants worth not re-breaking.

| area | change | evidence |
| --- | --- | --- |
| `LinkedProxy::enqueue` | take the item back out before discarding a segment that lost the `link_next` race | 200 048 → 200 000 consumed of 200 000 **[measured]** |
| `algo::Mutex` | a linked segment must close itself on full, permanently | ~400 lost per 100 000 → 0, all four families **[measured]** |
| `algo::HQ` | slow path no longer burns a cell when nothing is queued behind the head | worst trial 977/1024 → 1024/1024 **[measured]** |
| `mem::source::Pool` | caller-supplied retry predicate, cache re-check in `take()`, bounded spin and advance budget | — |
| `mem::source::Pool` | three rotation bugs: `repin()` protection hole, `retire()` using the pinned rather than global stage, single-advance give-up | — |
| `registry` | grid completed to 24 linked entries; `TestNames` so failures read `mem_mutex`, not `29` | — |
| build | GoogleTest 11 MB → 1.1 MB with a three-tier fallback; `.gitignore` `cmake/*` → `cmake/extern/`, which had silently excluded `cmake/clangd.in` from the repo | — |
| `meta::OptionsPack` | `ValueOption<K>` so an accepts-list can name an option *template*; the long-unused `get<K, Default>` is now the tuning mechanism | — |
| tunables | eleven hard-coded constants and one `#define` became options on their owner's pack, defaults asserted unchanged | `OptionsTest` |
| `LinkedProxy` | `ProxyOpt::segment_stats`, off by default and zero-size when off | `OptionsTest` |

## 8. Suggested order

1. Fix `admit::SegmentCount` (§4.1) and close the `AdmissionTest` gap that hid it. A queue that
   holds zero items at `chunks=1` is the most serious thing here.
2. Put every `seg::` alias in `SegmentLifecycleTest` by construction, and write the unwritten
   obligations down as cases there (§6).
3. Fix the `ItemCount` underflow (§4.2).
4. Add a fast smoke subset of `ConcurrencyTest`, then CI (§4.5, §6).
5. Bulk operations (§5.1) — the real performance work, and worth doing on a tree whose
   correctness is pinned by then.
6. Sweep the magic constants with the harness that already exists (§5.5).
