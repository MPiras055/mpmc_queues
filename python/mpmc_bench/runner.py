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
from dataclasses import dataclass
from pathlib import Path

from . import discovery, paths, schema
from .schema import DelayPair, Experiment
from .ticks import TickConverter

logger = logging.getLogger("mpmc.runner")

HEADERS = [
    "Queue", "Producers", "Consumers", "Size", "Items", "Pinning",
    "ProdDelay_NS", "ProdDelay_Amp", "ConsDelay_NS", "ConsDelay_Amp",
    "Throughput_Mean", "Throughput_StdDev", "Samples", "Status",
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

    def row(self, items: int) -> list:
        if self.samples:
            mean = f"{statistics.mean(self.samples):.2f}"
            stdev = f"{statistics.stdev(self.samples):.2f}" if len(self.samples) > 1 else "0.00"
        else:
            mean, stdev = "", ""
        return [
            self.queue, self.producers, self.consumers, self.size, items, self.pinning,
            self.delays.producer.ns, self.delays.producer.amplitude,
            self.delays.consumer.ns, self.delays.consumer.amplitude,
            mean, stdev, len(self.samples), self.status,
        ]


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

        for i, (queue, (prod, cons), size, pin, delays) in enumerate(combos, 1):
            pt = Point(queue, prod, cons, size, pin, delays, samples=[])

            try:
                p_ticks = converter.ticks_for(delays.producer.ns)
                c_ticks = converter.ticks_for(delays.consumer.ns)
            except RuntimeError as exc:
                pt.status = f"CALIBRATION_FAILED: {exc}"
                writer.writerow(pt.row(exp.items))
                fh.flush()
                continue

            cmd = _command(exe, pt, exp.items, p_ticks, c_ticks)
            logger.info(
                "[%d/%d] %s | %dP-%dC | size=%d | pin=%d | delay=%d/%dns",
                i, total, queue, prod, cons, size, int(pin),
                delays.producer.ns, delays.consumer.ns,
            )

            for _ in range(exp.repetitions):
                try:
                    res = subprocess.run(
                        cmd, capture_output=True, text=True, timeout=exp.timeout_s
                    )
                except subprocess.TimeoutExpired:
                    pt.status = f"TIMEOUT after {exp.timeout_s}s"
                    break
                if res.returncode != 0:
                    pt.status = f"EXIT_{res.returncode}: {res.stderr.strip()[:120]}"
                    break
                try:
                    pt.samples.append(float(res.stdout.strip()))
                except ValueError:
                    pt.status = f"BAD_OUTPUT: {res.stdout.strip()[:120]}"
                    break

            if pt.status != "OK":
                logger.error("  %s", pt.status)
            writer.writerow(pt.row(exp.items))
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
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args(argv)

    logging.basicConfig(
        level=logging.DEBUG if args.verbose else logging.INFO,
        format="%(asctime)s [%(levelname)s] %(message)s",
        datefmt="%H:%M:%S",
    )

    try:
        bdir = paths.build_dir(args.build_dir)
        exe = bdir / "benchmark"

        if args.list:
            for name in discovery.available(exe):
                print(name)
            return 0

        if not args.config:
            parser.error("a config file is required (or use --list)")

        experiments = schema.load(args.config)
        out_dir = Path(args.out_dir) if args.out_dir else Path(args.config).resolve().parent

        for exp in experiments:
            discovery.validate(exp.queues, exe)

        if args.dry_run:
            for exp in experiments:
                print(f"{exp.name or exp.output_file}: {exp.grid_size} points "
                      f"x {exp.repetitions} reps -> {out_dir / exp.output_file}")
            return 0

        ensure_built(bdir, ["benchmark", "timeTicks"])
        converter = TickConverter(bdir / "timeTicks")

        started = time.time()
        for exp in experiments:
            run_experiment(exp, exe, converter, out_dir)
        logger.info("done in %.1fs", time.time() - started)
        return 0

    except (schema.ConfigError, discovery.UnknownQueue, discovery.DiscoveryFailed,
            paths.BuildNotFound, RuntimeError) as exc:
        logger.critical("%s", exc)
        return 1


if __name__ == "__main__":
    sys.exit(main())
