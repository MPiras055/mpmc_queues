"""The plotting UI's headless core: extraction, splitting, shared axes, export, colour checks.

Everything the window does goes through mpmc_bench.gui.model, so these run without a display.
"""

import matplotlib

matplotlib.use("Agg")

import dataclasses  # noqa: E402

import matplotlib as mpl  # noqa: E402
import numpy as np  # noqa: E402
import pytest  # noqa: E402
from matplotlib.backends.backend_agg import FigureCanvasAgg  # noqa: E402
from matplotlib.figure import Figure  # noqa: E402

from mpmc_bench.gui import colors, model as m  # noqa: E402
from mpmc_bench.plotting import data as dataio  # noqa: E402

HEADER = ("Queue,Producers,Consumers,Size,Items,Pinning,ProdDelay_NS,ProdDelay_Amp,ConsDelay_NS,"
          "ConsDelay_Amp,Throughput_Mean,Throughput_StdDev,Samples,Status,Throughput_Median,"
          "Throughput_Min,Throughput_Max,Segments,SegmentCapacity,Produced,Consumed,"
          "SlotEfficiency,Extra_Metric\n")


def _row(q, p, c, size, pin, delay, tput, seg=10, eff=0.9, extra=1.0):
    amp = 0.5 if delay else 0.0
    return (f"{q},{p},{c},{size},1000,{pin},{delay},{amp},{delay},{amp},{tput},{tput / 10},2,OK,"
            f"{tput},{tput * 0.9},{tput * 1.1},{seg},1024,1000,1000,{eff},{extra}\n")


@pytest.fixture(autouse=True)
def _restore_rcparams():
    """render() applies a theme to rcParams; keep that from leaking into other test files."""
    with mpl.rc_context():
        yield


@pytest.fixture
def balanced(tmp_path):
    rows = []
    for q, base in (("u-pscq", 100.0), ("u-prq", 80.0)):
        for p in (1, 2, 4):
            for size in (1024, 4096):
                for pin in (True, False):
                    for delay in (0, 240):
                        rows.append(_row(q, p, p, size, pin, delay, base * p + size / 1024,
                                         extra=p * 2.0))
    path = tmp_path / "balanced.csv"
    path.write_text(HEADER + "".join(rows))
    return path


@pytest.fixture
def unbalanced(tmp_path):
    rows = [_row(q, p, 3 * p, 1024, True, 0, base * p)
            for q, base in (("u-pscq", 400.0), ("u-prq", 50.0)) for p in (1, 2, 4)]
    path = tmp_path / "unbalanced.csv"
    path.write_text(HEADER + "".join(rows))
    return path


def _state(*paths, **kw):
    frames = [m.load_frame(p) for p in paths]
    dims = m.dimension_values(frames)
    st = m.PlotState(library=[m.Panel(str(p)) for p in paths],
                     filters=m.default_filters(dims, m.correlations(frames)), **kw)
    return st, frames


def _clone(state, **kw):
    """A copy with fields replaced -- the shape an edit in the window makes."""
    fields = {f.name: getattr(state, f.name) for f in dataclasses.fields(state)}
    fields.update(kw)
    return m.PlotState(**fields)


def _pixels(fig):
    fig.canvas.draw()
    return np.asarray(fig.canvas.buffer_rgba()).copy()


def _agg_figure():
    fig = Figure(figsize=(11, 6), dpi=110, layout="constrained")
    FigureCanvasAgg(fig)
    return fig


