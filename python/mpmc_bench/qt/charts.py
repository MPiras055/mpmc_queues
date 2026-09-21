"""The live chart, drawn by pyqtgraph.

This is where the lag went. The Tk window rendered a matplotlib figure to a bitmap and
uploaded it on every change -- 650-960 ms for a full redraw, and zoom or pan meant doing it
again. Here the series are scene-graph items: Qt keeps them on the GPU, so zooming, panning
and hovering cost nothing at all, and changing a colour is a property assignment rather than
a re-render.

matplotlib has not gone away. It still draws every **export**, from the same
:class:`model.PlotState`, so what you save is unchanged and remains publication-grade. The
split is deliberate: an interactive view and a print-quality figure want different things,
and pretending one artefact can be both is what made the old preview slow.

Two things are kept faithful to the export so the preview does not lie: the palette and the
surface come from ``plotting.theme`` (the same objects matplotlib is given), and the marker
and dash vocabularies map one-to-one onto matplotlib's.
"""

from __future__ import annotations

import math
from dataclasses import dataclass

import numpy as np

import pyqtgraph as pg
from PySide6 import QtCore, QtGui, QtWidgets

from ..gui import model as m
from .theme import Skin

__all__ = ["ChartPane"]

# matplotlib -> pyqtgraph. Every marker the sidebar offers has a real equivalent; the two
# that do not ('P' and 'X' are filled plus/cross) borrow the unfilled ones, which read the
# same at preview size.
_SYMBOLS = {"o": "o", "s": "s", "^": "t1", "D": "d", "v": "t", "P": "+", "X": "x", "*": "star"}
_DASHES = {"-": QtCore.Qt.SolidLine, "--": QtCore.Qt.DashLine,
           "-.": QtCore.Qt.DashDotLine, ":": QtCore.Qt.DotLine}


def _pen(style: m.Resolved) -> QtGui.QPen:
    if style.linestyle == "":
        return pg.mkPen(None)
    return pg.mkPen(style.color, width=max(1.0, style.linewidth),
                    style=_DASHES.get(style.linestyle, QtCore.Qt.SolidLine))


def _tick_font() -> QtGui.QFont:
    """A tick font derived from the application's own.

    Built from ``QApplication.font()`` rather than ``QFont("", 9)``: an empty family is
    resolved lazily, and pyqtgraph measures tick labels straight into ``QPainter`` during
    paint, where an unresolved font segfaults instead of falling back.
    """
    app = QtWidgets.QApplication.instance()
    font = QtGui.QFont(app.font()) if app is not None else QtGui.QFont()
    font.setPointSizeF(max(7.0, font.pointSizeF() - 1.5))
    return font


def _fmt(v: float) -> str:
    if v == 0:
        return "0"
    a = abs(v)
    if a >= 1000 or a < 0.01:
        return f"{v:.4g}"
    return f"{v:,.3f}".rstrip("0").rstrip(".")


@dataclass
class _Hit:
    label: str
    x: float
    y: float
    lo: float | None
    hi: float | None
    color: str


