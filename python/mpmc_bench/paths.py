"""Locating the repository and its build tree.

Every path here is derived from this file's own location or from an explicit override.
Nothing is resolved against the current working directory: the previous scripts used
``BUILD_DIR = "../../build"``, which only worked if you happened to have cd'd into
``python/experiments`` first, and silently pointed somewhere else otherwise.
"""

from __future__ import annotations

import os
from pathlib import Path

__all__ = ["repo_root", "build_dir", "benchmark_exe", "timeticks_exe", "BuildNotFound"]


class BuildNotFound(RuntimeError):
    """No configured CMake build tree could be located."""


def repo_root() -> Path:
    """The repository root, i.e. the directory containing ``CMakeLists.txt``."""
    here = Path(__file__).resolve()
    for parent in here.parents:
        if (parent / "CMakeLists.txt").is_file():
            return parent
    # Fall back to the layout we ship: <root>/python/mpmc_bench/paths.py
    return here.parents[2]


def build_dir(override: str | os.PathLike[str] | None = None) -> Path:
    """Locate a configured build tree.

    Search order:

    1. ``override`` argument
    2. ``$MPMC_BUILD_DIR``
    3. the usual candidates under the repository root

    A directory only counts if it contains ``CMakeCache.txt``; otherwise a typo would
    yield a plausible-looking path and the failure would surface much later as a
    missing executable.
    """
    candidates: list[Path] = []
    if override is not None:
        candidates.append(Path(override))
    env = os.environ.get("MPMC_BUILD_DIR")
    if env:
        candidates.append(Path(env))

    root = repo_root()
    candidates += [root / "build", root / "cmake-build-release", root / "cmake-build-debug"]

    for c in candidates:
        if (c / "CMakeCache.txt").is_file():
            return c.resolve()

    raise BuildNotFound(
        "No configured build tree found (looked for CMakeCache.txt in: "
        + ", ".join(str(c) for c in candidates)
        + "). Configure one with `cmake -S . -B build`, or set MPMC_BUILD_DIR."
    )


def benchmark_exe(override: str | os.PathLike[str] | None = None) -> Path:
    return build_dir(override) / "benchmark"


def timeticks_exe(override: str | os.PathLike[str] | None = None) -> Path:
    return build_dir(override) / "timeTicks"
