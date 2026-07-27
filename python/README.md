# mpmc_bench — running the benchmarks and plotting the results

Python tooling for the `mpmc_queues` benchmarks: an experiment driver, a plotter, and the
CPU-topology generator CMake calls at configure time.

The list of queue implementations is **not** duplicated here. The tooling asks the binary
(`benchmark --list`), so the C++ registry in `include/registry/Registry.hpp` stays the only
place implementations are declared. Add one there and it shows up here with no Python change.

---

## Install

```bash
cmake -S . -B build && cmake --build build -j     # need `benchmark` and `timeTicks`
pip install -e python                             # add [test] for pytest
```

Everything resolves paths from the package location, so you can run from any directory.
The build tree is found automatically (a directory containing `CMakeCache.txt`); override
with `--build-dir` or `MPMC_BUILD_DIR`.

## Quick start

```bash
mpmc-run --list                        # what can be benchmarked
mpmc-run python/experiments/test_run.json
mpmc-plot results/smoke.csv --scale 1e6 --ylabel "Millions of ops/sec"
```

Three entry points, each also runnable as a module:

| Command | Module form | Does |
| --- | --- | --- |
| `mpmc-run` | `python -m mpmc_bench` | run an experiment grid, write CSV |
| `mpmc-plot` | `python -m mpmc_bench.plotting.plots` | plot a results CSV |
| `mpmc-topology` | `python -m mpmc_bench.topology.generator` | inspect CPUs / emit a pinning plan |

---

## Running experiments

```bash
mpmc-run CONFIG.json [--build-dir DIR] [--out-dir DIR] [--dry-run] [-v]
mpmc-run --list
```

`--dry-run` validates the config and prints the grid size without running anything — worth
doing before a sweep that will take hours.

Results are written **relative to the config file**, not the working directory, so a config
saying `"output_file": "results/x.csv"` always lands next to itself.

### Config format

A JSON object with `experiments`, or a bare list, or a single experiment object.

```json
{
  "experiments": [
    {
      "name": "balanced",
      "output_file": "results/balanced.csv",
      "queues": ["vyukov", "u-prq", "chunk-faaarray"],
      "queue_sizes": [1024, 4096],
      "threads": [[1, 1], [4, 4], [16, 16]],
      "items": 10000000,
      "repetitions": 5,
      "pinning": [true, false],
      "timeout_s": 300.0,
      "delays": [
        { "producer": {"ns": 0,   "amplitude": 0.0},
          "consumer": {"ns": 0,   "amplitude": 0.0} },
        { "producer": {"ns": 240, "amplitude": 0.5},
          "consumer": {"ns": 240, "amplitude": 0.5} }
      ]
    }
  ]
}
```

| Key | Required | Meaning |
| --- | --- | --- |
| `output_file` | yes | CSV path, relative to the config |
| `queues` | yes | registry names — check with `mpmc-run --list` |
| `queue_sizes` | yes | per-segment capacity (≥ 2) |
| `threads` | yes | `[producers, consumers]` pairs, both ≥ 1 |
| `items` | no (1e6) | items pushed per run |
| `repetitions` | no (1) | runs per grid point; mean and stdev are recorded |
| `pinning` | no (`[false]`) | pin threads to cores using `build/sys.topo` |
| `delays` | no (none) | simulated per-operation work, in nanoseconds |
| `timeout_s` | no (300) | per-run timeout |
| `name` | no | label for logs |

The grid is the **product** of `queues × threads × queue_sizes × pinning × delays`, each run
`repetitions` times. `--dry-run` prints the count; `experiments/full_sweep.json` is 8820
points, so check before starting.

Delays are given in nanoseconds and calibrated to spin counts on this machine via
`timeTicks`. `amplitude` (0–1) randomises the delay to avoid lockstep.

Unknown keys are rejected rather than ignored, so a typo in `repetitions` is an error
instead of silently meaning `1`.

### Output columns

`Queue, Producers, Consumers, Size, Items, Pinning, ProdDelay_NS, ProdDelay_Amp,
ConsDelay_NS, ConsDelay_Amp, Throughput_Mean, Throughput_StdDev, Samples, Status`

`Status` is `OK` or a reason — `EXIT_3: …`, `TIMEOUT after 300.0s`, `BAD_OUTPUT: …`,
`CALIBRATION_FAILED: …`. Rows that did not measure have empty throughput fields rather
than a zero that would quietly average into your results.

---

## Plotting

```bash
mpmc-plot CSV [--kind throughput|scalability]
              [--queues NAME...] [--size N...] [--pin | --no-pin]
              [--prod-delay NS...] [--cons-delay NS...]
              [--baseline N] [--scale F] [--ylabel S] [--title S] [--logx]
              [--save FILE] [--list]
```

```bash
mpmc-plot results/balanced.csv --queues u-prq u-scq --size 1024 --pin \
          --scale 1e6 --ylabel "Millions of ops/sec" --save throughput.png

mpmc-plot results/balanced.csv --kind scalability --baseline 2 --save scaling.png
```

`--list` shows which implementations a CSV contains. Colours and markers are derived from
the implementation name by a stable hash, so every registry entry plots distinctly without
anyone maintaining a style table.

Scalability normalises against `--baseline` total threads (default 2). An implementation
with no measurement at that thread count is omitted, with a warning naming it — there is no
honest way to normalise against a point that was never measured.

Rows whose `Status` is not `OK` are dropped before plotting. Result files from before the
CLI change are still readable: their `Queue` column holds old names, and their
`ProdDelay_Ticks` columns are accepted with a warning that ticks are machine-specific and
not comparable with nanosecond values from newer files.

---

## Topology and pinning

CMake runs this at configure time to produce `build/sys.topo`, which is what
`benchmark ... pin` reads. Run it directly to inspect a machine:

```bash
mpmc-topology --cpu                    # CPU/core/node layout
mpmc-topology --cluster --cache 2      # clusters sharing an L2
mpmc-topology --pin_cluster out.topo   # cluster-first pinning plan
mpmc-topology --pin_ping_pong out.topo 2
```

If `sys.topo` is missing, reconfigure (`cmake -S . -B build`) — the configure step reports a
warning when it cannot generate one, and `pin` is unavailable until it exists.

---

## Tests

```bash
pip install -e 'python[test]'
pytest python/tests            # integration tests skip themselves without a build tree
```

Unit tests use a stub benchmark binary and need no C++ build. `test_integration.py` runs the
real one, including a check that every registered implementation actually executes.

---

## Layout

```
python/
  mpmc_bench/
    paths.py          repo/build discovery (never CWD-relative)
    discovery.py      `benchmark --list` -> available names
    schema.py         config dataclasses + strict parsing
    ticks.py          nanoseconds -> spin counts via timeTicks
    runner.py         the grid engine and CSV writer
    plotting/         data loading, derived styles, plots
    topology/         CPU topology and pinning plans
  experiments/        configs and results
  old/                pre-refactor scripts and data (see SUPERSEDED.md)
  tests/
```
