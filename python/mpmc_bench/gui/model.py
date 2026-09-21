"""Headless core of the plotting UI: what a CSV contains, what to draw, and drawing it.

Nothing here imports Tk. The window in :mod:`app` owns a :class:`PlotState`, and every
redraw is ``render(figure, frames, state)`` -- so the whole pipeline from CSV to pixels is
testable with the Agg backend, and a saved session reproduces a figure exactly.

## Series identity

A series is an *implementation under one configuration*. When the filters leave exactly
one value of every run parameter, that is just the queue name. When they leave several --
two sizes ticked, say -- rows for the same queue collide at the same x, and the old CLI
averaged them into one line, which silently plots a number nobody measured. Here they are
**split** instead: ``u-pscq`` becomes ``u-pscq @ size 1024`` and ``u-pscq @ size 4096``.
Ticking two sizes is therefore how you compare sizes, not a way to corrupt a line.

The split is decided across all panels at once, so a series has the same key -- and so the
same colour and legend name -- in every panel it appears in.
"""

from __future__ import annotations

import dataclasses
import json
import logging
import math
from dataclasses import dataclass, field, replace
from pathlib import Path
from typing import Any

import numpy as np
import pandas as pd
from matplotlib import ticker
from matplotlib.backends.backend_agg import FigureCanvasAgg
from matplotlib.figure import Figure

from ..plotting import data as dataio
from ..plotting import theme as theming
from ..plotting.styles import MAX_SERIES, assign_slots, pretty_label
from .colors import Clash, check_colors

logger = logging.getLogger(__name__)

# --------------------------------------------------------------------------------------
# What a CSV can contain
# --------------------------------------------------------------------------------------

#: Thread-count columns. ``Total_Threads`` is derived on load.
THREAD_COLUMNS = ["Producers", "Consumers", "Total_Threads"]

#: Run parameters: describe how a row was run, not what it measured.
KNOWN_DIMENSIONS = ["Size", "Pinning", "ProdDelay_NS", "ConsDelay_NS",
                    "ProdDelay_Amp", "ConsDelay_Amp", "Items", "Governor"]

#: Never offered as a metric: identity, bookkeeping, or the error-bar sources of throughput.
_NOT_METRICS = {"Queue", "Status", "Samples", "SegmentCapacity", "Throughput_Mean",
                "Throughput_Median", "Throughput_StdDev", "Throughput_Min", "Throughput_Max",
                *THREAD_COLUMNS, *KNOWN_DIMENSIONS}

AXIS_NAMES = {
    "Producers": "Producers", "Consumers": "Consumers", "Total_Threads": "Total threads",
    "Size": "Queue size", "Pinning": "Pinning", "ProdDelay_NS": "Producer delay (ns)",
    "ConsDelay_NS": "Consumer delay (ns)", "ProdDelay_Amp": "Producer delay amplitude",
    "ConsDelay_Amp": "Consumer delay amplitude", "Items": "Items",
}

_SHORT = {"Size": "size", "ProdDelay_NS": "prod delay", "ConsDelay_NS": "cons delay",
          "ProdDelay_Amp": "prod amp", "ConsDelay_Amp": "cons amp", "Items": "items",
          "Governor": "governor", "Producers": "P", "Consumers": "C"}

KINDS = {"line": "Line", "bars": "Grouped bars", "speedup": "Speedup (scalability)"}
ERRORBARS = {"stddev": "± standard deviation", "minmax": "min – max", "none": "none"}
X_TICK_MODES = {"auto": "Automatic", "all": "Every data value", "every": "Every Nth value",
                "step": "Fixed spacing", "hidden": "Hidden"}
Y_TICK_MODES = {"auto": "Automatic", "step": "Fixed spacing", "count": "About N ticks",
                "hidden": "Hidden"}
LEGEND_PLACES = {"auto": "Automatic", "best": "Inside (best)", "right": "Outside right",
                 "bottom": "Below", "top": "Above", "none": "None"}
GRIDS = {"y": "Horizontal", "both": "Both", "none": "None"}
MARKERS = {"o": "circle", "s": "square", "^": "triangle", "D": "diamond", "v": "triangle down",
           "P": "plus", "X": "cross", "*": "star", "": "none"}
LINESTYLES = {"-": "solid", "--": "dashed", "-.": "dash-dot", ":": "dotted", "": "no line"}

_DEFAULT_MARKERS = ["o", "s", "^", "D", "v", "P", "X", "*"]
_DEFAULT_LINES = ["-", "--", "-.", ":"]


@dataclass(frozen=True)
class Metric:
    """Something that can go on the y axis."""

    key: str
    name: str
    ylabel: str
    scale: float = 1.0
    unit_prefix: str = ""           # prepended to ylabel when the scale is the default
    has_errors: bool = False


_BUILTIN_METRICS = [
    Metric("throughput", "Throughput", "ops/sec", 1e6, "Millions of ", has_errors=True),
    Metric("SlotEfficiency", "Slot efficiency", "Slot efficiency  i / (S·n)"),
    Metric("segments_per_item", "Segments per item", "Segments per item  S / i"),
    Metric("Segments", "Segments linked", "Segments"),
    Metric("WastedSlots", "Wasted slots", "Wasted slots"),
    Metric("WastedPerSegment", "Wasted slots per segment", "Wasted slots per segment"),
]


def _numeric(s: pd.Series) -> pd.Series:
    return pd.to_numeric(s, errors="coerce")


def _metric_column(df: pd.DataFrame, key: str, stat: str) -> pd.Series | None:
    """The raw y values of @p key, or None if this frame cannot produce them."""
    if key == "throughput":
        return df[dataio.stat_column(df, stat)]
    if key == "segments_per_item":
        if "Segments" not in df or "Produced" not in df:
            return None
        return _numeric(df["Segments"]) / _numeric(df["Produced"])
    if key in df.columns:
        return _numeric(df[key])
    return None


def available_metrics(frames: list[pd.DataFrame]) -> list[Metric]:
    """Metrics with real data in at least one frame.

    Built-ins first, then **any other numeric column** that carries data and is not constant
    -- so a column added to the runner later shows up without touching this file, while
    ``Produced`` (always equal to the item count) stays out of the menu.
    """
    out: list[Metric] = []
    for m in _BUILTIN_METRICS:
        if any((col := _metric_column(f, m.key, "median")) is not None and col.notna().any()
               for f in frames):
            out.append(m)
    known = {m.key for m in _BUILTIN_METRICS}
    seen: set[str] = set()
    for f in frames:
        for c in f.columns:
            if c in _NOT_METRICS or c in known or c in seen or c.startswith("_"):
                continue
            values = _numeric(f[c]).dropna()
            if len(values) and values.nunique() > 1:
                seen.add(c)
                out.append(Metric(c, c.replace("_", " "), c.replace("_", " ")))
    return out


def metric_by_key(key: str, frames: list[pd.DataFrame]) -> Metric:
    for m in _BUILTIN_METRICS:
        if m.key == key:
            return m
    return Metric(key, key, key.replace("_", " "))


def _sort_key(v: str):
    try:
        return (0, float(v), v)
    except ValueError:
        return (1, 0.0, v)


def dimension_values(frames: list[pd.DataFrame]) -> dict[str, list[str]]:
    """Every run parameter present, with its distinct values across all frames, as strings.

    Strings because that is how they round-trip through a session file and a checkbox;
    filtering compares ``astype(str)`` on the frame side, so ``1024`` and ``True`` match.
    """
    cols: list[str] = []
    for f in frames:
        extra = [c for c in f.columns
                 if c not in _NOT_METRICS and not pd.api.types.is_numeric_dtype(f[c])
                 and not pd.api.types.is_bool_dtype(f[c]) and not c.startswith("_")]
        for c in [*THREAD_COLUMNS[:2], *KNOWN_DIMENSIONS, *extra]:
            if c in f.columns and c not in cols:
                cols.append(c)
    out: dict[str, list[str]] = {}
    for c in cols:
        values: set[str] = set()
        for f in frames:
            if c in f.columns:
                values.update(f[c].dropna().astype(str).tolist())
        out[c] = sorted(values, key=_sort_key)
    return out


