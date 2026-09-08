"""Side-by-side panels of the same measurement under different conditions.

The question these answer is "does the ranking hold as the capacity changes?", and it is
a question a reader can only answer if the panels share a scale. So the y-axis is
**computed across every panel and then applied to all of them** -- the largest value
anywhere in the figure sets the top everywhere. Panels each auto-scaled to their own data
look comparable and are not: the shorter bar and the taller bar reach the same height.

The panels come from a JSON list, because the interesting comparisons are combinatorial
and nobody should be editing a plotting script to get one:

.. code-block:: json

    { "title": "PSCQ vs PRQ across sizes",
      "kind": "throughput",
      "share_y": true,
      "panels": [
        { "title": "256",  "csv": "pscq64_balanced.csv", "size": 256,
          "queues": ["i-u-pscq", "i-u-prq"] },
        { "title": "1024", "csv": "pscq64_balanced.csv", "size": 1024,
          "queues": ["i-u-pscq", "i-u-prq"] }
      ] }

Panel keys are the fields of :class:`data.Filters` -- ``queues``, ``size``, ``pinning``,
``prod_delay_ns``, ``cons_delay_ns`` -- so filtering behaves exactly as it does on the
command line. ``csv``, ``kind`` and ``baseline`` fall back to the top level.

The three-series cap
--------------------

Small multiples put every pair of colours on screen simultaneously, so they are governed
by the all-pairs measurement, not the adjacent-pair one that the line charts pass on all
eight hues. Measured with the dataviz validator:

- first three slots, all pairs: **PASS** in both modes -- worst CVD dE 9.2 light / 9.4
  dark, worst normal-vision dE 24.0 light / 20.9 dark.
- the moment a fourth slot joins, yellow lands beside orange: normal-vision dE **13.7**
  light, below the hard floor of 15, and CVD dE 4.8 dark.

Below the normal-vision floor a reader with *full* colour vision cannot tell the two
series apart, which is why markers and linestyles do not buy a fourth slot -- they are
relief for the colour-vision band, not for this one.

The cap is therefore three series **across the whole figure**, not merely three per
panel. Panels share one colour assignment, since a queue that changed hue between panels
would defeat the comparison; and it is only the *first three* slots that were measured
safe, not any three of the eight. A figure holding four distinct series would put an
unmeasured pair on screen even with three in each panel. Slots are compacted for the same
reason: two series always draw slots 0 and 1, whichever queues they are.
"""

from __future__ import annotations

import json
import logging
from dataclasses import dataclass, field, replace
from pathlib import Path
from typing import Any

import matplotlib.pyplot as plt

from . import data as dataio
from . import theme as theming
from .styles import assign_styles

logger = logging.getLogger("mpmc.plot")

__all__ = ["CompareSpec", "Panel", "CompareResult", "load_spec", "plot_compare",
           "MAX_PANEL_SERIES", "HEADROOM"]

#: See the module docstring: this is the measured limit of the all-pairs palette, not a
#: layout preference.
MAX_PANEL_SERIES = 3

#: Fraction of the data range left above the topmost mark, so a line does not run along
#: the frame. Applied once to the shared limit, hence identical on every panel.
HEADROOM = 0.05

#: Which measurement each kind draws, and what to call its x-axis. ``backoff-grid`` is
#: absent on purpose: it is a heatmap of one family, so there is no y-axis to share.
_KINDS = {
    "throughput": "Total threads",
    "scalability": "Producers",
    "slot-efficiency": "Total threads",
    "segments-per-item": "Total threads",
}

_FILTER_KEYS = ("queues", "size", "pinning", "prod_delay_ns", "cons_delay_ns")
_PANEL_KEYS = frozenset(_FILTER_KEYS + ("title", "csv", "kind", "baseline"))
_SPEC_KEYS = frozenset(("title", "kind", "csv", "baseline", "share_y", "share_x", "panels"))


@dataclass(frozen=True)
class Panel:
    title: str
    csv: Path
    kind: str
    filters: dataio.Filters
    baseline: int | None = None


