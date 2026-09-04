"""Execution engine: run the grid, record throughput.

Structure is carried over from the previous ``benchmark_runner.py``, which was sound:
build check, calibrate delays, product over the grid, repeat each point, write a row.
What changed is everything around the edges -- names instead of integers, validation
before measuring rather than during, paths anchored to the config file, and failures
recorded with a reason rather than a bare "FAILED".
"""

from __future__ import annotations

import argparse
import csv
import itertools
import logging
import statistics
import subprocess
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path

from . import discovery, paths, schema
from .schema import DelayPair, Experiment
from .ticks import TickConverter

logger = logging.getLogger("mpmc.runner")

HEADERS = [
    "Queue", "Producers", "Consumers", "Size", "Items", "Pinning",
    "ProdDelay_NS", "ProdDelay_Amp", "ConsDelay_NS", "ConsDelay_Amp",
    # Mean and stdev stay first so CSVs already on disk keep parsing. The median is what
    # should actually be read: on a machine whose clock ranges 400-2500 MHz the ramp produces
    # outliers that drag the mean, and the same binary has measured 2.58-6.75 M/s.
    "Throughput_Mean", "Throughput_StdDev", "Samples", "Status",
    "Throughput_Median", "Throughput_Min", "Throughput_Max",
    # From `--metrics`; blank when the run did not ask for it. S and n are the inputs to the
    # slot-efficiency formulas below.
    "Segments", "SegmentCapacity", "Produced", "Consumed",
    "WastedSlots", "WastedPerSegment", "SlotEfficiency",
    "Governor",
]


@dataclass
class Point:
    """One grid point and its measurements."""

    queue: str
    producers: int
    consumers: int
    size: int
    pinning: bool
    delays: DelayPair
    samples: list[float]
    status: str = "OK"
    #: One dict per repetition. Counters vary a lot between reps -- an unlucky FAAArray run
    #: can burn 5x the segments of a lucky one -- so they are reduced by median, like the
    #: throughput, rather than by "whichever ran last".
    metric_samples: list[dict[str, float]] = field(default_factory=list)

    @property
    def metrics(self) -> dict[str, float]:
        keys = {k for m in self.metric_samples for k in m}
        return {
            k: statistics.median([m[k] for m in self.metric_samples if k in m])
            for k in keys
        }

    def slot_efficiency(self) -> tuple[str, str, str]:
        """W_total = S*n - i, W_avg = W_total/S, eta = i/(S*n).

        Straight from the notes. Derived here rather than in the C++ so the definition lives in
        one place, and it needs no per-cell instrumentation -- only the segment count.
        """
        S = self.metrics.get("segments_linked")
        n = self.metrics.get("segment_capacity")
        i = self.metrics.get("produced")
        if not S or not n or not i:
            return "", "", ""
        total = S * n
        if total < i:  # S or n misread; better blank than a nonsense efficiency
            logger.warning("%s: S*n (%g) < items (%g); skipping efficiency", self.queue, total, i)
            return "", "", ""
        return f"{total - i:.0f}", f"{(total - i) / S:.2f}", f"{i / total:.4f}"

    def row(self, items: int, governor: str = "") -> list:
        if self.samples:
            mean = f"{statistics.mean(self.samples):.2f}"
            stdev = f"{statistics.stdev(self.samples):.2f}" if len(self.samples) > 1 else "0.00"
            median = f"{statistics.median(self.samples):.2f}"
            lo, hi = f"{min(self.samples):.2f}", f"{max(self.samples):.2f}"
        else:
            mean = stdev = median = lo = hi = ""
        m = self.metrics
        wasted, per_seg, eta = self.slot_efficiency()
        return [
            self.queue, self.producers, self.consumers, self.size, items, self.pinning,
            self.delays.producer.ns, self.delays.producer.amplitude,
            self.delays.consumer.ns, self.delays.consumer.amplitude,
            mean, stdev, len(self.samples), self.status,
            median, lo, hi,
            _opt(m.get("segments_linked")), _opt(m.get("segment_capacity")),
            _opt(m.get("produced")), _opt(m.get("consumed")),
            wasted, per_seg, eta,
            governor,
        ]


