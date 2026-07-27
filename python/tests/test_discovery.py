"""Implementation discovery: the binary is the source of truth."""

from __future__ import annotations

import pytest

from mpmc_bench import discovery
from mpmc_bench.discovery import DiscoveryFailed, UnknownQueue


def test_lists_names_from_the_binary(fake_benchmark):
    assert discovery.available(fake_benchmark) == ("vyukov", "mutex", "u-prq")


def test_result_is_cached(fake_benchmark):
    first = discovery.available(fake_benchmark)
    fake_benchmark.unlink()  # a second invocation would now fail
    assert discovery.available(fake_benchmark) == first


def test_validate_accepts_known(fake_benchmark):
    discovery.validate(["vyukov", "u-prq"], fake_benchmark)


def test_validate_names_the_offender_and_the_alternatives(fake_benchmark):
    with pytest.raises(UnknownQueue) as exc:
        discovery.validate(["vyukov", "VyukovBuffer"], fake_benchmark)
    msg = str(exc.value)
    # The old integer-keyed names are exactly what a stale config still contains, so the
    # error has to make the replacement obvious.
    assert "VyukovBuffer" in msg
    assert "vyukov" in msg and "u-prq" in msg


def test_missing_binary_is_actionable(tmp_path):
    with pytest.raises(DiscoveryFailed, match="not found"):
        discovery.available(tmp_path / "does-not-exist")


def test_binary_without_list_support(tmp_path):
    """A binary predating --list exits non-zero; say so rather than reporting no queues."""
    exe = tmp_path / "old"
    exe.write_text("#!/usr/bin/env python3\nimport sys; sys.stderr.write('bad args\\n'); sys.exit(1)\n")
    exe.chmod(0o755)
    with pytest.raises(DiscoveryFailed, match="exited"):
        discovery.available(exe)


def test_binary_printing_nothing(tmp_path):
    exe = tmp_path / "silent"
    exe.write_text("#!/usr/bin/env python3\n")
    exe.chmod(0o755)
    with pytest.raises(DiscoveryFailed, match="printed nothing"):
        discovery.available(exe)


def test_matches_the_real_binary(real_build_dir):
    """The registry and the tooling must not drift apart again."""
    names = discovery.available(real_build_dir / "benchmark")
    assert names, "the real binary listed no implementations"
    assert len(set(names)) == len(names), "duplicate names in the registry"
    assert all(n == n.strip() and " " not in n for n in names)
