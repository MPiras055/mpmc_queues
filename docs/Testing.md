\page testing Testing

What the suites cover, how to run them properly, and the techniques that actually found the
concurrency bugs in this tree.

---

## The suites

Thirteen GoogleTest binaries, registered in the `UNIT_TESTS` list in `CMakeLists.txt`. Adding a
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
| `ShardBlockTest` | the SPSC buffers, the all-to-all block's arena layout, both dispatch strategies' refusal contract, and the `ShardQueue` bridge | ✓ |
| `ConcurrencyTest` | loss, duplication and per-producer FIFO across every registered queue | ✓✓ |

`ConcurrencyTest` is the slow one — every registered implementation across five thread shapes,
several minutes. Everything else finishes in well under a second.

`ShardBlockTest` splits cleanly: `--gtest_filter='-Threaded*'` is the single-threaded half
(protocol, layout, strategies, bridge) and finishes instantly; `--gtest_filter='Threaded*'` is
loss / duplication / per-pair FIFO at seven `(P, C)` shapes under both strategies. The shard
blocks are deliberately **not** in `ConcurrencyTest` -- see `registry::Shard` for why.

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

### 1b. Running one queue, or a named list

The full `ConcurrencyTest` is five cases across every registry entry — minutes. When the question
is "did this break `u-pscq`", filter it. GoogleTest can do this directly:

```bash
./build/ConcurrencyTest --gtest_filter='Mpmc/u_faa.*'                 # one queue
./build/ConcurrencyTest --gtest_filter='Mpmc/u_faa.*:Mpmc/u_hq.*'     # a list
```

Two things make that easy to get wrong, and a third makes getting it wrong dangerous:

- **The suite prefix differs per binary**: `Mpmc/` in `ConcurrencyTest`, `QueueBehaviour/` in
  `RegistryConformanceTest`, `SegmentLifecycle/` in `SegmentLifecycleTest`.
- **Registry names are mangled.** `registry::TestNames::GetName` replaces every
  non-alphanumeric character with `_`, so `u-faa` is `u_faa` and `vyukov-dcas` is
  `vyukov_dcas` in a filter.
- **gtest exits 0 when a filter matches nothing.** A mistyped queue name is indistinguishable
  from a passing run — which, for the suite that catches lost items, is the worst way to fail.

`tools/qtest` wraps all three:

```bash
tools/qtest u-pscq                          # every registry suite, that queue
tools/qtest u-pscq u-prq u-scq              # a list
tools/qtest --suite ConcurrencyTest u-hq    # one binary
tools/qtest --suite SegmentLifecycleTest pscq
tools/qtest --build-dir /tmp/mpmc-verif-tsan u-pscq
tools/qtest --list                          # the registry names, unmangled
tools/qtest u-pscq -- --gtest_repeat=20 --gtest_shuffle
```

It mangles the names for you, picks the prefix per binary, wraps each run in `timeout` (a lock
bug hangs rather than fails), passes anything after `--` through to gtest, and **fails when the
filter selects nothing**:

```
$ tools/qtest --suite ConcurrencyTest u-faaa
qtest: filter 'Mpmc/u_faaa.*' matched no tests in ConcurrencyTest
       (names are mangled: '-' becomes '_'; try tools/qtest --list)
$ echo $?
1
$ ./build/ConcurrencyTest --gtest_filter='Mpmc/u_faaa.*'; echo $?
0
```

That contrast is the entire reason the wrapper exists.

`SegmentLifecycleTest` names its instances after the segment (`SegmentLifecycle/pscq`) rather
than by index; the tokens match the registry's segment spellings (`faa`, `dcas`, `noaba`) so one
queue name works across every suite. `TaggingTest` is not per-queue — its types are tag schemes.

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
./build/benchmark u-faa 4 4 1000000 1024                  # one bare number: items/sec
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

**Items in the CSV, operations in the plot.** The binaries report *items* per second — each
producer is handed a slice of the item count and the total is divided by wall time — but every
item costs the queue one enqueue *and* one dequeue. The loader therefore multiplies every
throughput column by `dataio.OPS_PER_ITEM` (= 2) on read, so a plotted figure is twice the
number in the CSV. The standard deviation carries the same factor: the conversion rescales each
underlying sample, so `sd(2X) = 2·sd(X)` and the error bars keep their true relative size.
Ratios — `scalability`, `slot-efficiency` — are unchanged, because the factor cancels.

#### Narrowing what is drawn

`--list` prints the queues present in the CSV, which is the quickest way to see what a sweep
actually produced. Everything else filters rows before plotting:

| flag                                       | effect                                        |
| ------------------------------------------ | --------------------------------------------- |
| `--queues u-faa-p0 u-hq-p0`                | only these implementations (default: all)     |
| `--size 1024 4096`                         | only these capacities                         |
| `--pin` / `--no-pin`                       | only pinned, or only unpinned, runs           |
| `--prod-delay 0` / `--cons-delay 0`        | only these simulated-work levels, in ns       |
| `--baseline 4`                             | thread count `scalability` normalises against |
| `--logx`, `--title`, `--ylabel`, `--scale` | axes                                          |

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