class TestExtraction:
    def test_run_parameters_and_their_values_are_found(self, balanced):
        dims = m.dimension_values([m.load_frame(balanced)])
        assert dims["Size"] == ["1024", "4096"]
        assert dims["Pinning"] == ["False", "True"]
        assert dims["ProdDelay_NS"] == ["0", "240"]
        assert dims["Producers"] == ["1", "2", "4"]

    def test_numeric_order_not_lexical(self, tmp_path):
        p = tmp_path / "r.csv"
        p.write_text(HEADER + "".join(_row("q", n, n, 16384 if n == 2 else 1024, True, 0, 1.0)
                                      for n in (1, 2, 16)))
        assert m.dimension_values([m.load_frame(p)])["Producers"] == ["1", "2", "16"]

    def test_metrics_with_data_are_offered_including_unknown_columns(self, balanced):
        keys = [x.key for x in m.available_metrics([m.load_frame(balanced)])]
        assert keys[:3] == ["throughput", "SlotEfficiency", "segments_per_item"]
        assert "Extra_Metric" in keys, "a new numeric column must show up without code changes"
        assert "Produced" not in keys, "a constant column carries no information"

    def test_metrics_without_data_are_not_offered(self, tmp_path):
        p = tmp_path / "r.csv"
        p.write_text(HEADER + _row("q", 1, 1, 1024, True, 0, 5.0, seg="", eff="", extra=""))
        keys = [x.key for x in m.available_metrics([m.load_frame(p)])]
        assert "SlotEfficiency" not in keys and "Segments" not in keys

    def test_x_candidates(self, balanced):
        xs = m.x_candidates([m.load_frame(balanced)])
        assert xs[:3] == ["Producers", "Consumers", "Total_Threads"]
        assert "Size" in xs and "ProdDelay_Amp" not in xs

    def test_default_filters_give_one_line_per_queue(self, balanced):
        st, frames = _state(balanced)
        assert st.filters["Pinning"] == ["True"]
        assert st.filters["ProdDelay_NS"] == ["0"]
        assert st.filters["Size"] == ["1024"]
        assert st.filters["Producers"] == ["1", "2", "4"]
        built = m.build_series(frames, st)
        assert sorted(built.keys()) == ["u-prq", "u-pscq"]
        assert built.split_by == []

    def test_default_amplitude_follows_the_delay(self, balanced):
        """Amplitude only exists beside a delay, so the defaults must agree about which."""
        st, frames = _state(balanced)
        assert st.filters["ProdDelay_Amp"] == ["0.0"]     # the amplitude measured at delay 0
        assert m.build_series(frames, st).keys()


class TestSplitting:
    def test_several_values_split_instead_of_averaging(self, balanced):
        st, frames = _state(balanced)
        st.filters["Size"] = ["1024", "4096"]
        built = m.build_series(frames, st)
        assert built.split_by == ["Size"]
        by_key = built.by_key()
        small, big = by_key["u-pscq-@Size=1024"], by_key["u-pscq-@Size=4096"]
        ops = dataio.OPS_PER_ITEM
        assert small.y == pytest.approx([(100 * p + 1) * ops for p in (1, 2, 4)])
        assert big.y == pytest.approx([(100 * p + 4) * ops for p in (1, 2, 4)])
        assert "size 4096" in big.default_label

    def test_split_keys_keep_the_pinned_colour(self, balanced):
        st, frames = _state(balanced)
        st.filters["Size"] = ["1024", "4096"]
        styles = m.resolve_styles(m.build_series(frames, st), st)
        blue = "#2a78d6"   # PSCQ's pinned slot in the light theme
        assert blue in {styles["u-pscq-@Size=1024"].color, styles["u-pscq-@Size=4096"].color}
        assert styles["u-pscq-@Size=1024"].color != styles["u-pscq-@Size=4096"].color

    def test_hiding_a_series_does_not_repaint_the_others(self, balanced):
        st, frames = _state(balanced)
        before = m.resolve_styles(m.build_series(frames, st), st)["u-prq"].color
        st.styles["u-pscq"] = m.SeriesStyle(visible=False)
        after = m.resolve_styles(m.build_series(frames, st), st)["u-prq"].color
        assert before == after

    def test_other_thread_count_splits_when_it_varies_at_one_x(self, balanced, unbalanced):
        st, frames = _state(balanced, unbalanced, x="Producers")
        # Same queue, same producer count, different consumer counts across the two files
        # is *not* a split: panels are grouped separately.
        assert m.build_series(frames, st).split_by == []


