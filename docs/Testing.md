\page testing Testing

What the suites cover, how to run them properly, and the techniques that actually found the
concurrency bugs in this tree.

---

## The suites

Twelve GoogleTest binaries, registered in the `UNIT_TESTS` list in `CMakeLists.txt`. Adding a
name there is all it takes; the file is `src/test/unit/<Name>.cpp`.

| suite | pins down | threaded |
| --- | --- | :-: |
| `RegistryConformanceTest` | every registered implementation satisfies its contract and behaves sequentially | |
| `SegmentLifecycleTest` | `close` / `is_closed` / `reopen` / `next`, directly on each segment type | |
| `TaggingTest` | the cell tagging policies that replaced three hand-rolled sentinel schemes | |
| `AdmissionTest` | the admission policies and the bounds they promise | ✓ |
| `MemoryLayoutTest` | single-block layout arithmetic, including a region-overlap guard | |
| `OptionsTest` | option packs, value options, and that the constants-to-options move retuned nothing | |
| `PoolReclamationTest` | the pooled source's epoch machine, driven deterministically | ✓ |
| `BucketTest` | the two index buckets the epoch reclaimer is built from | |
| `ThreadRegistryTest` | the lock-free registry both reclamation sources rest on | ✓ |
| `ThreadPinnerTest` | core placement and topology parsing, on any shape of machine | |
| `ProxyAccountingTest` | what the proxy still knows after a thread has left | ✓ |
| `ConcurrencyTest` | loss, duplication and per-producer FIFO across every registered queue | ✓✓ |

`ConcurrencyTest` is the slow one — every registered implementation across five thread shapes,
several minutes. Everything else finishes in well under a second.

### Two suites carry more weight than their size suggests

**`SegmentLifecycleTest` is where obligations live.** Concepts constrain signatures; they cannot
say "must refuse permanently once full". That sentence exists only as a test case here, and the
type list at the top of the file is what decides which segments it is enforced against. A type
missing from that list is an obligation that silently does not apply — which is exactly how
`algo::Mutex` shipped without self-closing, while the test for it sat green in the same file.

**`PoolReclamationTest` runs with `PhasedBucket`'s assertions live.** In a Debug build every
enqueue verifies it is landing on an empty cell. That assertion caught two separate rotation
bugs within a few hundred milliseconds of stress, both of which were invisible in Release.
Run this suite in Debug, repeatedly, after touching anything in `mem/source/Pool.hpp`.

### What was added, and which bug each one exists for

Every case below was written because something was actually broken. The bug it caught is named,
because a test whose purpose is forgotten gets deleted the first time it is inconvenient.

**`SegmentLifecycleTest`** — the type list now covers **all ten** segment types
(`Vyukov`, `PRQ`, `FAAArray`, `HQ`, `SCQ`, `Mutex`, `Spin`, `PSCQ`, `VyukovDCAS`, `VyukovNoABA`).
The last four were standalone comparators until they grew the linkage surface.

| case | exists because |
| --- | --- |
| `AClosedSegmentRefusesEveryEnqueue` | `LinkedProxy` retires a segment on "empty twice + successor". If a producer can still commit after that, the item is lost. Deliberately weak — an *advisory* close passes it too — and it says so; the enforcing half only shows under contention. |

**`PoolReclamationTest`**

| case | exists because |
| --- | --- |
| `ACacheHitIsServiceableWhileTheRotationIsFrozen` | the reuse cache is read *before* the pin; a second thread holds a stale pin so the rotation is genuinely frozen. Mutation-checked against a Pool whose cache is only reachable via the rotation. |
| `RenewReportsWhetherProtectionMoved` | `renew()` returns whether it moved; `LinkedProxy` gates its retry on that bool to tell a convoy from a real memory bound. |
| `ANoOpRenewLeavesExistingHandlesUsable` | the other half of that contract — a false return means handles stay valid. |
| `AHintNamingADepartedThreadDoesNotWedgeTheRotation` | `try_advance`'s blocker hint reads a payload directly, bypassing `is_active()`. Fails the moment `~guard` stops clearing the state byte. |
| `PoolConstraints.BothSourcesSatisfyTheRenewContract` | keeps `Hazard`'s constant `false` from drifting back to `void`. |

**`AdmissionTest`**