#### The plotting app

The same plots, driven from a window instead of flags:

```bash
cd python
pip install -e ".[gui]"                                      # PySide6 + pyqtgraph
mpmc-plot-ui ../titanic/hq_validation/hq64_balanced.csv
python -m mpmc_bench.qt ../titanic/hq_validation/*.csv       # no reinstall needed
python -m mpmc_bench.qt --session figure.json                # reopen a saved figure
```

It uses the CLI's loader, so every number matches `mpmc-plot` (including the ×2 ops
conversion). Three tabs on the left, the figure on the right, four tabs of readout underneath.

##### Qt draws the view, matplotlib draws the file

The window is **PySide6**, and the live chart is **pyqtgraph**. The earlier Tk window rendered a
matplotlib figure to a bitmap and uploaded it on every change, which cost 650–960 ms a frame and
made zoom and pan re-renders. Here the series are scene-graph items, so the interaction is free
and a redraw is a property assignment.

Exports did not change. **Save figure** still runs `model.save_figure` — the same matplotlib
code, the same `PlotState`, the same publication-grade PNG/SVG/PDF — on a worker thread. The
split is the point: an interactive view and a print figure want different things, and the old
design paid the print price on every keystroke.

Measured on the two HQ files, ten series across two plots:

| edit | cost |
| --- | --- |
| colour, marker, line style, width | **12 ms** |
| filter, metric, axis change (pandas + rebuild) | ~160 ms, off the UI thread |
| zoom, pan, hover | free — no redraw at all |
| first draw after loading | ~480 ms |

Three things get it there, and each is a test: a cosmetic edit never re-uploads the point arrays
(`test_the_chart_reuses_curves_across_a_restyle`); an unchanged style is not re-applied, because
every pyqtgraph setter repaints; and of the three readout tables, only the one on screen is
populated (`test_populates_the_visible_tab_only`) — filling three was 70 ms of every refresh.

##### The three tabs

**Data** — loaded files, titles, what to plot, the baseline, and the filters.
**Series** — one row per series: visible, colour, name, marker, line style, width, plus the
style library. **Axes** — which plot you are editing, then its labels, log scales, limits,
ticks, legend placement and grid; plus the plot grid, the shared axes and the export size,
which belong to the whole figure.

##### Reading the numbers, not just the shape

Four tabs under the chart, and a hover readout in the status bar naming the series, x, y and the
error range of the nearest point.

| tab | what it answers |
| --- | --- |
| **Table** | exactly what is plotted, one row per point, sortable; `Ctrl+K` → *Copy the table* pastes it into a spreadsheet |
| **Ranking** | at every thread count: the winner, the runner-up and the margin between them |
| **Summary** | per series: peak value, the x it peaks at, the final value, how many points |
| **Notes** | split-line explanations, dropped baselines, and colours too close to tell apart |

The table is also the **relief** the palette owes: three light-mode hues sit under 3:1 contrast,
and the rule in `plotting/theme.py` is that they are allowed only where the values are also
readable as text.

##### Baseline mode

`Data → Baseline` turns absolute throughput into a comparison, which is usually the real
question:

- **A series in each plot** — every line becomes a ratio (or a % difference) against one queue.
  The baseline stays, flat at 1.0 or 0%, because a reader needs to see where the axis crosses.
- **The same series in another plot** — the before/after comparison of two runs of the same
  queues, one CSV against another.

Points the baseline never measured are dropped rather than divided by something absent, and the
note says how many. Error bars are converted with the values. The source data is untouched:
switching back to *Absolute values* is exact, not a re-derivation.

The baseline is **remembered**. It used to be overwritten the moment the chosen series was
missing from a build — and switching metric can drop one for a single draw — so a round trip
through another metric silently reset the comparison. Now the choice is kept, an unusable one
falls back to absolute values *for that draw* and says so in Notes, and it round-trips through a
saved session. It is also applied by `model.render()`, so a saved PNG shows the ratios the
preview did rather than absolute values under a relative axis label.

##### The style library

A colour picked for `u-pscq` should be `u-pscq`'s colour in every figure, or two plots of one
experiment cannot be read side by side. `Series → Style library` pins the current overrides
**per queue** — the split is deliberately discarded, so a colour chosen while looking at size
1024 applies to that implementation everywhere — and writes them to
`$XDG_CONFIG_HOME/mpmc-bench/styles.json`.

Precedence is lowest first: the theme's palette slot, then the library, then anything you have
changed in this session. Pinning can therefore never silently overrule what is in front of you.

##### Loaded files, and which of them are plotted

Loading and plotting are separate. Every CSV you open stays parsed and cached; the tick box in
the first column decides what is drawn. Tick one to see it alone, tick several for side-by-side
plots sharing a y axis. Double-click, or **Only this**, switches in one step and costs nothing,
because nothing is re-read. The last ticked file cannot be unticked.

