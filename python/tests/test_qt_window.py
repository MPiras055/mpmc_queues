"""The window itself, driven offscreen.

Not a screenshot test -- it checks the wiring that is easy to break and invisible in review:
that a cosmetic edit does not rebuild the data, that hidden tabs are not populated, that the
shared y range is the union rather than one plot's, and that Qt's own method names have not
been shadowed by an attribute (which is a segfault, not an error, when Qt calls them).
"""

import os

import matplotlib
import pytest

matplotlib.use("Agg")
os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

pytest.importorskip("PySide6", reason="the plotting window needs PySide6")
pytest.importorskip("pyqtgraph", reason="the plotting window needs pyqtgraph")

from PySide6 import QtCore, QtWidgets  # noqa: E402

from mpmc_bench.gui import model as m  # noqa: E402
from mpmc_bench.qt.app import MainWindow  # noqa: E402
from tests.test_gui_model import HEADER, _row  # noqa: E402


@pytest.fixture(scope="session")
def qapp():
    app = QtWidgets.QApplication.instance() or QtWidgets.QApplication([])
    yield app


@pytest.fixture
def csvs(tmp_path):
    """Two files whose throughputs differ by 10x, so a shared y range is visible."""
    out = []
    for name, factor in (("low.csv", 1.0), ("high.csv", 10.0)):
        rows = [_row(q, p, p, size, True, 0, base * p * factor)
                for q, base in (("u-pscq", 100.0), ("u-prq", 80.0))
                for p in (1, 2, 4) for size in (1024, 4096)]
        path = tmp_path / name
        path.write_text(HEADER + "".join(rows))
        out.append(str(path))
    return out


@pytest.fixture
def window(qapp, csvs, tmp_path, monkeypatch):
    monkeypatch.setenv("XDG_CONFIG_HOME", str(tmp_path / "config"))
    w = MainWindow(csvs)
    w.resize(1200, 800)
    yield w
    w.close()
    w.deleteLater()
    qapp.processEvents()


def settle(qapp, window, timeout=20.0, after=None):
    """Pump the event loop until the background build has landed.

    @param after wait for a Built that is not this one -- a rebuild replaces the object, and
           without this the helper returns immediately on the stale one.
    """
    import time
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        qapp.processEvents(QtCore.QEventLoop.AllEvents, 10)
        fresh = after is None or id(window.built) != after
        if window.built.keys() and not window._pending_data and fresh:
            qapp.processEvents(QtCore.QEventLoop.AllEvents, 10)
            return
        time.sleep(0.005)
    raise AssertionError("the window never finished building")


class TestStartup:
    def test_draws_both_files(self, qapp, window):
        settle(qapp, window)
        assert window.built.keys() == ["u-prq", "u-pscq"]
        assert len(window.chart._plots) == 2
        assert len(window.chart._curves) == 4          # two series in each of two plots

    def test_populates_the_visible_tab_only(self, qapp, window):
        """Filling three tables is most of a refresh, and two are behind a tab."""
        settle(qapp, window)
        assert window.table.rowCount() > 0
        assert window.rank_table.rowCount() == 0
        window.bottom.setCurrentIndex(1)
        assert window.rank_table.rowCount() > 0

    def test_no_attribute_shadows_a_qwidget_method(self, window):
        """`self.metric = QComboBox()` hid QWidget.metric(), which Qt calls while painting.

        It did not raise -- it crashed inside the style engine. Cheap to assert, so it is
        asserted rather than remembered.
        """
        import inspect
        clashes = []
        for name, value in vars(window).items():
            # PySide6 caches bound signals in the instance dict on first access, so they
            # show up here without anyone having assigned them.
            if name.startswith("_") or type(value).__name__ == "SignalInstance":
                continue
            if inspect.isroutine(getattr(QtWidgets.QMainWindow, name, None)):
                clashes.append(name)
        assert clashes == [], f"these shadow QWidget methods Qt calls internally: {clashes}"


