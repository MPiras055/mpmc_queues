"""mpmc plot studio -- the Qt window.

Replaces the Tk window. Three things changed, and the third is why the other two were worth
doing:

1. **The preview is a live scene, not a bitmap.** pyqtgraph owns the curves, so zoom, pan,
   hover and every cosmetic edit are immediate. matplotlib still draws exports, unchanged.
2. **The interface is flat**, built from the chart's own palette -- see :mod:`theme`.
3. **The numbers are readable as numbers**: a hover readout, a table of exactly what is
   plotted, a ranking of who wins at each thread count, and a baseline mode that turns
   absolute throughput into "how much faster than PSCQ".

Only the *view* is new. Loading, filtering, correlated filters, series building, styling and
export are still :mod:`gui.model`, which has no toolkit in it and is covered by its own
tests.
"""

from __future__ import annotations

import argparse
import logging
import sys
import time
from pathlib import Path

import pandas as pd
from PySide6 import QtCore, QtGui, QtWidgets

from ..gui import model as m
from . import analysis
from .charts import ChartPane
from .stylelib import StyleLibrary
from .theme import skin, stylesheet
from .widgets import (Card, Choice, CommandPalette, FilterGroup, Segmented, Swatch,
                      form_row, heading, hint)

logger = logging.getLogger(__name__)

#: Edits are coalesced for this long before anything is recomputed. Long enough that typing
#: never triggers a rebuild, short enough to feel immediate.
_DEBOUNCE_MS = 70


# --------------------------------------------------------------------------------------
# Background work
# --------------------------------------------------------------------------------------

class _Signals(QtCore.QObject):
    built = QtCore.Signal(object, object)       # Built, PlotState
    failed = QtCore.Signal(str)
    saved = QtCore.Signal(object, object, str)  # path, table, error


class _BuildJob(QtCore.QRunnable):
    """`build_series` off the GUI thread: it is pandas, and pandas is not instant."""

    def __init__(self, frames, state, signals: _Signals) -> None:
        super().__init__()
        self.frames, self.state, self.signals = frames, state, signals

    def run(self) -> None:
        try:
            built = m.build_series(self.frames, self.state)
        except Exception as exc:                 # a bad filter must not kill the window
            logger.exception("build failed")
            self.signals.failed.emit(str(exc))
            return
        self.signals.built.emit(built, self.state)


class _SaveJob(QtCore.QRunnable):
    def __init__(self, frames, state, path, fmt, transparent, with_data,
                 signals: _Signals) -> None:
        super().__init__()
        self.args = (frames, state, path, fmt, transparent, with_data)
        self.signals = signals

    def run(self) -> None:
        frames, state, path, fmt, transparent, with_data = self.args
        try:
            written, rendered = m.save_figure(frames, state, path, fmt, transparent=transparent)
            table = m.export_table(rendered, written.with_suffix(".csv")) if with_data else None
            self.signals.saved.emit(written, table, "")
        except Exception as exc:
            logger.exception("save failed")
            self.signals.saved.emit(None, None, str(exc))


# --------------------------------------------------------------------------------------
# The window
# --------------------------------------------------------------------------------------