| case | exists because |
| --- | --- |
| `ChunkAndPoolAgreeAtTheSameSegmentCount` | the chunk count was a defaulted constructor argument nothing passed, so `chunk-*` ran at 4 segments while `mem-*` ran at `kPoolSize`. Now a **compile error** if they diverge. |
| `*ReachesItsStatedCapacity`, `CapacityIsSplitAcrossSegments` | a bound that admits nothing satisfies `placed <= capacity()`; these assert it is actually *reached*. |
| `AdmitNone.CostsTheProxyNothingComparedToACountingPolicy` | protects the emptiness of `admit::None` — the property that decided where the per-segment capacity lives. |

**`RegistryConformanceTest`**

| case | exists because |
| --- | --- |
| `CapacityRoundsUpToAPowerOfTwo` | all 47 entries, at sizes **3 / 100 / 1000** — deliberately *not* powers of two, since everything else in the tree uses 64 or 8 and would pass without exercising any rounding. |

**`OptionsTest`**

| case | exists because |
| --- | --- |
| `TheExactSizeOptOutWorksEverywhereItIsOffered` | covers the three modulo fallbacks added with `no_pow2`; wraps a 3-slot ring 100 times, which is where a wrong index or lap shows up. |
| `TheTwoOrderBasedAlgorithmsAlwaysRound` | `LFring`/`SCQ` deliberately offer no opt-out. |
| `TheProxyRetryBudgetIsTunable` | `ProxyOpt::acquire_retries`, including `0`. |
| `LockBasedControls.OnlyAStandaloneMutexParks` | a **linked** segment that parked when full would stall the proxy outright. A `static_assert`, so a regression is a build failure rather than a hang. |
| `LockBasedControls.CloseReleasesAParkedConsumer` / `...Producer` | the unbounded waits only terminate because `close()` notifies both condition variables. |
| `LockBasedControls.AClosedQueueStillDrainsAndCloseIsIdempotent` | closing twice is a no-op, and a closed queue still hands back what it holds. |
| `LockBasedControls.TryDequeueNeverWaits` | the non-blocking pair is what every generic drain uses. |

---

## Building and running

```bash
cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure
```

### The four configurations

Correctness here is a function of the build, so a green Release run means little on its own.

```bash
# Release -- optimiser-visible races, fastest
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release \
      -DSANITIZE_ADDR=OFF -DSANITIZE_THREAD=OFF

# Debug -- assertions live, including PhasedBucket's phase checks
cmake -S . -B build-debug   -DCMAKE_BUILD_TYPE=Debug \
      -DSANITIZE_ADDR=OFF -DSANITIZE_THREAD=OFF

# AddressSanitizer -- use-after-free in the reclamation paths
cmake -S . -B build-asan    -DCMAKE_BUILD_TYPE=Debug \
      -DSANITIZE_ADDR=ON  -DSANITIZE_THREAD=OFF

# ThreadSanitizer -- missing synchronisation. Cannot be combined with ASan.
cmake -S . -B build-tsan    -DCMAKE_BUILD_TYPE=Debug \
      -DSANITIZE_ADDR=OFF -DSANITIZE_THREAD=ON
```

> **Pass the sanitizer flags explicitly.** `SANITIZE_ADDR` currently defaults to **ON** in
> `CMakeLists.txt`, so a plain `-DCMAKE_BUILD_TYPE=Debug` tree is silently Debug+ASan, and
> `-DSANITIZE_THREAD=ON` alone is a hard error — ASan and TSan cannot coexist.

### Repetition matters more than item count

The bugs found here reproduced intermittently: a lost item in PRQ in 3 runs of 8, a livelock in
4 of 12. Raising `-DMPMC_REPEATS=N` is usually more informative than raising the item count.

```bash
ctest --test-dir build-debug -R PoolReclamationTest --repeat until-fail:20
```

---

## Finding a concurrency bug

The technique that found every one of them, and which no test file records:

### 1. Build an isolation harness, not a test

A standalone `.cpp` compiled straight against the headers, with **one axis varied at a time**:

```bash
clang++ -std=c++20 -O2 -mcx16 -pthread -Iinclude /tmp/iso.cpp -o /tmp/iso
```

The PRQ duplication was localised with four rows that differ in exactly two variables:

| configuration | reclamation | segment | result |
| --- | --- | --- | --- |
| `u-prq` (hazard) | none — `reopen()` never runs | PRQ | clean |
| `mem-prq` N=4096 | rare | PRQ | clean |
| `mem-scq` N=8 | heavy | SCQ | clean |
| `mem-prq` N=8 | heavy | PRQ | **duplicates every run** |

