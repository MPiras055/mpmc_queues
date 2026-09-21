"""A style that follows the implementation, not the file.

``u-pscq`` should be the same colour, marker and name in every figure you make, in every
session -- otherwise two plots of the same experiment cannot be read side by side, which is
the entire point of making two plots. :class:`model.PlotState` stores overrides per *series
key*, which includes the split (``u-pscq-@Size=1024``) and therefore changes when the filters
do; this stores them per **queue**, once, on disk.

Precedence, lowest first: the palette slot the theme assigns, then the library, then an
override the user typed for this exact series in this session. So pinning a colour here never
silently overrules what is in front of you.
"""

from __future__ import annotations

import json
import logging
import os
from dataclasses import asdict, dataclass, fields
from pathlib import Path

from ..gui import model as m

logger = logging.getLogger(__name__)

__all__ = ["StyleLibrary", "Pinned", "default_path"]


def default_path() -> Path:
    """`$XDG_CONFIG_HOME/mpmc-bench/styles.json`, the usual place for per-user settings."""
    base = os.environ.get("XDG_CONFIG_HOME") or str(Path.home() / ".config")
    return Path(base) / "mpmc-bench" / "styles.json"


@dataclass
class Pinned:
    """What is remembered about one implementation. None means "no opinion"."""

    label: str | None = None
    color: str | None = None
    marker: str | None = None
    linestyle: str | None = None
    linewidth: float | None = None

    @property
    def empty(self) -> bool:
        return all(getattr(self, f.name) is None for f in fields(self))


class StyleLibrary:
    """Per-queue styles, loaded once and written on change."""

    def __init__(self, path: Path | str | None = None) -> None:
        self.path = Path(path) if path else default_path()
        self.entries: dict[str, Pinned] = {}
        self.load()

    # -- persistence ---------------------------------------------------------------------

    def load(self) -> None:
        try:
            raw = json.loads(self.path.read_text())
        except FileNotFoundError:
            return
        except (OSError, ValueError) as exc:          # a corrupt file must not stop the app
            logger.warning("ignoring unreadable style library %s: %s", self.path, exc)
            return
        known = {f.name for f in fields(Pinned)}
        self.entries = {str(k): Pinned(**{a: b for a, b in v.items() if a in known})
                        for k, v in raw.get("queues", {}).items() if isinstance(v, dict)}

    def save(self) -> None:
        self.path.parent.mkdir(parents=True, exist_ok=True)
        body = {"version": 1,
                "queues": {k: {a: b for a, b in asdict(v).items() if b is not None}
                           for k, v in sorted(self.entries.items()) if not v.empty}}
        tmp = self.path.with_suffix(".tmp")
        tmp.write_text(json.dumps(body, indent=2))
        tmp.replace(self.path)                        # atomic: never a half-written library

    # -- using it ------------------------------------------------------------------------

    def apply(self, state: m.PlotState, built: m.Built) -> int:
        """Fill in defaults for series the library knows and the user has not touched.

        @return how many series it spoke for.
        """
        used = 0
        for key in built.keys():
            queue = built.by_key()[key].queue
            pinned = self.entries.get(queue)
            if pinned is None:
                continue
            current = state.styles.get(key, m.SeriesStyle())
            changed = False
            for attr in ("label", "color", "marker", "linestyle", "linewidth"):
                value = getattr(pinned, attr)
                if value is not None and getattr(current, attr) is None:
                    setattr(current, attr, value)
                    changed = True
            if changed:
                state.styles[key] = current
                used += 1
        return used

    def remember(self, state: m.PlotState, built: m.Built) -> int:
        """Pin every override currently in @p state, keyed by queue.

        The split is deliberately discarded: a colour chosen while looking at size 1024 is a
        colour for that implementation, not for that one slice of it.
        """
        by_key = built.by_key()
        count = 0
        for key, override in state.styles.items():
            if key not in by_key:
                continue
            entry = self.entries.setdefault(by_key[key].queue, Pinned())
            for attr in ("label", "color", "marker", "linestyle", "linewidth"):
                value = getattr(override, attr)
                if value is not None:
                    setattr(entry, attr, value)
            count += 1
        self.save()
        return count

    def forget(self, queue: str) -> None:
        self.entries.pop(queue, None)
        self.save()

    def clear(self) -> None:
        self.entries.clear()
        self.save()
