
This document stores benchmark insights needed to evaluate and compare various concurrent queue implementations. It includes brief descriptions of each queue, focusing on their progress guarantees, ABA-safety mechanisms, contention avoidance strategies, and specific trade-offs.

## Overview of Implementations

Some queues are stand-alone (lock-less, lock-free), while others require an external mechanism for lock-freedom, such as a LinkedProxy (e.g., MS-Queue).

### PRQ (Portable Ring Queue)
Portable Ring Queue is an obstruction-free bounded buffer with FIFO semantics. It borrows heavily from CRQ (Morrison & Afek 2015), using `fetch_add` (FAA) to manage the increment of the two authoritative monotonic counters for enqueue and dequeue. 
It employs a 3-step transaction that emulates CAS2 (unsupported in most RISC architectures) to lock and update a 128-bit cell packing a `value` field and a `seq` field. The `seq` field manages ABA safety in enqueue-dequeue transactions. 
Using FAA eliminates usual CAS hot-spots, but may cause an incoherent transaction regarding a dequeue on an empty queue. Since FAA cannot validate if the head counter has surpassed the tail counter, the tail counter must be advanced (possibly wasting an empty slot). This can lead to livelock if empty dequeues are frequent. To avoid this, enqueue operations stop looking for an empty slot after a starvation threshold ($2n$) and signal to all enqueuers that the buffer is permanently closed (though it stays open for drain operations). Thus, it needs to be coupled with a linked proxy to provide true lock-freedom.
*   **Bounded?**: Yes — the ring is bounded. Unboundedness comes from the proxy, not from PRQ.
*   **ABA-Safety**: Yes (via 3-step CAS2 transaction on 128-bit cell)
*   **Progress Guarantee**: Obstruction-free (P-C clash possible on frequent empty dequeues)
*   **Contention Avoidance**: FAA for monotonic indexes (prevents P-P/C-C clashes); unsafe enqueue prevents P-C clashes.
*   **Magic Constants**: `PRQOpt::max_dequeue_retries` (default **4096**) and
    `PRQOpt::tail_reload_period` (default **256**). Note these are fixed counts in *this*
    implementation, not the $2n$ of the paper — a segment's starvation bound is therefore
    independent of its capacity, which matters when sweeping the size axis.
*   **Value Type**: Valid pointer type (64-bit). Tagged through `cell::MsbTag`, so the reserved
    encodings live in the **top** bit, not the bottom. (`cell::LowTag` — the low-bit scheme — is
    what FAAArray and HQ use.)

### LFRing (Lock-Free Ring Queue)
Borrows from CRQ (Afek & Morrison 2015) but handles CAS2 non-universal support using single-word CAS operations and bit-packing (32-bit `seq` and `value` into a 64-bit cell). Uses an `eq_threshold` to limit empty dequeue attempts, achieving lock-freedom. The threshold is initially set to $4n$; empty dequeues decrement it, successful enqueues reset it. If negative, the operation aborts. To linearize the empty check, it needs $2n$ cells for $n$ items, resulting in an unlinearizable full check (full state may hold $n$ to $2n-1$ items). Can be composed with a linked-proxy for unbounded/chunked forms.
*   **ABA-Safety**: Yes (bit-packing 32-bit seq and value, single-word CAS)
*   **Progress Guarantee**: Lock-free
*   **Contention Avoidance**: FAA for monotonic indexes; unsafe enqueue to prevent P-C clashes.
*   **Magic Constants**: `LFringOpt::max_dequeue_retries` (default **10240**) and
    `tail_reload_period` (**256**). The $4n$ of the paper is a conservative upper bound; the
    implementation uses a fixed count.
*   **Value Type**: 32-bit unsigned type.
*   **Not linkable, by design** — its size is an *order* ($\log_2$), so a non-power-of-two is not
    expressible. It is the substrate for SCQ rather than a segment in its own right.

### SCQ (Scalable Circular Queue)
Built using two LFRing queues, resulting in a non-blocking arbitrary-type queue. Uses `free_slots` and `data_items` LFrings to hold slot indices (`uint32_t`) for an underlying contiguous linear buffer. 
Enqueue: 1) Get slot from `free_slots`, 2) Copy/move item to buffer, 3) Enqueue slot to `data_items`.
Dequeue is symmetrical. Not strictly lock-free due to the 3-step operations (potential priority inversion), but livelock-free assuming crash-free threads.