class TestRendering:
    def test_shared_y_is_the_largest_range_across_plots(self, balanced, unbalanced):
        st, frames = _state(balanced, unbalanced, x="Producers")
        fig = Figure()
        r = m.render(fig, frames, st)
        tops = [ax.get_ylim()[1] for ax in fig.axes]
        assert len(set(tops)) == 1
        largest = max(max(s.hi or s.y) for p in r.built.panels for s in p.values()) / 1e6
        assert tops[0] >= largest
        assert all(ax.get_ylim()[0] == 0 for ax in fig.axes)

    def test_unshared_y_keeps_each_plot_its_own_range(self, balanced, unbalanced):
        st, frames = _state(balanced, unbalanced, x="Producers", share_y=False)
        fig = Figure()
        m.render(fig, frames, st)
        assert fig.axes[0].get_ylim()[1] != fig.axes[1].get_ylim()[1]

    def test_titles(self, balanced, unbalanced):
        st, frames = _state(balanced, unbalanced)
        fig = Figure()
        m.render(fig, frames, st)
        assert [ax.get_title() for ax in fig.axes] == ["balanced", "unbalanced"]
        st.title, st.library[1].title = "Overall", "1:3"
        m.render(fig, frames, st)
        assert fig.get_suptitle() == "Overall"
        assert fig.axes[1].get_title() == "1:3"

    def test_renamed_series_reaches_the_legend(self, balanced):
        st, frames = _state(balanced)
        st.styles["u-pscq"] = m.SeriesStyle(label="PSCQ (mine)", color="#123456", marker="s")
        fig = Figure()
        r = m.render(fig, frames, st)
        texts = [t.get_text() for leg in fig.legends + [a.get_legend() for a in fig.axes if a.get_legend()]
                 for t in leg.get_texts()]
        assert "PSCQ (mine)" in texts
        assert r.styles["u-pscq"].color == "#123456"

    def test_x_ticks_every_value_and_every_nth(self, balanced):
        st, frames = _state(balanced, xticks=m.TickSpec("all"))
        fig = Figure()
        m.render(fig, frames, st)
        assert list(fig.axes[0].get_xticks()) == [2, 4, 8]
        st.xticks = m.TickSpec("every", 2)
        m.render(fig, frames, st)
        assert list(fig.axes[0].get_xticks()) == [2, 8]

    def test_hidden_ticks(self, balanced):
        st, frames = _state(balanced, xticks=m.TickSpec("hidden"), yticks=m.TickSpec("hidden"))
        fig = Figure()
        m.render(fig, frames, st)
        assert len(fig.axes[0].get_xticks()) == 0 and len(fig.axes[0].get_yticks()) == 0

    def test_y_spacing(self, balanced):
        st, frames = _state(balanced, yticks=m.TickSpec("step", 0.5))
        fig = Figure()
        m.render(fig, frames, st)
        ticks = fig.axes[0].get_yticks()
        assert all(abs((b - a) - 0.5) < 1e-9 for a, b in zip(ticks, ticks[1:]))

    def test_speedup_is_against_producers_with_an_ideal_line(self, unbalanced):
        st, frames = _state(unbalanced, kind="speedup", x="Total_Threads")
        assert st.effective_x == "Producers"
        fig = Figure()
        r = m.render(fig, frames, st)
        pscq = r.built.by_key()["u-pscq"]
        assert pscq.x == [1, 2, 4] and pscq.y == pytest.approx([1, 2, 4])
        assert any(line.get_label() == "Ideal (linear)" for line in fig.axes[0].get_lines())

    def test_bars(self, balanced):
        st, frames = _state(balanced, kind="bars")
        fig = Figure()
        m.render(fig, frames, st)
        assert len(fig.axes[0].patches) == 6   # 2 queues x 3 thread counts

    def test_other_metric(self, balanced):
        st, frames = _state(balanced, metric="Extra_Metric")
        fig = Figure()
        r = m.render(fig, frames, st)
        assert r.built.by_key()["u-pscq"].y == [2.0, 4.0, 8.0]

    def test_empty_selection_explains_itself(self, balanced):
        st, frames = _state(balanced)
        st.filters["Size"] = []
        r = m.render(Figure(), frames, st)
        assert any("matched no rows" in n for n in r.notes)


class TestColours:
    def test_port_matches_the_validator(self):
        """Numbers recorded in plotting/theme.py from the dataviz validator."""
        yellow, aqua, red, orange = "#eda100", "#1baf7a", "#e34948", "#eb6834"
        cvd = min(colors.delta_e(yellow, aqua, "protan"), colors.delta_e(yellow, aqua, "deutan"))
        assert round(cvd, 1) == 9.1
        assert round(colors.delta_e(red, orange), 1) == 7.1

    def test_default_palette_is_not_reported(self, balanced):
        st, frames = _state(balanced)
        assert m.render(Figure(), frames, st).clashes == []

    def test_a_chosen_colour_that_collides_is_reported(self, balanced):
        st, frames = _state(balanced)
        st.styles["u-prq"] = m.SeriesStyle(color="#2b79d7")   # next to PSCQ's blue
        clashes = m.render(Figure(), frames, st).clashes
        assert clashes and clashes[0].severity == "fail"

    def test_rejects_non_hex(self):
        with pytest.raises(ValueError):
            colors.delta_e("blue", "#000000")


