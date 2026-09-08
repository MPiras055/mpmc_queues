"""Styling and result loading."""

from __future__ import annotations

import subprocess
import sys
import textwrap
from pathlib import Path

import pytest

pytest.importorskip("pandas")

from mpmc_bench.plotting import data as dataio
from mpmc_bench.plotting.styles import pretty_label, style_for

CSV = """Queue,Producers,Consumers,Size,Items,Pinning,ProdDelay_NS,ProdDelay_Amp,ConsDelay_NS,ConsDelay_Amp,Throughput_Mean,Throughput_StdDev,Samples,Status
vyukov,1,1,1024,1000,True,0,0.0,0,0.0,100.0,1.0,3,OK
vyukov,2,2,1024,1000,True,0,0.0,0,0.0,180.0,2.0,3,OK
u-prq,1,1,1024,1000,True,0,0.0,0,0.0,90.0,1.0,3,OK
u-prq,2,2,1024,1000,True,0,0.0,0,0.0,150.0,2.0,3,OK
mutex,1,1,1024,1000,True,0,0.0,0,0.0,,,0,EXIT_3: boom
"""

LEGACY_CSV = """Queue,Producers,Consumers,Size,Items,Pinning,ProdDelay_NS,ProdDelay_Amp,ConsDelay_NS,ConsDelay_Amp,Throughput_Mean,Throughput_StdDev
VyukovBuffer,1,1,1024,1000,True,0,0.0,0,0.0,100.0,1.0
PSCQ,1,1,1024,1000,True,0,0.0,0,0.0,FAILED,0.0
"""


@pytest.fixture
def csv_file(tmp_path):
    p = tmp_path / "r.csv"; p.write_text(CSV); return p


class TestLoading:
    def test_failed_rows_are_dropped(self, csv_file):
        df = dataio.load_results(csv_file)
        assert "mutex" not in set(df["Queue"])
        assert len(df) == 4

    def test_legacy_csv_without_status_still_loads(self, tmp_path):
        """Old files have no Status column and wrote the literal 'FAILED'."""
        p = tmp_path / "old.csv"; p.write_text(LEGACY_CSV)
        df = dataio.load_results(p)
        assert list(df["Queue"]) == ["VyukovBuffer"]

    def test_total_threads_is_derived(self, csv_file):
        assert set(dataio.load_results(csv_file)["Total_Threads"]) == {2, 4}

    def test_missing_column_is_reported(self, tmp_path):
        p = tmp_path / "bad.csv"; p.write_text("Queue,Producers\nvyukov,1\n")
        with pytest.raises(ValueError, match="missing expected column"):
            dataio.load_results(p)

    def test_missing_file(self, tmp_path):
        with pytest.raises(FileNotFoundError):
            dataio.load_results(tmp_path / "nope.csv")


class TestFilters:
    def test_filters_compose(self, csv_file):
        df = dataio.load_results(csv_file)
        out = dataio.apply_filters(df, dataio.Filters(queues="vyukov", size=1024))
        assert set(out["Queue"]) == {"vyukov"}

    def test_no_filter_keeps_everything(self, csv_file):
        df = dataio.load_results(csv_file)
        assert len(dataio.apply_filters(df, dataio.Filters())) == len(df)

    def test_scalability_skips_queues_without_a_baseline(self, csv_file):
        df = dataio.load_results(csv_file)
        smallest = int(df["Producers"].min())
        df = df[df["Producers"] != smallest]        # remove every baseline point
        assert list(dataio.scalability(df, smallest)) == []

    def test_scalability_normalises_on_producers_not_threads(self, csv_file):
        """Consumers add no production capacity, so they must not count as scaling.

        Two sweeps at the same *total* thread count but different producer counts must not be
        held to the same speedup: that is what made the ideal line unreachable for a
        consumer-heavy sweep.
        """
        df = dataio.load_results(csv_file)
        base = int(df["Producers"].min())
        for _, group in dataio.scalability(df, base):
            # The x the plot uses is Producers, and the baseline row sits at exactly 1.0.
            at_base = group[group["Producers"] == base]["Scalability"]
            assert not at_base.empty and at_base.iloc[0] == pytest.approx(1.0)

    def test_stat_column_falls_back_when_median_absent(self, csv_file):
        """Older CSVs predate Throughput_Median; they must still plot, not raise."""
        df = dataio.load_results(csv_file)
        df = df.drop(columns=[c for c in ("Throughput_Median",) if c in df.columns])
        assert dataio.stat_column(df, "median") == "Throughput_Mean"