**Footprint, exactly.** Two index rings, each `2n` cells (LFRing's rule), each cell padded to
`CACHE_LINE` (128 B here): $2 \times 2n \times 128 = 512n$ bytes, plus the payload buffer
$n \cdot \texttt{sizeof(T)}$ — so **~520 B per item** for pointer payloads. That is the default,
**not** an inherent property: `SCQOpt::no_cell_padding` propagates through `RingOpt` to the two
rings and collapses them to 8 B per cell, i.e. ~40 B/item. Worth benchmarking both, since the
padded/unpadded pair isolates false sharing from the algorithm.
*   **ABA-Safety**: Yes (lock-free composition of LFRing)
*   **Progress Guarantee**: Livelock-free (assuming crash-free threads).
*   **Contention Avoidance**: Same as LFRing.
*   **Magic Constants**: No.
*   **Value Type**: 32-bit unsigned type (for slots), arbitrary for actual buffer.

### PSCQ
Combines PRQ (for ABA avoidance and 64-bit items) and LFRing (`eq_threshold` for lock-free bounding). Substitutes bit-packing with PRQ's CAS2 emulation. Maintains similar size to LFRing due to cache-line padding.
*   **ABA-Safety**: Yes (PRQ composition).
*   **Progress Guarantee**: Lock-free (LFRing composition).
*   **Contention Avoidance**: Same as PRQ.
*   **Magic Constants**: No.
*   **Value Type**: Same as PRQ.

### Vyukov
Implements the Vyukov Buffer policy. Uses two authoritative counters incremented via CAS-retry loops. Handles ABA via classic 64-bit `seq` tagged cells. Threads increment a monotonic counter to book a cell, write the value, then atomically update the `seq` field. This 3-step transition can create priority inversion.
*   **ABA-Safety**: Yes (64-bit seq tagged cells).
*   **Progress Guarantee**: Livelock-free (assuming crash-free threads).
*   **Contention Avoidance**: Poor (CAS-retry loop to get a unique slot).
*   **Magic Constants**: None.
*   **Value Type**: Arbitrary.
*   **Open question**: it reserves the ticket and *then* writes the cell and sequence — two steps
    — yet declares `needs_inflight_drain = false` on the grounds that "a single atomic step
    publishes the item". That is not literally true. No harness here has produced a loss for it,
    so it is left alone, but it is the first place to look if one ever appears. See
    `docs/notes/As Shipped.md`.

### Vyukov-DCAS
Makes Vyukov lock-free using the x86 `CMPXCHG16B` instruction, writing value + seq as one
indivisible 128-bit update.

**Reserves the tail ticket before that double-CAS.** Previously the double-CAS decided success
while the tail CAS was best-effort, which meant `close()` invalidated nothing and a producer
could publish into a segment the proxy had already drained and retired — measured losing items in
**12 of 25 trials** at 4P/4C on 16-slot segments. Reserving fixes the close; `needs_inflight_drain`
covers the remaining window between reserving and publishing.
*   **ABA-Safety**: Yes (via `CMPXCHG16B` - **NOT PORTABLE**).
*   **Progress Guarantee**: Lock-free.
*   **Contention Avoidance**: Poor.
*   **Magic Constants**: No.
*   **Value Type**: 64-bit unsigned pointer type.

### Vyukov-NO-ABA
Attempts a lock-free, portable Vyukov by leveraging the absence of ABA (e.g., via LL/SC on RISC) or assuming unique data. The lap number is folded into the empty cell itself, so a cell is one word rather than a value/seq pair — that is the thing this variant exists to measure.

**Now uses a mandatory tail reservation.** It previously advanced the tail even when the cell CAS
*failed*, so a ticket could be skipped and its cell left unfilled behind a head that had already
moved past it. Reserving closes both that hole and the advisory-close hole. The cost is that its
commit protocol is now essentially Vyukov's; what stays distinctive is the single-word cell.
**Numbers recorded before this change are void.**
*   **ABA-Safety**: Assumed.
*   **Progress Guarantee**: Lock-free.
*   **Contention Avoidance**: Poor.
*   **Magic Constants**: No.
*   **Value Type**: 64-bit unsigned pointer type.

### CacheRing
Useful for static memory pools. Essentially implements Vyukov-DCAS policy but substitutes DCAS with single CAS via bit-packing.
*   **ABA-Safety**: Yes (version indexing).
*   **Progress Guarantee**: Lock-free.
*   **Contention Avoidance**: Poor.
*   **Magic Constants**: No.
*   **Value Type**: 32-bit unsigned types.

### Phased Bucket
Wait-free bounded FIFO queue for explicit phases (MP-0C or 0P-MC). Since producers and consumers never run concurrently, both authoritative counters are packed in a single word. Uses FAA for increments and features an implicit reset mechanism.
*   **ABA-Safety**: Yes (due to strict phase barriers; buffer is not circular).
*   **Progress Guarantee**: Wait-free.
*   **Contention Avoidance**: Yes.
*   **Magic Constants**: No.
*   **Value Type**: Templated (typically 32-bit uint, but arbitrary works).

### Mutex
The blocking control. One mutex, one ring, and **two** condition variables (`not_empty_`,
`not_full_`) so that a producer waking for room does not also wake every consumer waiting for
work.

*   **The waits are unbounded, and `close()` is what ends them.** It sets the flag under the lock
    — it is part of both predicates — then notifies *all* waiters on both variables. After a
    close the queue still drains: `dequeue()` degrades to `try_dequeue()` and hands back what is
    left.
*   **The API splits blocking from non-blocking**: `enqueue`/`dequeue` may wait;
    `try_enqueue`/`try_dequeue` never do. Generic code must use the `try_` pair, since it is in
    no position to decide when to close. Every other algorithm implements both as the same
    function.
*   Blocking applies to the **standalone** queue only. As a linked segment it forwards to the
    try-versions, because the proxy reads a refusal on full as "link a successor" — parking there
    would wait for room the segment is never going to have.
*   **Magic Constants**: `MutexOpt::notify_all` (opt-in; `notify_one` is the default).

### Spin
The second lock-based control, and the other half of one experiment: same ring, same critical
sections as Mutex, but a lock that **never waits on the queue's state — only on the lock itself**.
`enqueue`/`dequeue` refuse immediately when full or empty, so whatever Spin differs from Mutex by
is attributable to blocking policy, and whatever it differs by from a lock-free ring is
attributable to lock acquisition.

*   Bounded spin, then park on `std::atomic::wait`. There is deliberately **no** contended-state
    tracking, so every release notifies; the critical section is a few instructions, so the wider
    state machine cost more on every acquire than the notify saves.
*   **The closed flag is the MSB of the lock word.** One word holds both, which means every
    update must be `fetch_or`/`fetch_and` — an exchange or a plain store would clobber the flag.
*   **Magic Constants**: `SpinOpt::spins_before_park` (default **64**; `0` parks immediately,
    making it a pure futex).

### FAAArrayQueue
Logically unbounded array (linear buffers) that **sidesteps ABA entirely** rather than defending
against it: a cell is written once and never reused, so there is no recycled state to be confused
about. Uses FAA to obtain unique cells and `EMPTY`/`SEEN` tags. Memory efficient — cells are
packed, not padded (`force_cell_padding` is off by default), which a linear buffer can afford
because adjacent cells are rarely contended by the same pair of threads. Requires constant buffer
provisioning.

Note for the size axis: FAAArray and HQ now round capacity up to a power of two like everything
else, but for them it is purely a **sizing** choice — they walk their cells linearly and close, so
there is no wrap and no mask to gain. Previously they took the request verbatim.
*   **ABA-Safety**: Yes (cells never reused).
*   **Progress Guarantee**: Lock-free (assuming MS-linkage).
*   **Contention Avoidance**: Yes (FAA on indexes).
*   **Magic Constants**: `FAAArrayOpt::patience` (default **1024**) — loads a consumer spends
    waiting for a straggling producer before invalidating the slot. (`SPIN_HINT` is the pause
    intrinsic, not the tunable.)
*   **Value Type**: Architecture pointer type, tagged through `cell::LowTag`.

### HybridQueue
Improves FAAArrayQueue with backpressure (`slowDequeue`/`fastDequeue`). If a buffer's `next` pointer is set, it stops accepting enqueues and switches to `fastDequeue`, reducing the invalidation upper bound to $t$ (thread count) instead of $n$ (capacity). `slowDequeue` uses the Vyukov-NO-ABA protocol, preventing automatic cell invalidation on empty queues and introducing backpressure (consumers clash, distancing from producers).
*   **ABA-Safety**: Yes (cells never reused).
*   **Progress Guarantee**: Lock-free (assuming MS-linkage).
*   **Contention Avoidance**: Yes.
*   **Magic Constants**: `HQOpt::patience` (default **1024**), same meaning as FAAArray's — which
    is what makes the untuned-vs-untuned comparison in the methodology section a fair one.
*   **Value Type**: Architecture pointer type, tagged through `cell::LowTag`.

## Summary Matrix

| Queue Family      | Progress Guarantee | ABA Strategy                   | Primary Trade-off / Bottleneck                             |
| :---------------- | :----------------- | :----------------------------- | :--------------------------------------------------------- |
| **PRQ**           | Obstruction-free   | 128-bit cell (CAS2 emulated)   | P-C clashes on frequent empty dequeues                     |
| **LFRing**        | Lock-free          | Bitpacking (32-bit seq+value)  | Needs $2n$ cells for $n$ items; size is an *order*         |
| **SCQ**           | Livelock-free      | Lock-free composition          | ~520 B/item **padded** — but see the footprint note        |
| **PSCQ**          | Lock-free          | PRQ cell + SCQ threshold       | 3-step insert, so a cell can be seen mid-write             |
| **Vyukov**        | Livelock-free      | 64-bit `seq` tagged cells      | CAS-retry loop for a slot; poor contention behaviour       |
| **Vyukov-DCAS**   | Lock-free          | `CMPXCHG16B`                   | Zero portability outside x86                               |
| **Vyukov-NO-ABA** | Lock-free          | Single-word cell, lap in-cell  | ABA-freedom *assumed*, not enforced                        |
| **CacheRing**     | Lock-free          | Version indexing               | Internal to the pooled source; 32-bit values only          |
| **Phased Bucket** | Wait-free          | Epoch strict barrier           | Extremely narrow use case (MP-0C / 0P-MC)                  |
| **FAAArrayQueue** | Lock-free          | Infinite linear buffer         | Cells never reused, so it needs constant provisioning      |
| **HybridQueue**   | Lock-free          | Infinite buffer + backpressure | Same, with the invalidation bound cut from $n$ to $t$      |
| **Mutex**         | Blocking           | N/A (mutual exclusion)         | The control: two CVs, unbounded waits, `close()` to end    |
| **Spin**          | Blocking (lock)    | N/A (mutual exclusion)         | The other control: lock cost only, never waits on state    |

## Capabilities — what composes with what

Machine-checked from each header's `core::segment_traits` specialisation, which stays the source
of truth; this table is a snapshot and will drift.

| Algorithm | Linkable | close hint | dequeue prepare | inflight drain | recyclable | null payload |
| :-------- | :------: | :--------: | :-------------: | :------------: | :--------: | :----------: |
| Vyukov        | yes | – | – | – | yes | yes |
| PRQ           | yes | **yes** | – | – | yes | tag-dependent |
| FAAArray      | yes | – | – | – | yes | tag-dependent |
| HQ            | yes | – | – | – | yes | tag-dependent |
| SCQ           | yes | **yes** | **yes** | – | yes | yes |
| PSCQ          | yes | **yes** | **yes** | **yes** | yes | tag-dependent |
| VyukovDCAS    | yes | – | – | **yes** | yes | **no** |
| VyukovNoABA   | yes | – | – | **yes** | yes | yes |
| Mutex         | yes | – | – | – | yes | yes |
| Spin          | yes | – | – | – | yes | yes |
| LFring        | **no** | — | — | — | — | — |
| CacheRing     | **no** | — | — | — | — | — |
| PhasedBucket  | **no** | — | — | — | — | — |

Reading the flags, because they predict result shapes:

- **close hint** — the proxy tells the segment "you are probably closed" so it does not re-enter
  its enqueue loop. PRQ needs it or a bounded proxy livelocks; SCQ and PSCQ inherit the same
  unsafe-cell path.
- **dequeue prepare** — an empty dequeue *spends* the threshold, so a segment about to be re-read
  after a link needs it restored. SCQ and PSCQ only.
- **inflight drain** — a producer can be part-way through an insert, so a segment must not be
  retired while one is outstanding. True for the three reserve-then-publish rings.
- **recyclable** — required by the pooled source. All of them are; note **PSCQ's `reopen()` is
  O(ring)** while every other segment realigns indices in O(1), which should show up as pooled
  PSCQ degrading under high segment turnover.

**The three non-linkable ones are deliberate.** `LFring` stores its size as an order, so a
non-power-of-two is not expressible; `CacheRing` and `PhasedBucket` are internals of the pooled
source (its reuse cache and its phased rotation) rather than queues in their own right. Everything
else is a segment in all four proxy families, which is why the registry is **47 entries**:
13 standalone-capable algorithms, of which 10 appear as `u-` / `item-` / `chunk-` / `mem-`.

# Benchmark Methodology

> Everything below the catalogue was written while fixing this tree, and the numbers are from
> **this machine** (4-core / 8-thread i5-1155G7, 400–2500 MHz, `powersave` governor). They are
> properties of the measurements, not of the algorithms. Re-derive them elsewhere.
>
> The behavioural claims in §5 are the benchmark-facing view of `docs/notes/As Shipped.md`,
> which carries the reasoning and the code they came from. If the two disagree, that one is the
> record and this is stale.

## 0. Measurement discipline — read this before trusting any number

The single most expensive lesson of the last session: on this box **the same binary measured
2.58–6.75 M/s across runs**. Anything under roughly 15% is not a result here, it is the clock.

**Warm up.** Burn all cores for ~3 s before timing. Frequency ramp otherwise lands inside the
first measurements of every process.

**Interleave repetitions across variants, never all reps of one config then the next.** Running
A's nine reps before B's hands B the entire ramp. This *fabricated a 2.3× win* for
`mem-faa pool=32` over `chunk-faa`, which disappeared completely once reps were rotated
A,B,C,A,B,C. `python/mpmc_bench/runner.py` currently loops repetitions innermost
(`for _ in range(exp.repetitions)` inside the config loop), so it has exactly this bias today.

**Median, not mean.** The runner records `Throughput_Mean` / `Throughput_StdDev`. With ramp
outliers the mean is the wrong estimator — keep every sample and report median, min, max.

**Prefer counters to the wall clock.** Counters do not care about frequency. The pooled-source
investigation was settled entirely by them (registry scans 433,589 → 16,955 per 2M items) and
*never* by a timing: the wall clock never moved outside the noise band, before or after. If a
question can be posed as a count, pose it that way.

**Compare as ratios inside one process**, against a control measured in the same run, so drift
cancels rather than accumulating across a sweep.

**Record the environment with the row**: governor, `registry::kPoolSize`, the RNG seed, and the
capacity the queue *reports* — not the one requested. They differ; see §5.

## 1. A correctness gate comes first

The harness will happily print a throughput for a run that lost items. This session found real
item loss in three segments (PSCQ, VyukovNoABA, VyukovDCAS — all silently passing throughput
runs), so before the campaign:

- the bench binary asserts `produced == consumed` and **fails the run** rather than emitting a
  number;
- the runner records that failure as a status, the way it already does for `TIMEOUT` and
  `BAD_OUTPUT`.

A benchmark that cannot distinguish "fast" from "wrong" is a CSV, not a measurement.

## 2. Instrumentation, and how to add it

### Wasted cells — FAAArray vs HybridQueue

The open question in these notes: how many cells does an enqueue burn stepping over `SEEN`
cells, tuned versus untuned.

**Do not change what `enqueue` returns.** It is fixed by `core::Queue` across all 47 registry
entries. Follow the idiom already in the tree for exactly this problem —
`proxy::ProxyOpt::segment_stats` with `detail::SegmentStatsOn` / `SegmentStatsOff` in
`include/proxy/LinkedProxy.hpp`:

1. a gated tag per algorithm, `FAAArrayOpt::count_wasted` and `HQOpt::count_wasted`;
2. a relaxed counter bumped where the enqueue loop skips an occupied cell;
3. summed into the proxy when the segment retires, exposed like `segments_linked()`;
4. **off by default**, so throughput runs pay nothing.

The comparison worth making is *untuned* FAAArray against *untuned* HQ, since HQ's whole claim is
that backpressure bounds the invalidation at `t` (threads) rather than `n` (capacity). That
predicts wasted-cells-per-item should be roughly flat in capacity for HQ and grow for FAAArray —
a falsifiable shape, which is better than a throughput delta.

### Memory pressure — segments and cells per item

`segments_linked() / segments_retired() / segments_discarded()` already exist on `LinkedProxy`,
but they are gated behind `ProxyOpt::segment_stats` and **no registry entry enables them**. Add a
parallel instrumented entry list rather than switching them on globally.

Then:

```
cells per item = segments_linked × per-segment capacity ÷ items
```

which is the efficiency metric these notes ask for. PRQ and an untuned FAAArray are expected to
be the worst here, because both use *linking a fresh segment* as their progress guarantee — under
oversubscription that turns into an allocation rate.

### One hard rule

**Never measure counters and throughput in the same run.** The counters are atomics on the hot
path; instrumentation perturbs the thing being measured. Two passes over the same grid.

## 3. Output format

`src/bench/main.cpp` prints one bare number and `runner.py` parses it with
`float(res.stdout.strip())`. Keep that as the default and add a `--metrics` mode emitting
`key=value` lines. New measurements (wasted cells, segments, refusals) then extend the schema
without breaking the tooling or invalidating CSVs already on disk.

## 4. Dynamic workload and dynamic threading

Today producers get a fixed `items / producers` slice and every thread runs start to finish.
Neither pathology in these notes reproduces under that shape.

- **Chunk stealing**: replace the fixed split with a shared `std::atomic<uint64_t>` and
  `fetch_add(chunk)`. Small chunks make the tail of the run ragged, which is what produces
  stragglers.
- **Dynamic threading**: a thread probabilistically releases its session and rejoins later. The
  proxy supports arbitrary thread counts through `join()`, and `ThreadRegistry` recycles the
  node — so this also exercises the detach path, which the fixed-thread benchmark never touches.
- **Seed the RNG per run and put the seed in the CSV.** A chaotic run that finds something and
  cannot be replayed is a rumour, not a result.

Simulated work already exists and is calibrated: `Delay{ns, amplitude}` →
`TickConverter` (`python/mpmc_bench/ticks.py`) → `util::timing::randint(center, amplitude)`.
Calibration converts nanoseconds to spin counts *per machine*, is cached, and fails loudly rather
than silently returning zero — so a delayed experiment cannot quietly become an undelayed one.

## 5. Things to look for — live caveats before the campaign

**`algo::Mutex`'s condition-variable path is currently not measured.** *(open, as of the blocking
rewrite)* The API now splits `enqueue`/`dequeue` (may block) from `try_enqueue`/`try_dequeue`
(never blocks). Generic code — including the benchmark's consumer loop — must use the `try_`
pair, because the blocking `dequeue()` only returns once somebody calls `close()`, and the
registry surface has no generic close. So the two condition variables are, right now, dead code
in the sweep. Resolving it needs either a close on the generic surface or a dedicated
non-generic run for the lock-based controls. **Decide this before the campaign, not after.**

