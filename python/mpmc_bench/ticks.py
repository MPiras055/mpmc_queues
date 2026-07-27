"""Converting nanosecond delays into the spin counts the benchmark expects.

The benchmark's simulated work is measured in ticks, not time, so a configuration
expressed in nanoseconds has to be calibrated against this machine via the ``timeTicks``
helper.
"""

from __future__ import annotations

import logging
import re
import subprocess
from pathlib import Path

logger = logging.getLogger(__name__)

__all__ = ["TickConverter"]


class TickConverter:
    def __init__(self, executable: str | Path, tolerance: float = 0.01, samples: int = 100):
        self.exe = str(executable)
        self.tolerance = tolerance
        self.samples = samples
        self._cache: dict[int, int] = {0: 0}

    def ticks_for(self, ns: int) -> int:
        """Spin count approximating @p ns on this machine. Cached; 0 maps to 0."""
        if ns in self._cache:
            return self._cache[ns]

        cmd = [self.exe, str(ns), str(self.tolerance), str(self.samples)]
        try:
            out = subprocess.run(
                cmd, capture_output=True, text=True, check=True, timeout=120
            ).stdout.strip()
        except (subprocess.CalledProcessError, subprocess.TimeoutExpired, FileNotFoundError) as exc:
            # Calibration failing is not fatal, but it must not masquerade as "no delay":
            # silently returning 0 would turn a delayed experiment into an undelayed one
            # and the CSV would not show it.
            raise RuntimeError(
                f"tick calibration failed for {ns}ns using {self.exe}: {exc}"
            ) from exc

        ticks = int(out) if out.isdigit() else self._last_integer(out)
        if ticks is None:
            raise RuntimeError(f"timeTicks produced no number for {ns}ns: {out!r}")

        self._cache[ns] = ticks
        logger.debug("calibrated %d ns -> %d ticks", ns, ticks)
        return ticks

    @staticmethod
    def _last_integer(text: str) -> int | None:
        matches = re.findall(r"\d+", text)
        return int(matches[-1]) if matches else None