def x_candidates(frames: list[pd.DataFrame]) -> list[str]:
    """Columns worth using as the x axis: thread counts, then numeric parameters that vary."""
    dims = dimension_values(frames)
    out = [c for c in THREAD_COLUMNS if any(c in f.columns for f in frames)]
    for c, values in dims.items():
        if c in out or len(values) < 2 or c.endswith("_Amp"):
            continue
        if all(_sort_key(v)[0] == 0 for v in values):
            out.append(c)
    return out


def default_filters(dims: dict[str, list[str]],
                    kwargs_links: dict[str, dict[str, dict[str, str]]] | None = None
                    ) -> dict[str, list[str]]:
    """A first plot that is one clean line per queue.

    Thread counts keep every value -- they are what the x axis sweeps. Everything else takes
    one value: pinned if that was measured, no delay if that was measured, else the first.

    Columns that follow another one -- delay amplitude follows the delay -- need no special
    case here: correlations() finds them, and propagate() keeps them consistent.
    """
    links = kwargs_links or {}
    out: dict[str, list[str]] = {}
    for c, values in dims.items():
        if c in THREAD_COLUMNS:
            out[c] = list(values)
        elif c == "Pinning" and "True" in values:
            out[c] = ["True"]
        elif c.endswith("Delay_NS") and "0" in values:
            out[c] = ["0"]
        elif values:
            out[c] = [values[0]]
    # One pass of the links, so the defaults are consistent from the very first plot: picking
    # "no delay" must also pick the amplitude that was measured beside it.
    for col in list(out):
        propagate(out, col, links)
    return out


# --------------------------------------------------------------------------------------
# State
# --------------------------------------------------------------------------------------

@dataclass
class Panel:
    """One loaded CSV. Loaded and *plotted* are different things.

    Files stay in the library, parsed and cached, whether or not they are on screen, so
    switching between them is a tick box rather than a reload.
    """

    csv: str
    #: None = automatic (the file name when there is more than one plot).
    title: str | None = None
    #: Drawn as one of the plots. Untick to keep the file loaded but off the figure.
    plotted: bool = True


@dataclass
class SeriesStyle:
    """User overrides for one series. None always means "use the default"."""

    label: str | None = None
    color: str | None = None
    marker: str | None = None
    linestyle: str | None = None
    linewidth: float | None = None
    visible: bool = True


@dataclass
class TickSpec:
    mode: str = "auto"
    value: float = 1.0


#: More plots than this and none of them is readable; a cross product of two compared
#: parameters reaches it quickly, so it is a cap rather than a warning.
MAX_PLOTS = 12

#: How several ticked values of one run parameter are shown.
COMPARE_MODES = {"one": "one value at a time", "facet": "a plot each", "overlay": "one plot"}


@dataclass(frozen=True)
class Slot:
    """One plot of the figure: a file, narrowed by the facet values that name it.

    Panels and plots stopped being the same thing when a compared parameter began spawning
    a plot per value. A panel is a *file*; a slot is a *plot*. ``Built.panels`` is indexed by
    slot, and so is every per-plot setting.
    """

    #: Index into PlotState.panels -- the file this plot draws from.
    panel: int
    csv: str
    #: (column, value) pairs this plot is narrowed to; empty when nothing is faceted.
    facet: tuple[tuple[str, str], ...] = ()

    @property
    def ident(self) -> str:
        """Key for this plot's own settings. Stable across rebuilds and sessions.

        The file path and the facet rather than the position: reordering the files must not
        move one plot's axis overrides onto another plot.
        """
        return self.csv + "".join(f"|{c}={v}" for c, v in self.facet)


@dataclass(frozen=True)
class AxesSpec:
    """What one plot actually draws with: the figure's settings under that plot's overrides."""

    title: str = ""
    xlabel: str | None = None
    ylabel: str | None = None
    xlog: bool = False
    ylog: bool = False
    ymin: float | None = None
    ymax: float | None = None
    xticks: TickSpec = field(default_factory=TickSpec)
    yticks: TickSpec = field(default_factory=TickSpec)
    grid: str = "y"
    legend: str = "auto"


#: Fields of AxesSpec a single plot may override. Everything else in the Axes tab -- the
#: plot grid, the shared axes, the export size -- is a property of the figure, not of a plot.
OVERRIDABLE = ("title", "xlabel", "ylabel", "xlog", "ylog", "ymin", "ymax",
               "xticks", "yticks", "grid", "legend")

#: Overrides that a shared axis makes meaningless: linked plots have one range and one scale.
_NEEDS_UNSHARED_Y = ("ylog", "ymin", "ymax")
_NEEDS_UNSHARED_X = ("xlog",)


def _as_tickspec(value) -> TickSpec:
    """A TickSpec however it arrived -- a session file holds it as a plain dict."""
    if isinstance(value, TickSpec):
        return value
    if isinstance(value, dict):
        return TickSpec(**{k: v for k, v in value.items() if k in ("mode", "value")})
    return TickSpec()