class TestStyles:
    def test_every_registry_name_gets_a_style(self):
        for name in ["vyukov", "u-prq", "chunk-faaarray", "mem-scq", "pscq", "brand-new"]:
            st = style_for(name)
            assert st.color.startswith("#") and st.marker and st.label

    def test_styles_are_stable_across_processes(self):
        """Derived from a salted hash() this would differ per run, so pin it."""
        code = textwrap.dedent("""
            import sys; sys.path.insert(0, %r)
            from mpmc_bench.plotting.styles import style_for
            print(style_for("u-prq").color, style_for("u-prq").marker)
        """ % str(__import__("pathlib").Path(__file__).resolve().parents[1]))
        outs = {
            subprocess.run([sys.executable, "-c", code], capture_output=True, text=True,
                           check=True).stdout.strip()
            for _ in range(2)
        }
        assert len(outs) == 1

    def test_labels_reflect_the_naming_scheme(self):
        assert pretty_label("u-prq") == "Unbounded / PRQ"
        assert pretty_label("mem-scq").startswith("Pool-bounded")
        assert pretty_label("mutex") == "Mutex (baseline)"


# --------------------------------------------------------------------------------------
# Themes, fixed-order assignment, comparison panels
# --------------------------------------------------------------------------------------

import json
import logging

import matplotlib
matplotlib.use("Agg")               # these tests render; never open a window

from mpmc_bench.plotting import compare, theme as theming
from mpmc_bench.plotting.plots import PlotConfig, plot_throughput
from mpmc_bench.plotting.styles import (
    MAX_SERIES, assign_slots, assign_styles, parse_label_assignments, set_labels,
)

# Two sizes with deliberately different maxima: 256 tops out at 100, 1024 at 400. The
# shared axis is only worth testing when the panels would otherwise disagree.
COMPARE_CSV = (
    "Queue,Producers,Consumers,Size,Items,Pinning,ProdDelay_NS,ProdDelay_Amp,"
    "ConsDelay_NS,ConsDelay_Amp,Throughput_Mean,Throughput_StdDev,Samples,Status\n"
) + "".join(
    f"{q},{p},{p},{size},1000,True,0,0.0,0,0.0,{value},0.0,3,OK\n"
    for q, size, p, value in [
        ("q-a", 256, 1, 50.0), ("q-a", 256, 2, 90.0),
        ("q-b", 256, 1, 60.0), ("q-b", 256, 2, 100.0),
        ("q-c", 256, 1, 55.0), ("q-c", 256, 2, 95.0),
        ("q-d", 256, 1, 57.0), ("q-d", 256, 2, 97.0),
        ("q-a", 1024, 1, 200.0), ("q-a", 1024, 2, 380.0),
        ("q-b", 1024, 1, 210.0), ("q-b", 1024, 2, 400.0),
    ]
)

PANEL_MAX = 400.0


@pytest.fixture(autouse=True)
def _no_label_overrides():
    """set_labels is module state; a leak would make neighbouring tests lie."""
    set_labels(None, replace=True)
    yield
    set_labels(None, replace=True)


@pytest.fixture
def compare_csv(tmp_path):
    p = tmp_path / "sizes.csv"; p.write_text(COMPARE_CSV); return p