**`PSCQ` accepts twice the capacity it advertises.** A known, documented deviation (see
*Known deviation: PSCQ does not honour its own capacity()* in `As Shipped.md`): its fullness
check tests the physical ring while `capacity()` reports half of it. `pscq` at "size 1024" is
really running at 2048, so it must not share a size axis with anything else. It is also only
verified correct *up to* its advertised capacity — whether the extra headroom preserves FIFO is
untested, which is a second reason not to lean on it.

**`VyukovNoABA`'s commit protocol changed** to a mandatory tail reservation (it previously
advanced the tail even when the cell CAS failed, which could strand a cell). Any earlier numbers
for it are void.

**Capacity finally means one thing.** Every algorithm now rounds up to a power of two, and
`kPoolSize` drives the `item-`, `chunk-` and `mem-` families alike, so a cross-family comparison
is comparing algorithms rather than geometries. Numbers for `vyukov`, `prq`, `faa`, `hq` and
`mutex` recorded before that change are not comparable.

**Oversubscription is where the interesting behaviour is.** Measured: `mem` ≈ `chunk` at 2P/2C,
but ~2× apart at 4P/4C on four physical cores. Thread shapes should deliberately straddle the
physical core count — the difference between 4 and 8 threads here is qualitative, not a scaling
curve.

