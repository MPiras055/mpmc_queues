"""End-to-end against the real binary. Skipped when there is no build tree."""

from __future__ import annotations

import csv
import json

import pytest

from mpmc_bench import discovery, runner, schema
from mpmc_bench.ticks import TickConverter


def test_tooling_and_binary_agree_on_the_name_list(real_build_dir):
    """The single-source-of-truth claim, checked rather than asserted."""
    from subprocess import run
    direct = run([str(real_build_dir / "benchmark"), "--list"],
                 capture_output=True, text=True, check=True).stdout.split()
    assert list(discovery.available(real_build_dir / "benchmark")) == direct


def test_small_grid_runs_clean(real_build_dir, tmp_path):
    names = discovery.available(real_build_dir / "benchmark")
    chosen = [n for n in ("vyukov", "mutex") if n in names] or [names[0]]

    (exp,) = schema.loads({
        "output_file": "out.csv",
        "queues": chosen,
        "queue_sizes": [1024],
        "threads": [[1, 1], [2, 2]],
        "items": 20000,
        "repetitions": 1,
        "timeout_s": 120.0,
    })
    out = runner.run_experiment(
        exp, real_build_dir / "benchmark", TickConverter(real_build_dir / "timeTicks"), tmp_path
    )

    rows = list(csv.DictReader(out.open()))
    assert len(rows) == exp.grid_size
    bad = [r for r in rows if r["Status"] != "OK"]
    assert not bad, f"grid points failed: {[(r['Queue'], r['Status']) for r in bad]}"
    assert all(float(r["Throughput_Mean"]) > 0 for r in rows)


def test_every_registered_implementation_is_runnable(real_build_dir, tmp_path):
    """A registry entry that cannot even run one item is worth catching here."""
    names = discovery.available(real_build_dir / "benchmark")
    (exp,) = schema.loads({
        "output_file": "all.csv", "queues": list(names), "queue_sizes": [256],
        "threads": [[1, 1]], "items": 5000, "repetitions": 1, "timeout_s": 120.0,
    })
    out = runner.run_experiment(
        exp, real_build_dir / "benchmark", TickConverter(real_build_dir / "timeTicks"), tmp_path
    )
    bad = [(r["Queue"], r["Status"]) for r in csv.DictReader(out.open()) if r["Status"] != "OK"]
    assert not bad, f"implementations failed to run: {bad}"


def test_dry_run_needs_no_binary_execution(real_build_dir, tmp_path, capsys):
    cfg = tmp_path / "c.json"
    names = discovery.available(real_build_dir / "benchmark")
    cfg.write_text(json.dumps({"experiments": [{
        "output_file": "x.csv", "queues": [names[0]], "queue_sizes": [1024],
        "threads": [[1, 1]], "items": 100,
    }]}))
    assert runner.main([str(cfg), "--dry-run", "--build-dir", str(real_build_dir)]) == 0
    assert "points" in capsys.readouterr().out