def _spec_file(tmp_path, csv, queues, **top):
    spec = {
        "title": "sizes", "kind": "throughput", "csv": str(csv),
        "panels": [{"title": "256", "size": 256, "queues": queues},
                   {"title": "1024", "size": 1024, "queues": queues}],
        **top,
    }
    p = tmp_path / "spec.json"; p.write_text(json.dumps(spec)); return p


def _config(**kw):
    return PlotConfig(show=False, scale=1.0, **kw)


class TestTheme:
    def test_both_modes_carry_eight_series(self):
        for mode in (theming.LIGHT, theming.DARK):
            assert len(mode.series) == MAX_SERIES == 8
            assert len(set(mode.series)) == 8

    def test_dark_is_selected_not_inverted(self):
        """Stepped for the dark surface. An inversion would be a pure function of light."""
        differing = [d for l, d in zip(theming.LIGHT.series, theming.DARK.series) if l != d]
        assert len(differing) >= 6          # green is deliberately mode-invariant
        assert theming.DARK.surface != theming.LIGHT.surface

    def test_slots_never_wrap(self):
        """Cycling is the failure the whole scheme exists to prevent, so it must raise."""
        with pytest.raises(IndexError, match="never cycled"):
            theming.LIGHT.color(MAX_SERIES)

    def test_resolve_honours_the_environment(self, monkeypatch):
        monkeypatch.setenv(theming.ENV_VAR, "dark")
        assert theming.resolve().name == "dark"
        assert theming.resolve("light").name == "light"     # explicit still wins

    def test_resolve_defaults_to_light(self, monkeypatch):
        monkeypatch.delenv(theming.ENV_VAR, raising=False)
        assert theming.resolve().name == "light"

    def test_unknown_theme_is_reported(self):
        with pytest.raises(ValueError, match="unknown theme"):
            theming.resolve("solarized")


class TestAssignment:
    def test_no_two_series_in_one_chart_share_a_hue(self):
        """The regression the hash produced: 8 colours, 6 names, collisions by birthday."""
        names = ["u-prq", "u-scq", "u-pscq", "mem-scq", "chunk-faaarray", "vyukov", "mutex"]
        colors = [s.color for s in assign_styles(names).values()]
        assert len(set(colors)) == len(names)

    def test_campaign_names_keep_their_slot_between_charts(self):
        """Fixed-order assignment gives colour stability up; the pins buy it back."""
        alone = assign_slots(["i-u-pscq"])["i-u-pscq"]
        crowded = assign_slots(["i-u-pscq", "i-u-prq", "i-u-scq", "zzz"])["i-u-pscq"]
        assert alone == crowded == 0

    def test_a_shared_pin_does_not_become_a_shared_colour(self):
        """'u-pscq' and 'i-u-pscq' both want slot 0; one of them must give way."""
        slots = assign_slots(["u-pscq", "i-u-pscq"])
        assert len(set(slots.values())) == 2

    def test_overflow_warns_and_still_gives_eight_distinct_hues(self, caplog):
        names = [f"queue-{i:02d}" for i in range(11)]
        with caplog.at_level(logging.WARNING, logger="mpmc_bench.plotting.styles"):
            styles = assign_styles(names)
        assert "Fold the tail" in caplog.text

        hues = [s.color for s in styles.values() if s.slot < MAX_SERIES]
        assert len(hues) == MAX_SERIES and len(set(hues)) == MAX_SERIES
        # The tail is muted ink, not a ninth hue and emphatically not a recycled one.
        assert {s.color for s in styles.values() if s.slot >= MAX_SERIES} == {
            theming.LIGHT.text_muted
        }
        assert theming.LIGHT.text_muted not in hues

    def test_compact_pulls_the_used_slots_down_but_keeps_their_order(self):
        slots = assign_slots(["hq", "pscq"], compact=True)      # pinned at 5 and 0
        assert slots == {"pscq": 0, "hq": 1}

    def test_theme_selects_the_hex(self):
        assert assign_styles(["pscq"], theming.DARK)["pscq"].color == theming.DARK.series[0]