**`Spin` vs `Mutex` isolates lock acquisition from blocking policy.** Same ring, same critical
sections; `Spin` never waits on queue state, only on the lock. Read the pair as one experiment,
and note that `Spin` currently notifies on every release (no contended-state tracking) — whether
that costs anything is itself worth one measurement.

**Counters worth collecting per item**, all frequency-independent and all more trustworthy than
a throughput delta on this hardware:

| counter | answers |
| --- | --- |
| refusals / item | how much work a bounded policy wastes at the ceiling |
| segments / item | allocation pressure; PRQ and untuned FAAArray should stand out |
| wasted cells / item | the FAAArray vs HQ question directly |
| `try_advance` scans / item | pooled-source contention (was ~61 per successful acquire) |

## 6. Sweep budget

47 registry entries. A full grid — 47 queues × 4 sizes × 5 thread shapes × 2 delay profiles × 5
reps — is ~9,400 runs and hours on this box, before the second instrumented pass.

Suggested tiering:

- **Tier A, headline**: one representative per family (`u-`, `item-`, `chunk-`, `mem-`) × the
  interesting algorithms, at 2 sizes and 3 thread shapes. Small enough to re-run often.
- **Tier B, full grid**: everything, run once, as the appendix.
- **Tier C, targeted**: the specific questions above (wasted cells, segments per item), which
  need the instrumented build and only a handful of entries.