@dataclass(frozen=True)
class CompareSpec:
    title: str
    kind: str
    panels: tuple[Panel, ...]
    share_y: bool = True
    share_x: bool = True


@dataclass
class CompareResult:
    """What was drawn, so the shared axis can be asserted rather than eyeballed."""

    figure: Any
    axes: list
    #: The largest y value anywhere in the figure, before headroom.
    data_max: float
    #: The limits every axis was given. Identical across panels when ``share_y``.
    ylim: tuple[float, float] | None
    xlim: tuple[float, float] | None
    records: list[dict] = field(default_factory=list)


def _unknown(keys, allowed, what: str) -> None:
    extra = sorted(set(keys) - set(allowed))
    if extra:
        raise ValueError(
            f"{what}: unknown key(s) {', '.join(extra)}. Allowed: {', '.join(sorted(allowed))}"
        )


def load_spec(path: str | Path, base_csv: str | Path | None = None) -> CompareSpec:
    """Read and check a comparison spec. Relative CSV paths resolve next to the spec."""
    spec_path = Path(path)
    if not spec_path.is_file():
        raise FileNotFoundError(f"comparison spec not found: {spec_path}")
    try:
        raw = json.loads(spec_path.read_text())
    except json.JSONDecodeError as exc:
        raise ValueError(f"{spec_path}: not valid JSON ({exc})") from None
    if not isinstance(raw, dict):
        raise ValueError(f"{spec_path}: expected a JSON object with a 'panels' list")
    _unknown(raw, _SPEC_KEYS, str(spec_path))

    panels_raw = raw.get("panels")
    if not isinstance(panels_raw, list) or not panels_raw:
        raise ValueError(f"{spec_path}: 'panels' must be a non-empty list")

    kind = raw.get("kind", "throughput")
    if kind not in _KINDS:
        raise ValueError(
            f"{spec_path}: kind {kind!r} cannot be panelled. Choose from "
            f"{', '.join(sorted(_KINDS))} -- backoff-grid is a heatmap, so there is no "
            "y-axis for the panels to share."
        )

    def resolve_csv(value, what):
        if value is None:
            raise ValueError(
                f"{what}: no 'csv'. Give one per panel, one at the top level, or pass a "
                "results CSV on the command line."
            )
        candidate = Path(value)
        if not candidate.is_absolute() and not candidate.exists():
            beside = spec_path.parent / candidate
            if beside.exists():
                return beside
        return candidate

    panels = []
    for i, item in enumerate(panels_raw):
        what = f"{spec_path}: panel {i + 1}"
        if not isinstance(item, dict):
            raise ValueError(f"{what}: expected an object")
        _unknown(item, _PANEL_KEYS, what)
        panel_kind = item.get("kind", kind)
        if panel_kind not in _KINDS:
            raise ValueError(f"{what}: kind {panel_kind!r} cannot be panelled")
        panels.append(Panel(
            title=str(item.get("title", f"Panel {i + 1}")),
            csv=resolve_csv(item.get("csv", raw.get("csv", base_csv)), what),
            kind=panel_kind,
            filters=dataio.Filters(**{k: item.get(k) for k in _FILTER_KEYS}),
            baseline=item.get("baseline", raw.get("baseline")),
        ))

    return CompareSpec(
        title=str(raw.get("title", spec_path.stem)),
        kind=kind,
        panels=tuple(panels),
        share_y=bool(raw.get("share_y", True)),
        share_x=bool(raw.get("share_x", True)),
    )