@dataclass
class PlotState:
    #: Every loaded CSV. What is drawn is the subset with ``plotted`` set -- see panels.
    library: list[Panel] = field(default_factory=list)
    #: None = automatic (the file name when there is one panel).
    title: str | None = None
    kind: str = "line"
    metric: str = "throughput"
    stat: str = "median"
    x: str = "Total_Threads"
    errorbars: str = "stddev"
    #: Column -> selected values. A column absent here is not filtered.
    filters: dict[str, list[str]] = field(default_factory=dict)
    #: Column -> "one" | "facet" | "overlay"; absent means "one".
    #:
    #: "one" is a radio button: ticking a value unticks the last. "facet" gives each ticked
    #: value its own plot -- comparing two queue sizes should put them side by side, not add
    #: lines to the plot you were reading. "overlay" is the old behaviour, several values as
    #: several lines on one axes, which is what a thread count needs and what the x axis
    #: column is forced to.
    compare: dict[str, str] = field(default_factory=dict)
    #: Slot ident -> the axis fields that plot overrides. A key present means "override";
    #: a dict rather than a dataclass of Nones because ymin=None already means "automatic",
    #: so None cannot also mean "not set".
    overrides: dict[str, dict[str, Any]] = field(default_factory=dict)
    styles: dict[str, SeriesStyle] = field(default_factory=dict)
    theme: str = "light"
    xlabel: str | None = None
    ylabel: str | None = None
    xlog: bool = False
    ylog: bool = False
    #: Divisor for y values; None = the metric's default (1e6 for throughput).
    yscale: float | None = None
    xticks: TickSpec = field(default_factory=TickSpec)
    yticks: TickSpec = field(default_factory=TickSpec)
    #: Off by default: two sweeps over different thread ranges would leave half a panel empty.
    share_x: bool = False
    #: "Normalise the y axis": every panel gets the largest range among them.
    share_y: bool = True
    #: Panels per row; 0 = automatic.
    columns: int = 0
    legend: str = "auto"
    grid: str = "y"
    y_from_zero: bool = True
    ymin: float | None = None
    ymax: float | None = None
    width: float = 10.0
    height: float = 5.5
    dpi: int = 200
    #: Express every series against a reference -- see qt.analysis.apply_baseline. Part of the
    #: state rather than of the window, so a session keeps the comparison it was saved with.
    baseline_mode: str = "off"
    baseline_scope: str = "series"
    baseline_target: str = ""

    # -- sessions ------------------------------------------------------------------------

    def to_json(self) -> str:
        return json.dumps(dataclasses.asdict(self), indent=2)

    @classmethod
    def from_json(cls, text: str) -> "PlotState":
        raw = json.loads(text)
        if not isinstance(raw, dict):
            raise ValueError("a session file must hold a JSON object")
        names = {f.name for f in dataclasses.fields(cls)}
        kw: dict[str, Any] = {k: v for k, v in raw.items() if k in names}
        # "panels" is what sessions written before the library existed called it; every file
        # in such a session was on screen, which is what Panel.plotted defaults to.
        kw["library"] = [Panel(**p) for p in raw.get("library", raw.get("panels", []))]
        kw["styles"] = {k: SeriesStyle(**v) for k, v in raw.get("styles", {}).items()}
        kw["xticks"] = TickSpec(**raw.get("xticks", {}))
        kw["yticks"] = TickSpec(**raw.get("yticks", {}))
        return cls(**kw)

    # -- derived -------------------------------------------------------------------------

    @property
    def panels(self) -> list[Panel]:
        """The files actually drawn, in library order."""
        return [p for p in self.library if p.plotted]

    @property
    def effective_x(self) -> str:
        """Speedup is always against producers: consumers add no production capacity."""
        return "Producers" if self.kind == "speedup" else self.x

    def facet_columns(self) -> list[str]:
        """Columns that spawn a plot per value: compared, several values ticked, not on x."""
        return [c for c, values in self.filters.items()
                if self.compare.get(c) == "facet" and len(values or []) > 1
                and c != self.effective_x]

    def slots(self) -> list[Slot]:
        """The plots of the figure, in drawing order: every file times every facet value.

        File-major, so a run's plots stay together. Capped at MAX_PLOTS -- two compared
        parameters of four values each is thirty-two plots, which is not a figure.
        """
        combos: list[tuple[tuple[str, str], ...]] = [()]
        for column in self.facet_columns():
            combos = [combo + ((column, v),) for combo in combos
                      for v in self.filters[column]]
        out = [Slot(i, p.csv, combo) for i, p in enumerate(self.panels) for combo in combos]
        return out[:MAX_PLOTS]

    def figure_title(self) -> str:
        if self.title is not None:
            return self.title
        return Path(self.panels[0].csv).stem if len(self.panels) == 1 else ""

    def panel_title(self, i: int) -> str:
        """Name of plot @p i. Indexed by *slot*, so a faceted plot says which value it holds."""
        slots = self.slots()
        if not 0 <= i < len(slots):
            return ""
        slot = slots[i]
        panel = self.panels[slot.panel]
        parts = []
        if panel.title is not None:
            parts.append(panel.title)
        elif len(self.panels) > 1:
            parts.append(Path(panel.csv).stem)
        parts += [_describe(c, v) for c, v in slot.facet]
        return " · ".join(x for x in parts if x)

    # -- per-plot axes -------------------------------------------------------------------

    def axes_for(self, i: int) -> AxesSpec:
        """The axis settings plot @p i draws with: the figure's, under that plot's overrides.

        Two of them cannot be honoured while the axis is shared, because a shared axis has
        one range and one scale by definition -- see :func:`ignored_overrides`, which is what
        says so on screen instead of leaving the control looking broken.
        """
        slots = self.slots()
        over = self.overrides.get(slots[i].ident, {}) if 0 <= i < len(slots) else {}
        base = {"title": self.panel_title(i), "xlabel": self.xlabel, "ylabel": self.ylabel,
                "xlog": self.xlog, "ylog": self.ylog, "ymin": self.ymin, "ymax": self.ymax,
                "xticks": self.xticks, "yticks": self.yticks, "grid": self.grid,
                "legend": self.legend}
        for field_name, value in over.items():
            if field_name not in OVERRIDABLE:
                continue
            if self.share_y and field_name in _NEEDS_UNSHARED_Y:
                continue
            if self.share_x and field_name in _NEEDS_UNSHARED_X:
                continue
            base[field_name] = value
        base["xticks"] = _as_tickspec(base["xticks"])
        base["yticks"] = _as_tickspec(base["yticks"])
        return AxesSpec(**base)

    def ignored_overrides(self) -> list[str]:
        """Per-plot settings a shared axis is currently overruling, as sentences."""
        out = []
        for ident, over in self.overrides.items():
            blocked = [f for f in over
                       if (self.share_y and f in _NEEDS_UNSHARED_Y)
                       or (self.share_x and f in _NEEDS_UNSHARED_X)]
            if blocked:
                axis = "y" if blocked[0] in _NEEDS_UNSHARED_Y else "x"
                out.append(f"{Path(ident.split('|')[0]).stem}: {', '.join(sorted(blocked))} "
                           f"ignored while the {axis} axis is shared across plots")
        return out

    def legend_place(self, i: int) -> str:
        """Where plot @p i's legend goes.

        Once any plot has its own, only the plots that *asked* get one: repeating the same
        ten names beside every plot is not a legend, it is wallpaper.
        """
        if not self.per_plot_legends:
            return self.legend
        slots = self.slots()
        over = self.overrides.get(slots[i].ident, {}) if 0 <= i < len(slots) else {}
        return over.get("legend", "none")

    @property
    def per_plot_legends(self) -> bool:
        """One legend per plot, because at least one plot asked for its own placement.

        All or nothing: a figure-wide legend beside a per-plot one lists the same series
        twice, which is worse than either.
        """
        return any("legend" in o for o in self.overrides.values())


def data_signature(state: PlotState) -> tuple:
    """Everything build_series() reads. Equal signatures mean the Built can be reused."""
    return (tuple(s.ident for s in state.slots()), state.kind, state.metric, state.stat,
            state.effective_x, state.errorbars,
            tuple(sorted((k, tuple(v)) for k, v in state.filters.items())))


def structure_signature(state: PlotState) -> tuple:
    """Everything a redraw needs *rebuilt* for -- that is, all of the state except the
    cosmetic per-series fields.

    Colour, marker, line style and width can be pushed onto artists that already exist (see
    restyle). Two things cannot, and both are here:

    - **visibility**, because hiding a series changes the autoscaled limits and the legend;
    - **the legend name**, because its text extents move the legend box. Measured: renaming
      one series through restyle left 14k pixels of the legend different from a full render,
      while every other cosmetic property was pixel-identical.
    """
    hidden = tuple(sorted(k for k, o in state.styles.items() if not o.visible))
    labels = tuple(sorted((k, o.label) for k, o in state.styles.items() if o.label))
    titles = tuple(state.panel_title(i) for i in range(len(state.slots())))
    per_plot = tuple(sorted((k, tuple(sorted((f, str(v)) for f, v in o.items())))
                            for k, o in state.overrides.items()))
    return (data_signature(state), state.theme, state.xlabel, state.ylabel, state.xlog,
            state.ylog, state.yscale, (state.xticks.mode, state.xticks.value),
            (state.yticks.mode, state.yticks.value), state.share_x, state.share_y,
            state.columns, state.legend, state.grid, state.y_from_zero, state.ymin, state.ymax,
            titles, state.figure_title(), hidden, labels, per_plot)


def cosmetic_signature(state: PlotState) -> tuple:
    """Every per-series override, including the ones restyle() can push onto live artists.

    structure_signature() deliberately ignores colour and marker; this does not. Together
    they say which of the three redraw paths a request needs: same structure *and* same
    cosmetics means nothing about the picture changed, so only the viewport can have moved
    and the previous raster can simply be re-cropped.
    """
    return tuple(sorted((k, dataclasses.astuple(o)) for k, o in state.styles.items()))


# --------------------------------------------------------------------------------------
# Zooming and panning the rendered image
# --------------------------------------------------------------------------------------
#
# This is a *view* concern, not part of PlotState: it never reaches a saved session or an
# export, because what you exported should not depend on where you happened to be looking.
#
# Zoom magnifies pixels rather than narrowing the axis limits, which is what zooming an
# image means -- text grows with the plot, and no autoscale, tick or shared-axis decision is
# disturbed. To stay sharp the figure is redrawn at a higher DPI rather than upsampled, but
# only at powers of two (see raster_zoom), so a wheel step usually re-crops a raster that is
# already in hand instead of paying for a draw.

#: Beyond 8x a benchmark plot is individual antialiased pixels; there is nothing left to see.
MAX_ZOOM = 8.0

#: Ceiling on the supersampled raster. 32 MP is ~128 MB of RGBA while Agg draws it and 96 MB
#: kept as RGB afterwards; past that the memory buys less than the softness costs.
RASTER_BUDGET_PX = 32_000_000