class TestRefreshCost:
    def test_a_colour_change_does_not_rebuild_the_data(self, qapp, window):
        settle(qapp, window)
        before = id(window.built)
        token = window._rows_token
        window._style(window.built.keys()[0], color="#123456")
        qapp.processEvents()
        window._flush()
        assert id(window.built) == before, "pandas must not run for a colour"
        assert window._rows_token == token, "the table rows cannot have moved"

    def test_a_filter_change_does_rebuild(self, qapp, window):
        settle(qapp, window)
        before = id(window.built)
        window._filter_changed("Size", ["1024"])
        settle(qapp, window, after=before)
        assert id(window.built) != before

    def test_the_chart_reuses_curves_across_a_restyle(self, qapp, window):
        settle(qapp, window)
        curves = dict(window.chart._curves)
        window._style(window.built.keys()[0], linewidth=4.0)
        window._flush()
        assert {k: id(v) for k, v in window.chart._curves.items()} == \
               {k: id(v) for k, v in curves.items()}


class TestRanges:
    def test_shared_y_is_the_union_not_one_plot(self, qapp, window):
        """Linked views take one range; a plot peaking 10x higher then drew off its axis."""
        settle(qapp, window)
        window.state.share_y = True
        window.state.y_from_zero = True
        window._flush()
        tops = [p.viewRange()[1][1] for p in window.chart._plots]
        assert tops[0] == pytest.approx(tops[1])
        biggest = max(max(c.yData) for c in window.chart._curves.values())
        assert tops[0] >= biggest

    def test_unshared_y_lets_each_plot_fit_itself(self, qapp, window):
        settle(qapp, window)
        window.state.share_y = False
        window._flush()
        tops = [p.viewRange()[1][1] for p in window.chart._plots]
        assert tops[0] < tops[1], "the 10x file should not squash the other one"


class TestSeriesControls:
    def test_hiding_a_series_removes_its_curve(self, qapp, window):
        settle(qapp, window)
        key = window.built.keys()[0]
        window._style(key, visible=False)
        window._flush()
        assert all(k != key for _slot, k in window.chart._curves)

    def test_the_legend_click_toggles(self, qapp, window):
        settle(qapp, window)
        key = window.built.keys()[0]
        window._toggle_series(key)
        window._flush()
        assert window.state.styles[key].visible is False

    def test_reset_styles_clears_every_override(self, qapp, window):
        settle(qapp, window)
        window._style(window.built.keys()[0], color="#010203")
        window.reset_styles()
        assert window.state.styles == {}


class TestBaselineInTheWindow:
    def test_switching_to_percent_flattens_the_baseline(self, qapp, window):
        settle(qapp, window)
        key = window.built.keys()[0]
        window._set_baseline_target(key)
        window._set_baseline_mode("percent")
        window._flush()
        drawn = window.shown.panels[0][key]
        assert drawn.y == [0.0] * len(drawn.y)
        assert window.built.panels[0][key].y != [0.0] * len(drawn.y), "the source is untouched"

    def test_turning_it_off_restores_absolute_values(self, qapp, window):
        settle(qapp, window)
        window._set_baseline_target(window.built.keys()[0])
        window._set_baseline_mode("ratio")
        window._flush()
        window._set_baseline_mode("off")
        window._flush()
        assert window.shown is window.built


class TestExport:
    def test_saves_a_figure_through_matplotlib(self, qapp, window, tmp_path):
        settle(qapp, window)
        out = tmp_path / "figure.png"
        window.state.width, window.state.height, window.state.dpi = 6, 4, 100
        path, rendered = m.save_figure(window.frames(), window.state, out, "png")
        assert path.stat().st_size > 0 and rendered.built.keys()

    def test_the_session_round_trips(self, qapp, window, tmp_path):
        settle(qapp, window)
        window.state.title = "A title"
        text = window.state.to_json()
        restored = m.PlotState.from_json(text)
        assert restored.title == "A title"
        assert [p.csv for p in restored.library] == [p.csv for p in window.state.library]


def _box(window, key) -> QtWidgets.QCheckBox:
    """The visibility checkbox of one series row."""
    return window._rows[key].findChild(QtWidgets.QCheckBox)