def _series(df, panel: Panel, stat: str, scale: float):
    """(name -> (xs, ys, yerrs)) for one panel, in the shape its kind measures."""
    kind = panel.kind
    if kind == "scalability":
        baseline = panel.baseline
        if baseline is None:
            baseline = int(df["Producers"].min())
        out = {}
        for name, group in dataio.scalability(df, int(baseline), stat):
            out[name] = (list(group["Producers"]), list(group["Scalability"]), None)
        return out

    if kind == "throughput":
        col = dataio.stat_column(df, stat)
        value, how, err = col, "mean", "Throughput_StdDev"
    elif kind == "slot-efficiency":
        value, how, err = "SlotEfficiency", "median", None
    else:  # segments-per-item
        value, how, err = "SegPerItem", "median", None
        if "Segments" not in df.columns or "Produced" not in df.columns:
            raise ValueError(
                "segments-per-item needs Segments and Produced, which this CSV does not "
                'have. Re-run the experiment with "metrics": true.'
            )
        df = df.copy()
        df["SegPerItem"] = df["Segments"] / df["Produced"]

    if value not in df.columns:
        raise ValueError(
            f"{panel.kind} needs {value}, which {panel.csv} does not have. Re-run the "
            'experiment with "metrics": true.'
        )

    out = {}
    agg = {value: how} | ({err: "mean"} if err else {})
    for name in dataio.queues_in(df):
        group = df[df["Queue"] == name].dropna(subset=[value])
        if group.empty:
            continue
        rolled = group.groupby("Total_Threads", as_index=False).agg(agg)
        out[str(name)] = (
            list(rolled["Total_Threads"]),
            [v / scale for v in rolled[value]],
            [v / scale for v in rolled[err]] if err else None,
        )
    return out


def _check_cap(panels: list[tuple[Panel, dict]]) -> list[str]:
    """Refuse a figure the palette cannot carry, saying what was measured and why."""
    reason = (
        f"Comparison panels are small multiples, so every pair of colours is on screen at "
        f"once. Only the first {MAX_PANEL_SERIES} palette slots clear the all-pairs floors "
        f"(worst CVD dE 9.2 light / 9.4 dark, normal-vision 24.0 / 20.9). A fourth slot "
        f"puts yellow beside orange: normal-vision dE 13.7 light -- below the hard floor of "
        f"15 -- and CVD dE 4.8 dark. Below that floor a reader with full colour vision "
        f"cannot separate the two, so markers and linestyles do not buy the slot back. "
        f"Split the extras into more panels, or drop to {MAX_PANEL_SERIES}."
    )
    for panel, series in panels:
        if len(series) > MAX_PANEL_SERIES:
            raise ValueError(
                f"panel {panel.title!r} draws {len(series)} series "
                f"({', '.join(sorted(series))}). {reason}"
            )

    union = sorted({name for _, series in panels for name in series})
    if len(union) > MAX_PANEL_SERIES:
        raise ValueError(
            f"this comparison holds {len(union)} distinct series across its panels "
            f"({', '.join(union)}), though no single panel exceeds {MAX_PANEL_SERIES}. "
            "Panels share one colour assignment -- a queue that changed hue between them "
            "would defeat the comparison -- so the figure as a whole draws that many "
            f"slots. {reason}"
        )
    return union


