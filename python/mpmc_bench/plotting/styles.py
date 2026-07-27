"""Plot styling that does not need maintaining.

The previous ``STYLE_MAP`` hardcoded five queue names. The registry now holds far more,
and the names changed shape entirely (``u-prq``, ``chunk-faaarray``, ``mem-scq``), so
every implementation added since plotted as an unstyled default -- or not at all.

Styles are therefore *derived* from the name: a stable hash picks a colour and marker, so
any implementation plots sensibly the day it is registered. A small override table exists
only for names worth presenting with a particular label or colour.
"""

from __future__ import annotations

from dataclasses import dataclass
from hashlib import blake2b

__all__ = ["Style", "style_for", "pretty_label"]

# Colour-blind-safe qualitative palette (Okabe-Ito), which the default matplotlib cycle
# is not.
_PALETTE = [
    "#0072B2",  # blue
    "#D55E00",  # vermillion
    "#009E73",  # bluish green
    "#CC79A7",  # reddish purple
    "#E69F00",  # orange
    "#56B4E9",  # sky blue
    "#F0E442",  # yellow
    "#000000",  # black
]
_MARKERS = ["o", "s", "^", "v", "D", "P", "X", "*"]
_LINESTYLES = ["-", "--", "-.", ":"]

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


@dataclass(frozen=True)
class Style:
    label: str
    color: str
    marker: str
    linestyle: str


def _slot(name: str, n: int) -> int:
    """Stable across runs and machines -- unlike hash(), which is salted per process."""
    digest = blake2b(name.encode(), digest_size=8).digest()
    return int.from_bytes(digest, "big") % n


def pretty_label(name: str) -> str:
    """Human-readable name, derived from the registry naming scheme."""
    if name in _LABEL_OVERRIDES:
        return _LABEL_OVERRIDES[name]
    for prefix, family in _FAMILY_PREFIX.items():
        if name.startswith(prefix):
            segment = name[len(prefix) :]
            return f"{family} / {_LABEL_OVERRIDES.get(segment, segment.upper())}"
    return name


def style_for(name: str) -> Style:
    """A stable, distinct style for any implementation name."""
    return Style(
        label=pretty_label(name),
        color=_PALETTE[_slot(name, len(_PALETTE))],
        marker=_MARKERS[_slot(name + "m", len(_MARKERS))],
        # Vary linestyle by family so bounded variants of one segment stay
        # distinguishable when they land on the same colour.
        linestyle=_LINESTYLES[_slot(name.split("-")[0], len(_LINESTYLES))],
    )
