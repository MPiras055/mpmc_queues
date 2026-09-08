"""Light and dark chart themes, applied through matplotlib rcParams.

Two things make this more than a list of colours.

First, the chart body is written against *roles* -- ``surface``, ``text_primary``,
``grid``, ``series[]`` -- rather than raw hex, so changing mode changes one object
instead of every call site.

Second, dark is **selected, not inverted**: each mode's eight series steps were
chosen for that mode's surface and validated against it. Flipping the light values
would leave hues tuned for ``#fcfcfb`` sitting on ``#1a1a19``.

Measured with the dataviz validator (OKLab dE x100, >=8 CVD target, >=15
normal-vision floor):

===========================  ======================================================
pairlist / mode              result
===========================  ======================================================
adjacent, light              all 8 PASS, worst CVD dE 9.1 (yellow<->aqua, protan)
adjacent, dark               all 8 PASS, worst CVD dE 8.4 (yellow<->aqua, protan)
all-pairs, light             FAIL: green<->orange 3.2 protan, red<->orange 7.1 normal
all-pairs, first 3, light    PASS, worst CVD dE 9.2, normal-vision 24.0
all-pairs, first 3, dark     PASS, worst CVD dE 9.4, normal-vision 20.9
===========================  ======================================================

The first two rows are why the line charts may use all eight hues: they compare
adjacent slots, which is what a legend-ordered line chart puts side by side. The
last three are why :mod:`compare` caps a small-multiple panel at three series --
small multiples put every pair in play at once, and at eight slots red and orange
are 7.1 apart for a reader with *full* colour vision. That is a hard gate; markers
and linestyles do not excuse it.

Light mode carries a contrast WARN: aqua (2.74:1), yellow (2.11:1) and magenta
(2.62:1) measure below 3:1 on the light surface. The relief rule obliges visible
labels or a table view, which is what ``--table`` is for -- it is the relief, not a
convenience.

Re-run the validator for any hex changed here, both modes and both pairlists. A
palette edit that was not re-validated is a regression.
"""

from __future__ import annotations

import os
from dataclasses import dataclass, field

__all__ = ["Theme", "LIGHT", "DARK", "THEMES", "resolve", "apply"]


@dataclass(frozen=True)
class Theme:
    """Chart colours addressed by the job they do, never by hue."""

    name: str
    surface: str          # the plotting area and figure background
    page: str             # the plane the figure sits on, when one is drawn
    text_primary: str     # titles, and anything that must read first
    text_secondary: str   # axis labels, legend text
    text_muted: str       # tick labels, reference lines
    grid: str             # hairline gridlines -- recessive by construction
    axis: str             # spines and the baseline
    series: tuple[str, ...] = field(default=())

    def color(self, slot: int) -> str:
        """Slot -> hex. Raises past the last slot rather than wrapping.

        Wrapping is the failure this whole module exists to prevent: two series in one
        figure sharing a hue is indistinguishable from a bug in the data.
        """
        if not 0 <= slot < len(self.series):
            raise IndexError(
                f"series slot {slot} is outside the {len(self.series)}-colour palette; "
                "hues are never cycled -- fold the extra series into 'Other' or facet"
            )
        return self.series[slot]


LIGHT = Theme(
    name="light",
    surface="#fcfcfb",
    page="#f9f9f7",
    text_primary="#0b0b0b",
    text_secondary="#52514e",
    text_muted="#898781",
    grid="#e1e0d9",
    axis="#c3c2b7",
    series=(
        "#2a78d6",  # 1 blue
        "#eb6834",  # 2 orange
        "#1baf7a",  # 3 aqua      -- 2.74:1, relief required
        "#eda100",  # 4 yellow    -- 2.11:1, relief required
        "#e87ba4",  # 5 magenta   -- 2.62:1, relief required
        "#008300",  # 6 green
        "#4a3aa7",  # 7 violet
        "#e34948",  # 8 red
    ),
)