class TestCompare:
    def test_ticking_a_second_value_replaces_the_first_by_default(self, qapp, window):
        """Not a tick box until you say so: switching size should switch, not accumulate."""
        settle(qapp, window)
        group = window._filter_widgets["Size"]
        assert group.mode == "one"
        group._boxes["4096"].setChecked(True)
        assert group.selected == ["4096"]
        assert window.state.filters["Size"] == ["4096"]

    def test_the_last_value_cannot_be_unticked(self, qapp, window):
        settle(qapp, window)
        group = window._filter_widgets["Size"]
        group._boxes[group.selected[0]].setChecked(False)
        assert len(group.selected) == 1, "an empty filter plots nothing and reads as a bug"

    def test_comparing_spawns_a_plot_per_value(self, qapp, window):
        settle(qapp, window)
        before = id(window.built)
        group = window._filter_widgets["Size"]
        group.compare.setChecked(True)
        group._boxes["4096"].setChecked(True)
        settle(qapp, window, after=before)
        assert window.state.compare["Size"] == "facet"
        assert len(window.chart._plots) == 4, "two files times two sizes"
        assert "size 4096" in window.state.panel_title(1)

    def test_comparing_on_one_plot_splits_the_lines_instead(self, qapp, window):
        settle(qapp, window)
        before = id(window.built)
        group = window._filter_widgets["Size"]
        group.compare.setChecked(True)
        group.how.set_value("overlay")
        group.how.chosen.emit("overlay")
        group._boxes["4096"].setChecked(True)
        settle(qapp, window, after=before)
        assert len(window.chart._plots) == 2
        assert len(window.built.keys()) == 4

    def test_unticking_compare_goes_back_to_one_value(self, qapp, window):
        settle(qapp, window)
        group = window._filter_widgets["Size"]
        group.compare.setChecked(True)
        group._boxes["4096"].setChecked(True)
        settle(qapp, window, after=id(window.built))
        group.compare.setChecked(False)
        assert group.selected == ["1024"]
        assert "Size" not in window.state.compare

    def test_the_x_column_is_locked_to_every_value(self, qapp, window):
        settle(qapp, window)
        before = id(window.built)
        window._set("x", "Producers")
        settle(qapp, window, after=before)
        group = window._filter_widgets["Producers"]
        assert group.mode == "overlay" and not group.compare.isEnabled()


class TestPerPlotAxes:
    def test_editing_one_plot_leaves_the_other_alone(self, qapp, window):
        settle(qapp, window)
        window._edit_plot("1")
        window.ylabel.setText("just this one")
        window.ylabel.editingFinished.emit()
        window._flush()
        assert window.state.overrides[window.state.slots()[1].ident]["ylabel"] \
            == "just this one"
        assert window.chart._plots[1].getAxis("left").labelText == "just this one"
        assert window.chart._plots[0].getAxis("left").labelText != "just this one"

    def test_the_figure_is_edited_when_no_plot_is_selected(self, qapp, window):
        settle(qapp, window)
        window._edit_plot("")
        window.ylabel.setText("everything")
        window.ylabel.editingFinished.emit()
        assert window.state.ylabel == "everything"
        assert window.state.overrides == {}

    def test_resetting_gives_the_plot_back_to_the_figure(self, qapp, window):
        settle(qapp, window)
        window._edit_plot("0")
        window._set_tick("xticks", mode="hidden")
        assert window.state.overrides
        window.reset_plot_axes()
        assert window.state.overrides == {}

    def test_the_picker_follows_the_plots(self, qapp, window):
        settle(qapp, window)
        before = id(window.built)
        group = window._filter_widgets["Size"]
        group.compare.setChecked(True)
        group._boxes["4096"].setChecked(True)
        settle(qapp, window, after=before)
        assert len(window.plot_pick._keys) == 5      # "all plots" and four plots

    def test_clicking_a_plot_selects_it(self, qapp, window):
        settle(qapp, window)
        window.chart.plotSelected.emit(1)
        assert window.editing == "1"
        assert window.chart._selected == 1


