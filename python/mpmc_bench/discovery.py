"""Which queue implementations exist, according to the binary itself.

This replaces the old ``config.QUEUE_MAP``, which mapped names to the integers 0-4 that
the benchmark's ``switch`` used to accept. That map was a second copy of a list that
lives in C++ (``include/registry/Registry.hpp``), and it went stale twice over: the CLI
became name-based, and the registry grew well past the five queues the map knew about.

Asking the binary means the C++ registry stays the only place implementations are
declared -- including when entries are commented out there, which the map could never
have reflected.
"""

from __future__ import annotations

import subprocess
from functools import lru_cache
from pathlib import Path

from . import paths

__all__ = ["available", "validate", "UnknownQueue", "DiscoveryFailed"]


class DiscoveryFailed(RuntimeError):
    """The benchmark binary could not be asked for its implementation list."""


class UnknownQueue(ValueError):
    """A configuration named an implementation the binary does not provide."""


@lru_cache(maxsize=8)
def _list_names(exe: str) -> tuple[str, ...]:
    try:
        proc = subprocess.run(
            [exe, "--list"], capture_output=True, text=True, timeout=30, check=True
        )
    except FileNotFoundError as exc:
        raise DiscoveryFailed(
            f"benchmark executable not found at {exe}. Build it with "
            f"`cmake --build <build-dir> --target benchmark`."
        ) from exc
    except subprocess.CalledProcessError as exc:
        raise DiscoveryFailed(
            f"`{exe} --list` exited {exc.returncode}. Is the binary older than the "
            f"registry refactor? stderr: {exc.stderr.strip()}"
        ) from exc
    except subprocess.TimeoutExpired as exc:
        raise DiscoveryFailed(f"`{exe} --list` timed out") from exc

    names = tuple(line.strip() for line in proc.stdout.splitlines() if line.strip())
    if not names:
        raise DiscoveryFailed(f"`{exe} --list` printed nothing")
    return names


def available(exe: str | Path | None = None) -> tuple[str, ...]:
    """Names the binary accepts, in registry order. Cached per executable path."""
    path = str(exe) if exe is not None else str(paths.benchmark_exe())
    return _list_names(path)


def validate(names: list[str], exe: str | Path | None = None) -> None:
    """Raise if any requested name is not provided by the binary.

    Called before any measurement starts. Previously an unrecognised name produced a
    row of ``FAILED`` per grid point and the run continued for hours.
    """
    known = set(available(exe))
    unknown = [n for n in names if n not in known]
    if unknown:
        raise UnknownQueue(
            "Unknown queue name(s): "
            + ", ".join(sorted(unknown))
            + "\nAvailable: "
            + ", ".join(available(exe))
        )