class TestPersistence:
    def test_session_round_trip(self, balanced):
        st, _ = _state(balanced, title="T", kind="bars", xticks=m.TickSpec("every", 3))
        st.styles["u-pscq"] = m.SeriesStyle(label="P", color="#000000", visible=False)
        assert m.PlotState.from_json(st.to_json()) == st

    def test_session_ignores_unknown_keys(self):
        assert m.PlotState.from_json('{"kind": "bars", "from_the_future": 1}').kind == "bars"

    @pytest.mark.parametrize("fmt", ["png", "svg", "pdf"])
    def test_save_figure(self, balanced, tmp_path, fmt):
        st, frames = _state(balanced, width=6, height=4, dpi=100)
        path, rendered = m.save_figure(frames, st, tmp_path / "out", fmt)
        assert path.suffix == f".{fmt}" and path.stat().st_size > 0
        if fmt == "png":
            from PIL import Image
            assert Image.open(path).size == (600, 400)
        if fmt == "svg":
            assert "<text" in path.read_text(), "SVG text must stay editable text"
        table = m.export_table(rendered, tmp_path / "values.csv")
        assert "Series" in table.read_text().splitlines()[0]

    def test_unknown_format(self, balanced, tmp_path):
        st, frames = _state(balanced)
        with pytest.raises(ValueError):
            m.save_figure(frames, st, tmp_path / "x", "gif")


class TestCorrelations:
    def test_parameters_swept_together_are_found(self, balanced):
        links = m.correlations([m.load_frame(balanced)])
        assert links["ProdDelay_NS"]["ConsDelay_NS"] == {"0": "0", "240": "240"}
        assert links["ProdDelay_NS"]["ProdDelay_Amp"] == {"0": "0.0", "240": "0.5"}
        assert "Consumers" in links["Producers"], "a balanced sweep has P == C"

    def test_a_link_disappears_when_the_data_contradicts_it(self, balanced, unbalanced):
        """4 producers sits beside 4 consumers in one file and 12 in the other."""
        frames = [m.load_frame(balanced), m.load_frame(unbalanced)]
        assert "Consumers" not in m.correlations(frames).get("Producers", {})

    def test_unrelated_parameters_are_not_linked(self, balanced):
        links = m.correlations([m.load_frame(balanced)])
        assert "Size" not in links["ProdDelay_NS"]

    def test_changing_one_filter_carries_to_the_linked_ones(self, balanced):
        st, frames = _state(balanced)
        links = m.correlations(frames)
        st.filters["ProdDelay_NS"] = ["240"]
        touched = m.propagate(st.filters, "ProdDelay_NS", links)
        assert st.filters["ConsDelay_NS"] == ["240"]
        assert st.filters["ProdDelay_Amp"] == ["0.5"]
        assert set(touched) >= {"ConsDelay_NS", "ProdDelay_Amp"}

    def test_mutual_links_terminate(self, balanced):
        """Producer and consumer delay imply each other; following that must not loop."""
        st, frames = _state(balanced)
        st.filters["ConsDelay_NS"] = ["240"]
        m.propagate(st.filters, "ConsDelay_NS", m.correlations(frames))
        assert st.filters["ProdDelay_NS"] == ["240"]

    def test_a_selection_a_link_cannot_explain_is_left_alone(self, balanced):
        st, frames = _state(balanced)
        st.filters["ProdDelay_NS"] = []
        before = list(st.filters["ConsDelay_NS"])
        m.propagate(st.filters, "ProdDelay_NS", m.correlations(frames))
        assert st.filters["ConsDelay_NS"] == before