@dataclass(frozen=True)
class Viewport:
    """Which part of the figure the image view shows.

    @p cx and @p cy are the centre in normalised figure coordinates -- (0.5, 0.5) is the
    middle, y measured downwards from the top, matching the raster.
    """

    zoom: float = 1.0
    cx: float = 0.5
    cy: float = 0.5

    @property
    def whole(self) -> bool:
        return self.zoom <= 1.0 + 1e-9

    def clamped(self) -> "Viewport":
        """Inside [1, MAX_ZOOM], and never showing anything past the edge of the figure."""
        z = min(max(self.zoom, 1.0), MAX_ZOOM)
        half = 0.5 / z
        return Viewport(z, min(max(self.cx, half), 1 - half),
                        min(max(self.cy, half), 1 - half))

    def zoomed(self, factor: float, ax: float = 0.5, ay: float = 0.5) -> "Viewport":
        """Scale by @p factor about (@p ax, @p ay), given as fractions of the *view*.

        The point under the cursor stays under the cursor, which is what makes wheel zoom
        feel like a magnifying glass rather than a slider.
        """
        old = self.clamped()
        z = min(max(old.zoom * factor, 1.0), MAX_ZOOM)
        fx = old.cx + (ax - 0.5) / old.zoom      # figure coordinate under the anchor
        fy = old.cy + (ay - 0.5) / old.zoom
        return Viewport(z, fx - (ax - 0.5) / z, fy - (ay - 0.5) / z).clamped()

    def panned(self, dx: float, dy: float) -> "Viewport":
        """Drag by (@p dx, @p dy) fractions of the view; the image follows the pointer."""
        old = self.clamped()
        return Viewport(old.zoom, old.cx - dx / old.zoom, old.cy - dy / old.zoom).clamped()


def raster_zoom(zoom: float, width: int, height: int) -> float:
    """The supersample to actually draw at for a view zoom of @p zoom.

    Powers of two, so zooming 1.0 -> 1.2 -> 1.6 -> 2.0 draws once and re-crops three times;
    and never more than RASTER_BUDGET_PX, so a big window cannot ask for a gigabyte. Above
    that ceiling the crop is interpolated up instead, which is soft but bounded.
    """
    z = 1.0
    while z < zoom - 1e-9 and z < MAX_ZOOM:
        nxt = z * 2
        if (width * nxt) * (height * nxt) > RASTER_BUDGET_PX:
            break
        z = nxt
    return z


def viewport_pixels(raster: np.ndarray, vp: Viewport, width: int, height: int) -> np.ndarray:
    """The (@p height, @p width, 3) region of @p raster that @p vp is looking at.

    When the raster was drawn at exactly this zoom the region is a whole number of pixels
    and this is a plain slice -- the case that makes panning cost nothing. Otherwise it is
    bilinear, which is either a downscale from a supersampled draw (an improvement) or the
    soft upscale past the budget.
    """
    vp = vp.clamped()
    rh, rw = raster.shape[:2]
    half = 0.5 / vp.zoom
    x0, x1 = (vp.cx - half) * rw, (vp.cx + half) * rw
    y0, y1 = (vp.cy - half) * rh, (vp.cy + half) * rh
    if abs((x1 - x0) - width) < 0.5 and abs((y1 - y0) - height) < 0.5:
        x, y = int(round(x0)), int(round(y0))
        x = min(max(x, 0), max(rw - width, 0))
        y = min(max(y, 0), max(rh - height, 0))
        return raster[y:y + height, x:x + width]
    return _bilinear(raster, x0, y0, x1, y1, width, height)


def _bilinear(src: np.ndarray, x0: float, y0: float, x1: float, y1: float,
              width: int, height: int) -> np.ndarray:
    """Bilinear resample of ``src[y0:y1, x0:x1]`` into a (height, width, 3) image.

    Three things keep this near the cost of a copy, which is what a pan has to be.

    It slices the source down to the part in view first: at 4x supersampling the raster is
    25 MP and a tenth of it is on screen, and every gather below then pays only for that.

    It is **separable, vertically first**. Gathering with two index arrays at once costs
    47 ms a pass here, against 27 ms along the columns and 1 ms along the rows -- row
    selection is a contiguous copy. So it blends rows first, which is nearly free and
    immediately cuts the image to its final height, and only then pays for the column
    gather, on a quarter as many rows as the raster had.

    And it interpolates in 8.8 fixed point rather than float32: the weights sum to 256 and
    the inputs are bytes, so ``255 * 256`` is the largest product and uint16 holds it.
    """
    sh, sw = src.shape[:2]
    xs = x0 + (np.arange(width) + 0.5) * (x1 - x0) / width - 0.5
    ys = y0 + (np.arange(height) + 0.5) * (y1 - y0) / height - 0.5
    xi = np.clip(np.floor(xs), 0, sw - 2).astype(np.intp)
    yi = np.clip(np.floor(ys), 0, sh - 2).astype(np.intp)
    wx = (np.clip(xs - xi, 0.0, 1.0) * 256).astype(np.uint16)[None, :, None]
    wy = (np.clip(ys - yi, 0.0, 1.0) * 256).astype(np.uint16)[:, None, None]

    x_lo, y_lo = int(xi[0]), int(yi[0])          # both are monotonic, so these are the mins
    sub = src[y_lo:int(yi[-1]) + 2, x_lo:int(xi[-1]) + 2]

    r = yi - y_lo
    top = sub[r].astype(np.uint16)
    mid = (((top * (256 - wy) + sub[r + 1].astype(np.uint16) * wy + 128) >> 8)
           .astype(np.uint8))                    # back to bytes: the column gather is the
                                                 # expensive pass and uint16 would double it
    cols = xi - x_lo
    left = mid[:, cols].astype(np.uint16)
    return (((left * (256 - wx) + mid[:, cols + 1].astype(np.uint16) * wx + 128) >> 8)
            .astype(np.uint8))


# --------------------------------------------------------------------------------------
# Correlated filters
# --------------------------------------------------------------------------------------

def correlations(frames: list[pd.DataFrame]) -> dict[str, dict[str, dict[str, str]]]:
    """Which run parameters move together, read out of the data rather than assumed.

    ``A -> B`` is a link when every value of A appears beside exactly one value of B. In these
    CSVs that finds producer delay <-> consumer delay (they are swept as a pair), delay ->
    amplitude (0 -> 0.0, anything else -> 0.5), and producers <-> consumers in a balanced
    sweep. Load a 1:3 sweep alongside a balanced one and the producers/consumers link
    correctly disappears, because 4 producers then sits beside both 4 and 12 consumers.

    @return ``{A: {B: {value of A: implied value of B}}}``.
    """
    cols = [c for c, v in dimension_values(frames).items() if len(v) > 1]
    out: dict[str, dict[str, dict[str, str]]] = {c: {} for c in cols}
    for a in cols:
        for b in cols:
            if a == b:
                continue
            mapping: dict[str, str] = {}
            functional = True
            for f in frames:
                if a not in f.columns or b not in f.columns:
                    continue
                pairs = f[[a, b]].astype(str).drop_duplicates()
                for av, bv in zip(pairs[a], pairs[b]):
                    if mapping.setdefault(av, bv) != bv:
                        functional = False
                        break
                if not functional:
                    break
            if functional and len(mapping) > 1:
                out[a][b] = mapping
    return out


def propagate(filters: dict[str, list[str]], changed: str,
              links: dict[str, dict[str, dict[str, str]]],
              pinned: set[str] | None = None) -> list[str]:
    """Carry a filter change out to the columns linked to @p changed. Mutates @p filters.

    One step at a time, breadth-first with a visited set: producer delay and consumer delay
    are linked *both* ways, and following that pair naively would not terminate.

    @param pinned columns the link may not rewrite. A column the user has put into compare
           mode holds a selection they made on purpose -- delay implies one amplitude, so
           without this, comparing two amplitudes is undone by the next edit to the delay.
    @return the columns whose selection changed, for the caller to re-tick.
    """
    touched: list[str] = []
    seen = {changed}
    pinned = pinned or set()
    queue = [changed]
    while queue:
        col = queue.pop(0)
        selected = filters.get(col) or []
        for other, mapping in links.get(col, {}).items():
            if other in seen or other not in filters or other in pinned:
                continue
            implied = [mapping[v] for v in selected if v in mapping]
            # Ordered, de-duplicated, and only when it actually says something: an empty
            # implication would silently blank a filter the user cannot see being blanked.
            wanted = list(dict.fromkeys(implied))
            if wanted and wanted != filters[other]:
                filters[other] = wanted
                touched.append(other)
            seen.add(other)
            queue.append(other)
    return touched


# --------------------------------------------------------------------------------------
# Building series
# --------------------------------------------------------------------------------------

