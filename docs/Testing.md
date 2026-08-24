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

## Conventions

- **The threaded suites are yours to run.** They are built in every configuration and left for
  you; an agent working in this repository builds them but does not execute them.
- **`mem-vyukov` has a history of livelocking.** If a run hangs rather than fails, that is the
  first entry to suspect. `ConcurrencyTest`'s producers give up after a bounded number of
  refused enqueues and report a stall, so a livelock fails the test rather than hanging it —
  the check is on *global* progress, not one producer's, because these queues are lock-free
  rather than wait-free and a starved producer is not a stall.
- **Python tests** live in `python/tests/` and run with `pytest` from `python/`.