class TestSignatures:
    def test_cosmetic_edits_are_not_structural(self, balanced):
        st, _ = _state(balanced)
        for change in ({"color": "#123456"}, {"marker": "*"}, {"linestyle": ":"},
                       {"linewidth": 3.0}):
            other = _clone(st, styles={"u-pscq": m.SeriesStyle(**change)})
            assert m.structure_signature(st) == m.structure_signature(other), change

    @pytest.mark.parametrize("change", [{"label": "renamed"}, {"visible": False}])
    def test_label_and_visibility_are_structural(self, balanced, change):
        """Both move the legend or the limits, so neither can be patched in place."""
        st, _ = _state(balanced)
        other = _clone(st, styles={"u-pscq": m.SeriesStyle(**change)})
        assert m.structure_signature(st) != m.structure_signature(other)

    def test_data_signature_ignores_appearance_but_not_filters(self, balanced):
        st, _ = _state(balanced)
        assert m.data_signature(st) == m.data_signature(_clone(st, theme="dark"))
        assert m.data_signature(st) != m.data_signature(
            _clone(st, filters={**st.filters, "Size": ["4096"]}))

    def test_a_cached_build_renders_the_same_figure(self, balanced):
        st, frames = _state(balanced)
        fig = _agg_figure()
        first = m.render(fig, frames, st)
        a = _pixels(fig)
        fig2 = _agg_figure()
        m.render(fig2, frames, st, built=first.built)
        assert np.array_equal(a, _pixels(fig2))


class TestRestyle:
    STYLES = {"u-pscq": m.SeriesStyle(color="#4a3aa7", marker="*", linestyle=":"),
              "u-prq": m.SeriesStyle(color="#008300", linewidth=3.0)}

    @pytest.mark.parametrize("kind", ["line", "bars", "speedup"])
    @pytest.mark.parametrize("theme", ["light", "dark"])
    def test_restyle_matches_a_full_render(self, balanced, kind, theme):
        """The fast path must be indistinguishable from drawing it again, to the pixel.

        This is the whole licence for the fast path. Error-bar caps carry their colour in
        markeredgecolor rather than color, and that one omission was 122 visibly wrong pixels
        -- which is exactly the class of bug this catches.
        """
        st, frames = _state(balanced, kind=kind, theme=theme)
        after = _clone(st, styles=self.STYLES)
        fig = _agg_figure()
        first = m.render(fig, frames, st)
        _pixels(fig)
        m.freeze_layout(fig)                 # as the render thread does
        m.restyle(fig, frames, after, first)
        fast = _pixels(fig)

        fresh = _agg_figure()
        m.render(fresh, frames, after)
        assert np.array_equal(fast, _pixels(fresh))

    def test_restyle_reports_the_new_colours(self, balanced):
        st, frames = _state(balanced)
        fig = _agg_figure()
        first = m.render(fig, frames, st)
        after = _clone(st, styles={"u-pscq": m.SeriesStyle(color="#4a3aa7")})
        out = m.restyle(fig, frames, after, first)
        assert out.styles["u-pscq"].color == "#4a3aa7"
        assert out.records and out.built is first.built

    def test_restyle_refuses_a_figure_it_did_not_draw(self, balanced):
        st, frames = _state(balanced)
        fig = _agg_figure()
        rendered = m.render(fig, frames, st)
        with pytest.raises(ValueError):
            m.restyle(_agg_figure(), frames, st, rendered)


class TestLibrary:
    def test_only_ticked_files_are_plotted(self, balanced, unbalanced):
        st, frames = _state(balanced, unbalanced)
        assert len(st.panels) == 2
        st.library[1].plotted = False
        assert [p.csv for p in st.panels] == [st.library[0].csv]
        fig = _agg_figure()
        m.render(fig, frames[:1], st)
        assert len(fig.axes) == 1

    def test_titles_follow_the_plotted_subset(self, balanced, unbalanced):
        st, _ = _state(balanced, unbalanced)
        st.library[0].plotted = False
        assert st.panel_title(0) == ""          # one plot left: the figure title names it
        assert st.figure_title() == "unbalanced"

    def test_a_session_from_before_the_library_still_loads(self, balanced):
        old = '{"panels": [{"csv": "a.csv", "title": "A"}], "kind": "bars"}'
        state = m.PlotState.from_json(old)
        assert [p.csv for p in state.panels] == ["a.csv"]
        assert state.library[0].plotted and state.library[0].title == "A"