class TestTheAuditedControls:
    def test_hiding_everything_unticks_every_checkbox(self, qapp, window):
        """The rows were rebuilt from the styles of the *previous* draw."""
        settle(qapp, window)
        window._set_all_visible(False)
        assert [_box(window, k).isChecked() for k in window.built.keys()] == [False, False]
        window._set_all_visible(True)
        assert all(_box(window, k).isChecked() for k in window.built.keys())

    def test_a_legend_click_moves_the_checkbox_too(self, qapp, window):
        settle(qapp, window)
        key = window.built.keys()[0]
        window._toggle_series(key)
        assert _box(window, key).isChecked() is False

    def test_moving_the_legend_or_the_grid_relays_out_cleanly(self, qapp, window):
        """A legend added to a cell a plot already holds corrupts pyqtgraph's layout for good:
        the item ends up in it twice, and the next rebuild raises from view.clear()."""
        settle(qapp, window)
        for legend, columns in (("bottom", 1), ("top", 2), ("right", 1), ("none", 0),
                                ("auto", 3)):
            window.state.legend, window.state.columns = legend, columns
            window._flush()
            assert len(window.chart._plots) == 2
        window.state.library[1].plotted = False
        window._flush()                          # this is where a stale cell used to raise
        assert len(window.chart._plots) == 1

    def test_reset_view_keeps_the_configured_floor(self, qapp, window):
        """Autorange alone forgot 'start y at zero', because the limits were skipped."""
        settle(qapp, window)
        window.state.y_from_zero = True
        window._flush()
        window.chart._plots[0].setYRange(100.0, 200.0)
        window.chart.reset_view()
        assert window.chart._plots[0].viewRange()[1][0] == pytest.approx(0.0)

    def test_a_y_limit_is_in_the_units_on_the_axis(self, qapp, window):
        """The preview divided it by the metric scale; the export did not."""
        settle(qapp, window)
        window.state.share_y = False
        window.state.ymin, window.state.ymax = 0.00002, 0.0004
        window._flush()
        lo, hi = window.chart._plots[0].viewRange()[1]
        assert lo == pytest.approx(0.00002)
        assert hi >= 0.0004

    def test_the_exported_values_are_the_ones_drawn(self, qapp, window, tmp_path):
        """A baseline makes them ratios; dividing those by 1e6 exported a fiction."""
        settle(qapp, window)
        key = window.built.keys()[0]
        window._set_baseline_target(key)
        window._set_baseline_mode("ratio")
        window._flush()
        out = tmp_path / "plotted.csv"
        monkey = QtWidgets.QFileDialog.getSaveFileName
        QtWidgets.QFileDialog.getSaveFileName = staticmethod(lambda *a, **k: (str(out), ""))
        try:
            window.ask_export_data()
        finally:
            QtWidgets.QFileDialog.getSaveFileName = monkey
        import csv
        rows = list(csv.DictReader(out.open()))
        mine = [float(r["Y"]) for r in rows if r["Key"] == key]
        assert mine == [1.0] * len(mine)


class TestBaselineIsRemembered:
    def test_the_target_survives_a_build_that_lacks_it(self, qapp, window):
        """Switching metric can drop a series for one draw; coming back must find it again."""
        settle(qapp, window)
        key = window.built.keys()[0]
        window._set_baseline_target(key)
        window._set_baseline_mode("ratio")
        window.built = m.Built([{}, {}])         # the series is briefly not there
        window._sync_baseline_targets()
        assert window.state.baseline_target == key
        window._present()
        assert any("not in this plot" in n for n in window.notes.toPlainText().splitlines())

    def test_it_is_part_of_a_saved_session(self, qapp, window):
        settle(qapp, window)
        window._set_baseline_target(window.built.keys()[0])
        window._set_baseline_mode("percent")
        back = m.PlotState.from_json(window.state.to_json())
        assert (back.baseline_mode, back.baseline_target) == \
               ("percent", window.state.baseline_target)