@dataclass
class Series:
    key: str
    queue: str
    split: tuple[tuple[str, str], ...]
    x: list[float]
    y: list[float]
    lo: list[float] | None = None
    hi: list[float] | None = None

    @property
    def default_label(self) -> str:
        base = pretty_label(self.queue)
        if not self.split:
            return base
        return f"{base} ({', '.join(_describe(c, v) for c, v in self.split)})"


def _describe(col: str, value: str) -> str:
    if col == "Pinning":
        return "pinned" if value == "True" else "unpinned"
    if col.endswith("Delay_NS"):
        return f"{_SHORT[col]} {value} ns"
    if col in ("Producers", "Consumers"):
        return f"{value}{_SHORT[col]}"
    return f"{_SHORT.get(col, col)} {value}"


def series_key(queue: str, split) -> str:
    # '-'-separated so styles.assign_slots still recognises the campaign name inside it and
    # gives 'u-pscq-@Size=1024' PSCQ's pinned colour.
    return queue + "".join(f"-@{c}={v}" for c, v in split)


@dataclass
class Built:
    panels: list[dict[str, Series]]
    notes: list[str] = field(default_factory=list)
    split_by: list[str] = field(default_factory=list)

    def keys(self) -> list[str]:
        seen: dict[str, None] = {}
        for p in self.panels:
            seen.update(dict.fromkeys(p))
        return list(seen)

    def by_key(self) -> dict[str, Series]:
        out: dict[str, Series] = {}
        for p in self.panels:
            for k, s in p.items():
                out.setdefault(k, s)
        return out


def _filtered(df: pd.DataFrame, filters: dict[str, list[str]]) -> pd.DataFrame:
    for col, wanted in filters.items():
        if col in df.columns and wanted is not None:
            df = df[df[col].astype(str).isin([str(w) for w in wanted])]
    return df


def build_series(frames: list[pd.DataFrame], state: PlotState) -> Built:
    """Filter, split, aggregate. One dict of series per *plot*, in drawing order.

    @param frames one per plotted file, in PlotState.panels order. A plot is a slot, not a
           file: a compared parameter draws the same file several times, once per value.
    """
    x = state.effective_x
    notes: list[str] = []
    parts = []
    slots = state.slots()
    facets = state.facet_columns()
    if facets:
        notes.append("a plot each for " + ", ".join(_SHORT.get(c, c) for c in facets))
        wanted = len(state.panels)
        for c in facets:
            wanted *= len(state.filters[c])
        if wanted > MAX_PLOTS:
            notes.append(f"{wanted} plots asked for; showing the first {MAX_PLOTS}")
    notes += state.ignored_overrides()
    for i, slot in enumerate(slots):
        df = frames[slot.panel] if slot.panel < len(frames) else None
        if df is None:
            continue
        if x not in df.columns:
            notes.append(f"{state.panel_title(i) or f'plot {i + 1}'}: no '{x}' column")
            continue
        df = _filtered(df, state.filters)
        df = _filtered(df, {c: [v] for c, v in slot.facet})
        y = _metric_column(df, state.metric, state.stat)
        if y is None:
            notes.append(f"{Path(slot.csv).name}: has no data for this metric")
            continue
        part = df.assign(_panel=i, _y=_numeric(y), _x=_numeric(df[x].astype(str)
                                                               .replace({"True": "1", "False": "0"})))
        lo = hi = None
        if state.metric == "throughput" and state.kind != "speedup":
            if state.errorbars == "stddev" and "Throughput_StdDev" in df:
                lo = part["_y"] - _numeric(df["Throughput_StdDev"])
                hi = part["_y"] + _numeric(df["Throughput_StdDev"])
            elif state.errorbars == "minmax" and {"Throughput_Min", "Throughput_Max"} <= set(df):
                lo, hi = _numeric(df["Throughput_Min"]), _numeric(df["Throughput_Max"])
        part = part.assign(_lo=lo if lo is not None else float("nan"),
                           _hi=hi if hi is not None else float("nan"))
        parts.append(part.dropna(subset=["_y", "_x"]))

    empty = Built([{} for _ in slots], notes)
    if not parts or all(p.empty for p in parts):
        empty.notes.append("nothing to plot: the filters matched no rows")
        return empty
    rows = pd.concat([p for p in parts if not p.empty], ignore_index=True)

    # Split on every run parameter that still takes more than one value at the same
    # (panel, queue, x). Thread counts other than x are candidates too: plotting against
    # Producers in a sweep with several consumer counts per producer count is exactly that.
    candidates = [c for c in [*dimension_values([rows])] if c != x and c in rows.columns
                  and c != "Total_Threads" and c not in facets]
    split_by = [c for c in candidates
                if rows.groupby(["_panel", "Queue", "_x"])[c].nunique(dropna=False).max() > 1]
    if split_by:
        notes.append("lines split by " + ", ".join(_SHORT.get(c, c) for c in split_by)
                     + " (several values are selected)")

    out: list[dict[str, Series]] = [{} for _ in slots]
    group_cols = ["_panel", "Queue", *split_by]
    for gkey, g in rows.groupby(group_cols, sort=False, dropna=False):
        gkey = gkey if isinstance(gkey, tuple) else (gkey,)
        panel, queue = int(gkey[0]), str(gkey[1])
        split = tuple((c, str(v)) for c, v in zip(split_by, gkey[2:]))
        agg = g.groupby("_x", as_index=False)[["_y", "_lo", "_hi"]].median().sort_values("_x")
        has_err = agg["_lo"].notna().any()
        s = Series(series_key(queue, split), queue, split, agg["_x"].tolist(), agg["_y"].tolist(),
                   agg["_lo"].tolist() if has_err else None,
                   agg["_hi"].tolist() if has_err else None)
        out[panel][s.key] = s

    if state.kind == "speedup":
        _to_speedup(out, notes)

    # Sorted, so the legend and the style table read in a stable order.
    out = [dict(sorted(p.items())) for p in out]
    return Built(out, notes, split_by)


def _to_speedup(panels: list[dict[str, Series]], notes: list[str]) -> None:
    """Normalise each series on the smallest producer count in its panel.

    One baseline per panel, not per series: a series normalised on its own first point
    would start at 1.0 wherever it happened to start, and the ideal line would no longer
    mean the same thing for every line on the chart.
    """
    for panel in panels:
        if not panel:
            continue
        base_x = min(min(s.x) for s in panel.values())
        skipped = []
        for key in list(panel):
            s = panel[key]
            if base_x not in s.x or s.y[s.x.index(base_x)] <= 0:
                skipped.append(s.default_label)
                del panel[key]
                continue
            b = s.y[s.x.index(base_x)]
            s.y = [v / b for v in s.y]
            s.lo = s.hi = None
        if skipped:
            notes.append(f"no {base_x:g}-producer baseline for {', '.join(skipped)}; omitted")


# --------------------------------------------------------------------------------------
# Baselines
# --------------------------------------------------------------------------------------
#
# Expressing a series against a reference is a transform of the data, not a way of drawing
# it, so it belongs here rather than in the window: render() applies it too, and an exported
# figure therefore shows the same ratios the preview did. qt.analysis re-exports these.

#: What a baseline turns the values into.
BASELINE_MODES = ("off", "ratio", "percent")


def _at(xs: list[float], ys: list[float]) -> dict[float, float]:
    return {x: y for x, y in zip(xs, ys)}


def _rescale(s: Series, base: dict[float, float], mode: str) -> Series | None:
    """@p s expressed against @p base, keeping only the x values both have.

    Dropping the rest is deliberate: a ratio at an x the baseline never measured would be a
    number with nothing on the other side of the division.
    """
    x, y, lo, hi = [], [], [], []
    for i, xv in enumerate(s.x):
        b = base.get(xv)
        if b is None or b == 0:
            continue
        conv = (lambda v: v / b) if mode == "ratio" else (lambda v: (v / b - 1.0) * 100.0)
        x.append(xv)
        y.append(conv(s.y[i]))
        if s.lo is not None and s.hi is not None:
            lo.append(conv(s.lo[i]))
            hi.append(conv(s.hi[i]))
    if not x:
        return None
    return replace(s, x=x, y=y, lo=lo or None, hi=hi or None)