class ChartPane(QtWidgets.QWidget):
    """One or more plots sharing a y axis, plus the crosshair readout.

    @signal pointHovered emitted with a human-readable description of the nearest point, or
            an empty string when the cursor leaves the data.
    @signal seriesToggled emitted with a series key when its legend entry is clicked.
    """

    pointHovered = QtCore.Signal(str)
    seriesToggled = QtCore.Signal(str)
    plotSelected = QtCore.Signal(int)

    def __init__(self, skin: Skin, parent=None) -> None:
        super().__init__(parent)
        self.skin = skin
        self._plots: list[pg.PlotItem] = []
        self._curves: dict[tuple[int, str], pg.PlotDataItem] = {}
        self._bars: dict[tuple[int, str], pg.ErrorBarItem] = {}
        self._crosshair: list[tuple[pg.InfiniteLine, pg.InfiniteLine]] = []
        self._built: m.Built | None = None
        self._styles: dict[str, m.Resolved] = {}
        self._state: m.PlotState | None = None
        self._label = ""
        self._scale_value = 1.0
        self._data_token: tuple | None = None
        self._applied: dict[tuple[int, str], m.Resolved] = {}
        self._limit_token: tuple | None = None
        self._legend_token: dict[object, tuple] = {}
        self._legend_item: pg.LegendItem | None = None
        self._samples: list = []
        self._tick_values: dict[int, list[float]] = {}
        self._legend_items: list[pg.LegendItem] = []
        self._selected = -1
        self._grid: tuple | None = None
        self._first_plot_row = 1

        pg.setConfigOptions(antialias=True, imageAxisOrder="row-major")
        self.view = pg.GraphicsLayoutWidget()
        self.view.setObjectName("ChartHost")
        self._title = self.view.addLabel("", row=0, col=0, colspan=8)

        layout = QtWidgets.QVBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.addWidget(self.view)
        self.view.scene().sigMouseMoved.connect(self._moved)
        self.view.scene().sigMouseClicked.connect(self._clicked)
        self.apply_skin(skin)

    # -- appearance ----------------------------------------------------------------------

    def apply_skin(self, skin: Skin) -> None:
        self.skin = skin
        self.view.setBackground(skin.chart.surface)
        if self._built is not None and self._state is not None:
            self.draw(self._built, self._styles, self._state, self._label, self._scale_value)

    # -- drawing -------------------------------------------------------------------------

    def draw(self, built: m.Built, styles: dict[str, m.Resolved], state: m.PlotState,
             ylabel: str, scale: float = 1.0) -> None:
        """Show @p built.

        @param scale divisor already applied to the y values -- the metric's own (1e6 for
               throughput), or 1.0 when a baseline has turned them into ratios. Passed in
               rather than recomputed so the chart cannot disagree with the axis label.
        """
        # Re-uploading twenty curves' worth of points to change one colour is most of the
        # cost of a redraw, so the arrays are only touched when the numbers behind them
        # actually moved. Identity of the Built is enough: build_series always returns a new
        # one, and nothing mutates it in place.
        token = (id(built), scale, state.effective_x, state.kind)
        data_changed = token != self._data_token
        self._data_token = token

        self._built, self._styles, self._state, self._label = built, styles, state, ylabel
        self._scale_value = max(scale, 1e-300) if scale else 1.0
        theme = self.skin.chart
        # A plot is a slot, not a file: a compared parameter draws one file several times.
        slot_count = len(state.slots())
        panels = [(i, p) for i, p in enumerate(built.panels) if i < slot_count]
        columns = state.columns or min(len(panels), 3) or 1
        specs = [state.axes_for(i) for i, _ in panels]

        # The grid, not just the count: changing "plots per row" or moving the legend has to
        # rebuild it, or the new legend lands in a cell a plot is already sitting in -- which
        # Qt reports as "cell is already taken" and pyqtgraph's bookkeeping never recovers
        # from, because the item then sits in the layout twice.
        grid = (len(panels), columns, "own" if state.per_plot_legends else state.legend)
        if grid != self._grid:
            self._grid = grid
            self._rebuild_plots(len(panels), columns, grid[2])

        self._title.setText(state.figure_title() or "",
                            color=theme.text_primary, size="12pt", bold=True)

        live_keys = set()
        for slot, (index, panel) in enumerate(panels):
            plot = self._plots[slot]
            plot.setTitle(specs[slot].title or None,
                          color=theme.text_secondary, size="10pt")
            for key, series in panel.items():
                st = styles.get(key)
                if st is None or not st.visible:
                    continue
                live_keys.add((slot, key))
                self._draw_series(slot, plot, key, series, st, state, data_changed)
            self._style_axes(plot, state, specs[slot], ylabel, slot, columns, len(panels))

        for ident in [k for k in self._curves if k not in live_keys]:
            self._plots[ident[0]].removeItem(self._curves.pop(ident))
            self._applied.pop(ident, None)
        for ident in [k for k in self._bars if k not in live_keys]:
            self._plots[ident[0]].removeItem(self._bars.pop(ident))

        self._legend(built, styles, state, specs, columns, len(panels))
        self._apply_limits(state, specs)
        # Ticks after the limits, not before: fixed y spacing is generated across the visible
        # range, and reading it from the previous range put the ticks somewhere else.
        for slot, plot in enumerate(self._plots):
            self._ticks(plot, specs[slot] if slot < len(specs) else state.axes_for(slot),
                        slot, data_changed)
        self.select(self._selected)

    def _rebuild_plots(self, count: int, columns: int, place: str = "auto") -> None:
        """Lay the figure out from scratch.

        The rows are *reserved*, not shuffled: the title, a row for a legend above, the
        plots, a row for a legend below, and the right-hand legend in its own column.
        Reserving them is what lets the legend move without every plot changing cell.
        """
        self._drop_legends()
        self.view.clear()
        self._plots.clear()
        self._curves.clear()
        self._bars.clear()
        self._crosshair.clear()
        self._legend_token.clear()
        self._tick_values = {}
        self._applied.clear()
        self._limit_token = None
        self._first_plot_row = 2 if place == "top" else 1
        self._title = self.view.addLabel("", row=0, col=0, colspan=max(columns, 1) + 1)
        for i in range(count):
            row, col = divmod(i, max(columns, 1))
            plot = self.view.addPlot(row=row + self._first_plot_row, col=col)
            plot.setMenuEnabled(False)
            plot.setClipToView(True)
            plot.setDownsampling(auto=True, mode="peak")
            self._plots.append(plot)
            v = pg.InfiniteLine(angle=90, movable=False,
                                pen=pg.mkPen(self.skin.subtle, width=1,
                                             style=QtCore.Qt.DotLine))
            h = pg.InfiniteLine(angle=0, movable=False,
                                pen=pg.mkPen(self.skin.subtle, width=1,
                                             style=QtCore.Qt.DotLine))
            for line in (v, h):
                line.setVisible(False)
                line.setZValue(-10)
                plot.addItem(line, ignoreBounds=True)
            self._crosshair.append((v, h))

    def _draw_series(self, slot: int, plot: pg.PlotItem, key: str, s: m.Series,
                     st: m.Resolved, state: m.PlotState, data_changed: bool) -> None:
        scale = self._scale_value
        ident = (slot, key)
        curve = self._curves.get(ident)
        fresh = curve is None
        if fresh:
            curve = pg.PlotDataItem()
            curve.setZValue(1)
            plot.addItem(curve)
            self._curves[ident] = curve
        if data_changed or fresh:
            curve.setData(list(s.x), [v / scale for v in s.y], connect="finite")
        # Each of these setters invalidates and repaints the item, so re-applying an
        # unchanged style to twenty curves costs as much as a real edit did.
        if fresh or self._applied.get(ident) != st:
            self._applied[ident] = st
            curve.setPen(_pen(st))
            curve.setSymbol(_SYMBOLS.get(st.marker) if st.marker else None)
            curve.setSymbolSize(max(6.0, st.linewidth * 3.5))
            curve.setSymbolBrush(pg.mkBrush(st.color))
            curve.setSymbolPen(pg.mkPen(self.skin.chart.surface, width=1))
        bars = self._bars.get(ident)
        show_errors = s.lo is not None and s.hi is not None and state.errorbars != "none"
        if show_errors and (data_changed or fresh or bars is None):
            top = [max(0.0, (hi - v) / scale) for v, hi in zip(s.y, s.hi)]
            bottom = [max(0.0, (v - lo) / scale) for v, lo in zip(s.y, s.lo)]
            pen = pg.mkPen(st.color, width=1)
            if bars is None:
                bars = pg.ErrorBarItem(pen=pen)
                bars.setZValue(0)
                plot.addItem(bars)
                self._bars[ident] = bars
            bars.setData(x=np.asarray(s.x, dtype=float),
                         y=np.asarray([v / scale for v in s.y], dtype=float),
                         top=np.asarray(top, dtype=float),
                         bottom=np.asarray(bottom, dtype=float), beam=0.0, pen=pen)
        elif show_errors:
            bars.setOpts(pen=pg.mkPen(st.color, width=1))
        elif bars is not None:
            plot.removeItem(self._bars.pop(ident))

    def _style_axes(self, plot: pg.PlotItem, state: m.PlotState, spec: m.AxesSpec,
                    ylabel: str, slot: int, columns: int, total: int) -> None:
        theme = self.skin.chart
        plot.showGrid(x=spec.grid == "both", y=spec.grid in ("y", "both"), alpha=0.25)
        plot.setLogMode(x=spec.xlog, y=spec.ylog)
        bottom_row = slot >= total - columns
        for side in ("left", "bottom"):
            axis = plot.getAxis(side)
            axis.setPen(pg.mkPen(theme.axis))
            axis.setTextPen(pg.mkPen(theme.text_muted))
            axis.setStyle(tickFont=_tick_font())
        # As in the export: a label this plot asked for is drawn on this plot, wherever it
        # sits; only the automatic one is left to the first column and the bottom row.
        plot.setLabel("left",
                      spec.ylabel or (ylabel if slot % max(columns, 1) == 0 else ""),
                      color=theme.text_secondary)
        plot.setLabel("bottom",
                      spec.xlabel or (m.AXIS_NAMES.get(state.effective_x, state.effective_x)
                                      if bottom_row else ""),
                      color=theme.text_secondary)

    def _x_values(self, slot: int, data_changed: bool) -> list[float]:
        """The x values drawn on one plot.

        Per plot, not per figure: a faceted plot holds one value of the compared parameter
        and must not be given ticks for x values it does not contain.
        """
        if data_changed or slot not in self._tick_values:
            self._tick_values[slot] = sorted({float(v) for (s, _k), c in self._curves.items()
                                              if s == slot and c.xData is not None
                                              for v in c.xData})
        return self._tick_values[slot]

    def _ticks(self, plot: pg.PlotItem, spec: m.AxesSpec, slot: int,
               data_changed: bool = True) -> None:
        """Tick positions for one plot.

        On a log axis pyqtgraph works in log10 of the data, so an explicit tick is placed at
        ``log10(v)`` while still reading as *v* -- which is also what keeps the preview
        agreeing with the export, where matplotlib uses a base-2 log axis with a plain
        formatter rather than pyqtgraph's ``10^n``.
        """
        axis = plot.getAxis("bottom")
        values = self._x_values(slot, data_changed)
        mode = spec.xticks.mode
        if spec.xlog and mode == "auto":
            mode = "all"                       # else the axis reads 10^0, 10^0.6, 10^1.2
        pos = (lambda v: math.log10(v)) if spec.xlog else (lambda v: v)
        if spec.xlog:
            values = [v for v in values if v > 0]
        if mode == "hidden":
            axis.setTicks([[]])
        elif mode == "all" and values:
            axis.setTicks([[(pos(v), _fmt(v)) for v in values]])
        elif mode == "every" and values:
            n = max(1, int(spec.xticks.value))
            axis.setTicks([[(pos(v), _fmt(v)) for v in values[::n]]])
        elif mode == "step" and spec.xticks.value > 0 and values and not spec.xlog:
            step = spec.xticks.value
            ticks, v = [], math.floor(min(values) / step) * step
            while v <= max(values) + step / 2 and len(ticks) < 200:
                ticks.append((v, _fmt(v)))
                v += step
            axis.setTicks([ticks])
        else:
            axis.setTicks(None)

        left = plot.getAxis("left")
        if spec.yticks.mode == "hidden":
            left.setTicks([[]])
        elif spec.yticks.mode == "step" and spec.yticks.value > 0 and not spec.ylog:
            lo, hi = plot.viewRange()[1]
            step = spec.yticks.value
            ticks, v = [], math.floor(lo / step) * step
            while v <= hi and len(ticks) < 200:
                ticks.append((v, _fmt(v)))
                v += step
            left.setTicks([ticks])
        elif spec.yticks.mode == "count" and spec.yticks.value >= 2 and not spec.ylog:
            lo, hi = plot.viewRange()[1]
            n = int(spec.yticks.value)
            step = (hi - lo) / max(1, n - 1)
            left.setTicks([[(lo + k * step, _fmt(lo + k * step)) for k in range(n)]])
        else:
            left.setTicks(None)

    def _bounds(self, slots: list[int] | None = None) -> tuple[float, float] | None:
        """Extent of the drawn y values, error bars included."""
        lo = hi = None
        for (slot, key), curve in self._curves.items():
            if slots is not None and slot not in slots:
                continue
            if curve.yData is None or not len(curve.yData):
                continue
            values = [float(curve.yData.min()), float(curve.yData.max())]
            bars = self._bars.get((slot, key))
            if bars is not None:
                opts = bars.opts
                if opts.get("top") is not None and opts.get("y") is not None:
                    values.append(float((opts["y"] + opts["top"]).max()))
                    values.append(float((opts["y"] - opts["bottom"]).min()))
            lo = min(values) if lo is None else min(lo, *values)
            hi = max(values) if hi is None else max(hi, *values)
        return None if lo is None else (lo, hi)

    def _apply_limits(self, state: m.PlotState, specs: list[m.AxesSpec] | None = None) -> None:
        """Set every plot's y range.

        Deliberately not ``setYLink``: linked views take *one* range rather than the union
        of what they hold, so a plot peaking at 3.0 beside one peaking at 1.0 was drawn
        entirely above its own axis. "Same y range on every plot" has to mean the widest
        range among them, which is what matplotlib does on export, so the union is computed
        here and applied to each.
        """
        specs = specs or [state.axes_for(i) for i in range(len(self._plots))]
        token = (self._data_token, state.y_from_zero, state.share_y, state.share_x,
                 repr(specs),
                 tuple(sorted(k for k, v in self._styles.items() if v.visible)))
        if token == self._limit_token:
            return                       # the ranges cannot have moved
        self._limit_token = token

        shared = self._bounds() if state.share_y else None
        x_all: tuple[float, float] | None = None
        if state.share_x:
            xs = [float(v) for c in self._curves.values()
                  if c.xData is not None for v in c.xData]
            x_all = (min(xs), max(xs)) if xs else None

        for slot, plot in enumerate(self._plots):
            spec = specs[slot] if slot < len(specs) else state.axes_for(slot)
            plot.enableAutoRange(axis="xy")
            if x_all is not None:
                span = max(x_all[1] - x_all[0], 1e-12)
                plot.setXRange(x_all[0] - 0.02 * span, x_all[1] + 0.02 * span, padding=0)
            if spec.ylog:
                continue
            extent = shared if shared is not None else self._bounds([slot])
            if extent is None:
                continue
            lo, hi = extent
            if state.y_from_zero:
                lo = min(0.0, lo)
            # ymin and ymax are in the units on the axis, the ones the table shows -- the
            # curve data was divided by the metric's scale before it got here, and dividing
            # them again put the limit a million times too low in the preview while the
            # export, which applies them as written, was right.
            if spec.ymin is not None:
                lo = spec.ymin
            if spec.ymax is not None:
                hi = spec.ymax
            span = max(hi - lo, 1e-12)
            plot.setYRange(lo, hi + 0.05 * span, padding=0)

    # -- legend --------------------------------------------------------------------------

    def _drop_legends(self) -> None:
        """Take every legend out of the scene, forgivingly.

        pyqtgraph raises from removeItem() when its row bookkeeping and the layout disagree,
        which is exactly the state a doubly-added item leaves behind; the scene is the
        authority, so fall back to it rather than let a stale index kill the redraw.
        """
        for item in self._legend_items:
            scene = item.scene()
            if scene is not None:
                scene.removeItem(item)
        self._legend_items = []
        item, self._legend_item = self._legend_item, None
        if item is None:
            return
        try:
            self.view.removeItem(item)
        except (KeyError, ValueError):
            scene = item.scene()
            if scene is not None:
                scene.removeItem(item)

    def _sample(self, st: m.Resolved) -> pg.PlotDataItem:
        """The little line-and-marker a legend entry shows. Greyed out when hidden."""
        return pg.PlotDataItem(
            [0, 1], [0, 0],
            pen=_pen(st) if st.visible else pg.mkPen(self.skin.subtle, width=1,
                                                     style=QtCore.Qt.DotLine),
            symbol=_SYMBOLS.get(st.marker) if st.marker else None,
            symbolSize=7,
            symbolBrush=pg.mkBrush(st.color if st.visible else self.skin.subtle),
            symbolPen=pg.mkPen(None))

    def _clickable(self, legend: pg.LegendItem, keys: list[str]) -> None:
        for (_sample, label), key in zip(legend.items, keys):
            label.mousePressEvent = self._legend_click(key)
            label.setCursor(QtCore.Qt.PointingHandCursor)

    def _legend(self, built: m.Built, styles: dict[str, m.Resolved], state: m.PlotState,
                specs: list[m.AxesSpec], columns: int, panels: int) -> None:
        """One legend for the figure, in its own cell rather than on top of the data.

        A per-plot legend inside the axes covered the lines it was naming as soon as there
        were more than a handful of series -- and with ten queues that is the normal case.
        Giving it a column of the layout costs a little width and no data.

        A plot that overrides its legend placement gets its own, listing only what it draws,
        and then *every* plot does: one figure legend beside a per-plot one would list the
        same series twice. Entries are clickable either way -- hiding a series is the
        commonest edit there is.
        """
        self._samples = []

        if state.per_plot_legends:
            self._drop_legends()
            self._legend_token.clear()
            self._per_plot_legends(built, styles, specs)
            return

        place = state.legend
        keys = [k for k in built.keys() if k in styles]
        if place == "none" or not keys:
            self._drop_legends()
            self._legend_token.clear()
            return

        token = (place, columns, panels,
                 tuple((k, styles[k].label, styles[k].visible, styles[k].color,
                        styles[k].marker, styles[k].linestyle) for k in keys))
        if token == self._legend_token.get("figure"):
            if self._legend_item is not None:
                self._legend_item.setVisible(True)
            return
        self._legend_token["figure"] = token

        rows = max(1, -(-panels // max(columns, 1)))
        self._drop_legends()
        legend = pg.LegendItem(offset=None, labelTextColor=self.skin.chart.text_secondary,
                               brush=pg.mkBrush(self.skin.chart.surface),
                               pen=pg.mkPen(self.skin.chart.grid),
                               verSpacing=-2)
        self._legend_item = legend
        first = self._first_plot_row
        if place == "bottom":
            legend.setColumnCount(min(4, max(1, len(keys))))
            self.view.addItem(legend, row=first + rows, col=0, colspan=max(columns, 1))
        elif place == "top":
            legend.setColumnCount(min(4, max(1, len(keys))))
            self.view.addItem(legend, row=1, col=0, colspan=max(columns, 1))
        else:                                   # auto, best, right
            self.view.addItem(legend, row=first, col=max(columns, 1), rowspan=rows)

        for key in keys:
            st = styles[key]
            sample = self._sample(st)
            self._samples.append(sample)
            legend.addItem(sample, st.label if st.visible else f"◻ {st.label}")
        self._clickable(legend, keys)

    #: offset inside the plot, by placement. Positive counts from the left and top edges,
    #: negative from the right and bottom -- pyqtgraph's own convention.
    _LEGEND_SPOTS = {"bottom": (35, -12), "top": (35, 12), "right": (-12, 12),
                     "best": (-12, 12), "auto": (-12, 12)}

    def _per_plot_legends(self, built: m.Built, styles: dict[str, m.Resolved],
                          specs: list[m.AxesSpec]) -> None:
        for slot, plot in enumerate(self._plots):
            place = self._state.legend_place(slot) if self._state is not None else "auto"
            keys = [k for k in (built.panels[slot] if slot < len(built.panels) else {})
                    if k in styles]
            if place == "none" or not keys:
                continue
            legend = pg.LegendItem(offset=self._LEGEND_SPOTS.get(place, (-12, 12)),
                                   labelTextColor=self.skin.chart.text_secondary,
                                   brush=pg.mkBrush(self.skin.chart.surface),
                                   pen=pg.mkPen(self.skin.chart.grid), verSpacing=-2)
            legend.setParentItem(plot.getViewBox())
            self._legend_items.append(legend)
            for key in keys:
                st = styles[key]
                sample = self._sample(st)
                self._samples.append(sample)
                legend.addItem(sample, st.label if st.visible else f"◻ {st.label}")
            self._clickable(legend, keys)

    def _legend_click(self, key: str):
        def handler(event):
            self.seriesToggled.emit(key)
            event.accept()
        return handler

    # -- hover ---------------------------------------------------------------------------

    def _moved(self, pos) -> None:
        for slot, plot in enumerate(self._plots):
            if not plot.sceneBoundingRect().contains(pos):
                continue
            point = plot.vb.mapSceneToView(pos)
            hit = self._nearest(slot, point.x(), point.y(), plot)
            v, h = self._crosshair[slot]
            for other, (ov, oh) in enumerate(self._crosshair):
                if other != slot:
                    ov.setVisible(False)
                    oh.setVisible(False)
            if hit is None:
                v.setVisible(False)
                h.setVisible(False)
                self.pointHovered.emit("")
                return
            v.setPos(hit.x)
            h.setPos(hit.y)
            v.setVisible(True)
            h.setVisible(True)
            text = f"{hit.label}   x = {_fmt(hit.x)}   y = {_fmt(hit.y)}"
            if hit.lo is not None and hit.hi is not None:
                text += f"   ({_fmt(hit.lo)} – {_fmt(hit.hi)})"
            self.pointHovered.emit(text)
            return
        for v, h in self._crosshair:
            v.setVisible(False)
            h.setVisible(False)
        self.pointHovered.emit("")

    def _nearest(self, slot: int, x: float, y: float, plot: pg.PlotItem) -> _Hit | None:
        """Nearest point in *pixels*, so a tall axis does not make everything snap sideways."""
        vb = plot.vb
        span_x, span_y = vb.viewRange()
        width = max(1e-12, span_x[1] - span_x[0])
        height = max(1e-12, span_y[1] - span_y[0])
        best: _Hit | None = None
        best_d = 0.05 ** 2                    # within 5% of the viewport, else no answer
        for (s, key), curve in self._curves.items():
            if s != slot or curve.xData is None:
                continue
            st = self._styles.get(key)
            if st is None or not st.visible:
                continue
            series = self._built.panels[slot].get(key) if self._built else None
            for i, (px, py) in enumerate(zip(curve.xData, curve.yData)):
                d = ((px - x) / width) ** 2 + ((py - y) / height) ** 2
                if d < best_d:
                    scale = self._scale_value
                    lo = hi = None
                    if series is not None and series.lo is not None and series.hi is not None:
                        lo, hi = series.lo[i] / scale, series.hi[i] / scale
                    best_d, best = d, _Hit(st.label, float(px), float(py), lo, hi, st.color)
        return best

    # -- view control --------------------------------------------------------------------

    def reset_view(self) -> None:
        for plot in self._plots:
            plot.enableAutoRange(axis="xy")
        # Clear the token first: _apply_limits skips work when nothing about the ranges has
        # changed, and after a zoom nothing has -- so without this, "Reset view" left the
        # plots autoranging and quietly dropped "start y at zero" and the y limits.
        self._limit_token = None
        if self._state is not None:
            self._apply_limits(self._state)

    # -- selection -----------------------------------------------------------------------

    def select(self, slot: int) -> None:
        """Mark one plot as the one being edited. -1 selects none."""
        self._selected = slot if 0 <= slot < len(self._plots) else -1
        for i, plot in enumerate(self._plots):
            plot.getViewBox().setBorder(
                pg.mkPen(self.skin.accent, width=2) if i == self._selected else None)

    def _clicked(self, event) -> None:
        pos = event.scenePos() if hasattr(event, "scenePos") else event
        for slot, plot in enumerate(self._plots):
            if plot.sceneBoundingRect().contains(pos):
                if slot != self._selected:
                    self.select(slot)
                    self.plotSelected.emit(slot)
                return