---

# Benchmark Methodology (Insights)

## PSCQ Validation Comparisons
PSCQ merges PRQ and SCQ logic to produce a lock-free, bounded, directed FIFO queue. To comprehensively validate its performance, the testing methodology evaluates PSCQ against PRQ, SCQ, and other directed lock-free bounded FIFO queues.

Because these queues have differing architectural guarantees, the comparisons are split into distinct testing categories:
- **Linked Comparison (vs. PRQ):** PRQ is obstruction-free and requires an external source for guarantees, making it lighter weight than PSCQ, which adds complexity to achieve lock-freedom.
    - _Expectations:_ PSCQ should demonstrate significant performance improvements in oversubscribed scenarios and on NUMA machines (PRQ's pathological case). In other scenarios, performance should be comparable, with slight degradation acceptable.
- **Bounded Comparison (vs. SCQ):** SCQ is livelock-free (relying on two LFRings for index management of an underlying buffer) and has a massive memory footprint ($513n$ bytes compared to PSCQ's $128n$ bytes).
    - _Expectations:_ PSCQ should outperform SCQ in over-producer scenarios due to SCQ suffering from additional cache interference during copies to the unpadded underlying buffer. SCQ is also expected to degrade severely on high core-count NUMA machines due to its memory footprint. PSCQ may show slight degradation in balanced and oversubscribed scenarios due to its higher directed threshold.
- **CAS-Loop Lock-Free (vs. Vyukov-NO-ABA):** Tested using a tailored workload restricted to non-repeating 64-bit data types, as Vyukov-NO-ABA strictly requires ABA freedom.
- **CAS1 vs. CAS2 (vs. LFRing):** Compares single-width CAS against CAS2 emulation. While LFRing only accepts 32-bit data types, it shares the same physical size and threshold values as PSCQ.
**Implementation Constraint:** Because PSCQ reserves the most significant bit (MSB) of enqueued pointers, validation tests must exclusively enqueue even pointer types to ensure fair cross-queue compatibility.
## HybridQueue Validation
HybridQueue (HQ) introduces an improved heuristic over FAAArrayQueue. Both are unbounded linked queues that avoid the ABA problem by abandoning circular buffers. This reduces the memory footprint of individual segments but requires more frequent memory allocation.
In FAAArrayQueue, buffers are obstruction-free; consumers unconditionally examine cells. If a cell is empty, producers are destructively preempted from enqueuing there. While this performs well in static, balanced workloads, it causes severe performance degradation in oversubscribed scenarios. FAAArrayQueue attempts to mitigate this with a spin-loop backoff, but this heuristic remains suboptimal, often invalidating entire buffers, wasting slots, and adding heavy contention to the memory allocator during linking retries.

HybridQueue minimizes this waste by implementing two dequeue policies to manage producer-consumer cross-contention:
- **`slowDequeue`:** Engaged when no subsequent segment is linked. It uses a CAS-loop extraction that adds consumer-side contention, buying inflight producers time to enqueue. This prevents consumers from invalidating empty cells and handles the FAAArray backoff schema natively.
- **`fastDequeue`:** Engaged once a new buffer is linked. It mirrors FAAArrayQueue's dequeue logic but removes the spin-backoff.
This dual-policy approach lowers the upper bound of wasted cells per segment from $O(n)$ to $O(p-1)$, where $p$ is the number of inflight producers still processing the current segment.
**Testing Objectives:**
- **Cell Waste Impact:** Measure empirical throughput of FAAArrayQueue with and without spin-backoff. We expect pathological failures without backoff, particularly regarding throughput on NUMA hardware and in oversubscribed environments.
- **Contention Overhead:** Quantify how wasted cells translate into decreased throughput by measuring the increase in active and wasted segments, which forces producers to link new segments more frequently.
- **HybridQueue Performance:** Validate that reducing wasted cells improves both memory footprint and throughput. HQ should match FAAArrayQueue (with backoff) in balanced scenarios, but significantly outperform it when backoff is disabled and in oversubscribed scenarios.
- **Backoff Resilience:** Conduct a grid-search on static throughput across various backoff values to isolate how backoff tuning affects both queues and to test HQ's resilience.
## The HQ campaign, as configured for 64 physical cores

Four experiment files under `python/experiments/`, **3,760 runs** total. Split so they can be run
and re-run independently; `hq64_ratio` is the cheap one to start with.

| config | grid | runs | answers |
| --- | --- | ---: | --- |
| `hq64_balanced.json` | 12 queues x 3 sizes x 9 balanced shapes | 1620 | does HQ match FAA-with-backoff when balanced, and beat it without |
| `hq64_unbalanced.json` | 12 x 2 sizes x 12 shapes (2:1 and 1:2) | 1440 | over-producer and over-consumer, deliberately no worse than 2:1 |
| `hq64_ratio.json` | 5 queues x 5 ratios **at exactly 64 threads** x pinned/unpinned | 250 | the P:C ratio in isolation, with the core count held constant |
| `hq64_work.json` | 6 queues x 5 shapes x 3 work levels | 450 | whether the waste survives realistic per-operation work |

### Why these shapes

**Balanced** runs `(1,1) ... (64,64)`, so the total thread count is 2, 4, 8, 16, 32, 48, **64**,
96, 128. That deliberately straddles the machine: `(32,32)` fills all 64 cores exactly, and
`(48,48)` and `(64,64)` are 1.5x and 2x oversubscribed. Oversubscription is where FAAArray's
destructive preemption is supposed to become pathological, so the interesting rows are the last
two.

**Unbalanced** stays at 2:1 in both directions (`(32,16)`, `(16,32)`, and the rest), which is
"unbalanced" without becoming a different experiment. `(42,21)` and `(21,42)` are the 2:1 points
that land at 63 threads, i.e. the machine essentially full.

**`hq64_ratio` holds the total at 64** and sweeps only the ratio — `(16,48)`, `(21,42)`,
`(32,32)`, `(42,21)`, `(48,16)`. Everything else changes two variables at once; this changes one.
It also runs pinned and unpinned, which is the cheapest way to find out how much of any effect is
placement rather than algorithm.

### Reading the entry names

`patience` defaults to **1024** for both algorithms, so `u-faa-p1024` and `u-hq-p1024` *are* the
stock FAAArray and HQ — instrumented, but otherwise the registry defaults. `-p0` is the
no-backoff case the notes predict will fail. That makes the two comparisons the campaign exists
for read directly off the grid:

- **`u-hq-p0` vs `u-faa-p0`** — HQ's dual-policy dequeue against FAAArray with its mitigation
  removed. Measured locally at 4P/4C: 11.73 vs 4.41 M/s, and 196 vs 1311 segments.
- **`u-hq-p1024` vs `u-faa-p1024`** — both tuned. The notes predict parity here.

### Two practical notes

**Pinning is safe when oversubscribed.** `ThreadPinner` wraps with `cores_[i % cores_.size()]`,
so `(64,64)` on 64 cores places two threads per core deterministically rather than failing.
Generate the topology file first (`mpmc-topology`) so the core order is the machine's, not
ascending logical ids.

**Instrumentation is on for every entry in the tuning registry, and that is fine here.** The
earlier rule in this document — never measure counters and throughput in the same run — was
written for hypothetical *per-cell* counters. `segment_stats` increments once per segment
**link** (`LinkedProxy` lines 294/429/442/516), i.e. once per ~1024 items at these sizes. The
cost is not measurable; a per-cell counter would have been.

### What to expect — a canary run, on 4 cores

`hq64_canary.json` (20 points, 3 reps) exists to prove the pipeline before committing hours. Run
at 200k items on a **4-core** laptop — so heavily oversubscribed, and *not* a substitute for the
real thing — it already separates the two algorithms cleanly. All 20 points passed the
`produced == consumed` gate.

| queue | 8P/8C | 32P/32C | 64P/64C | **32P/16C** | 16P/32C |
| --- | ---: | ---: | ---: | ---: | ---: |
| `u-faa-p0` eta | 0.997 | 0.997 | **0.150** | **0.040** | **0.091** |
| `u-faa-p0` M/s | 12.6 | 14.2 | 5.1 | **0.75** | 0.76 |
| `u-hq-p0` eta | 0.997 | 0.997 | **0.997** | **0.997** | **0.997** |
| `u-hq-p0` M/s | 12.0 | 12.1 | 12.2 | **12.8** | 6.5 |

Three things worth carrying into the real campaign:

**eta = 0.9965 is the floor**, not a good score: 200k items in 1024-slot segments needs
ceil(200000/1024) = 196 segments, and 200000/(196 x 1024) = 0.9965. HQ hits the theoretical
optimum at *every* shape. FAAArray without backoff reaches 4906 segments where 196 would do.

**The worst case is unbalanced, not oversubscribed.** `u-faa-p0` is fine at 32P/32C and degrades
at 64P/64C, but its collapse is at **32P/16C** — eta 0.040, 17x slower than HQ at the same shape.
Over-producer with too few consumers is where a consumer's destructive preemption compounds:
every invalidated cell forces another link, and the producers that caused it are still arriving.
This is the single strongest argument for `hq64_unbalanced.json` existing, and the reason the 2:1
shapes are not an afterthought.

**Backoff rescues FAAArray but does not fully close the gap**: `u-faa-p1024` holds eta ~= 0.99
almost everywhere, but dips to 0.948 at 16P/32C — the over-consumer case, where there are more
threads able to invalidate a cell than to fill it.

These are 4-core numbers. On 64 cores the shapes stop being oversubscribed until `(48,48)`, so
expect the FAAArray collapse to move rightward and the balanced rows to look far healthier; the
unbalanced rows are the ones to watch.

### Running it

```bash
cd python
python3 -m mpmc_bench experiments/hq64_canary.json     --build-dir ../build   # minutes; proves the pipeline
python3 -m mpmc_bench experiments/hq64_ratio.json      --build-dir ../build   # then the cheap real one
python3 -m mpmc_bench experiments/hq64_balanced.json   --build-dir ../build
python3 -m mpmc_bench experiments/hq64_unbalanced.json --build-dir ../build
python3 -m mpmc_bench experiments/hq64_work.json       --build-dir ../build

python3 -m mpmc_bench.plotting.plots experiments/hq64_balanced.csv \
        --kind slot-efficiency --save hq64_efficiency.png
python3 -m mpmc_bench.plotting.plots experiments/hq64_balanced.csv \
        --kind backoff-grid --save hq64_backoff.png
```

Add `--dry-run` to print the grid without measuring. Every run is gated on
`produced == consumed`, so a lost item shows up as `LOST_ITEMS` in the `Status` column rather
than as a plausible throughput.

## Slot Efficiency Metrics
To empirically measure overall queue efficiency and wasted slots during static workloads, the following formulas will be applied. Let $i$ represent the total items transferred, $S$ represent the total active segments used, and $n$ represent the number of slots per segment:
**Total Wasted Slots:**
$$W_{\text{total}} = (S \cdot n) - i$$

**Average Wasted Slots per Segment:**
$$W_{\text{avg}} = \frac{W_{\text{total}}}{S} = n - \frac{i}{S}$$
**Overall Slot Efficiency:**
$$\eta_{\text{slot}} = \frac{i}{S \cdot n} = 1 - \frac{W_{\text{avg}}}{n}$$