class MainWindow(QtWidgets.QMainWindow):
    def __init__(self, csvs: list[str] | None = None, session: str | None = None) -> None:
        super().__init__()
        self.state = m.PlotState(theme="dark")
        self.cache: dict[str, tuple[float, pd.DataFrame]] = {}
        self.links: dict[str, dict[str, dict[str, str]]] = {}
        self.built = m.Built([], [])
        self.styles: dict[str, m.Resolved] = {}
        self.library = StyleLibrary()
        self.use_library = True
        #: "" = the whole figure; otherwise the index of the plot the Axes tab is editing.
        self.editing = ""
        self._series_keys: list[str] = []
        self._filter_widgets: dict[str, FilterGroup] = {}
        self._dims_signature: tuple = ()
        self._slot_signature: tuple = ()
        self._loading = False
        self._pending_data = False
        self._last_build_ms = 0

        self.signals = _Signals()
        self.signals.built.connect(self._built_ready)
        self.signals.failed.connect(lambda msg: self._status(f"✖ {msg}", bad=True))
        self.signals.saved.connect(self._save_done)
        self.pool = QtCore.QThreadPool.globalInstance()

        self._timer = QtCore.QTimer(self)
        self._timer.setSingleShot(True)
        self._timer.setInterval(_DEBOUNCE_MS)
        self._timer.timeout.connect(self._flush)

        self.setWindowTitle("mpmc plot studio")
        self.resize(1560, 960)
        self._build_ui()
        self._apply_theme()
        self._shortcuts()

        if session:
            self.load_session(session)
        elif csvs:
            self.add_csvs(csvs)
        else:
            self._sync_controls()

    # -- construction --------------------------------------------------------------------

    def _build_ui(self) -> None:
        self._build_toolbar()

        splitter = QtWidgets.QSplitter(QtCore.Qt.Horizontal)
        splitter.setHandleWidth(6)
        splitter.addWidget(self._build_sidebar())
        right = QtWidgets.QSplitter(QtCore.Qt.Vertical)
        right.setHandleWidth(6)
        self.chart = ChartPane(skin(self.state.theme))
        self.chart.pointHovered.connect(self._hovered)
        self.chart.seriesToggled.connect(self._toggle_series)
        self.chart.plotSelected.connect(self._plot_clicked)
        right.addWidget(self.chart)
        right.addWidget(self._build_bottom())
        right.setStretchFactor(0, 4)
        right.setStretchFactor(1, 1)
        splitter.addWidget(right)
        splitter.setStretchFactor(0, 0)
        splitter.setStretchFactor(1, 1)
        splitter.setSizes([430, 1100])

        container = QtWidgets.QWidget()
        box = QtWidgets.QVBoxLayout(container)
        box.setContentsMargins(10, 6, 10, 6)
        box.addWidget(splitter)
        self.setCentralWidget(container)

        bar = self.statusBar()
        self.readout = QtWidgets.QLabel("")
        self.chip = QtWidgets.QLabel("Ready")
        self.chip.setObjectName("Chip")
        bar.addWidget(self.readout, 1)
        bar.addPermanentWidget(self.chip)

    def _build_toolbar(self) -> None:
        bar = QtWidgets.QToolBar()
        bar.setMovable(False)
        bar.setIconSize(QtCore.QSize(16, 16))
        self.addToolBar(bar)

        def button(text: str, slot, primary: bool = False) -> QtWidgets.QPushButton:
            b = QtWidgets.QPushButton(text)
            if primary:
                b.setObjectName("Primary")
            b.clicked.connect(slot)
            bar.addWidget(b)
            return b

        button("Add CSV…", self.ask_add_csv, primary=True)
        button("Save figure…", self.ask_save_figure)
        button("Export data…", self.ask_export_data)
        bar.addSeparator()
        button("Reset view", lambda: self.chart.reset_view())
        button("Commands  ⌘K", self.open_palette)
        spacer = QtWidgets.QWidget()
        spacer.setSizePolicy(QtWidgets.QSizePolicy.Expanding, QtWidgets.QSizePolicy.Preferred)
        bar.addWidget(spacer)
        self.theme_switch = Segmented({"dark": "Dark", "light": "Light"})
        self.theme_switch.set_value(self.state.theme)
        self.theme_switch.changed.connect(self._set_theme)
        bar.addWidget(self.theme_switch)

    def _scroll_tab(self) -> tuple[QtWidgets.QScrollArea, QtWidgets.QVBoxLayout]:
        area = QtWidgets.QScrollArea()
        area.setWidgetResizable(True)
        area.setHorizontalScrollBarPolicy(QtCore.Qt.ScrollBarAlwaysOff)
        inner = QtWidgets.QWidget()
        inner.setObjectName("Sidebar")
        box = QtWidgets.QVBoxLayout(inner)
        box.setContentsMargins(4, 4, 8, 4)
        box.setSpacing(10)
        area.setWidget(inner)
        return area, box

    def _build_sidebar(self) -> QtWidgets.QWidget:
        self.sidebar = QtWidgets.QTabWidget()
        self.sidebar.setDocumentMode(True)
        self.sidebar.addTab(self._tab_data(), "Data")
        self.sidebar.addTab(self._tab_series(), "Series")
        self.sidebar.addTab(self._tab_axes(), "Axes")
        self.sidebar.setMinimumWidth(380)
        return self.sidebar

    # ---- Data tab

    def _tab_data(self) -> QtWidgets.QWidget:
        area, box = self._scroll_tab()

        files = Card("Loaded files")
        self.tree = QtWidgets.QTreeWidget()
        self.tree.setHeaderLabels(["File", "Plot title"])
        self.tree.setRootIsDecorated(False)
        self.tree.setAlternatingRowColors(True)
        self.tree.setMinimumHeight(130)
        self.tree.itemChanged.connect(self._tree_changed)
        self.tree.currentItemChanged.connect(lambda *_: self._sync_panel_title())
        self.tree.itemDoubleClicked.connect(lambda *_: self.only_selected())
        files.add(self.tree)
        row = QtWidgets.QHBoxLayout()
        for text, slot in (("Add…", self.ask_add_csv), ("Only this", self.only_selected),
                           ("Remove", self.remove_selected), ("↑", lambda: self.move_panel(-1)),
                           ("↓", lambda: self.move_panel(1))):
            b = QtWidgets.QPushButton(text)
            b.clicked.connect(slot)
            row.addWidget(b)
        files.add_layout(row)
        self.file_info = hint("")
        files.add(self.file_info)
        box.addWidget(files)

        titles = Card("Titles")
        self.figure_title = QtWidgets.QLineEdit()
        self.figure_title.setPlaceholderText("automatic")
        self.figure_title.editingFinished.connect(self._commit_titles)
        titles.add_layout(form_row("Figure", self.figure_title))
        self.plot_title = QtWidgets.QLineEdit()
        self.plot_title.setPlaceholderText("automatic")
        self.plot_title.editingFinished.connect(self._commit_titles)
        titles.add_layout(form_row("Selected file", self.plot_title))
        titles.add(hint("A compared parameter draws one file as several plots; name those "
                        "one at a time in Axes › Which plot."))
        box.addWidget(titles)

        what = Card("What to plot")
        self.kind = Choice(m.KINDS)
        self.kind.chosen.connect(lambda v: self._set("kind", v))
        what.add_layout(form_row("Plot type", self.kind))
        self.metric_choice = Choice()
        self.metric_choice.chosen.connect(lambda v: self._set("metric", v))
        what.add_layout(form_row("Y: metric", self.metric_choice))
        self.stat = Choice({"median": "Median of runs", "mean": "Mean of runs",
                            "min": "Minimum", "max": "Maximum"})
        self.stat.chosen.connect(lambda v: self._set("stat", v))
        what.add_layout(form_row("Statistic", self.stat))
        self.xaxis = Choice()
        self.xaxis.chosen.connect(lambda v: self._set("x", v))
        what.add_layout(form_row("X axis", self.xaxis))
        self.errorbars = Choice(m.ERRORBARS)
        self.errorbars.chosen.connect(lambda v: self._set("errorbars", v))
        what.add_layout(form_row("Error bars", self.errorbars))
        box.addWidget(what)

        base = Card("Baseline")
        base.add(hint("Express every series against a reference instead of in absolute units."))
        self.rel_mode = Choice(analysis.RELATIVE_MODES)
        self.rel_mode.chosen.connect(self._set_baseline_mode)
        base.add_layout(form_row("Show", self.rel_mode))
        self.rel_scope = Choice({"series": "A series in each plot",
                                 "panel": "The same series in another plot"})
        self.rel_scope.chosen.connect(self._set_baseline_scope)
        base.add_layout(form_row("Compare to", self.rel_scope))
        self.rel_target = Choice()
        self.rel_target.chosen.connect(self._set_baseline_target)
        base.add_layout(form_row("Baseline", self.rel_target))
        box.addWidget(base)

        filters = Card("Filters")
        filters.add(hint("Tick several values of one parameter to compare them: the lines "
                         "split per value instead of being averaged together."))
        self.link_filters = QtWidgets.QCheckBox("Link related filters")
        self.link_filters.setChecked(True)
        filters.add(self.link_filters)
        self.filter_box = QtWidgets.QVBoxLayout()
        self.filter_box.setSpacing(2)
        filters.add_layout(self.filter_box)
        box.addWidget(filters)

        box.addStretch(1)
        return area

    # ---- Series tab

    def _tab_series(self) -> QtWidgets.QWidget:
        area, box = self._scroll_tab()

        tools = Card("Series")
        self.search = QtWidgets.QLineEdit()
        self.search.setPlaceholderText("Filter series…")
        self.search.textChanged.connect(self._filter_series_rows)
        tools.add(self.search)
        row = QtWidgets.QHBoxLayout()
        for text, slot in (("Show all", lambda: self._set_all_visible(True)),
                           ("Hide all", lambda: self._set_all_visible(False)),
                           ("Reset styles", self.reset_styles)):
            b = QtWidgets.QPushButton(text)
            b.clicked.connect(slot)
            row.addWidget(b)
        tools.add_layout(row)
        box.addWidget(tools)

        self.series_card = Card("")
        self.series_box = QtWidgets.QVBoxLayout()
        self.series_box.setSpacing(6)
        self.series_card.add_layout(self.series_box)
        box.addWidget(self.series_card)

        lib = Card("Style library")
        lib.add(hint("Remembered per implementation, so u-pscq keeps its colour, marker and "
                     "name in every figure and every session."))
        self.library_on = QtWidgets.QCheckBox("Use the library")
        self.library_on.setChecked(True)
        self.library_on.toggled.connect(self._toggle_library)
        lib.add(self.library_on)
        row = QtWidgets.QHBoxLayout()
        pin = QtWidgets.QPushButton("Pin current styles")
        pin.clicked.connect(self.pin_styles)
        forget = QtWidgets.QPushButton("Forget all")
        forget.clicked.connect(self.forget_styles)
        row.addWidget(pin)
        row.addWidget(forget)
        lib.add_layout(row)
        self.library_info = hint("")
        lib.add(self.library_info)
        box.addWidget(lib)

        box.addStretch(1)
        return area

    # ---- Axes tab

    def _tab_axes(self) -> QtWidgets.QWidget:
        area, box = self._scroll_tab()

        which = Card("Which plot")
        which.add(hint("Labels, ticks, scales, grid and legend belong to the plot chosen "
                       "here — click a plot on the chart to select it. The plot grid, the "
                       "shared axes and the export size are always the whole figure."))
        self.plot_pick = Choice({"": "All plots"})
        self.plot_pick.chosen.connect(self._edit_plot)
        which.add_layout(form_row("Editing", self.plot_pick))
        self.override_info = hint("")
        which.add(self.override_info)
        self.reset_plot_button = QtWidgets.QPushButton("Reset this plot")
        self.reset_plot_button.clicked.connect(self.reset_plot_axes)
        which.add(self.reset_plot_button)
        box.addWidget(which)

        labels = Card("Labels")
        self.plot_title_axes = QtWidgets.QLineEdit()
        self.plot_title_axes.setPlaceholderText("automatic")
        self.plot_title_axes.editingFinished.connect(
            lambda: self._set_axis("title", self.plot_title_axes.text() or None))
        labels.add_layout(form_row("Plot title", self.plot_title_axes))
        self.xlabel = QtWidgets.QLineEdit()
        self.xlabel.setPlaceholderText("automatic")
        self.xlabel.editingFinished.connect(
            lambda: self._set_axis("xlabel", self.xlabel.text() or None))
        labels.add_layout(form_row("X label", self.xlabel))
        self.ylabel = QtWidgets.QLineEdit()
        self.ylabel.setPlaceholderText("automatic")
        self.ylabel.editingFinished.connect(
            lambda: self._set_axis("ylabel", self.ylabel.text() or None))
        labels.add_layout(form_row("Y label", self.ylabel))
        box.addWidget(labels)

        scales = Card("Scales")
        self.xlog = QtWidgets.QCheckBox("Logarithmic x")
        self.xlog.toggled.connect(lambda v: self._set_axis("xlog", v))
        self.ylog = QtWidgets.QCheckBox("Logarithmic y")
        self.ylog.toggled.connect(lambda v: self._set_axis("ylog", v))
        self.zero = QtWidgets.QCheckBox("Start y at zero")
        self.zero.toggled.connect(lambda v: self._set("y_from_zero", v))
        for w in (self.xlog, self.ylog, self.zero):
            scales.add(w)
        self.ymin = QtWidgets.QLineEdit()
        self.ymin.setPlaceholderText("auto")
        self.ymin.editingFinished.connect(lambda: self._set_number("ymin", self.ymin.text()))
        scales.add_layout(form_row("Y minimum", self.ymin))
        self.ymax = QtWidgets.QLineEdit()
        self.ymax.setPlaceholderText("auto")
        self.ymax.editingFinished.connect(lambda: self._set_number("ymax", self.ymax.text()))
        scales.add_layout(form_row("Y maximum", self.ymax))
        scales.add(hint("A y limit or a log y is a property of one axis, so it needs "
                        "“normalise y” off to apply to a single plot."))
        box.addWidget(scales)

        ticks = Card("Ticks")
        self.xtick_mode = Choice(m.X_TICK_MODES)
        self.xtick_mode.chosen.connect(lambda v: self._set_tick("xticks", mode=v))
        ticks.add_layout(form_row("X ticks", self.xtick_mode))
        self.xtick_value = QtWidgets.QDoubleSpinBox()
        self.xtick_value.setRange(0.0001, 1e9)
        self.xtick_value.setDecimals(4)
        self.xtick_value.valueChanged.connect(lambda v: self._set_tick("xticks", value=v))
        ticks.add_layout(form_row("N / spacing", self.xtick_value))
        self.ytick_mode = Choice(m.Y_TICK_MODES)
        self.ytick_mode.chosen.connect(lambda v: self._set_tick("yticks", mode=v))
        ticks.add_layout(form_row("Y ticks", self.ytick_mode))
        self.ytick_value = QtWidgets.QDoubleSpinBox()
        self.ytick_value.setRange(0.0001, 1e9)
        self.ytick_value.setDecimals(4)
        self.ytick_value.valueChanged.connect(lambda v: self._set_tick("yticks", value=v))
        ticks.add_layout(form_row("N / spacing", self.ytick_value))
        box.addWidget(ticks)

        layout = Card("Layout")
        self.legend = Choice(m.LEGEND_PLACES)
        self.legend.chosen.connect(lambda v: self._set_axis("legend", v))
        layout.add_layout(form_row("Legend", self.legend))
        self.grid = Choice(m.GRIDS)
        self.grid.chosen.connect(lambda v: self._set_axis("grid", v))
        layout.add_layout(form_row("Grid", self.grid))
        self.columns = QtWidgets.QSpinBox()
        self.columns.setRange(0, 6)
        self.columns.setSpecialValueText("automatic")
        self.columns.valueChanged.connect(lambda v: self._set("columns", v))
        layout.add_layout(form_row("Plots per row", self.columns))
        self.share_y = QtWidgets.QCheckBox("Normalise y across plots")
        self.share_y.setToolTip("Every plot gets the widest y range among them, so heights "
                                "can be compared between plots.")
        self.share_y.toggled.connect(lambda v: self._set("share_y", v))
        layout.add(self.share_y)
        self.share_x = QtWidgets.QCheckBox("Normalise x across plots")
        self.share_x.setToolTip("Off by default: two sweeps over different thread ranges "
                                "would leave half a plot empty.")
        self.share_x.toggled.connect(lambda v: self._set("share_x", v))
        layout.add(self.share_x)
        box.addWidget(layout)

        export = Card("Export size")
        self.width_box = QtWidgets.QDoubleSpinBox()
        self.width_box.setRange(2, 40)
        self.width_box.setSuffix(" in")
        self.width_box.valueChanged.connect(lambda v: setattr(self.state, "width", v))
        export.add_layout(form_row("Width", self.width_box))
        self.height_box = QtWidgets.QDoubleSpinBox()
        self.height_box.setRange(2, 40)
        self.height_box.setSuffix(" in")
        self.height_box.valueChanged.connect(lambda v: setattr(self.state, "height", v))
        export.add_layout(form_row("Height", self.height_box))
        self.dpi = QtWidgets.QSpinBox()
        self.dpi.setRange(72, 600)
        self.dpi.valueChanged.connect(lambda v: setattr(self.state, "dpi", v))
        export.add_layout(form_row("DPI (PNG)", self.dpi))
        box.addWidget(export)

        box.addStretch(1)
        return area

    # ---- bottom dock

    def _build_bottom(self) -> QtWidgets.QWidget:
        self.bottom = QtWidgets.QTabWidget()
        self.bottom.setDocumentMode(True)

        self.table = QtWidgets.QTableWidget()
        self.table.setSortingEnabled(True)
        self.table.setAlternatingRowColors(True)
        self.table.setEditTriggers(QtWidgets.QAbstractItemView.NoEditTriggers)
        self.table.setSelectionBehavior(QtWidgets.QAbstractItemView.SelectRows)
        self.bottom.addTab(self.table, "Table")

        self.rank_table = QtWidgets.QTableWidget()
        self.rank_table.setAlternatingRowColors(True)
        self.rank_table.setEditTriggers(QtWidgets.QAbstractItemView.NoEditTriggers)
        self.bottom.addTab(self.rank_table, "Ranking")

        self.peak_table = QtWidgets.QTableWidget()
        self.peak_table.setAlternatingRowColors(True)
        self.peak_table.setEditTriggers(QtWidgets.QAbstractItemView.NoEditTriggers)
        self.bottom.addTab(self.peak_table, "Summary")

        self.notes = QtWidgets.QPlainTextEdit()
        self.notes.setReadOnly(True)
        self.bottom.addTab(self.notes, "Notes")
        self.bottom.setMinimumHeight(150)
        self._stale_tabs = {0, 1, 2}
        self._tab_rows: dict[int, list[dict]] = {}
        self.bottom.currentChanged.connect(self._tab_shown)
        return self.bottom

    def _shortcuts(self) -> None:
        for keys, slot in (("Ctrl+O", self.ask_add_csv), ("Ctrl+S", self.ask_save_figure),
                           ("Ctrl+E", self.ask_export_data), ("Ctrl+R", self.reload),
                           ("Ctrl+K", self.open_palette), ("Ctrl+0", lambda: self.chart.reset_view()),
                           ("Ctrl+Shift+S", self.ask_save_session),
                           ("Ctrl+L", self.ask_load_session)):
            QtGui.QShortcut(QtGui.QKeySequence(keys), self, activated=slot)

    # -- theme ---------------------------------------------------------------------------

    def _apply_theme(self) -> None:
        """Dress this window.

        On the window rather than on the application: a style sheet set on QApplication is
        repolished onto every widget that exists, so with more than one window open -- or,
        in the tests, more than one built in a session -- each new theme costs the whole
        process. Qt cascades into children, and the dialogs are parented here, so this
        reaches everything this window owns and nothing it does not.
        """
        s = skin(self.state.theme)
        self.setStyleSheet(stylesheet(s, 1.0))
        self.chart.apply_skin(s)

    def _set_theme(self, value: str) -> None:
        self.state.theme = value
        self.theme_switch.set_value(value)       # so the palette and the switch agree
        self._apply_theme()
        self.redraw()

    # -- files ---------------------------------------------------------------------------

    def ask_add_csv(self) -> None:
        paths, _ = QtWidgets.QFileDialog.getOpenFileNames(
            self, "Add results CSV", str(Path.cwd()), "CSV files (*.csv);;All files (*)")
        if paths:
            self.add_csvs(paths)

    def add_csvs(self, paths: list[str]) -> None:
        known = {p.csv for p in self.state.library}
        for raw in paths:
            path = str(Path(raw).expanduser().resolve())
            if path in known:
                continue
            try:
                self._frame(path)
            except (OSError, ValueError) as exc:
                QtWidgets.QMessageBox.critical(self, "Could not read", f"{path}\n\n{exc}")
                continue
            self.state.library.append(m.Panel(path))
        self._refresh_tree()
        self.schedule(data=True)

    def _frame(self, path: str) -> pd.DataFrame:
        stamp = Path(path).stat().st_mtime
        hit = self.cache.get(path)
        if hit is None or hit[0] != stamp:
            self.cache[path] = (stamp, m.load_frame(path))
        return self.cache[path][1]

    def frames(self) -> list[pd.DataFrame]:
        return [self._frame(p.csv) for p in self.state.panels]

    def all_frames(self) -> list[pd.DataFrame]:
        return [self._frame(p.csv) for p in self.state.library]

    def reload(self) -> None:
        self.cache.clear()
        self.schedule(data=True)
        self._status("Reloaded from disk")

    def _refresh_tree(self) -> None:
        self._loading = True
        self.tree.clear()
        for panel in self.state.library:
            item = QtWidgets.QTreeWidgetItem([Path(panel.csv).name, panel.title or ""])
            item.setFlags(item.flags() | QtCore.Qt.ItemIsUserCheckable
                          | QtCore.Qt.ItemIsEditable)
            item.setCheckState(0, QtCore.Qt.Checked if panel.plotted else QtCore.Qt.Unchecked)
            item.setToolTip(0, panel.csv)
            self.tree.addTopLevelItem(item)
        self.tree.resizeColumnToContents(0)
        if self.state.library and self.tree.currentItem() is None:
            self.tree.setCurrentItem(self.tree.topLevelItem(0))
        self._loading = False
        self._describe_files()

    def _describe_files(self) -> None:
        frames = self.all_frames()
        if not frames:
            self.file_info.setText("No files loaded.")
            return
        rows = sum(len(f) for f in frames)
        queues = len({q for f in frames for q in f["Queue"].unique()}) if frames else 0
        dims = m.dimension_values(frames)
        varying = [f"{m.AXIS_NAMES.get(c, c).lower()} {len(v)}" for c, v in dims.items()
                   if len(v) > 1]
        self.file_info.setText(f"{rows} rows · {queues} queues · varies: "
                               + (", ".join(varying) or "nothing"))

    def _tree_changed(self, item: QtWidgets.QTreeWidgetItem, column: int) -> None:
        if self._loading:
            return
        index = self.tree.indexOfTopLevelItem(item)
        if not 0 <= index < len(self.state.library):
            return
        if column == 0:
            wanted = item.checkState(0) == QtCore.Qt.Checked
            if not wanted and len(self.state.panels) == 1 and self.state.library[index].plotted:
                item.setCheckState(0, QtCore.Qt.Checked)      # never leave nothing plotted
                return
            self.state.library[index].plotted = wanted
        else:
            self.state.library[index].title = item.text(1) or None
        self.schedule(data=True)

    def only_selected(self) -> None:
        index = self.tree.indexOfTopLevelItem(self.tree.currentItem())
        if index < 0:
            return
        for i, panel in enumerate(self.state.library):
            panel.plotted = (i == index)
        self._refresh_tree()
        self.schedule(data=True)

    def remove_selected(self) -> None:
        index = self.tree.indexOfTopLevelItem(self.tree.currentItem())
        if index < 0 or len(self.state.library) <= 1:
            return
        del self.state.library[index]
        if not self.state.panels:
            self.state.library[0].plotted = True
        self._refresh_tree()
        self.schedule(data=True)

    def move_panel(self, step: int) -> None:
        i = self.tree.indexOfTopLevelItem(self.tree.currentItem())
        j = i + step
        if i < 0 or not 0 <= j < len(self.state.library):
            return
        lib = self.state.library
        lib[i], lib[j] = lib[j], lib[i]
        self._refresh_tree()
        self.tree.setCurrentItem(self.tree.topLevelItem(j))
        self.schedule(data=True)

    def _sync_panel_title(self) -> None:
        i = self.tree.indexOfTopLevelItem(self.tree.currentItem())
        if 0 <= i < len(self.state.library):
            self.plot_title.setText(self.state.library[i].title or "")

    def _commit_titles(self) -> None:
        self.state.title = self.figure_title.text() or None
        i = self.tree.indexOfTopLevelItem(self.tree.currentItem())
        if 0 <= i < len(self.state.library):
            self.state.library[i].title = self.plot_title.text() or None
            self._refresh_tree()
        self.redraw()

    # -- state edits ---------------------------------------------------------------------

    def _set(self, field: str, value) -> None:
        """Edit a figure-wide field. Ignored while the controls are being filled in."""
        if self._loading or getattr(self.state, field) == value:
            return
        setattr(self.state, field, value)
        self.schedule(data=field in ("kind", "metric", "stat", "x", "errorbars"))

    def _set_number(self, field: str, text: str) -> None:
        try:
            value = float(text) if text.strip() else None
        except ValueError:
            value = None
        self._set_axis(field, value)

    # -- per-plot axes -------------------------------------------------------------------

    def _editing_slot(self) -> m.Slot | None:
        """The plot the Axes tab is pointed at, or None for the whole figure."""
        if not self.editing:
            return None
        slots = self.state.slots()
        i = int(self.editing)
        return slots[i] if 0 <= i < len(slots) else None

    def _set_axis(self, field: str, value) -> None:
        """Edit an axis field: the figure's, or the selected plot's override of it."""
        if self._loading:
            return
        slot = self._editing_slot()
        if slot is None:
            self._set(field, value)
            return
        over = self.state.overrides.setdefault(slot.ident, {})
        if field in over and over[field] == value:
            return
        over[field] = value
        self.redraw()
        self._describe_overrides()

    def _set_tick(self, which: str, mode: str | None = None, value: float | None = None) -> None:
        if self._loading:
            return
        if self._editing_slot() is None:
            spec = getattr(self.state, which)
            if mode is not None:
                spec.mode = mode
            if value is not None:
                spec.value = value
            self.redraw()
            return
        base = getattr(self.state.axes_for(int(self.editing)), which)
        self._set_axis(which, m.TickSpec(mode if mode is not None else base.mode,
                                         value if value is not None else base.value))

    def _plot_clicked(self, slot: int) -> None:
        """Clicking a plot means "this one": show the tab that edits it."""
        self._edit_plot(str(slot))
        self.sidebar.setCurrentIndex(2)

    def _edit_plot(self, value: str) -> None:
        self.editing = value if value and value.isdigit() else ""
        self.plot_pick.set_value(self.editing)
        self.chart.select(int(self.editing) if self.editing else -1)
        self._sync_axes_controls()

    def reset_plot_axes(self) -> None:
        """Drop this plot's overrides, so it follows the figure again."""
        slot = self._editing_slot()
        if slot is None or slot.ident not in self.state.overrides:
            return
        del self.state.overrides[slot.ident]
        self.redraw()
        self._sync_axes_controls()

    def _sync_plot_picker(self) -> None:
        slots = self.state.slots()
        options = {"": "All plots"}
        for i, slot in enumerate(slots):
            options[str(i)] = (f"Plot {i + 1} · "
                               f"{self.state.panel_title(i) or Path(slot.csv).stem}")
        signature = tuple(options.items())
        if signature != self._slot_signature:
            self._slot_signature = signature
            self.plot_pick.set_options(options)
        if self.editing and int(self.editing) >= len(slots):
            self._edit_plot("")                  # the plot it was editing is gone
        else:
            self.plot_pick.set_value(self.editing)
            self._sync_axes_controls()

    def _figure_axes(self) -> m.AxesSpec:
        s = self.state
        return m.AxesSpec(title="", xlabel=s.xlabel, ylabel=s.ylabel, xlog=s.xlog,
                          ylog=s.ylog, ymin=s.ymin, ymax=s.ymax, xticks=s.xticks,
                          yticks=s.yticks, grid=s.grid, legend=s.legend)

    def _sync_axes_controls(self) -> None:
        """Fill the Axes tab from whatever it is pointed at."""
        slot = self._editing_slot()
        single = slot is not None
        spec = self.state.axes_for(int(self.editing)) if single else self._figure_axes()
        self._loading = True
        try:
            self.plot_title_axes.setEnabled(single)
            self.plot_title_axes.setText(spec.title if single else "")
            self.plot_title_axes.setPlaceholderText("automatic" if single
                                                    else "select a plot to name it")
            self.xlabel.setText(spec.xlabel or "")
            self.ylabel.setText(spec.ylabel or "")
            self.xlog.setChecked(spec.xlog)
            self.ylog.setChecked(spec.ylog)
            self.ymin.setText("" if spec.ymin is None else f"{spec.ymin:g}")
            self.ymax.setText("" if spec.ymax is None else f"{spec.ymax:g}")
            self.xtick_mode.set_value(spec.xticks.mode)
            self.xtick_value.setValue(max(0.0001, spec.xticks.value))
            self.ytick_mode.set_value(spec.yticks.mode)
            self.ytick_value.setValue(max(0.0001, spec.yticks.value))
            self.legend.set_value(self.state.legend_place(int(self.editing)) if single
                                  else spec.legend)
            self.grid.set_value(spec.grid)
            self.reset_plot_button.setEnabled(single)
        finally:
            self._loading = False
        self._describe_overrides()

    def _describe_overrides(self) -> None:
        slot = self._editing_slot()
        if slot is None:
            n = len(self.state.overrides)
            self.override_info.setText(
                f"Editing every plot · {n} plot{'s' if n != 1 else ''} with their own settings"
                if n else "Editing every plot.")
            return
        over = self.state.overrides.get(slot.ident, {})
        self.override_info.setText(
            ("Its own: " + ", ".join(sorted(over))) if over
            else "Follows the figure in everything.")

    def _set_baseline_mode(self, value: str) -> None:
        self.state.baseline_mode = value
        self.redraw()

    def _set_baseline_scope(self, value: str) -> None:
        self.state.baseline_scope = value
        self._sync_baseline_targets()
        self.redraw()

    def _set_baseline_target(self, value: str) -> None:
        self.state.baseline_target = value
        self.redraw()

    # -- filters -------------------------------------------------------------------------

    def _rebuild_filters(self, dims: dict[str, list[str]]) -> None:
        while self.filter_box.count():
            item = self.filter_box.takeAt(0)
            if item.widget():
                item.widget().deleteLater()
        self._filter_widgets.clear()
        for column, values in dims.items():
            if len(values) < 2 and column not in self.state.filters:
                continue
            linked = ", ".join(m.AXIS_NAMES.get(c, c).lower()
                               for c in self.links.get(column, {}))
            selected = self.state.filters.get(column, values)
            group = FilterGroup(column, m.AXIS_NAMES.get(column, column), values,
                                selected, linked, self._compare_mode(column, selected),
                                locked=column == self.state.effective_x)
            group.changed.connect(self._filter_changed)
            group.modeChanged.connect(self._compare_changed)
            self.filter_box.addWidget(group)
            self._filter_widgets[column] = group

    def _compare_mode(self, column: str, selected: list[str]) -> str:
        """What this parameter's ticked values mean, defaulted from what is already ticked.

        Several values ticked with nothing said about them is the old behaviour -- several
        lines on one plot -- and that is what the thread counts arrive in.
        """
        return self.state.compare.get(column) or ("overlay" if len(selected) > 1 else "one")

    def _filter_changed(self, column: str, values: list[str]) -> None:
        self.state.filters[column] = values
        if self.link_filters.isChecked():
            # A column being compared holds a selection made on purpose; the link may fill in
            # a column nobody has touched, but it may not undo one.
            pinned = {c for c, w in self._filter_widgets.items()
                      if c != column and w.mode != "one"}
            for moved in m.propagate(self.state.filters, column, self.links, pinned):
                widget = self._filter_widgets.get(moved)
                if widget is not None:
                    widget.set_selected(self.state.filters[moved])
        self.schedule(data=True)

    def _compare_changed(self, column: str, mode: str) -> None:
        if mode == "one":
            self.state.compare.pop(column, None)
        else:
            self.state.compare[column] = mode
        self.schedule(data=True)                 # the plot plan itself changed

    # -- series --------------------------------------------------------------------------

    def _rebuild_series_rows(self) -> None:
        while self.series_box.count():
            item = self.series_box.takeAt(0)
            if item.widget():
                item.widget().deleteLater()
            elif item.layout():
                QtWidgets.QWidget().setLayout(item.layout())
        self._rows = {}
        # Resolved here rather than trusting self.styles: those are from the last _present(),
        # and "Hide all" edits the state and rebuilds the rows before the next one -- which
        # is why every checkbox stayed ticked after hiding everything.
        self.styles = m.resolve_styles(self.built, self.state)
        for key in self.built.keys():
            st = self.styles.get(key)
            if st is None:
                continue
            row = QtWidgets.QWidget()
            grid = QtWidgets.QGridLayout(row)
            grid.setContentsMargins(0, 0, 0, 0)
            grid.setSpacing(5)

            visible = QtWidgets.QCheckBox()
            visible.setChecked(st.visible)
            visible.toggled.connect(lambda v, k=key: self._style(k, visible=v))
            swatch = Swatch()
            swatch.set_color(st.color)
            swatch.picked.connect(lambda c, k=key: self._style(k, color=c))
            name = QtWidgets.QLineEdit(st.label)
            name.editingFinished.connect(lambda k=key, e=name: self._style(k, label=e.text()))
            grid.addWidget(visible, 0, 0)
            grid.addWidget(swatch, 0, 1)
            grid.addWidget(name, 0, 2, 1, 3)

            marker = Choice({**{"": "no marker"}, **{k: v for k, v in m.MARKERS.items() if k}})
            marker.setSizeAdjustPolicy(QtWidgets.QComboBox.AdjustToMinimumContentsLengthWithIcon)
            marker.setMinimumContentsLength(6)
            marker.set_value(st.marker)
            marker.chosen.connect(lambda v, k=key: self._style(k, marker=v))
            line = Choice(m.LINESTYLES)
            line.setSizeAdjustPolicy(QtWidgets.QComboBox.AdjustToMinimumContentsLengthWithIcon)
            line.setMinimumContentsLength(6)
            line.set_value(st.linestyle)
            line.chosen.connect(lambda v, k=key: self._style(k, linestyle=v))
            width = QtWidgets.QDoubleSpinBox()
            width.setRange(0.5, 8.0)
            width.setSingleStep(0.5)
            width.setValue(st.linewidth)
            width.setMaximumWidth(78)
            width.valueChanged.connect(lambda v, k=key: self._style(k, linewidth=v))
            # The whole row's width, not the three columns left over beside the swatch:
            # three combos at their natural size do not fit a 430 px sidebar, and the
            # scroll area has no horizontal bar to reach the one that falls off the end.
            controls = QtWidgets.QHBoxLayout()
            controls.setSpacing(5)
            for w, stretch in ((marker, 1), (line, 1), (width, 0)):
                w.setMinimumWidth(0)
                controls.addWidget(w, stretch)
            grid.addLayout(controls, 1, 0, 1, 5)
            grid.setColumnStretch(2, 1)

            self.series_box.addWidget(row)
            self._rows[key] = row
        self._filter_series_rows(self.search.text())

    def _filter_series_rows(self, text: str) -> None:
        needle = text.lower().strip()
        for key, row in getattr(self, "_rows", {}).items():
            label = self.styles[key].label.lower() if key in self.styles else key.lower()
            row.setVisible(needle in label or needle in key.lower())

    def _style(self, key: str, **changes) -> None:
        override = self.state.styles.setdefault(key, m.SeriesStyle())
        for field, value in changes.items():
            setattr(override, field, value)
        self.redraw()

    def _toggle_series(self, key: str) -> None:
        current = self.styles.get(key)
        if current is not None:
            self._style(key, visible=not current.visible)
            self._rebuild_series_rows()

    def _set_all_visible(self, visible: bool) -> None:
        for key in self.built.keys():
            self.state.styles.setdefault(key, m.SeriesStyle()).visible = visible
        self.redraw()
        self._rebuild_series_rows()

    def reset_styles(self) -> None:
        self.state.styles.clear()
        self.redraw()
        self._rebuild_series_rows()

    # -- style library -------------------------------------------------------------------

    def _toggle_library(self, on: bool) -> None:
        self.use_library = on
        self.redraw()

    def pin_styles(self) -> None:
        count = self.library.remember(self.state, self.built)
        self._status(f"Pinned {count} series to the style library")
        self._describe_library()

    def forget_styles(self) -> None:
        self.library.clear()
        self._status("Style library cleared")
        self._describe_library()

    def _describe_library(self) -> None:
        n = len(self.library.entries)
        self.library_info.setText(
            f"{n} implementation{'s' if n != 1 else ''} remembered · {self.library.path}")

    # -- the update cycle ----------------------------------------------------------------

    def schedule(self, data: bool = False) -> None:
        """Ask for a refresh. Coalesced, so a burst of edits costs one rebuild."""
        self._pending_data = self._pending_data or data
        self._timer.start()

    def redraw(self) -> None:
        self.schedule(data=False)

    def _flush(self) -> None:
        if not self.state.panels:
            self.chart.draw(m.Built([], []), {}, self.state, "", 1.0)
            self._status("Add a results CSV to start  (Ctrl+O)")
            return
        if self._pending_data:
            self._pending_data = False
            self._status("Working…")
            frames = self.frames()
            self._reconcile(frames)
            self.pool.start(_BuildJob(frames, self.state, self.signals))
        else:
            self._present()

    def _built_ready(self, built: m.Built, state: m.PlotState) -> None:
        if state is not self.state:
            return
        self.built = built
        self._present()

    def _present(self) -> None:
        start = time.perf_counter()
        if self.use_library:
            self.library.apply(self.state, self.built)
        built = self.built
        scale = 1.0
        metric = m.metric_by_key(self.state.metric, [])
        ylabel = m.ylabel_for(self.state, self.all_frames())
        if self.state.kind != "speedup":
            scale = float(self.state.yscale or metric.scale or 1.0)

        self.styles = m.resolve_styles(built, self.state)
        shown = built
        notes = list(built.notes)
        target = m.baseline_for(built, self.state)
        if self.state.baseline_mode != "off" and self.state.baseline_target and not target:
            notes.append(f"baseline '{self.state.baseline_target}' is not in this plot; "
                         "showing absolute values")
        if self.state.baseline_mode != "off" and target:
            shown = analysis.apply_baseline(built, target, self.state.baseline_mode,
                                            self.state.baseline_scope)
            notes = list(shown.notes)
            scale = 1.0
            ylabel = analysis.relative_ylabel(self.state.baseline_mode, ylabel)
        self.shown = shown
        self.scale = scale

        self.chart.draw(shown, self.styles, self.state, ylabel, scale)
        if self.built.keys() != self._series_keys:
            self._series_keys = self.built.keys()
            self._rebuild_series_rows()
            self._sync_baseline_targets()
        self._sync_plot_picker()
        self._fill_tables(shown, scale)
        self._show_notes(notes)
        self._status(f"Ready · {round(1000 * (time.perf_counter() - start))} ms")

    def _reconcile(self, frames: list[pd.DataFrame]) -> None:
        """Keep the menus and filters in step with what the loaded files actually contain."""
        dims = m.dimension_values(frames)
        # The x column is locked to "every value", so a change of x has to reach the groups.
        signature = (self.state.effective_x, tuple((c, tuple(v)) for c, v in dims.items()))
        if signature != self._dims_signature:
            self.links = m.correlations(frames)
            if not self.state.filters:
                self.state.filters = m.default_filters(dims, self.links)
            else:
                for column, values in dims.items():
                    self.state.filters.setdefault(column, list(values))
            self._dims_signature = signature
            self._rebuild_filters(dims)

        metrics = {x.key: x.name for x in m.available_metrics(frames)}
        self.metric_choice.set_options(metrics)
        if metrics and self.state.metric not in metrics:
            self.state.metric = next(iter(metrics))
        self.metric_choice.set_value(self.state.metric)
        xs = {c: m.AXIS_NAMES.get(c, c) for c in m.x_candidates(frames)}
        self.xaxis.set_options(xs)
        if xs and self.state.x not in xs:
            self.state.x = "Total_Threads" if "Total_Threads" in xs else next(iter(xs))
        self.xaxis.set_value(self.state.effective_x)
        self.xaxis.setEnabled(self.state.kind != "speedup")
        self.errorbars.setEnabled(self.state.metric == "throughput"
                                  and self.state.kind != "speedup")

    def _sync_baseline_targets(self) -> None:
        """Offer the baselines this plot has, without forgetting the one that was chosen.

        The target used to be overwritten the moment it was missing from a build -- and
        switching metric can drop a series for one draw -- so a round trip through another
        metric silently reset the comparison. The choice is kept; :meth:`_baseline_target`
        decides what is usable *now*, and says so in Notes when it is nothing.
        """
        if self.state.baseline_scope == "panel":
            options = {str(i): (self.state.panel_title(i) or Path(s.csv).stem)
                       for i, s in enumerate(self.state.slots())}
        else:
            options = {k: (self.styles[k].label if k in self.styles else k)
                       for k in self.built.keys()}
        self.rel_target.set_options(options)
        if not self.state.baseline_target and options:
            self.state.baseline_target = next(iter(options))
        self.rel_target.set_value(self.state.baseline_target)

    # -- the bottom tabs -----------------------------------------------------------------

    def _fill_tables(self, built: m.Built, scale: float) -> None:
        """Recompute the rows, but only *populate* the tab on screen.

        Filling three tables is 70 ms of the refresh, and two of them are behind a tab
        nobody is looking at. The rows are cheap; the QTableWidgetItems are not.
        """
        token = (id(built), scale, self.state.effective_x,
                 tuple((k, v.label, v.visible) for k, v in sorted(self.styles.items())))
        if token == getattr(self, "_rows_token", None):
            return                       # a colour change moves no numbers and no names
        self._rows_token = token
        self._tab_rows = {0: self._value_rows(built, scale),
                          1: self._rank_rows(built, scale),
                          2: self._peak_rows(built, scale)}
        self._stale_tabs = {0, 1, 2}
        self._tab_shown(self.bottom.currentIndex())

    def _tab_shown(self, index: int) -> None:
        if index not in self._stale_tabs:
            return
        widget = {0: self.table, 1: self.rank_table, 2: self.peak_table}.get(index)
        if widget is None:
            return
        self._stale_tabs.discard(index)
        self._fill(widget, self._tab_rows.get(index, []))

    def _value_rows(self, built: m.Built, scale: float) -> list[dict]:
        rows = []
        for i, panel in enumerate(built.panels):
            for key, s in panel.items():
                st = self.styles.get(key)
                if st is None or not st.visible:
                    continue
                rows.extend(m._records(self.state, i, st.label, s, scale))
        return rows

    def _rank_rows(self, built: m.Built, scale: float) -> list[dict]:
        return [{"Plot": self.state.panel_title(r.panel) or f"plot {r.panel + 1}",
                 m.AXIS_NAMES.get(self.state.effective_x, "x"): r.x,
                 "Winner": r.winner, "Value": round(r.value / scale, 4),
                 "Runner-up": r.runner_up or "",
                 "Margin %": "" if r.margin is None else round(r.margin, 2)}
                for r in analysis.ranking(built, self.styles)]

    def _peak_rows(self, built: m.Built, scale: float) -> list[dict]:
        return [{"Plot": self.state.panel_title(p.panel) or f"plot {p.panel + 1}",
                 "Series": p.label, "Peak": round(p.peak / scale, 4),
                 "Peak at": p.peak_at, "Final": round(p.final / scale, 4),
                 "Points": p.points}
                for p in analysis.summary(built, self.styles)]

    @staticmethod
    def _fill(table: QtWidgets.QTableWidget, rows: list[dict]) -> None:
        table.setSortingEnabled(False)
        table.setUpdatesEnabled(False)
        try:
            if not rows:
                table.clear()
                table.setRowCount(0)
                table.setColumnCount(0)
                return
            columns = list(rows[0])
            existing = [table.horizontalHeaderItem(c).text() if table.horizontalHeaderItem(c)
                        else "" for c in range(table.columnCount())]
            new_shape = existing != columns
            if new_shape:
                table.clear()
                table.setColumnCount(len(columns))
                table.setHorizontalHeaderLabels(columns)
            table.setRowCount(len(rows))
            for r, row in enumerate(rows):
                for c, column in enumerate(columns):
                    value = row.get(column, "")
                    item = table.item(r, c)
                    if item is None:
                        item = QtWidgets.QTableWidgetItem()
                        table.setItem(r, c, item)
                    if isinstance(value, (int, float)) and not isinstance(value, bool):
                        item.setData(QtCore.Qt.DisplayRole, float(value))
                    else:
                        item.setData(QtCore.Qt.DisplayRole, str(value))
            if new_shape:
                # Only when the columns themselves changed: measuring every cell is the
                # single most expensive thing a QTableWidget does.
                table.resizeColumnsToContents()
        finally:
            table.setUpdatesEnabled(True)
            table.setSortingEnabled(True)

    def _show_notes(self, notes: list[str]) -> None:
        text = "\n".join(f"· {n}" for n in notes)
        clash_source = getattr(self, "styles", {})
        from ..gui.colors import check_colors
        named = {st.label: st.color for st in clash_source.values() if st.visible}
        clashes = check_colors(named, skin(self.state.theme).chart.surface)
        if clashes:
            text += ("\n" if text else "") + "\n".join(
                f"{'✖' if c.severity == 'fail' else '⚠'} {c.message}" for c in clashes)
        self.notes.setPlainText(text or "Nothing to report.")
        self.bottom.setTabText(3, f"Notes ({len(notes) + len(clashes)})"
                               if (notes or clashes) else "Notes")

    def _hovered(self, text: str) -> None:
        self.readout.setText(text)

    def _status(self, text: str, bad: bool = False) -> None:
        self.chip.setText(text)
        self.chip.setStyleSheet(f"color: {skin(self.state.theme).danger};" if bad else "")

    # -- export --------------------------------------------------------------------------

    def ask_save_figure(self) -> None:
        if not self.state.panels:
            return
        base = self.state.figure_title() or Path(self.state.panels[0].csv).stem
        path, selected = QtWidgets.QFileDialog.getSaveFileName(
            self, "Save figure", str(Path.cwd() / f"{base}.png"),
            "PNG image (*.png);;SVG vector (*.svg);;PDF document (*.pdf)")
        if not path:
            return
        fmt = Path(path).suffix.lstrip(".").lower() or "png"
        if fmt not in ("png", "svg", "pdf"):
            fmt = "png"
            path += ".png"
        self._status("Saving…")
        self.pool.start(_SaveJob(self.frames(), self.state, path, fmt, False, False,
                                 self.signals))

    def ask_export_data(self) -> None:
        if not self.state.panels:
            return
        path, _ = QtWidgets.QFileDialog.getSaveFileName(
            self, "Export plotted values", str(Path.cwd() / "plotted.csv"), "CSV (*.csv)")
        if not path:
            return
        # The scale the chart actually drew with: a baseline turns the values into ratios,
        # and dividing those by the metric's scale again exports a number nobody plotted.
        scale = getattr(self, "scale", 1.0)
        rows = []
        for i, panel in enumerate(getattr(self, "shown", self.built).panels):
            for key, s in panel.items():
                st = self.styles.get(key)
                if st is not None and st.visible:
                    rows.extend(m._records(self.state, i, st.label, s, scale))
        pd.DataFrame(rows).to_csv(path, index=False)
        self._status(f"Wrote {Path(path).name}")

    def _save_done(self, path, table, error: str) -> None:
        if error:
            QtWidgets.QMessageBox.critical(self, "Could not save", error)
            self._status("✖ save failed", bad=True)
            return
        extra = f" (+ {table.name})" if table else ""
        self._status(f"Saved {Path(path).name}{extra}")

    # -- sessions ------------------------------------------------------------------------

    def ask_save_session(self) -> None:
        path, _ = QtWidgets.QFileDialog.getSaveFileName(
            self, "Save session", str(Path.cwd() / "session.json"), "JSON (*.json)")
        if path:
            Path(path).write_text(self.state.to_json())
            self._status(f"Session written to {Path(path).name}")

    def ask_load_session(self) -> None:
        path, _ = QtWidgets.QFileDialog.getOpenFileName(
            self, "Open session", str(Path.cwd()), "JSON (*.json)")
        if path:
            self.load_session(path)

    def load_session(self, path: str) -> None:
        try:
            self.state = m.PlotState.from_json(Path(path).read_text())
        except (OSError, ValueError) as exc:
            QtWidgets.QMessageBox.critical(self, "Could not open session", str(exc))
            return
        self._dims_signature = ()
        self._slot_signature = ()
        self._series_keys = []
        self.editing = ""
        self._refresh_tree()
        self._sync_controls()
        self._apply_theme()
        self.schedule(data=True)

    # -- odds and ends -------------------------------------------------------------------

    def open_palette(self) -> None:
        actions = {
            "Add CSV…": self.ask_add_csv,
            "Reload data from disk": self.reload,
            "Save figure…": self.ask_save_figure,
            "Export plotted values…": self.ask_export_data,
            "Save session…": self.ask_save_session,
            "Open session…": self.ask_load_session,
            "Reset the view": self.chart.reset_view,
            "Show all series": lambda: self._set_all_visible(True),
            "Hide all series": lambda: self._set_all_visible(False),
            "Edit every plot's axes": lambda: self._edit_plot(""),
            "Reset this plot's axes": self.reset_plot_axes,
            "Normalise y across plots": lambda: self.share_y.setChecked(
                not self.state.share_y),
            "Normalise x across plots": lambda: self.share_x.setChecked(
                not self.state.share_x),
            "Reset series styles": self.reset_styles,
            "Pin styles to the library": self.pin_styles,
            "Forget the style library": self.forget_styles,
            "Switch to dark theme": lambda: self._set_theme("dark"),
            "Switch to light theme": lambda: self._set_theme("light"),
            "Copy the table to the clipboard": self.copy_table,
        }
        CommandPalette(actions, self).exec()

    def copy_table(self) -> None:
        """The tab on screen, as TSV -- pastes straight into a spreadsheet."""
        rows = self._tab_rows.get(self.bottom.currentIndex(), [])
        if not rows:
            self._status("Nothing to copy")
            return
        columns = list(rows[0])
        text = "\n".join(["\t".join(columns)]
                         + ["\t".join(str(r.get(c, "")) for c in columns) for r in rows])
        QtWidgets.QApplication.clipboard().setText(text)
        self._status(f"Copied {len(rows)} rows")

    def _sync_controls(self) -> None:
        s = self.state
        self._loading = True
        self.kind.set_value(s.kind)
        self.stat.set_value(s.stat)
        self.errorbars.set_value(s.errorbars)
        self.figure_title.setText(s.title or "")
        self.zero.setChecked(s.y_from_zero)
        self.columns.setValue(s.columns)
        self.share_y.setChecked(s.share_y)
        self.share_x.setChecked(s.share_x)
        self.width_box.setValue(s.width)
        self.height_box.setValue(s.height)
        self.dpi.setValue(s.dpi)
        self.theme_switch.set_value(s.theme)
        self.rel_mode.set_value(s.baseline_mode)
        self.rel_scope.set_value(s.baseline_scope)
        self.library_on.setChecked(self.use_library)
        self.plot_pick.set_value(self.editing)
        self._describe_library()
        self._loading = False
        self._sync_axes_controls()


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="mpmc-plot-ui",
                                     description="Interactive benchmark plotting.")
    parser.add_argument("csv", nargs="*", help="results CSVs to open")
    parser.add_argument("--session", help="a session saved from the app")
    args = parser.parse_args(argv)
    logging.basicConfig(level=logging.WARNING, format="%(levelname)s: %(message)s")

    QtWidgets.QApplication.setAttribute(QtCore.Qt.AA_DontUseNativeMenuBar, False)
    app = QtWidgets.QApplication(sys.argv[:1])
    app.setApplicationName("mpmc plot studio")
    window = MainWindow(args.csv, args.session)
    window.show()
    return app.exec()


if __name__ == "__main__":
    sys.exit(main())