class TestLabels:
    def test_override_changes_the_legend_and_nothing_else(self):
        before = style_for("u-prq")
        set_labels({"u-prq": "PRQ (unbounded)"})
        after = style_for("u-prq")
        assert after.label == "PRQ (unbounded)"
        assert (after.color, after.marker, after.linestyle) == (
            before.color, before.marker, before.linestyle)

    def test_unmapped_names_still_derive_a_label(self):
        set_labels({"u-prq": "PRQ (unbounded)"})
        assert pretty_label("mem-scq") == "Pool-bounded / SCQ"
        assert pretty_label("mutex") == "Mutex (baseline)"

    def test_label_arguments_parse(self):
        assert parse_label_assignments(['u-pscq=PSCQ (unbounded)']) == {
            "u-pscq": "PSCQ (unbounded)"}
        with pytest.raises(ValueError, match="NAME=LABEL"):
            parse_label_assignments(["u-pscq"])

    def test_labels_file_round_trips(self, tmp_path):
        from mpmc_bench.plotting.styles import load_labels
        p = tmp_path / "names.json"
        p.write_text(json.dumps({"u-prq": "PRQ"}))
        assert load_labels(p) == {"u-prq": "PRQ"}


class TestCompare:
    def test_shared_y_is_the_max_across_panels(self, tmp_path, compare_csv):
        """Arithmetic, so assert it. The 256 panel peaks at 100 and the 1024 panel at 400."""
        spec = compare.load_spec(_spec_file(tmp_path, compare_csv, ["q-a", "q-b"]))
        result = compare.plot_compare(spec, _config())

        assert result.data_max == pytest.approx(PANEL_MAX)
        limits = {ax.get_ylim() for ax in result.axes}
        assert len(limits) == 1, "panels must share one y-axis, not one each"
        assert result.ylim[1] == pytest.approx(PANEL_MAX * (1 + compare.HEADROOM))
        assert result.ylim == result.axes[0].get_ylim()

    def test_each_panel_would_otherwise_scale_to_itself(self, tmp_path, compare_csv):
        """The control: without sharing, the 100 panel and the 400 panel disagree."""
        spec = compare.load_spec(
            _spec_file(tmp_path, compare_csv, ["q-a", "q-b"], share_y=False))
        result = compare.plot_compare(spec, _config())
        assert result.ylim is None
        assert len({ax.get_ylim() for ax in result.axes}) == 2

    def test_x_is_shared_too(self, tmp_path, compare_csv):
        spec = compare.load_spec(_spec_file(tmp_path, compare_csv, ["q-a", "q-b"]))
        result = compare.plot_compare(spec, _config())
        assert len({ax.get_xlim() for ax in result.axes}) == 1

    def test_three_series_render(self, tmp_path, compare_csv):
        spec = compare.load_spec(
            _spec_file(tmp_path, compare_csv, ["q-a", "q-b", "q-c"],
                       panels=[{"title": "256", "size": 256,
                                "queues": ["q-a", "q-b", "q-c"]}]))
        result = compare.plot_compare(spec, _config())
        assert len(result.axes) == 1

    def test_a_fourth_series_is_refused_with_the_measurement(self, tmp_path, compare_csv):
        spec = compare.load_spec(
            _spec_file(tmp_path, compare_csv, ["q-a", "q-b", "q-c", "q-d"],
                       panels=[{"title": "256", "size": 256,
                                "queues": ["q-a", "q-b", "q-c", "q-d"]}]))
        with pytest.raises(ValueError) as exc:
            compare.plot_compare(spec, _config())
        message = str(exc.value)
        # Naming the number is the point: a bare refusal just gets worked around.
        assert "13.7" in message and "4.8" in message
        assert "yellow" in message and "orange" in message

    def test_the_cap_counts_the_whole_figure_not_just_one_panel(self, tmp_path, compare_csv):
        """Panels share a colour assignment, so four across two panels is still four."""
        spec = compare.load_spec(_spec_file(
            tmp_path, compare_csv, [],
            panels=[{"title": "a", "size": 256, "queues": ["q-a", "q-b"]},
                    {"title": "b", "size": 256, "queues": ["q-c", "q-d"]}]))
        with pytest.raises(ValueError, match="4 distinct series"):
            compare.plot_compare(spec, _config())

    def test_panels_draw_the_validated_leading_slots(self, tmp_path, compare_csv):
        """Only slots 1-3 were measured all-pairs safe, so that is what panels may use."""
        spec = compare.load_spec(_spec_file(tmp_path, compare_csv, ["q-a", "q-b"]))
        compare.plot_compare(spec, _config())
        used = {s.slot for s in assign_styles(["q-a", "q-b"], compact=True).values()}
        assert used == {0, 1}

    def test_a_queue_keeps_its_colour_across_panels(self, tmp_path, compare_csv):
        spec = compare.load_spec(_spec_file(tmp_path, compare_csv, ["q-a", "q-b"]))
        result = compare.plot_compare(spec, _config())
        # One legend for the figure, so exactly one entry per series -- not one per panel.
        assert len(result.figure.legends) == 1
        assert len(result.figure.legends[0].get_texts()) == 2

    def test_relative_csv_resolves_next_to_the_spec(self, tmp_path, compare_csv):
        spec_path = tmp_path / "spec.json"
        spec_path.write_text(json.dumps({
            "kind": "throughput", "csv": compare_csv.name,
            "panels": [{"title": "256", "size": 256, "queues": ["q-a"]}],
        }))
        assert compare.load_spec(spec_path).panels[0].csv == compare_csv

    def test_a_typo_in_a_panel_key_is_reported(self, tmp_path, compare_csv):
        spec_path = tmp_path / "spec.json"
        spec_path.write_text(json.dumps({
            "csv": str(compare_csv),
            "panels": [{"title": "256", "sizes": 256}],      # 'sizes', not 'size'
        }))
        with pytest.raises(ValueError, match="unknown key"):
            compare.load_spec(spec_path)

    def test_backoff_grid_cannot_be_panelled(self, tmp_path, compare_csv):
        spec_path = tmp_path / "spec.json"
        spec_path.write_text(json.dumps({
            "kind": "backoff-grid", "csv": str(compare_csv),
            "panels": [{"title": "256", "size": 256}],
        }))
        with pytest.raises(ValueError, match="no y-axis for the panels to share"):
            compare.load_spec(spec_path)

    def test_empty_panel_names_itself(self, tmp_path, compare_csv):
        spec = compare.load_spec(
            _spec_file(tmp_path, compare_csv, [],
                       panels=[{"title": "empty", "size": 999}]))
        with pytest.raises(ValueError, match="panel 'empty'"):
            compare.plot_compare(spec, _config())


class TestTable:
    def test_table_holds_what_was_plotted(self, tmp_path, csv_file):
        out = tmp_path / "values.csv"
        df = dataio.load_results(csv_file)
        plot_throughput(df, _config(table_path=out, ylabel="Millions of ops/sec"))

        import pandas as pd
        table = pd.read_csv(out)
        assert list(table.columns) == [
            "Series", "Total threads", "Millions of ops/sec", "YErr"]
        # Both queues, both thread counts -- the relief has to carry every plotted value.
        assert len(table) == 4
        assert set(table["Total threads"]) == {2, 4}

    def test_no_table_without_a_path(self, tmp_path, csv_file):
        out = tmp_path / "written"; out.mkdir()
        df = dataio.load_results(csv_file)
        plot_throughput(df, _config())
        assert not list(out.iterdir())

    def test_table_path_defaults_beside_the_figure(self):
        from mpmc_bench.plotting.plots import _table_path_for
        assert _table_path_for("", "out/chart.png") == Path("out/chart.csv")
        assert _table_path_for("x.csv", None) == Path("x.csv")
        assert _table_path_for(None, "out/chart.png") is None
        with pytest.raises(ValueError, match="needs a path"):
            _table_path_for("", None)