DARK = Theme(
    name="dark",
    surface="#1a1a19",
    page="#0d0d0d",
    text_primary="#ffffff",
    text_secondary="#c3c2b7",
    text_muted="#898781",
    grid="#2c2c2a",
    axis="#383835",
    series=(
        "#3987e5",  # 1 blue
        "#d95926",  # 2 orange
        "#199e70",  # 3 aqua
        "#c98500",  # 4 yellow
        "#d55181",  # 5 magenta
        "#008300",  # 6 green -- mode-invariant, it clears both surfaces
        "#9085e9",  # 7 violet
        "#e66767",  # 8 red
    ),
)

THEMES = {LIGHT.name: LIGHT, DARK.name: DARK}

#: Set once in a shell (``export MPMC_PLOT_THEME=dark``) rather than passed to every
#: invocation.
ENV_VAR = "MPMC_PLOT_THEME"


def resolve(name: str | None = None) -> Theme:
    """Pick a theme: explicit argument, then the environment, then light."""
    wanted = name or os.environ.get(ENV_VAR) or LIGHT.name
    try:
        return THEMES[wanted.strip().lower()]
    except KeyError:
        raise ValueError(
            f"unknown theme {wanted!r}; choose from {', '.join(sorted(THEMES))}"
        ) from None


def apply(theme: Theme) -> Theme:
    """Push @p theme into matplotlib's rcParams, and return it.

    Everything text-shaped takes a text token. A series colour never lands on a label:
    that is what keeps identity in the mark and legibility in the ink, and it is most of
    what makes dark mode readable.
    """
    import matplotlib as mpl

    mpl.rcParams.update({
        "figure.facecolor": theme.surface,
        "figure.edgecolor": theme.surface,
        "savefig.facecolor": theme.surface,
        "savefig.edgecolor": theme.surface,
        "axes.facecolor": theme.surface,
        "axes.edgecolor": theme.axis,
        "axes.labelcolor": theme.text_secondary,
        "axes.titlecolor": theme.text_primary,
        "axes.titlesize": "large",
        "axes.titleweight": "medium",
        "axes.linewidth": 0.8,
        # Spines mostly off: the grid already carries the reading, and four boxes drawn
        # around every panel is chrome competing with the data.
        "axes.spines.top": False,
        "axes.spines.right": False,
        "axes.grid": True,
        "axes.grid.axis": "y",
        "grid.color": theme.grid,
        "grid.linewidth": 0.8,
        "grid.alpha": 1.0,
        "xtick.color": theme.axis,
        "ytick.color": theme.axis,
        "xtick.labelcolor": theme.text_muted,
        "ytick.labelcolor": theme.text_muted,
        "xtick.labelsize": "small",
        "ytick.labelsize": "small",
        "text.color": theme.text_primary,
        "legend.frameon": False,
        "legend.labelcolor": theme.text_secondary,
        "legend.fontsize": "small",
        "font.family": "sans-serif",
        # Marks sit above the grid, and the grid sits below everything.
        "axes.axisbelow": True,
        "lines.solid_capstyle": "round",
        # Explicit colours are passed at every call site; this only decides where a
        # stray unstyled call lands, and in-palette beats matplotlib's default C0..C9.
        "axes.prop_cycle": mpl.cycler(color=list(theme.series)),
    })
    return theme


def style_axes(ax, theme: Theme, *, grid_axis: str = "y") -> None:
    """Dress one Axes in @p theme, without relying on :func:`apply` having run.

    ``apply`` sets process-wide defaults, which is right for the CLI; this makes a single
    Axes correct on its own, which is what the plot functions and the comparison panels
    need when they are called as a library.
    """
    ax.figure.set_facecolor(theme.surface)
    ax.set_facecolor(theme.surface)
    ax.grid(True, axis=grid_axis, color=theme.grid, linewidth=0.8, alpha=1.0)
    ax.set_axisbelow(True)
    for side in ("top", "right"):
        ax.spines[side].set_visible(False)
    for side in ("left", "bottom"):
        ax.spines[side].set_color(theme.axis)
        ax.spines[side].set_linewidth(0.8)
    ax.tick_params(colors=theme.axis, labelcolor=theme.text_muted, labelsize="small")
    ax.xaxis.label.set_color(theme.text_secondary)
    ax.yaxis.label.set_color(theme.text_secondary)
    ax.title.set_color(theme.text_primary)