Swapping the segment with everything else fixed cleared it; removing recycling with the segment
fixed cleared it. That is what turned "something is wrong with the pool" into a specific claim
about one function, and it was wrong twice before it was right — reasoning alone had produced a
confident and incorrect hypothesis.

### 2. Compare against the unmodified header

Build the *same* harness against the previous version, using a shadow include directory that
takes precedence:

```bash
mkdir -p /tmp/shadow/algo
git show HEAD:include/algo/HQ.hpp > /tmp/shadow/algo/HQ.hpp
clang++ -std=c++20 -O2 -mcx16 -pthread -I/tmp/shadow -Iinclude /tmp/iso.cpp -o /tmp/iso_old
```

Now "before" and "after" are the same binary but for one header, and the difference is
attributable. This is also how a fix is shown to be load-bearing rather than coincidental.

### 3. Make the new test fail first

A regression test that has never failed is not known to test anything. Build it against the old
header and confirm it fails there.

This is not a formality. The first version of the HQ capacity test **passed against both**
versions: its producer delay was `std::atomic_signal_fence`, a compiler barrier that generates
no delay at all, so the consumer never reached the window where the bug lived. The test looked
reasonable and asserted nothing. Only running it against the known-broken header exposed that.

### 4. Then fix, and re-measure

Quote the numbers in the comment at the fix site. The comments in this tree that say
*"Measured before this fix: 20004 items consumed against 20000 produced"* are the reason the
next bug of the same shape was recognised quickly.

---

## Adding a test

1. Create `src/test/unit/YourTest.cpp` with a `@file` / `@brief` block saying what it pins.
2. Add `YourTest` to the `UNIT_TESTS` list in `CMakeLists.txt`.
3. To run over every registered implementation, use the registry-driven typed pattern:

```cpp
using AllTypes = registry::AsTypes<registry::All<Item>>::apply<::testing::Types>;
TYPED_TEST_SUITE(MySuite, AllTypes, registry::TestNames<registry::All<Item>>);
```

`registry::TestNames` takes the case names from the registry, so a failure reads
`MySuite/mem_mutex` rather than `MySuite/29`, and `--gtest_filter='MySuite/mem_mutex.*'`
works. Hyphens become underscores because gtest rejects anything else.

> The generated output still carries `, where TypeParam = <type>` on each line. Tests are
> compiled with `GTEST_HAS_RTTI=0` for exactly this reason: without it, the trailer is the
> fully expanded template-id, several hundred characters naming the same segment three times.
> Nothing in the project or its tests uses `dynamic_cast` or `typeid`.

---

## Running everything, start to finish

A copy-pasteable sequence. Every step that can hang carries a `timeout`, because the failure
mode for a lock or a blocking queue is a hang, not a red test.

### 1. The unit suites, in all four configurations

```bash
# Release
cmake -S . -B build && cmake --build build -j
(cd build && timeout 400 ctest -E '^ConcurrencyTest$' --output-on-failure)

# Debug / ASan / TSan
for cfg in "debug::" \
           "asan:-fsanitize=address -fno-omit-frame-pointer:-fsanitize=address" \
           "tsan:-fsanitize=thread:-fsanitize=thread"; do
  name="${cfg%%:*}"; rest="${cfg#*:}"; cxxf="${rest%%:*}"; ldf="${rest#*:}"
  cmake -S . -B build-$name -DCMAKE_BUILD_TYPE=Debug \
        -DCMAKE_CXX_FLAGS="$cxxf" -DCMAKE_EXE_LINKER_FLAGS="$ldf"
  cmake --build build-$name -j
  (cd build-$name && timeout 400 ctest -E '^ConcurrencyTest$' --output-on-failure)
done
```

`ConcurrencyTest` is excluded above and run separately — it is the slow one, and it is the
acceptance test for anything touching a segment or the proxy:

```bash
timeout 1800 ./build/ConcurrencyTest            # and once per sanitizer config
```

### 2. Stress the suites that are timing-dependent

A single green run of these means little; they are cheap, so repeat them.

```bash
for i in $(seq 1 20); do timeout 60 ./build-debug/PoolReclamationTest >/dev/null || echo "FAIL $i"; done
for i in $(seq 1 20); do timeout 60 ./build-tsan/SegmentLifecycleTest  >/dev/null || echo "FAIL $i"; done
```

### 3. Warnings — both compilers, ours only