def apply_baseline(built: Built, target: str, mode: str, scope: str = "series") -> Built:
    """Express every series against @p target.

    @param target a series key when @p scope is "series", else the index of the baseline
                  plot, as a string.
    @param mode   "off", "ratio" or "percent"; "off" returns @p built untouched.
    @param scope  "series" divides by one series inside each plot -- the usual
                  "how much faster is everything than PSCQ". "panel" divides each series by
                  *itself* in another plot, which is the before/after comparison of two runs.

    The baseline itself is kept, flat at 1.0 or 0%. It is the reference line, and a chart
    that silently dropped it would leave the reader guessing where the axis crosses.
    """
    if mode not in BASELINE_MODES or mode == "off":
        return built
    notes = list(built.notes)
    panels: list[dict[str, Series]] = []

    if scope == "panel":
        try:
            ref = int(target)
        except (TypeError, ValueError):
            return built
        if not 0 <= ref < len(built.panels):
            notes.append(f"baseline plot {target} does not exist; showing absolute values")
            return Built(built.panels, notes, built.split_by)
        base_panel = built.panels[ref]
        for panel in built.panels:
            out: dict[str, Series] = {}
            for key, s in panel.items():
                b = base_panel.get(key)
                if b is None:
                    continue
                r = _rescale(s, _at(b.x, b.y), mode)
                if r is not None:
                    out[key] = r
            panels.append(out)
        missing = sum(len(p) for p in built.panels) - sum(len(p) for p in panels)
        if missing:
            notes.append(f"{missing} series are not in the baseline plot and were dropped")
    else:
        for panel in built.panels:
            b = panel.get(target)
            if b is None:
                panels.append({})
                if panel:
                    notes.append(f"'{target}' is not in every plot; those plots are empty")
                continue
            base = _at(b.x, b.y)
            out = {}
            for key, s in panel.items():
                r = _rescale(s, base, mode)
                if r is not None:
                    out[key] = r
            panels.append(out)

    notes.append("ratio to baseline" if mode == "ratio" else "percent difference from baseline")
    return Built(panels, notes, built.split_by)


def relative_ylabel(mode: str, label: str) -> str:
    if mode == "ratio":
        return "Relative to baseline  (x)"
    if mode == "percent":
        return "Difference from baseline  (%)"
    return label


def baseline_for(built: Built, state: PlotState) -> str:
    """The baseline @p state asks for, if @p built has it -- else "" (absolute values).

    Never a *different* series: a chosen baseline that is briefly missing (switching metric
    can drop one) must come back when it reappears, not be quietly replaced.
    """
    target = state.baseline_target
    if state.baseline_mode == "off" or not target:
        return ""
    if state.baseline_scope == "panel":
        return target if target.isdigit() and int(target) < len(built.panels) else ""
    return target if target in built.keys() else ""


# --------------------------------------------------------------------------------------
# Styles
# --------------------------------------------------------------------------------------

@dataclass(frozen=True)
class Resolved:
    label: str
    color: str
    marker: str
    linestyle: str
    linewidth: float
    visible: bool


def resolve_styles(built: Built, state: PlotState) -> dict[str, Resolved]:
    """Default style for every series, with the user's overrides on top.

    Slots are assigned over **every** series, hidden ones included. Colour follows the
    entity: unticking one implementation must not repaint the others.
    """
    theme = theming.resolve(state.theme)
    by_key = built.by_key()
    slots = assign_slots(by_key)
    out = {}
    for key, slot in slots.items():
        o = state.styles.get(key, SeriesStyle())
        default_color = theme.color(slot) if slot < MAX_SERIES else theme.text_muted
        out[key] = Resolved(
            label=o.label if o.label else by_key[key].default_label,
            color=o.color or default_color,
            marker=o.marker if o.marker is not None else _DEFAULT_MARKERS[slot % 8],
            linestyle=o.linestyle if o.linestyle is not None else _DEFAULT_LINES[slot % 4],
            linewidth=o.linewidth or 2.0,
            visible=o.visible,
        )
    return out


# --------------------------------------------------------------------------------------
# Rendering
# --------------------------------------------------------------------------------------

@dataclass
class Rendered:
    built: Built
    styles: dict[str, Resolved]
    notes: list[str]
    clashes: list[Clash]
    records: list[dict]


def _fmt(v: float) -> str:
    return f"{int(v)}" if float(v).is_integer() else f"{v:g}"


def _apply_x_ticks(ax, spec: TickSpec, values: list[float], log: bool, notes: list[str]) -> None:
    plain = ticker.FuncFormatter(lambda v, _: _fmt(v))
    if spec.mode == "hidden":
        ax.set_xticks([])
        ax.xaxis.set_minor_locator(ticker.NullLocator())
        return
    if log:
        # log2 ticks read as '2^4' by default; the numbers are thread counts, so say 16.
        ax.xaxis.set_major_formatter(plain)
        ax.xaxis.set_minor_locator(ticker.NullLocator())
    if spec.mode == "all" and values:
        ax.set_xticks(values)
    elif spec.mode == "every" and values:
        n = max(1, int(spec.value))
        ax.set_xticks(values[::n])
    elif spec.mode == "step":
        if log:
            notes.append("fixed x spacing does not apply to a log axis; using every value")
            ax.set_xticks(values)
        elif spec.value > 0:
            ax.xaxis.set_major_locator(ticker.MultipleLocator(spec.value))
    if not log:
        ax.xaxis.set_major_formatter(plain)


def _apply_y_ticks(ax, spec: TickSpec, log: bool, notes: list[str]) -> None:
    if spec.mode == "hidden":
        ax.set_yticks([])
        ax.yaxis.set_minor_locator(ticker.NullLocator())
    elif spec.mode == "step":
        if log:
            notes.append("fixed y spacing does not apply to a log axis")
        elif spec.value > 0:
            ax.yaxis.set_major_locator(ticker.MultipleLocator(spec.value))
    elif spec.mode == "count" and spec.value >= 2:
        if not log:
            ax.yaxis.set_major_locator(ticker.MaxNLocator(int(spec.value)))


def ylabel_for(state: PlotState, frames: list[pd.DataFrame], relative: bool = False) -> str:
    """The y axis label.

    @param relative the baseline was actually applied. A flag rather than a look at
           state.baseline_mode, because a baseline that is not in this plot is not applied,
           and labelling those values "relative to baseline" would be a lie.
    """
    if state.ylabel:
        return state.ylabel
    if relative:
        return relative_ylabel(state.baseline_mode, "")
    m = metric_by_key(state.metric, frames)
    if state.kind == "speedup":
        return f"{m.name} speedup vs smallest producer count"
    scale = state.yscale if state.yscale else m.scale
    if scale == m.scale and m.unit_prefix:
        return f"{m.name} ({m.unit_prefix.strip()} {m.ylabel})"
    return m.ylabel if scale == 1 else f"{m.ylabel} (÷ {scale:g})"


def freeze_layout(fig: Figure) -> None:
    """Stop re-running constrained layout on a figure whose positions are already settled.

    Constrained layout is about half the cost of a draw (measured: 136 ms -> 93 ms for one
    panel), and it recomputes the same answer every time unless something moved. Call this
    after the first draw following a render(); render() turns it back on.
    """
    fig.set_layout_engine("none")


