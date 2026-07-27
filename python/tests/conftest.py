"""Shared fixtures. A fake benchmark binary lets the runner be tested without C++."""

from __future__ import annotations

import os
import stat
import sys
from pathlib import Path

import pytest

NAMES = ["vyukov", "mutex", "u-prq"]


def _write_exe(path: Path, body: str) -> Path:
    path.write_text("#!/usr/bin/env python3\n" + body)
    path.chmod(path.stat().st_mode | stat.S_IEXEC | stat.S_IXGRP | stat.S_IXOTH)
    return path


@pytest.fixture
def fake_benchmark(tmp_path: Path) -> Path:
    """Stands in for the real binary: honours --list, prints a throughput otherwise."""
    return _write_exe(tmp_path / "benchmark", f"""
import sys
NAMES = {NAMES!r}
if len(sys.argv) == 2 and sys.argv[1] == "--list":
    print("\\n".join(NAMES)); sys.exit(0)
if len(sys.argv) < 6:
    sys.stderr.write("usage\\n"); sys.exit(1)
if sys.argv[1] not in NAMES:
    sys.stderr.write("unknown queue: " + sys.argv[1] + "\\n"); sys.exit(1)
print(1234567.5)
""")


@pytest.fixture
def failing_benchmark(tmp_path: Path) -> Path:
    return _write_exe(tmp_path / "bad", f"""
import sys
NAMES = {NAMES!r}
if len(sys.argv) == 2 and sys.argv[1] == "--list":
    print("\\n".join(NAMES)); sys.exit(0)
sys.stderr.write("segfault-ish\\n"); sys.exit(3)
""")


@pytest.fixture
def garbage_benchmark(tmp_path: Path) -> Path:
    return _write_exe(tmp_path / "garbage", f"""
import sys
NAMES = {NAMES!r}
if len(sys.argv) == 2 and sys.argv[1] == "--list":
    print("\\n".join(NAMES)); sys.exit(0)
print("not-a-number")
""")


@pytest.fixture
def fake_timeticks(tmp_path: Path) -> Path:
    return _write_exe(tmp_path / "timeTicks", """
import sys
ns = int(sys.argv[1]) if len(sys.argv) > 1 else 0
print(ns * 3)
""")


@pytest.fixture(autouse=True)
def _clear_discovery_cache():
    """discovery caches per executable path; tmp paths differ per test but be explicit."""
    from mpmc_bench import discovery
    discovery._list_names.cache_clear()
    yield
    discovery._list_names.cache_clear()


@pytest.fixture
def real_build_dir():
    """The repository's own build tree, or skip."""
    from mpmc_bench import paths
    try:
        bdir = paths.build_dir()
    except paths.BuildNotFound:
        pytest.skip("no configured build tree")
    if not (bdir / "benchmark").exists():
        pytest.skip("benchmark not built")
    return bdir