class TestViewport:
    """Zoom and pan are pure geometry, so they are checked as geometry."""

    def test_the_default_is_the_whole_figure(self):
        vp = m.Viewport()
        assert vp.whole and vp.clamped() == vp

    def test_zoom_is_clamped_to_the_useful_range(self):
        assert m.Viewport(0.2).clamped().zoom == 1.0
        assert m.Viewport(1000.0).clamped().zoom == m.MAX_ZOOM

    def test_the_centre_never_leaves_the_figure(self):
        vp = m.Viewport(2.0, 0.0, 1.0).clamped()
        assert vp.cx == pytest.approx(0.25) and vp.cy == pytest.approx(0.75)

    @pytest.mark.parametrize("ax,ay", [(0.5, 0.5), (0.0, 0.0), (1.0, 0.25), (0.3, 0.9)])
    def test_zooming_keeps_the_point_under_the_cursor(self, ax, ay):
        """The property that makes wheel zoom feel like a magnifying glass."""
        before = m.Viewport(2.0, 0.5, 0.5)
        under = (before.cx + (ax - 0.5) / before.zoom, before.cy + (ay - 0.5) / before.zoom)
        after = before.zoomed(1.6, ax, ay)
        still = (after.cx + (ax - 0.5) / after.zoom, after.cy + (ay - 0.5) / after.zoom)
        assert still == pytest.approx(under, abs=1e-9)

    def test_panning_follows_the_pointer_and_stops_at_the_edge(self):
        vp = m.Viewport(2.0, 0.5, 0.5)
        assert vp.panned(0.1, 0).cx == pytest.approx(0.45)   # drag right, image moves right
        assert vp.panned(-9, 0).cx == pytest.approx(0.75)    # and stops at the border
        assert m.Viewport().panned(0.4, 0.4).cx == 0.5       # nothing to pan when it fits

    def test_raster_zoom_takes_powers_of_two(self):
        assert m.raster_zoom(1.0, 800, 500) == 1.0
        assert m.raster_zoom(1.3, 800, 500) == 2.0
        assert m.raster_zoom(2.0, 800, 500) == 2.0
        assert m.raster_zoom(4.5, 800, 500) == 8.0

    def test_raster_zoom_respects_the_memory_budget(self):
        """A big window must not be able to ask for a gigabyte of raster."""
        w, h = 2560, 1400
        z = m.raster_zoom(m.MAX_ZOOM, w, h)
        assert (w * z) * (h * z) <= m.RASTER_BUDGET_PX
        assert z < m.MAX_ZOOM, "this window is large enough that the budget should bite"

    def test_the_whole_view_of_a_native_raster_is_the_raster(self):
        raster = np.random.default_rng(0).integers(0, 255, (60, 100, 3), dtype=np.uint8)
        out = m.viewport_pixels(raster, m.Viewport(), 100, 60)
        assert np.array_equal(out, raster)

    def test_a_native_zoom_is_an_exact_crop(self):
        """2x from a 2x raster must be a slice, not a resample: that is the cheap path."""
        raster = np.random.default_rng(1).integers(0, 255, (120, 200, 3), dtype=np.uint8)
        out = m.viewport_pixels(raster, m.Viewport(2.0, 0.5, 0.5), 100, 60)
        assert np.array_equal(out, raster[30:90, 50:150])

    def test_panning_moves_the_crop_by_the_expected_pixels(self):
        raster = np.random.default_rng(2).integers(0, 255, (120, 200, 3), dtype=np.uint8)
        out = m.viewport_pixels(raster, m.Viewport(2.0, 0.25, 0.5), 100, 60)
        assert np.array_equal(out, raster[30:90, 0:100])

    def test_a_non_native_zoom_is_resampled_to_the_view(self):
        raster = np.random.default_rng(3).integers(0, 255, (120, 200, 3), dtype=np.uint8)
        out = m.viewport_pixels(raster, m.Viewport(1.5, 0.5, 0.5), 100, 60)
        assert out.shape == (60, 100, 3) and out.dtype == np.uint8

    def test_a_flat_image_survives_resampling_unchanged(self):
        """Interpolation must not introduce colour of its own."""
        raster = np.full((120, 200, 3), 173, dtype=np.uint8)
        out = m.viewport_pixels(raster, m.Viewport(1.7, 0.4, 0.6), 100, 60)
        assert np.array_equal(out, np.full((60, 100, 3), 173, dtype=np.uint8))

    def test_the_viewport_is_not_part_of_a_session(self):
        """Where you were looking must not change what a reopened session draws."""
        assert "zoom" not in m.PlotState().to_json()