def render(fig: Figure, frames: list[pd.DataFrame], state: PlotState,
           built: Built | None = None) -> Rendered:
    """Draw @p state onto @p fig, replacing whatever was there.

    @param built a Built from an earlier call whose data_signature() matches, to skip the
           pandas work. Omit it and the data is rebuilt, which is what the CLI does.
    """
    theme = theming.apply(theming.resolve(state.theme))
    if not hasattr(fig.canvas, "get_renderer"):
        FigureCanvasAgg(fig)   # a bare Figure: legends are measured, which needs a renderer
    fig.clear()
    fig.set_facecolor(theme.surface)
    fig.set_layout_engine("constrained")

    built = build_series(frames, state) if built is None else built
    # Styles first, from the *absolute* build: a baseline can drop a series, and re-resolving
    # afterwards would hand the survivors different colours than the preview gave them.
    styles = resolve_styles(built, state)
    metric = metric_by_key(state.metric, frames)
    scale = 1.0 if state.kind == "speedup" else (state.yscale or metric.scale)
    target = baseline_for(built, state)
    if target:
        built = apply_baseline(built, target, state.baseline_mode, state.baseline_scope)
        scale = 1.0
    notes = list(built.notes)
    if state.baseline_mode != "off" and state.baseline_target and not target:
        notes.append(f"baseline '{state.baseline_target}' is not in this plot; "
                     "showing absolute values")
    ylabel = ylabel_for(state, frames, relative=bool(target))
    bars = state.kind == "bars"
    x = state.effective_x

    n = max(1, len(state.slots()))
    cols = state.columns if state.columns > 0 else min(n, 3)
    rows = math.ceil(n / cols)
    axes = fig.subplots(rows, cols, squeeze=False,
                        sharex=bool(state.share_x and not bars),
                        sharey=bool(state.share_y))
    flat = [a for row in axes for a in row]
    for extra in flat[n:]:
        extra.set_visible(False)

    visible_count = len({k for p in built.panels for k in p if styles[k].visible})
    if visible_count > MAX_SERIES:
        notes.append(f"{visible_count} series visible, but only {MAX_SERIES} palette colours "
                     "clear the separation floors; consider hiding some or splitting panels")

    records: list[dict] = []
    handles: dict[str, Any] = {}
    #: series key -> the artists drawn for it, so restyle() can reach them without guessing.
    artists: dict[str, list] = {}
    if bars and state.xlog:
        notes.append("bars use a categorical x axis; log x is ignored")
    all_x = sorted({v for p in built.panels for s in p.values() if styles[s.key].visible
                    for v in s.x})

    #: per axes: (series key, handle) in drawing order, and any handle that is not a series.
    panel_keys: list[list[tuple[str, Any]]] = []
    extras: list[dict[str, Any]] = []

    for i, ax in enumerate(flat[:n]):
        spec = state.axes_for(i)
        theming.style_axes(ax, theme, grid_axis=spec.grid if spec.grid != "none" else "y")
        if spec.grid == "none":
            ax.grid(False)
        series = [s for s in built.panels[i].values() if styles[s.key].visible] \
            if i < len(built.panels) else []
        panel_x = sorted({v for s in series for v in s.x})
        panel_keys.append([])
        extras.append({})

        if bars:
            cats = all_x if state.share_x else panel_x
            pos = {v: j for j, v in enumerate(cats)}
            width = 0.8 / max(1, len(series))
            for j, s in enumerate(series):
                st = styles[s.key]
                xs = [pos[v] - 0.4 + width * (j + 0.5) for v in s.x]
                ys = [v / scale for v in s.y]
                err = _yerr(s, scale)
                h = ax.bar(xs, ys, width=width, color=st.color, label=st.label, yerr=err,
                           capsize=2, edgecolor=theme.surface, linewidth=1,
                           error_kw={"ecolor": theme.text_muted, "elinewidth": 1})
                handles.setdefault(st.label, h)
                artists.setdefault(s.key, []).append(h)
                panel_keys[-1].append((s.key, h))
                records += _records(state, i, st.label, s, scale)
            ax.set_xticks(range(len(cats)), [_fmt(v) for v in cats])
            if spec.xticks.mode == "hidden":
                ax.set_xticks([])
            elif spec.xticks.mode == "every":
                step = max(1, int(spec.xticks.value))
                ax.set_xticks(range(0, len(cats), step), [_fmt(v) for v in cats[::step]])
        else:
            for s in series:
                st = styles[s.key]
                h = ax.errorbar(s.x, [v / scale for v in s.y], yerr=_yerr(s, scale),
                                label=st.label, color=st.color, marker=st.marker or None,
                                linestyle=st.linestyle or "none", linewidth=st.linewidth,
                                markersize=6, capsize=3, elinewidth=1,
                                markeredgecolor=theme.surface, markeredgewidth=1)
                handles.setdefault(st.label, h)
                artists.setdefault(s.key, []).append(h)
                panel_keys[-1].append((s.key, h))
                records += _records(state, i, st.label, s, scale)
            if state.kind == "speedup" and panel_x:
                b = panel_x[0]
                h = ax.plot(panel_x, [v / b for v in panel_x], color=theme.text_muted,
                            linestyle="--", linewidth=1, label="Ideal (linear)")[0]
                handles.setdefault("Ideal (linear)", h)
                extras[i]["Ideal (linear)"] = h
            if spec.xlog:
                ax.set_xscale("log", base=2)
            _apply_x_ticks(ax, spec.xticks, all_x if state.share_x else panel_x,
                           spec.xlog, notes)
        if spec.ylog:
            ax.set_yscale("log")
        _apply_y_ticks(ax, spec.yticks, spec.ylog, notes)

        ax.set_title(spec.title)
        is_bottom = i + cols >= n
        is_left = i % cols == 0
        # A label the plot asked for itself is drawn wherever the plot is: the tidy-up that
        # leaves the label to the left column only is for the automatic one.
        if is_bottom or spec.xlabel or not (state.share_x and not bars):
            ax.set_xlabel(spec.xlabel or AXIS_NAMES.get(x, x))
        if is_left or spec.ylabel or not state.share_y:
            ax.set_ylabel(spec.ylabel or ylabel)
        if not series:
            ax.text(0.5, 0.5, "no data for this selection", transform=ax.transAxes,
                    ha="center", va="center", color=theme.text_muted)

    # Limits last, once every panel has data: with sharey the autoscaled top already is the
    # maximum across panels, which is what "normalise the y axis" asks for.
    for i, ax in enumerate(flat[:n]):
        spec = state.axes_for(i)
        # A log axis has no zero, so a floor of 0 is dropped rather than passed on --
        # matplotlib would warn and ignore it, which is a warning nobody can act on.
        floor_ok = not spec.ylog
        if spec.ymin is not None or spec.ymax is not None:
            bottom = spec.ymin if (spec.ymin is None or spec.ymin > 0 or floor_ok) else None
            ax.set_ylim(bottom=bottom, top=spec.ymax)
        elif (state.y_from_zero or bars) and floor_ok:
            ax.set_ylim(bottom=0)
        if state.share_y:
            break  # shared: setting one sets them all

    _draw_legends(fig, flat[:n], panel_keys, extras, styles, state, theme)
    title = state.figure_title()
    if title:
        fig.suptitle(title, color=theme.text_primary)

    # Only colours the user chose are checked. The defaults are the validated palette, whose
    # known trade-offs are documented in plotting/theme.py; re-reporting them on every redraw
    # would bury the one warning that matters -- a hand-picked colour that collides.
    shown = {styles[k].label: styles[k].color for p in built.panels for k in p
             if styles[k].visible}
    custom = {styles[k].label for k, o in state.styles.items()
              if o.color and k in styles and styles[k].visible}
    clashes = check_colors(shown, theme.surface, only=custom) if custom else []

    # Kept on the figure rather than in Rendered: Rendered crosses a thread boundary (see
    # gui/service.py) and matplotlib artists must not. The figure never leaves its thread.
    fig._mpmc = _Drawn(artists, list(handles), scale, theme, state.legend,
                       flat[0] if n == 1 else None, flat[:n], panel_keys, extras)
    return Rendered(built, styles, notes, clashes, records)


@dataclass
class _Drawn:
    """What restyle() needs to find its way around a figure render() drew."""

    artists: dict[str, list]
    legend_order: list[str]      # labels, in the order the legend listed them
    scale: float
    theme: Any
    legend: str
    single_ax: Any
    axes: list = field(default_factory=list)
    panel_keys: list = field(default_factory=list)
    extras: list = field(default_factory=list)


def _set_series_style(handle, st: Resolved, theme) -> None:
    """Push one series' cosmetics onto the artists already drawn for it.

    Every property set here is one render() passes at creation time, and nothing else is
    touched -- that correspondence is what test_restyle_matches_a_full_render checks.
    """
    if hasattr(handle, "patches"):                      # a BarContainer
        for patch in handle.patches:
            patch.set_facecolor(st.color)
        handle.patches[0].set_label(st.label)
        return
    if hasattr(handle, "lines"):                        # an ErrorbarContainer
        line, caps, bars = handle.lines
        if line is not None:
            line.set_color(st.color)
            line.set_marker(st.marker or "none")
            line.set_linestyle(st.linestyle or "none")
            line.set_linewidth(st.linewidth)
            line.set_markeredgecolor(theme.surface)
            line.set_label(st.label)
        for cap in caps:
            # A cap is a marker-only Line2D, so its colour is the marker's edge colour --
            # set_color alone leaves it the old hue, which is what the pixel-equality test
            # caught when this was written.
            cap.set_color(st.color)
            cap.set_markeredgecolor(st.color)
        for bar in bars:
            bar.set_color(st.color)
        handle.set_label(st.label)


