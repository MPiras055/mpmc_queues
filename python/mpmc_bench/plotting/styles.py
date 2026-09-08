"""Assigning a colour, a marker and a label to each implementation in a chart.

The previous version derived colour from a hash of the queue name. That kept a queue's
hue stable across charts, but it decided each name's colour without reference to the
others, so two series *in the same figure* could land on the same hue -- with 8 colours
and 6 series, more likely than not. Two identical lines in one chart is not a styling
blemish; it is indistinguishable from a bug in the data.

So colour is now assigned in **fixed order over the series actually present**, which is
the only assignment that cannot collide. The cost is that a queue's hue depends on its
company: add a series and the others can shift. A small table pins the campaign names to
fixed slots so the ones that appear in the writeup keep their colour from chart to chart;
everything else takes the next free slot.

Marker and linestyle vary alongside colour on purpose. They are the secondary encoding
that the CVD floor band asks for, and they are what carries series identity into
greyscale print and into a screenshot someone has recompressed twice.
"""

from __future__ import annotations

import json
import logging
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, Mapping

from .theme import LIGHT, Theme

logger = logging.getLogger(__name__)

__all__ = [
    "Style", "assign_slots", "assign_styles", "style_for", "pretty_label",
    "set_labels", "load_labels", "parse_label_assignments", "MAX_SERIES",
]

#: One slot per palette hue. Past this, hues are not generated and not cycled.
MAX_SERIES = len(LIGHT.series)

_MARKERS = ["o", "s", "^", "D", "v", "P", "X", "*"]
_LINESTYLES = ["-", "--", "-.", ":"]

# The names the campaign is written about. Pinning them costs nothing -- there are eight
# slots and six pins -- and buys a reader the one thing fixed-order assignment gives up:
# PSCQ is the same blue in every figure. Matched against any '-'-separated segment, so
# 'i-u-pscq', 'u-pscq' and 'pscq' all land on the same pin.
_PINNED_SLOTS = {
    "pscq": 0,
    "prq": 1,
    "scq": 2,
    "lfring": 3,
    "faa": 4,
    "hq": 5,
}

# Only for names where a specific presentation matters. Anything absent is derived.
_LABEL_OVERRIDES = {
    "vyukov": "Vyukov (CAS loop)",
    "vyukov-noaba": "Vyukov (ABA-free)",
    "vyukov-dcas": "Vyukov (DCAS)",
    "prq": "PRQ",
    "scq": "SCQ",
    "pscq": "PSCQ",
    "mutex": "Mutex (baseline)",
}

_FAMILY_PREFIX = {
    "u-": "Unbounded",
    "item-": "Item-bounded",
    "chunk-": "Chunk-bounded",
    "mem-": "Pool-bounded",
}

# Set from --label / --labels. Consulted before anything is derived, so a caller can
# rename one series without having to spell out the rest.
_USER_LABELS: dict[str, str] = {}


@dataclass(frozen=True)
class Style:
    label: str
    color: str
    marker: str
    linestyle: str
    slot: int


def set_labels(mapping: Mapping[str, str] | None, *, replace: bool = False) -> dict[str, str]:
    """Install legend-name overrides, merging over whatever is already set."""
    if replace:
        _USER_LABELS.clear()
    if mapping:
        _USER_LABELS.update({str(k): str(v) for k, v in mapping.items()})
    return dict(_USER_LABELS)


def load_labels(path: str | Path) -> dict[str, str]:
    """Read a ``{"queue-name": "Legend text"}`` JSON file."""
    p = Path(path)
    if not p.is_file():
        raise FileNotFoundError(f"label file not found: {p}")
    loaded = json.loads(p.read_text())
    if not isinstance(loaded, dict):
        raise ValueError(f"{p}: expected a JSON object mapping queue name -> label")
    return {str(k): str(v) for k, v in loaded.items()}