```bash
for cxx in g++ clang++; do
  $cxx -std=c++20 -O2 -DNDEBUG -mcx16 -Wall -Wextra -Iinclude -fsyntax-only \
       -Ibuild/_deps/googletest-src/googletest/include src/test/unit/OptionsTest.cpp 2>&1 \
    | grep -v googletest | grep -c "warning:"
done
```

### 4. The benchmarks

Two binaries over the same harness, so they cannot drift in how they measure:

| binary | sweeps | for |
| --- | --- | --- |
| `benchmark` | `registry::All` — **47** entries | the headline throughput campaign |
| `mpmc_tune` | `registry::Tuning` — **19** entries | instrumented (`i-*`) and backoff (`*-p<N>`) variants |

```bash
./build/benchmark --list                                  # 47 names
./build/mpmc_tune  --list                                 # 19 names

# <name> <producers> <consumers> <items> <capacity> [pin] [prod_ticks amp] [cons_ticks amp]
./build/benchmark u-faa 4 4 1000000 1024                  # one bare number: ops/sec
./build/mpmc_tune  u-faa-p0 4 4 1000000 1024 --metrics    # key=value lines
```

**The correctness gate.** Both binaries count what went in and what came out, and **exit 2**
without printing a throughput if they disagree — this tree has had three segments lose items
while looking fast. `runner.py` records that as `LOST_ITEMS`.

```
$ ./build/benchmark u-faa 4 4 200000 1024
LOST ITEMS: produced=200000 consumed=199999 delta=1     # on stderr, exit 2
```

**`--metrics`** adds `segments_linked`, `segment_capacity`, `produced`, `consumed`, from which
the Python side derives the slot-efficiency figures in the benchmark notes:

$$
W_{\text{total}} = S \cdot n - i, \qquad \eta_{\text{slot}} = \frac{i}{S \cdot n}
$$

Note `segments_linked` only appears for entries built with `ProxyOpt::segment_stats` — i.e. the
`mpmc_tune` set. That is deliberate: those counters are atomics on the link path, so a counter
run and a throughput run must be **separate passes**.

### 5. Benchmark series and plots

```bash
cd python
python3 -m pytest -q                                       # the harness's own tests

# --list has no config to consult, so it needs telling which binary to query.
python3 -m mpmc_bench --list --build-dir ../build                      # 47
python3 -m mpmc_bench --list --executable mpmc_tune --build-dir ../build   # 19

python3 -m mpmc_bench experiments/smoke_metrics.json --build-dir ../build --dry-run
python3 -m mpmc_bench experiments/smoke_metrics.json --build-dir ../build
python3 -m mpmc_bench experiments/hq_validation.json --build-dir ../build
```

The runner resolves the binary **per experiment** from the `executable` field and builds every
one an experiment asks for, so a config targeting `mpmc_tune` needs no extra flags.

Counters are reduced across repetitions by **median**, like the throughput. That matters: an
unlucky `u-faa-p0` run burns several times the segments of a lucky one, so recording whichever
rep happened to finish last would put an arbitrary sample in the `Segments` column.

An experiment is JSON; `executable` picks the binary and `metrics` turns on the counter columns:

```json
{ "output_file": "hq_validation.csv", "executable": "mpmc_tune", "metrics": true,
  "queues": ["u-faa-p0", "u-hq-p0"], "queue_sizes": [1024],
  "threads": [[1,1],[2,2],[4,4],[8,8]], "items": 1000000, "repetitions": 5 }
```

Plotting the resulting CSV is the next section.

### 6. Plotting results

```bash
cd python
python3 -m mpmc_bench.plotting.plots <results.csv> --kind <kind> [--save out.png]
```

`mpmc-plot <results.csv> ...` is the same thing once the package is installed. Without `--save`
it opens a window; with it, writes a PNG and prints where.

#### The five kinds

| `--kind` | shows | needs `"metrics": true` |
| --- | --- | :-: |
| `throughput` *(default)* | ops/sec against total threads | |
| `scalability` | speedup against a baseline thread count | |
| `slot-efficiency` | $\eta = i / (S \cdot n)$ — the headline for the HQ claim | ✓ |
| `segments-per-item` | $S / i$, allocation pressure, log y | ✓ |
| `backoff-grid` | heatmap over patience × threads, one panel per family | ✓ |

The y-axis defaults follow the kind: throughput divides by `1e6` and is labelled "Millions of
ops/sec", the ratios do not. Override with `--scale` and `--ylabel` if you need to.

#### Narrowing what is drawn

`--list` prints the queues present in the CSV, which is the quickest way to see what a sweep
actually produced. Everything else filters rows before plotting:

