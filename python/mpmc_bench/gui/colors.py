"""Separation checks for colours a user picks by hand.

The themes ship a validated palette, but the UI lets any series take any colour, and a
hand-picked pair can be indistinguishable. This is a port of the dataviz validator's
distance metric -- OKLab Delta E x100, Machado 2009 CVD simulation -- so the app can say
*which* pair collides and by how much, instead of leaving it to eyeballing.

It warns; it does not refuse. Choosing colours is the point of the tool.
"""

from __future__ import annotations

import math
import re
from dataclasses import dataclass
from itertools import combinations

__all__ = ["delta_e", "contrast", "Clash", "check_colors",
           "NORMAL_FLOOR", "CVD_FLOOR", "CVD_TARGET", "CONTRAST_MIN"]

#: Below this, two colours are hard to tell apart even with full colour vision.
NORMAL_FLOOR = 15.0
#: Below the target the pair needs secondary encoding (marker/linestyle); below the floor
#: it fails even with it.
CVD_TARGET, CVD_FLOOR = 8.0, 6.0
CONTRAST_MIN = 3.0

_MACHADO = {
    "protan": ((0.152286, 1.052583, -0.204868),
               (0.114503, 0.786281, 0.099216),
               (-0.003882, -0.048116, 1.051998)),
    "deutan": ((0.367322, 0.860646, -0.227968),
               (0.280085, 0.672501, 0.047413),
               (-0.011820, 0.042940, 0.968881)),
}

_HEX = re.compile(r"#?[0-9a-fA-F]{6}")


def _lin(h: str) -> tuple[float, float, float]:
    h = h.strip()
    if not _HEX.fullmatch(h):
        raise ValueError(f"not a #rrggbb colour: {h!r}")
    h = h.lstrip("#")
    out = []
    for i in (0, 2, 4):
        c = int(h[i:i + 2], 16) / 255
        out.append(c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4)
    return out[0], out[1], out[2]


def _oklab(r: float, g: float, b: float) -> tuple[float, float, float]:
    l = (0.4122214708 * r + 0.5363325363 * g + 0.0514459929 * b) ** (1 / 3)
    m = (0.2119034982 * r + 0.6806995451 * g + 0.1073969566 * b) ** (1 / 3)
    s = (0.0883024619 * r + 0.2817188376 * g + 0.6299787005 * b) ** (1 / 3)
    return (0.2104542553 * l + 0.7936177850 * m - 0.0040720468 * s,
            1.9779984951 * l - 2.4285922050 * m + 0.4505937099 * s,
            0.0259040371 * l + 0.7827717662 * m - 0.8086757660 * s)


def _simulate(rgb, kind):
    M = _MACHADO[kind]
    return tuple(max(0.0, min(1.0, sum(M[i][j] * rgb[j] for j in range(3)))) for i in range(3))


def delta_e(a: str, b: str, kind: str | None = None) -> float:
    """OKLab distance x100 between two hex colours; @p kind simulates protan/deutan."""
    la, lb = _lin(a), _lin(b)
    if kind:
        la, lb = _simulate(la, kind), _simulate(lb, kind)
    return 100 * math.dist(_oklab(*la), _oklab(*lb))


def contrast(a: str, b: str) -> float:
    """WCAG contrast ratio."""
    def lum(h):
        r, g, bb = _lin(h)
        return 0.2126 * r + 0.7152 * g + 0.0722 * bb
    hi, lo = sorted((lum(a), lum(b)), reverse=True)
    return (hi + 0.05) / (lo + 0.05)


@dataclass(frozen=True)
class Clash:
    """One problem with the colours currently on screen, phrased for a status line."""

    severity: str   # "fail" | "warn"
    message: str


def check_colors(named: dict[str, str], surface: str,
                 only: set[str] | None = None) -> list[Clash]:
    """Every pair that is too close, and every colour too faint against @p surface.

    All pairs, not adjacent ones: a user-coloured legend has no fixed order, so any two
    series can end up side by side. With @p only, a pair is reported only if at least one
    side is in it, and contrast only for those names.
    """
    out: list[Clash] = []
    items = [(n, c) for n, c in named.items() if c]
    for (na, ca), (nb, cb) in combinations(items, 2):
        if only is not None and na not in only and nb not in only:
            continue
        normal = delta_e(ca, cb)
        if normal < NORMAL_FLOOR:
            out.append(Clash("fail", f"'{na}' and '{nb}' are too similar "
                                     f"(ΔE {normal:.1f} < {NORMAL_FLOOR:.0f})"))
            continue
        cvd = min(delta_e(ca, cb, "protan"), delta_e(ca, cb, "deutan"))
        if cvd < CVD_FLOOR:
            out.append(Clash("fail", f"'{na}' and '{nb}' merge for colour-blind readers "
                                     f"(ΔE {cvd:.1f} < {CVD_FLOOR:.0f})"))
        elif cvd < CVD_TARGET:
            out.append(Clash("warn", f"'{na}' and '{nb}' are close for colour-blind readers "
                                     f"(ΔE {cvd:.1f}); keep their markers/lines different"))
    for n, c in items:
        if only is not None and n not in only:
            continue
        ratio = contrast(c, surface)
        if ratio < CONTRAST_MIN:
            out.append(Clash("warn", f"'{n}' is faint on this background ({ratio:.2f}:1)"))
    return out