class TestComparing:
    """Ticking a second value of a run parameter: one plot each, or one plot."""

    def _sized(self, balanced, mode="facet"):
        st, frames = _state(balanced)
        st.filters["Size"] = ["1024", "4096"]
        st.compare["Size"] = mode
        return st, frames

    def test_a_compared_parameter_gets_a_plot_per_value(self, balanced):
        st, frames = self._sized(balanced)
        assert [s.facet for s in st.slots()] == [(("Size", "1024"),), (("Size", "4096"),)]
        built = m.build_series(frames, st)
        assert len(built.panels) == 2

    def test_each_plot_holds_only_its_own_value(self, balanced):
        """The point of a facet: the lines are not split, they are in different plots."""
        st, frames = self._sized(balanced)
        built = m.build_series(frames, st)
        assert built.split_by == []
        assert sorted(built.panels[0]) == ["u-prq", "u-pscq"]
        assert built.panels[0]["u-pscq"].y != built.panels[1]["u-pscq"].y

    def test_the_plots_are_named_by_the_value(self, balanced):
        st, _ = self._sized(balanced)
        assert st.panel_title(0) == "size 1024"
        assert st.panel_title(1) == "size 4096"

    def test_overlay_keeps_the_old_one_plot_behaviour(self, balanced):
        st, frames = self._sized(balanced, mode="overlay")
        built = m.build_series(frames, st)
        assert len(built.panels) == 1
        assert built.split_by == ["Size"]
        assert sorted(built.keys()) == ["u-prq-@Size=1024", "u-prq-@Size=4096",
                                        "u-pscq-@Size=1024", "u-pscq-@Size=4096"]

    def test_two_compared_parameters_multiply(self, balanced):
        st, frames = self._sized(balanced)
        st.filters["Pinning"] = ["True", "False"]
        st.compare["Pinning"] = "facet"
        assert len(st.slots()) == 4
        assert st.panel_title(3) == "size 4096 · unpinned"

    def test_the_plot_count_is_capped_and_says_so(self, balanced):
        st, frames = self._sized(balanced)
        st.filters["Pinning"] = ["True", "False"]
        st.filters["ProdDelay_NS"] = ["0", "240"]
        st.filters["Producers"] = ["1", "2", "4"]
        st.compare.update({"Pinning": "facet", "ProdDelay_NS": "facet", "Producers": "facet"})
        assert len(st.slots()) == m.MAX_PLOTS
        assert any("showing the first" in n for n in m.build_series(frames, st).notes)

    def test_the_x_column_is_never_faceted(self, balanced):
        """Every value of the x column is the sweep; one plot each would be one point each."""
        st, frames = _state(balanced)
        st.compare["Producers"] = "facet"
        st.x = "Producers"
        assert st.facet_columns() == []
        assert len(st.slots()) == 1

    def test_files_and_facets_both_become_plots(self, balanced, unbalanced):
        st, frames = _state(balanced, unbalanced)
        st.filters["Size"] = ["1024", "4096"]
        st.compare["Size"] = "facet"
        assert len(st.slots()) == 4
        assert st.panel_title(0) == "balanced · size 1024"
        assert [s.panel for s in st.slots()] == [0, 0, 1, 1]

    def test_render_draws_one_axes_per_plot(self, balanced):
        st, frames = self._sized(balanced)
        fig = _agg_figure()
        m.render(fig, frames, st)
        drawn = [a for a in fig.axes if a.get_visible()]
        assert len(drawn) == 2
        assert [a.get_title() for a in drawn] == ["size 1024", "size 4096"]

    def test_a_link_may_not_undo_a_compared_selection(self, balanced):
        """Delay implies one amplitude; comparing amplitudes must survive a delay edit."""
        st, _ = _state(balanced)
        st.filters["ProdDelay_Amp"] = ["0.0", "0.5"]
        links = m.correlations([m.load_frame(balanced)])
        m.propagate(st.filters, "ProdDelay_NS", links, pinned={"ProdDelay_Amp"})
        assert st.filters["ProdDelay_Amp"] == ["0.0", "0.5"]
        m.propagate(st.filters, "ProdDelay_NS", links)
        assert st.filters["ProdDelay_Amp"] == ["0.0"], "unpinned, it still follows"