def _opt(v: float | None) -> str:
    return "" if v is None else f"{v:.0f}"


def parse_metrics(stdout: str) -> dict[str, float]:
    """`key=value` lines from `--metrics`. Ignores anything that is not a number."""
    out: dict[str, float] = {}
    for line in stdout.splitlines():
        key, _, value = line.partition("=")
        if not _:
            continue
        try:
            out[key.strip()] = float(value)
        except ValueError:
            pass
    return out


def cpu_governor() -> str:
    """Recorded per row: it changes what the numbers mean, and it is easy to forget."""
    try:
        return Path(
            "/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor"
        ).read_text().strip()
    except OSError:
        return "unknown"


def ensure_built(build_dir: Path, targets: list[str]) -> None:
    """Build any missing target rather than failing on a fresh checkout."""
    missing = [t for t in targets if not (build_dir / t).exists()]
    if not missing:
        return
    logger.warning("missing %s; building", ", ".join(missing))
    cmd = ["cmake", "--build", str(build_dir), "--target", *missing, "--parallel"]
    try:
        subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL)
    except subprocess.CalledProcessError as exc:
        raise RuntimeError(f"build failed ({exc.returncode}): {' '.join(cmd)}") from exc
    still = [t for t in targets if not (build_dir / t).exists()]
    if still:
        raise RuntimeError(f"build did not produce: {', '.join(still)}")


def _command(exe: Path, pt: Point, items: int, p_ticks: int, c_ticks: int) -> list[str]:
    cmd = [str(exe), pt.queue, str(pt.producers), str(pt.consumers), str(items), str(pt.size)]
    if pt.pinning:
        cmd.append("pin")
    if p_ticks or c_ticks:
        cmd += [
            str(p_ticks), str(pt.delays.producer.amplitude),
            str(c_ticks), str(pt.delays.consumer.amplitude),
        ]
    return cmd