def parse_label_assignments(assignments: Iterable[str] | None) -> dict[str, str]:
    """Parse repeated ``--label name=Text`` arguments."""
    out: dict[str, str] = {}
    for item in assignments or ():
        name, sep, label = item.partition("=")
        if not sep or not name.strip():
            raise ValueError(f"--label expects NAME=LABEL, got {item!r}")
        out[name.strip()] = label
    return out


def pretty_label(name: str) -> str:
    """Human-readable name: caller's override, then the table, then the naming scheme."""
    if name in _USER_LABELS:
        return _USER_LABELS[name]
    if name in _LABEL_OVERRIDES:
        return _LABEL_OVERRIDES[name]
    for prefix, family in _FAMILY_PREFIX.items():
        if name.startswith(prefix):
            segment = name[len(prefix):]
            return f"{family} / {_LABEL_OVERRIDES.get(segment, segment.upper())}"
    return name


def _pin_for(name: str) -> int | None:
    """The pinned slot for @p name, if one of the campaign names is in it."""
    for segment in name.split("-"):
        if segment in _PINNED_SLOTS:
            return _PINNED_SLOTS[segment]
    return None


def assign_slots(names: Iterable[str], *, compact: bool = False) -> dict[str, int]:
    """Give every name in one chart its own slot. Sorted, so it is reproducible.

    With @p compact the slots are squeezed down onto 0..n-1 afterwards, keeping their
    relative order. Pins then still decide *which* series is blue, but the figure only
    ever touches the lowest slots -- which is what a caller needs when its guarantee is
    about a leading subset of the palette rather than about adjacent pairs. See
    :mod:`compare`.
    """
    ordered = sorted(dict.fromkeys(str(n) for n in names))
    slots: dict[str, int] = {}
    taken: set[int] = set()

    # Pinned names first, so an unpinned neighbour can never take a pin's slot from under
    # it. Two names can share a pin ('u-pscq' and 'i-u-pscq'); the first in sorted order
    # keeps it and the other falls through to the next free slot, because one figure
    # holding both still may not draw them the same colour.
    for name in ordered:
        pin = _pin_for(name)
        if pin is not None and pin not in taken:
            slots[name] = pin
            taken.add(pin)

    nxt = 0
    for name in ordered:
        if name in slots:
            continue
        while nxt in taken:
            nxt += 1
        slots[name] = nxt
        taken.add(nxt)

    if compact:
        rank = {slot: i for i, slot in enumerate(sorted(slots.values()))}
        slots = {name: rank[slot] for name, slot in slots.items()}

    return slots


def assign_styles(names: Iterable[str], theme: Theme = LIGHT, *,
                  compact: bool = False) -> dict[str, Style]:
    """Styles for the series of one figure, guaranteed distinct up to ``MAX_SERIES``.

    Past eight the overflow is drawn in the muted ink rather than a ninth hue: there is no
    ninth hue that clears the separation floors, and inventing one -- or cycling back to
    the first -- would put two series on the same colour, which is the failure this
    function exists to prevent.
    """
    slots = assign_slots(names, compact=compact)
    if len(slots) > MAX_SERIES:
        overflow = sorted(n for n, s in slots.items() if s >= MAX_SERIES)
        logger.warning(
            "%d series in one chart, but only %d hues clear the separation floors. "
            "%s will be drawn in the muted ink, not a new colour. Fold the tail into an "
            "'Other' series, or facet the chart (--compare) instead.",
            len(slots), MAX_SERIES, ", ".join(overflow),
        )

    styles = {}
    for name, slot in slots.items():
        styles[name] = Style(
            label=pretty_label(name),
            color=theme.color(slot) if slot < MAX_SERIES else theme.text_muted,
            marker=_MARKERS[slot % len(_MARKERS)],
            linestyle=_LINESTYLES[slot % len(_LINESTYLES)],
            slot=slot,
        )
    return styles


def style_for(name: str, theme: Theme = LIGHT) -> Style:
    """The style @p name gets in a chart where it is the only series.

    Convenience for one-off callers. A chart with more than one series must go through
    :func:`assign_styles`, which is the only way the result is collision-free.
    """
    return assign_styles([name], theme)[name]
