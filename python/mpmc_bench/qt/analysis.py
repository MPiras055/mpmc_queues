"""Reading a plot rather than drawing it: baselines, rankings and summaries.

Pure functions over :class:`model.Built`, so every claim the window makes about the numbers
is testable without a display. Nothing here imports Qt.

The three questions these answer are the ones actually asked of a benchmark plot:

- *how much better is this than that?* -- :func:`apply_baseline`
- *which one wins, and by how much?* -- :func:`ranking`
- *where does each one peak?* -- :func:`summary`

The first rewrites the data and is therefore part of the plot; the other two only read it.
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import Iterable

from ..gui import model as m
# The baseline rewrites the data, so it lives with the data: matplotlib draws every export
# through model.render(), which has to apply the same transform or a saved figure would show
# absolute values under a relative axis label.
from ..gui.model import apply_baseline, relative_ylabel  # noqa: F401

__all__ = ["RELATIVE_MODES", "BASELINE_SCOPES", "apply_baseline", "ranking", "summary",
           "Rank", "Peak"]

#: How a series is expressed once a baseline is chosen.
RELATIVE_MODES = {
    "off": "Absolute values",
    "ratio": "Ratio to baseline (1.0 = equal)",
    "percent": "% difference from baseline",
}

#: What the baseline *is*.
BASELINE_SCOPES = {
    "series": "one series, within each plot",
    "panel": "the same series in another plot",   # CSV against CSV
}


@dataclass(frozen=True)
class Rank:
    """Who won at one x, in one plot."""

    panel: int
    x: float
    winner: str
    value: float
    runner_up: str | None
    runner_value: float | None

    @property
    def margin(self) -> float | None:
        """How far ahead the winner is, as a percentage of the runner-up."""
        if self.runner_value in (None, 0):
            return None
        return (self.value / self.runner_value - 1.0) * 100.0


def ranking(built: m.Built, styles: dict[str, m.Resolved] | None = None,
            higher_is_better: bool = True) -> list[Rank]:
    """The winner at every x, and by how much.

    Hidden series do not compete: what is not on the chart should not be declared the
    winner of it.
    """
    styles = styles or {}
    out: list[Rank] = []
    for i, panel in enumerate(built.panels):
        live = {k: s for k, s in panel.items() if getattr(styles.get(k), "visible", True)}
        for x in sorted({v for s in live.values() for v in s.x}):
            scores = []
            for key, s in live.items():
                if x in s.x:
                    label = styles[key].label if key in styles else s.default_label
                    scores.append((s.y[s.x.index(x)], label))
            if not scores:
                continue
            scores.sort(reverse=higher_is_better)
            best = scores[0]
            second = scores[1] if len(scores) > 1 else (None, None)
            out.append(Rank(i, x, best[1], best[0], second[1], second[0]))
    return out


@dataclass(frozen=True)
class Peak:
    """One series, summarised."""

    panel: int
    key: str
    label: str
    peak: float
    peak_at: float
    final: float
    points: int


def summary(built: m.Built, styles: dict[str, m.Resolved] | None = None,
            higher_is_better: bool = True) -> list[Peak]:
    """Peak value and where it happens, per series.

    Worth more than the maximum alone on a scalability plot: two queues reaching the same
    peak at 8 and at 32 threads are not the same result.
    """
    styles = styles or {}
    out: list[Peak] = []
    for i, panel in enumerate(built.panels):
        for key, s in panel.items():
            if not s.y or not getattr(styles.get(key), "visible", True):
                continue
            j = (max if higher_is_better else min)(range(len(s.y)), key=lambda k: s.y[k])
            label = styles[key].label if key in styles else s.default_label
            out.append(Peak(i, key, label, s.y[j], s.x[j], s.y[-1], len(s.y)))
    return out


def table_rows(built: m.Built, state: m.PlotState,
               styles: dict[str, m.Resolved]) -> list[dict]:
    """Exactly what is plotted, one row per point -- the relief for a low-contrast hue.

    Shares :func:`model._records` so the table and the exported CSV cannot disagree.
    """
    metric = m.metric_by_key(state.metric, [])
    scale = state.yscale if state.yscale else (metric.scale if state.yscale is None else 1.0)
    rows: list[dict] = []
    for i, panel in enumerate(built.panels):
        for key, s in panel.items():
            if not styles.get(key, None) or styles[key].visible:
                label = styles[key].label if key in styles else s.default_label
                rows.extend(m._records(state, i, label, s, scale or 1.0))
    return rows


def visible_keys(built: m.Built, styles: dict[str, m.Resolved]) -> Iterable[str]:
    for key in built.keys():
        if styles.get(key) is None or styles[key].visible:
            yield key