**"Normalise y across plots" means the union**, not one plot's. pyqtgraph's own view linking
takes a single range, which drew a file peaking at 12 M ops/s entirely above its own axis when
it sat beside one peaking at 1 M; the range is computed across every plot and applied to each,
which is what matplotlib does on export. `test_shared_y_is_the_union_not_one_plot` holds it
there.

##### Filters: one value, or a comparison

Every run parameter becomes a group of tick boxes with a **compare** switch, and the switch is
what the boxes mean:

| compare | what a second ticked value does |
| --- | --- |
| off (default) | nothing — it is a radio button. Ticking 4096 unticks 1024, which is what *switching* queue size should do. The last ticked value cannot be unticked, because an empty filter draws nothing and reads as a bug. |
| on → **separate plots** | a plot per value, side by side: `size 1024`, `size 4096`, `size 16384`. The plot you were reading is still there. |
| on → **same plot** | the lines **split** per value on one axes (`u-pscq (size 1024)`), which is right for a sweep and wrong for a comparison. |

Separate plots is the default once compare is on, because "compare two sizes" nearly always
means side by side. Comparing two parameters at once multiplies: two sizes and two pinnings is
four plots, and the count is capped at `MAX_PLOTS` with a note rather than filling the window
with strips.

The parameter on the **x axis** is locked to *same plot* and says so: every value of it is the
sweep, so one plot each would be one point each.

A plot is therefore a *slot* — a file under a set of compared values — not a file. Everything
per-plot is keyed by that (`<csv>|Size=4096`), so reordering the files does not move one plot's
settings onto another.

Which filters move together is read out of the data, not hardcoded: `A` links to `B` when every
value of A appears beside exactly one value of B. In these files that finds producer delay ↔
consumer delay and both → their amplitudes; each group says what it moves, and **Link related
filters** turns it off. Load a 1:3 sweep beside a balanced one and the producers/consumers link
disappears by itself, because 4 producers then sits beside two different consumer counts.

##### Ticks

X: automatic, every data value, every Nth data value, a fixed spacing, or hidden. Y: automatic,
a fixed spacing, about N ticks, or hidden. The number box beside each is N or the spacing. A log
x axis is ticked at the data values, labelled as the numbers themselves, rather than as powers
of ten — matplotlib draws the export with a base-2 log axis and a plain formatter, and the
preview has to agree with it.

##### Editing one plot

`Axes → Which plot` picks what the rest of the tab edits: **All plots**, or one of them.
Clicking a plot on the chart selects it too — it gets an accent border — and opens this tab.
Per plot: the title, the x and y labels, the log scales, the y limits, both tick specs, the grid
and the legend placement. **Reset this plot** gives it back to the figure, and the line under
the picker names what it currently overrides.

Two settings cannot be per-plot while the axis is shared, and the app says so instead of
ignoring you quietly: a y limit or a log y needs **Normalise y** off, and a log x needs
**Normalise x** off. A shared axis has one range and one scale by definition.

The legend is all-or-nothing: as soon as one plot asks for its own placement, the figure-wide
legend goes and *only the plots that asked* get one. A figure legend beside a per-plot one
lists every series twice, and repeating the same ten names beside every plot is wallpaper.

##### Zoom, pan and the legend

Wheel to zoom, drag to pan, **Reset view** (Ctrl+0) to fit — all free, since nothing is
redrawn. The legend sits in its own column rather than on top of the data: inside the axes it
covered the lines it was naming as soon as there were more than a handful of series, and with
ten queues that is the normal case. Clicking a legend entry hides or shows that series.

**Reset view** re-applies the configured limits, not just autorange. It used to leave the plots
autoranging, which quietly dropped *start y at zero* and any y limit — the limit pass skipped
its work because, from its point of view, nothing about the ranges had changed.

`Y minimum` and `Y maximum` are in **the units on the axis** — the ones the table shows. The
preview used to divide them by the metric's scale as well, so a limit of 5 flattened the chart
while the export, which applies them as written, was right.

##### Saving

**Save figure…** (Ctrl+S) writes PNG, SVG or PDF at the size and DPI on the Axes tab, re-drawn
by matplotlib at that size rather than scaled from the view, so the legend and layout suit the
file. **Export data…** (Ctrl+E) writes the plotted values as CSV. Both run off the UI thread.

##### Everything else

**Save/Open session** (Ctrl+Shift+S / Ctrl+L) stores the whole figure as JSON — files, which are
ticked, filters, names, colours, axes — and sessions written by the Tk version still load.
**Ctrl+K** opens a command palette with every action in one searchable list, which is the
keyboard route to the ones with no button. Files are re-read when they change on disk;
**Ctrl+R** forces it.

**Colour warnings.** A colour you pick is checked against the others on screen with the same
measure as the palette validator (OKLab ΔE, with colour-blindness simulation), and the Notes tab
names the pair that is too close. It warns, never blocks.

##### The Tk window is still there

`mpmc-plot-ui-tk` runs the previous window, and its `gui/` package and tests are untouched —
`gui/model.py` is the shared headless core both front ends are built on, so the loader, filters,
correlations, series building, styling and export have exactly one implementation.

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