def plot_compare(spec: CompareSpec, config, stat: str = "median") -> CompareResult:
    """Render @p spec, giving every panel the same limits. Returns what it drew."""
    from .plots import DEFAULTS, _legend, write_table

    theme = config.theme
    frames: dict[Path, Any] = {}
    built: list[tuple[Panel, dict]] = []

    for panel in spec.panels:
        if panel.csv not in frames:
            frames[panel.csv] = dataio.load_results(panel.csv)
        df = dataio.apply_filters(frames[panel.csv], panel.filters)
        if df.empty:
            raise ValueError(
                f"panel {panel.title!r}: the filters matched no rows in {panel.csv}"
            )
        series = _series(df, panel, stat, config.scale)
        if not series:
            # Rows survived the filters but the kind dropped them all -- scalability does
            # that to every queue lacking a baseline measurement. A blank panel beside
            # populated ones reads as "this configuration scored zero", which is a lie.
            raise ValueError(
                f"panel {panel.title!r}: {panel.kind} yielded no series from {panel.csv}. "
                "For scalability that means no queue has a measurement at the baseline "
                "producer count; set 'baseline' on the panel or at the top level."
            )
        built.append((panel, series))

    union = _check_cap(built)
    # Compacted, so two series are always slots 0 and 1 -- the measured pair -- whichever
    # queues they happen to be. Assigned over the union, so a queue keeps its hue across
    # every panel.
    styles = assign_styles(union, theme, compact=True)

    n = len(built)
    # Constrained layout, because this figure has a suptitle, a shared x label and an
    # outside legend all competing for the margins; tight_layout does not reserve space
    # for the legend and drops it on top of the x label.
    fig, axes = plt.subplots(
        1, n, figsize=(max(4.0, 9.0 / max(n, 1)) * n, 5.0), squeeze=False,
        layout="constrained",
    )
    axes = list(axes[0])
    fig.set_facecolor(theme.surface)

    records: list[dict] = []
    handles: dict[str, Any] = {}
    data_max, data_min = None, None
    x_max, x_min = None, None

    for ax, (panel, series) in zip(axes, built):
        for name in sorted(series):
            xs, ys, errs = series[name]
            st = styles[name]
            if errs is not None and any(errs):
                line = ax.errorbar(xs, ys, yerr=errs, label=st.label, color=st.color,
                                   marker=st.marker, linestyle=st.linestyle,
                                   capsize=3, markersize=5, linewidth=1.5)
            else:
                line, = ax.plot(xs, ys, label=st.label, color=st.color, marker=st.marker,
                                linestyle=st.linestyle, markersize=5, linewidth=1.5)
            handles.setdefault(st.label, line)
            records += [{"Panel": panel.title, "Series": st.label, "X": x, "Y": y}
                        for x, y in zip(xs, ys)]
            data_max = max([data_max, *ys]) if data_max is not None else max(ys)
            data_min = min([data_min, *ys]) if data_min is not None else min(ys)
            x_max = max([x_max, *xs]) if x_max is not None else max(xs)
            x_min = min([x_min, *xs]) if x_min is not None else min(xs)

        ax.set_title(panel.title)
        if config.logx:
            ax.set_xscale("log", base=2)
        theming.style_axes(ax, theme)

    # The point of the exercise: one scale, computed once every panel is built, then given
    # to all of them. Done before this, a panel would be scaled to its own data.
    ylim = xlim = None
    if spec.share_y and data_max is not None:
        span = data_max - min(data_min, 0.0)
        top = data_max + (span or abs(data_max) or 1.0) * HEADROOM
        ylim = (min(data_min, 0.0), top)
        for ax in axes:
            ax.set_ylim(*ylim)
    if spec.share_x and x_max is not None:
        pad = (x_max - x_min) * 0.03 or 1.0
        xlim = (x_min - pad, x_max + pad)
        for ax in axes:
            ax.set_xlim(*xlim)

    # Axis names once each: repeating an identical label under every panel is noise, and
    # the y label belongs to the leftmost panel because the scale is shared.
    axes[0].set_ylabel(config.ylabel or DEFAULTS[spec.kind][1])
    axes[0].yaxis.label.set_color(theme.text_secondary)
    # On the centre panel rather than as a figure-level supxlabel: a supxlabel sits at a
    # fixed height that the layout engine does not reserve for, so it lands underneath the
    # legend. An Axes label is placed by the same engine that places the legend.
    middle = axes[len(axes) // 2]
    middle.set_xlabel(_KINDS[spec.kind])
    middle.xaxis.label.set_color(theme.text_secondary)
    fig.suptitle(spec.title, color=theme.text_primary, fontsize="large")

    # One legend for the figure. The same series repeated under every panel is the noise
    # small multiples exist to remove.
    _legend(fig, theme, list(handles.values()), list(handles),
            loc="outside lower center", ncol=min(len(handles), MAX_PANEL_SERIES))

    table_config = replace(config, xlabel=_KINDS[spec.kind],
                           ylabel=config.ylabel or DEFAULTS[spec.kind][1])
    write_table(table_config, records)
    if config.save_path:
        fig.savefig(config.save_path, dpi=150, facecolor=theme.surface)
        logger.info("saved %s", config.save_path)
    if config.show:
        plt.show()
    else:
        plt.close(fig)

    return CompareResult(figure=fig, axes=axes, data_max=data_max,
                         ylim=ylim, xlim=xlim, records=records)