def run_experiment(exp: Experiment, exe: Path, converter: TickConverter, out_dir: Path) -> Path:
    """Run one experiment's grid, writing rows as they complete."""
    # Fail before measuring anything, naming the valid set. Previously an unknown name
    # produced a FAILED row per grid point and the run continued regardless.
    discovery.validate(exp.queues, exe)

    out_path = out_dir / exp.output_file
    out_path.parent.mkdir(parents=True, exist_ok=True)

    combos = list(
        itertools.product(exp.queues, exp.threads, exp.queue_sizes, exp.pinning, exp.delays)
    )
    total = len(combos)
    logger.info(
        "%s: %d grid points x %d reps -> %s",
        exp.name or exp.output_file, total, exp.repetitions, out_path,
    )

    with out_path.open("w", newline="") as fh:
        writer = csv.writer(fh)
        writer.writerow(HEADERS)

        points: dict = {}
        for queue, (prod, cons), size, pin, delays in combos:
            points[(queue, prod, cons, size, pin, delays)] = Point(
                queue, prod, cons, size, pin, delays, samples=[]
            )

        # Repetitions OUTERMOST, grid points inner. This is the whole point: running all of a
        # configuration's reps before moving on hands whichever runs later the CPU's frequency
        # ramp, and on this hardware that fabricated a 2.3x difference between two queues that
        # were in fact identical. Rotating means every point sees the same drift.
        governor = cpu_governor()
        for rep in range(exp.repetitions):
            for i, key in enumerate(points, 1):
                pt = points[key]
                if pt.status != "OK":
                    continue  # already failed; do not keep paying for it

                try:
                    p_ticks = converter.ticks_for(pt.delays.producer.ns)
                    c_ticks = converter.ticks_for(pt.delays.consumer.ns)
                except RuntimeError as exc:
                    pt.status = f"CALIBRATION_FAILED: {exc}"
                    continue

                cmd = _command(exe, pt, exp.items, p_ticks, c_ticks)
                if exp.metrics:
                    cmd.append("--metrics")
                logger.info(
                    "[rep %d/%d] [%d/%d] %s | %dP-%dC | size=%d | pin=%d | delay=%d/%dns",
                    rep + 1, exp.repetitions, i, total, pt.queue, pt.producers, pt.consumers,
                    pt.size, int(pt.pinning), pt.delays.producer.ns, pt.delays.consumer.ns,
                )
                try:
                    res = subprocess.run(
                        cmd, capture_output=True, text=True, timeout=exp.timeout_s
                    )
                except subprocess.TimeoutExpired:
                    pt.status = f"TIMEOUT after {exp.timeout_s}s"
                    continue
                if res.returncode != 0:
                    # rc 2 is the benchmark's correctness gate: it lost or duplicated items.
                    label = "LOST_ITEMS" if res.returncode == 2 else f"EXIT_{res.returncode}"
                    pt.status = f"{label}: {res.stderr.strip()[:120]}"
                    continue

                if exp.metrics:
                    m = parse_metrics(res.stdout)
                    if "throughput" not in m:
                        pt.status = f"BAD_OUTPUT: {res.stdout.strip()[:120]}"
                        continue
                    pt.samples.append(m["throughput"])
                    pt.metric_samples.append(m)
                else:
                    try:
                        pt.samples.append(float(res.stdout.strip()))
                    except ValueError:
                        pt.status = f"BAD_OUTPUT: {res.stdout.strip()[:120]}"
                        continue

        for pt in points.values():
            if pt.status != "OK":
                logger.error("  %s: %s", pt.queue, pt.status)
            writer.writerow(pt.row(exp.items, governor))
            fh.flush()

    return out_path


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="mpmc-run", description="Run the mpmc_queues benchmark grid."
    )
    parser.add_argument("config", nargs="?", help="experiment config (JSON)")
    parser.add_argument("--list", action="store_true", help="list available implementations and exit")
    parser.add_argument("--build-dir", help="build tree (default: auto-detect, or $MPMC_BUILD_DIR)")
    parser.add_argument("--out-dir", help="where to write results (default: alongside the config)")
    parser.add_argument("--dry-run", action="store_true", help="validate and print the grid, run nothing")
    parser.add_argument("--executable", default="benchmark",
                        help="which binary --list should query: benchmark (default) or mpmc_tune")
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args(argv)

    logging.basicConfig(
        level=logging.DEBUG if args.verbose else logging.INFO,
        format="%(asctime)s [%(levelname)s] %(message)s",
        datefmt="%H:%M:%S",
    )

    try:
        bdir = paths.build_dir(args.build_dir)

        if args.list:
            # No config yet, so there is no per-experiment executable to consult.
            ensure_built(bdir, [args.executable])
            for name in discovery.available(bdir / args.executable):
                print(name)
            return 0

        if not args.config:
            parser.error("a config file is required (or use --list)")

        experiments = schema.load(args.config)
        out_dir = Path(args.out_dir) if args.out_dir else Path(args.config).resolve().parent

        # Each experiment names its own binary -- "benchmark" for the 47 headline entries,
        # "mpmc_tune" for the instrumented and backoff variants -- so resolve it per
        # experiment rather than once. Build every one that is actually asked for.
        needed = sorted({exp.executable for exp in experiments} | {"timeTicks"})
        ensure_built(bdir, needed)

        for exp in experiments:
            discovery.validate(exp.queues, bdir / exp.executable)

        if args.dry_run:
            for exp in experiments:
                print(f"{exp.name or exp.output_file}: {exp.grid_size} points "
                      f"x {exp.repetitions} reps -> {out_dir / exp.output_file}")
            return 0

        converter = TickConverter(bdir / "timeTicks")

        started = time.time()
        for exp in experiments:
            run_experiment(exp, bdir / exp.executable, converter, out_dir)
        logger.info("done in %.1fs", time.time() - started)
        return 0

    except (schema.ConfigError, discovery.UnknownQueue, discovery.DiscoveryFailed,
            paths.BuildNotFound, RuntimeError) as exc:
        logger.critical("%s", exc)
        return 1


if __name__ == "__main__":
    sys.exit(main())