| flag | effect |
| --- | --- |
| `--queues u-faa-p0 u-hq-p0` | only these implementations (default: all) |
| `--size 1024 4096` | only these capacities |
| `--pin` / `--no-pin` | only pinned, or only unpinned, runs |
| `--prod-delay 0` / `--cons-delay 0` | only these simulated-work levels, in ns |
| `--baseline 4` | thread count `scalability` normalises against |
| `--logx`, `--title`, `--ylabel`, `--scale` | axes |

Filtering matters once a sweep has more than one size or delay in it: without `--size`, rows at
different capacities are averaged together at the same x, and the line is a blend of two
different queues' worth of behaviour.

#### Worked example — the HQ campaign

```bash
python3 -m mpmc_bench experiments/hq64_balanced.json --build-dir ../build

# does HQ hit the floor while FAAArray-without-backoff collapses?
python3 -m mpmc_bench.plotting.plots experiments/hq64_balanced.csv \
        --kind slot-efficiency --size 1024 --save hq64_efficiency.png

# the same story as allocation pressure
python3 -m mpmc_bench.plotting.plots experiments/hq64_balanced.csv \
        --kind segments-per-item --size 1024 --save hq64_segments.png

# the grid-search: patience against thread count, faa and hq side by side
python3 -m mpmc_bench.plotting.plots experiments/hq64_balanced.csv \
        --kind backoff-grid --size 1024 --save hq64_backoff.png

# and the plain throughput view, tuned variants only
python3 -m mpmc_bench.plotting.plots experiments/hq64_balanced.csv \
        --kind throughput --queues u-faa-p1024 u-hq-p1024 --size 1024 \
        --save hq64_throughput.png
```

#### Three errors you will hit, and what they mean

**A CSV without counters.** The three metrics plots refuse rather than drawing an empty chart:

```
plot_slot_efficiency needs SlotEfficiency, which this CSV does not have.
Re-run the experiment with "metrics": true (it drives the benchmark's --metrics mode).
```

**A `scalability` baseline that is not in the data.** It defaults to 2 *total* threads, but a
sweep whose smallest shape is `[2, 2]` starts at 4:

```
no implementation has a 2-thread measurement to normalise against
```

Pass `--baseline 4`, or whatever the smallest total in the sweep is.

**Filters that match nothing.** The commonest one, and usually a typo or a queue that was not in
that sweep:

```
nothing to plot: the filters matched no rows
```

`--list` is the fix: it prints exactly what the CSV contains, so a name can be checked rather
than guessed.

`backoff-grid` recovers the patience value from the entry *name*, so it only works on names
ending `-p<N>` — the `registry::Tuning` entries do (`u-faa-p0`, `u-hq-p1024`). It sorts them
numerically, not lexically, or 1024 would come before 16.

### 7. Ad-hoc harnesses — the ones that actually found the bugs

Three of this tree's worst bugs were found by standalone harnesses, not by the suites, because
each needed a shape the suites do not produce. They were written under `/tmp` and are **not
preserved** — if one is needed again it has to be rewritten, so the shape matters more than the
code:

- **Proxy race** — 4P/4C at capacity 16 (4-slot segments), 25 trials, asserting
  `produced == consumed` through a real proxy. Found item loss in `PSCQ` (13/25), `chunk-noaba`
  (20/25) and `VyukovDCAS` (12/25 — which was passing `ConcurrencyTest` at the time).
- **Close race** — drives a segment to the proxy's exact unlink point (empty twice while closed),
  then checks whether a straggling producer can still commit.
- **Sentinel probe** — enqueues a specific word into a segment directly. This is what showed
  `cell::LowTag` reserves `1` as *consumed*, so the benchmark's first item was a reserved
  encoding: 8 enqueued, 7 drained.

The technique is in *Finding a concurrency bug* above; the lesson is that a harness reproducing
the failure **before** the fix is worth more than a test added after it.

---

## Conventions

- **The threaded suites are yours to run.** They are built in every configuration and left for
  you; an agent working in this repository builds them but does not execute them.
- **`mem-vyukov` has a history of livelocking.** If a run hangs rather than fails, that is the
  first entry to suspect. `ConcurrencyTest`'s producers give up after a bounded number of
  refused enqueues and report a stall, so a livelock fails the test rather than hanging it —
  the check is on *global* progress, not one producer's, because these queues are lock-free
  rather than wait-free and a starved producer is not a stall.
- **Python tests** live in `python/tests/` and run with `pytest` from `python/`.