class TestPerPlotAxes:
    def _two(self, balanced, unbalanced):
        st, frames = _state(balanced, unbalanced)
        return st, frames, st.slots()

    def test_an_override_reaches_only_its_own_plot(self, balanced, unbalanced):
        st, frames, slots = self._two(balanced, unbalanced)
        st.overrides[slots[1].ident] = {"ylabel": "just this one", "title": "second"}
        assert st.axes_for(1).ylabel == "just this one"
        assert st.axes_for(0).ylabel is None
        fig = _agg_figure()
        m.render(fig, frames, st)
        drawn = [a for a in fig.axes if a.get_visible()]
        assert drawn[1].get_ylabel() == "just this one"
        assert drawn[1].get_title() == "second"
        assert drawn[0].get_ylabel() != "just this one"

    def test_per_plot_ticks_reach_the_export(self, balanced, unbalanced):
        st, frames, slots = self._two(balanced, unbalanced)
        st.overrides[slots[0].ident] = {"xticks": m.TickSpec("hidden", 1.0)}
        fig = _agg_figure()
        m.render(fig, frames, st)
        drawn = [a for a in fig.axes if a.get_visible()]
        assert list(drawn[0].get_xticks()) == []
        assert list(drawn[1].get_xticks()) != []

    def test_a_y_limit_needs_the_shared_axis_off(self, balanced, unbalanced):
        """A shared axis has one range: honouring one plot's limit would move every plot."""
        st, frames, slots = self._two(balanced, unbalanced)
        st.overrides[slots[0].ident] = {"ymax": 3.0}
        st.share_y = True
        assert st.axes_for(0).ymax is None
        assert any("ignored while the y axis is shared" in n
                   for n in m.build_series(frames, st).notes)
        st.share_y = False
        assert st.axes_for(0).ymax == 3.0

    def test_a_plot_with_its_own_legend_takes_over_from_the_figure(self, balanced, unbalanced):
        """Only the plot that asked gets one: the same ten names beside every plot is not a
        legend, and a figure legend beside a per-plot one lists everything twice."""
        st, frames, slots = self._two(balanced, unbalanced)
        assert not st.per_plot_legends
        st.overrides[slots[0].ident] = {"legend": "best"}
        assert st.per_plot_legends
        assert (st.legend_place(0), st.legend_place(1)) == ("best", "none")
        fig = _agg_figure()
        m.render(fig, frames, st)
        assert not fig.legends
        assert [a.get_legend() is not None for a in fig.axes if a.get_visible()] == [True, False]

    def test_overrides_survive_a_session(self, balanced):
        st, _ = _state(balanced)
        ident = st.slots()[0].ident
        st.overrides[ident] = {"xlabel": "threads", "yticks": m.TickSpec("count", 4.0)}
        back = m.PlotState.from_json(st.to_json())
        assert back.axes_for(0).xlabel == "threads"
        assert back.axes_for(0).yticks == m.TickSpec("count", 4.0)

    def test_a_compared_plot_keeps_its_settings_when_the_order_changes(self, balanced,
                                                                       unbalanced):
        """Keyed by file and facet, not by position: reordering must not move the override."""
        st, frames = _state(balanced, unbalanced)
        second = st.slots()[1].ident
        st.overrides[second] = {"ylabel": "mine"}
        st.library.reverse()
        assert st.slots()[0].ident == second
        assert st.axes_for(0).ylabel == "mine"


class TestBaselineInRender:
    def test_the_export_shows_the_ratios_the_preview_did(self, balanced):
        st, frames = _state(balanced)
        st.baseline_mode = "ratio"
        st.baseline_target = "u-pscq"
        fig = _agg_figure()
        rendered = m.render(fig, frames, st)
        assert rendered.built.panels[0]["u-pscq"].y == [1.0, 1.0, 1.0]
        assert "baseline" in fig.axes[0].get_ylabel().lower()

    def test_a_baseline_that_is_not_there_falls_back_to_absolute(self, balanced):
        st, frames = _state(balanced)
        st.baseline_mode = "ratio"
        st.baseline_target = "gone"
        rendered = m.render(_agg_figure(), frames, st)
        assert rendered.built.panels[0]["u-pscq"].y != [1.0, 1.0, 1.0]
        assert any("not in this plot" in n for n in rendered.notes)

    def test_the_chosen_baseline_is_remembered_even_while_it_is_missing(self, balanced):
        st, _ = _state(balanced)
        st.baseline_mode = "ratio"
        st.baseline_target = "u-pscq"
        empty = m.Built([{}])
        assert m.baseline_for(empty, st) == ""
        assert st.baseline_target == "u-pscq", "the choice is kept, only unusable right now"