def restyle(fig: Figure, frames: list[pd.DataFrame], state: PlotState,
            previous: Rendered) -> Rendered:
    """Re-colour and re-label what is already drawn, instead of drawing it again.

    Only legal when structure_signature() is unchanged -- see that function for what "only
    cosmetic" means, and why the legend name is not in that set. Raises if the figure was not
    produced by render(), so a caller that gets the condition wrong fails loudly rather than
    showing a stale plot.

    None of the properties it touches affect any extent, so the caller may leave the layout
    frozen; :func:`freeze_layout` is what does that.
    """
    drawn: _Drawn = getattr(fig, "_mpmc", None)
    if drawn is None:
        raise ValueError("restyle() needs a figure that render() drew")

    styles = resolve_styles(previous.built, state)
    for key, handles in drawn.artists.items():
        for h in handles:
            _set_series_style(h, styles[key], drawn.theme)

    # The legend holds copies of the artists, so it is rebuilt rather than patched. Same
    # order as the original -- _draw_legends walks the plots in drawing order, which is the
    # order render() built them in -- or the entries would shuffle under the reader.
    drawn.legend_order = _draw_legends(fig, drawn.axes, drawn.panel_keys, drawn.extras,
                                       styles, state, drawn.theme)

    records: list[dict] = []
    for i, panel in enumerate(previous.built.panels):
        for key, s in panel.items():
            if styles[key].visible:
                records += _records(state, i, styles[key].label, s, drawn.scale)
    shown = {styles[k].label: styles[k].color for p in previous.built.panels for k in p
             if styles[k].visible}
    custom = {styles[k].label for k, o in state.styles.items()
              if o.color and k in styles and styles[k].visible}
    clashes = check_colors(shown, drawn.theme.surface, only=custom) if custom else []
    return Rendered(previous.built, styles, previous.notes, clashes, records)


def _yerr(s: Series, scale: float):
    if s.lo is None or s.hi is None:
        return None
    lower = [max(0.0, (y - lo) / scale) if lo == lo else 0.0 for y, lo in zip(s.y, s.lo)]
    upper = [max(0.0, (hi - y) / scale) if hi == hi else 0.0 for y, hi in zip(s.y, s.hi)]
    return [lower, upper]


def _records(state: PlotState, panel: int, label: str, s: Series, scale: float) -> list[dict]:
    rows = []
    slots = state.slots()
    name = state.panel_title(panel) or (Path(slots[panel].csv).stem
                                        if panel < len(slots) else f"plot {panel + 1}")
    for j, (xv, yv) in enumerate(zip(s.x, s.y)):
        r = {"Plot": name,
             "Series": label, "Key": s.key, state.effective_x: xv, "Y": yv / scale}
        if s.lo is not None and s.hi is not None:
            r["Y_low"], r["Y_high"] = s.lo[j] / scale, s.hi[j] / scale
        rows.append(r)
    return rows


def _draw_legends(fig, axes: list, panel_keys: list, extras: list,
                  styles: dict[str, Resolved], state: PlotState, theme) -> list[str]:
    """Name the series: one legend for the figure, or one for each plot.

    All or nothing -- see PlotState.per_plot_legends. A figure-wide legend beside a per-plot
    one lists the same series twice, which is worse than either on its own.

    @return the labels listed, in order, for restyle() to reproduce.
    """
    for leg in list(fig.legends):
        leg.remove()
    for ax in axes:
        if ax.get_legend() is not None:
            ax.get_legend().remove()

    if state.per_plot_legends:
        order: list[str] = []
        for i, ax in enumerate(axes):
            handles: dict[str, Any] = {}
            for key, handle in (panel_keys[i] if i < len(panel_keys) else []):
                handles.setdefault(styles[key].label if key in styles else key, handle)
            for label, handle in (extras[i] if i < len(extras) else {}).items():
                handles.setdefault(label, handle)
            order += [label for label in handles if label not in order]
            _axes_legend(ax, handles, state.legend_place(i), theme)
        return order

    handles = {}
    for i in range(len(panel_keys)):
        for key, handle in panel_keys[i]:
            handles.setdefault(styles[key].label if key in styles else key, handle)
        for label, handle in (extras[i] if i < len(extras) else {}).items():
            handles.setdefault(label, handle)
    _place_legend(fig, axes[0] if len(axes) == 1 else None, handles, state, theme)
    return list(handles)


def _axes_legend(ax, handles: dict, place: str, theme) -> None:
    """A legend inside one plot. The outside placements are anchored rather than 'outside
    <x>', which is a figure-level location matplotlib will not accept from an axes."""
    if place == "none" or len(handles) < 2:
        return
    loc, anchor = {"right": ("center left", (1.02, 0.5)),
                   "bottom": ("upper center", (0.5, -0.12)),
                   "top": ("lower center", (0.5, 1.02))}.get(place, ("best", None))
    leg = ax.legend(list(handles.values()), list(handles), loc=loc, bbox_to_anchor=anchor)
    for t in leg.get_texts():
        t.set_color(theme.text_secondary)


def _place_legend(fig, single_ax, handles: dict, state: PlotState, theme) -> None:
    """One legend. Two or more entries only: a lone series is named by the title."""
    place = state.legend
    if place == "none" or len(handles) < 2:
        return
    if place == "auto":
        if single_ax is None:
            place = "bottom"
        else:
            # A long legend inside the axes sits on the data; past a handful, move it out.
            place = "best" if len(handles) <= 6 else "right"
    labels, hs = list(handles), list(handles.values())
    if place == "best" and single_ax is not None:
        leg = single_ax.legend(hs, labels, loc="best")
    else:
        # 'right center', not 'right upper': an upper legend runs into a centred suptitle.
        loc = {"right": "outside right center", "bottom": "outside lower center",
               "top": "outside upper center", "best": "outside lower center"}[place]
        if place == "right":
            leg = fig.legend(hs, labels, loc=loc, ncols=1)
        else:
            # As many columns as fit the figure's width, measured rather than guessed: a
            # legend wider than the figure is clipped at both edges.
            renderer = fig.canvas.get_renderer()
            for ncol in range(min(len(labels), 4), 0, -1):
                leg = fig.legend(hs, labels, loc=loc, ncols=ncol)
                if ncol == 1 or leg.get_window_extent(renderer).width <= fig.bbox.width * 0.98:
                    break
                leg.remove()
    for t in leg.get_texts():
        t.set_color(theme.text_secondary)


def load_frame(path: str | Path) -> pd.DataFrame:
    """Load through the CLI's loader, so the UI sees exactly the same numbers (ops, not items)."""
    return dataio.load_results(path)


def export_table(rendered: Rendered, path: str | Path) -> Path:
    p = Path(path)
    pd.DataFrame(rendered.records).to_csv(p, index=False)
    return p


FORMATS = ("png", "svg", "pdf")


def save_figure(frames: list[pd.DataFrame], state: PlotState, path: str | Path, fmt: str,
                transparent: bool = False) -> tuple[Path, Rendered]:
    """Render @p state afresh at the export size and write it.

    A fresh render, not a resized preview: the legend's column count and the layout are
    measured against the figure they are drawn on, so the file gets a layout computed for
    the file's size rather than for whatever size the window happened to be.
    """
    if fmt not in FORMATS:
        raise ValueError(f"unsupported format {fmt!r}; choose from {', '.join(FORMATS)}")
    p = Path(path)
    if p.suffix.lower() != f".{fmt}":
        p = p.with_suffix(f".{fmt}")
    fig = Figure(figsize=(state.width, state.height), dpi=state.dpi)
    FigureCanvasAgg(fig)
    rendered = render(fig, frames, state)
    import matplotlib as mpl
    # Keep SVG text as text, so a title can still be edited in Inkscape afterwards.
    with mpl.rc_context({"svg.fonttype": "none"}):
        fig.savefig(p, format=fmt, dpi=state.dpi, transparent=transparent,
                    facecolor="none" if transparent else fig.get_facecolor())
    return p, rendered
