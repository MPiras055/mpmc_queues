"""The render thread: coalescing, the fast path, saves, and surviving a failure.

No Tk here. The service is deliberately a plain object with two queues, so the threading
rules can be tested without a display.
"""

import matplotlib

matplotlib.use("Agg")

import time  # noqa: E402

import pytest  # noqa: E402

from mpmc_bench.gui import model as m, service as sv  # noqa: E402
from tests.test_gui_model import HEADER, _row  # noqa: E402

TIMEOUT = 60


@pytest.fixture
def csv(tmp_path):
    rows = [_row(q, p, p, size, True, 0, base * p)
            for q, base in (("u-pscq", 100.0), ("u-prq", 80.0))
            for p in (1, 2, 4) for size in (1024, 4096)]
    path = tmp_path / "r.csv"
    path.write_text(HEADER + "".join(rows))
    return path


@pytest.fixture
def state(csv):
    frames = [m.load_frame(csv)]
    st = m.PlotState(library=[m.Panel(str(csv))],
                     filters=m.default_filters(m.dimension_values(frames),
                                               m.correlations(frames)))
    return st, frames


@pytest.fixture
def service():
    s = sv.RenderService()
    yield s
    s.stop()


def _collect(service, count=1, timeout=TIMEOUT):
    """Drain until @p count results have arrived, the way the window's poll loop does."""
    out, deadline = [], time.monotonic() + timeout
    while len(out) < count and time.monotonic() < deadline:
        out += service.drain()
        time.sleep(0.01)
    assert len(out) >= count, f"only {len(out)} of {count} results in {timeout}s"
    return out


def _ppm_size(data: bytes) -> tuple[int, int]:
    parts = data.split(b"\n", 3)
    assert parts[0] == b"P6"
    return tuple(int(v) for v in parts[1].split())


class TestPreview:
    def test_renders_and_returns_pixels(self, service, state):
        st, frames = state
        seq = service.request_preview("main", st, frames, 800, 500, 100)
        result = _collect(service)[0]
        assert result.seq == seq and result.error is None
        assert _ppm_size(result.ppm) == (800, 500)
        assert result.rendered.built.keys() == ["u-prq", "u-pscq"]
        assert result.ms > 0

    def test_busy_clears_when_the_work_is_done(self, service, state):
        st, frames = state
        service.request_preview("main", st, frames, 600, 400, 100)
        _collect(service)
        assert not service.busy

    def test_a_cosmetic_change_takes_the_fast_path(self, service, state):
        st, frames = state
        service.request_preview("main", st, frames, 700, 450, 100)
        assert _collect(service)[0].fast is False       # nothing to reuse yet

        after = m.PlotState(**{**st.__dict__,
                               "styles": {"u-pscq": m.SeriesStyle(color="#4a3aa7")}})
        service.request_preview("main", after, frames, 700, 450, 100)
        assert _collect(service)[0].fast is True

    @pytest.mark.parametrize("change", [
        {"filters": {"Size": ["4096"]}},               # different data
        {"theme": "dark"},                             # different everything
        {"styles": {"u-pscq": m.SeriesStyle(visible=False)}},   # different limits and legend
        {"styles": {"u-pscq": m.SeriesStyle(label="x")}},       # different legend extents
    ])
    def test_structural_changes_do_not(self, service, state, change):
        st, frames = state
        service.request_preview("main", st, frames, 700, 450, 100)
        _collect(service)
        fields = {**st.__dict__}
        fields.update({k: ({**st.filters, **v} if k == "filters" else v)
                       for k, v in change.items()})
        service.request_preview("main", m.PlotState(**fields), frames, 700, 450, 100)
        assert _collect(service)[0].fast is False

    def test_resizing_is_structural(self, service, state):
        """A different pixel size needs a fresh layout, not the frozen one."""
        st, frames = state
        service.request_preview("main", st, frames, 700, 450, 100)
        _collect(service)
        service.request_preview("main", st, frames, 900, 450, 100)
        result = _collect(service)[0]
        assert result.fast is False and _ppm_size(result.ppm) == (900, 450)

    def test_views_are_independent(self, service, state):
        st, frames = state
        service.request_preview("main", st, frames, 600, 400, 100)
        service.request_preview("window-1", st, frames, 300, 200, 100)
        sizes = {r.view: _ppm_size(r.ppm) for r in _collect(service, 2)}
        assert sizes == {"main": (600, 400), "window-1": (300, 200)}


class TestCoalescing:
    def test_only_the_newest_preview_per_view_is_drawn(self, service, state):
        """Intermediate frames nobody will see are wasted seconds, so they are dropped."""
        st, frames = state
        seqs = [service.request_preview("main", st, frames, 640 + 10 * i, 400, 100)
                for i in range(5)]
        results = _collect(service, 1)
        deadline = time.monotonic() + 2
        while time.monotonic() < deadline:
            results += service.drain()
            time.sleep(0.05)
        assert results, "nothing came back"
        assert results[-1].seq == seqs[-1], "the newest request must be the one drawn"
        assert len(results) < len(seqs), "earlier requests should have been dropped"
        assert not service.busy, "dropped jobs must still be counted off"

    def test_a_save_queued_behind_previews_is_not_dropped(self, service, state, tmp_path):
        st, frames = state
        out = tmp_path / "fig.png"
        for _ in range(3):
            service.request_preview("main", st, frames, 640, 400, 100)
        service.request_save(st, frames, str(out), "png")
        for _ in range(3):
            service.request_preview("main", st, frames, 640, 400, 100)
        saves = [r for r in _collect(service, 2) if isinstance(r, sv.SaveResult)]
        deadline = time.monotonic() + 10
        while not saves and time.monotonic() < deadline:
            saves = [r for r in service.drain() if isinstance(r, sv.SaveResult)]
            time.sleep(0.05)
        assert saves and saves[0].path == out and out.stat().st_size > 0


class TestSave:
    def test_writes_the_figure_and_optionally_the_values(self, service, state, tmp_path):
        st, frames = state
        st.width, st.height, st.dpi = 6, 4, 100
        service.request_save(st, frames, str(tmp_path / "fig.svg"), "svg", with_data=True)
        result = [r for r in _collect(service) if isinstance(r, sv.SaveResult)][0]
        assert result.error is None
        assert result.path.suffix == ".svg" and result.path.stat().st_size > 0
        assert result.table.name == "fig.csv" and result.table.stat().st_size > 0

    def test_a_bad_format_is_reported_not_raised(self, service, state, tmp_path):
        st, frames = state
        service.request_save(st, frames, str(tmp_path / "fig.gif"), "gif")
        result = [r for r in _collect(service) if isinstance(r, sv.SaveResult)][0]
        assert result.error and "gif" in result.error


class TestFailure:
    def test_a_failed_render_is_reported_and_the_thread_survives(self, service, state,
                                                                 monkeypatch):
        st, frames = state
        boom = {"n": 0}

        def explode(*a, **kw):
            boom["n"] += 1
            raise RuntimeError("kaboom")

        monkeypatch.setattr(m, "render", explode)
        service.request_preview("main", st, frames, 600, 400, 100)
        bad = _collect(service)[0]
        assert bad.error == "kaboom" and bad.ppm is None
        assert not service.busy

        monkeypatch.undo()
        service.request_preview("main", st, frames, 600, 400, 100)
        good = _collect(service)[0]
        assert good.error is None and good.ppm, "the thread must still be alive"


class TestViewportPath:
    """Zoom and pan must be cheap *and* identical to drawing that region from scratch."""

    def test_panning_needs_no_draw(self, service, state):
        st, frames = state
        service.request_preview("main", st, frames, 640, 400, 100, m.Viewport(2, 0.5, 0.5))
        assert _collect(service)[0].path == "full"
        service.request_preview("main", st, frames, 640, 400, 100, m.Viewport(2, 0.3, 0.5))
        moved = _collect(service)[0]
        assert moved.path == "crop" and _ppm_size(moved.ppm) == (640, 400)

    def test_a_zoom_step_inside_the_same_raster_needs_no_draw(self):
        """1.0 -> 1.4 stays on the 2x raster, which is why wheel zoom feels immediate."""
        assert m.raster_zoom(1.0, 640, 400) == 1.0 and m.raster_zoom(1.4, 640, 400) == 2.0
        assert m.raster_zoom(1.9, 640, 400) == 2.0

    def test_crossing_a_power_of_two_redraws(self, service, state):
        st, frames = state
        service.request_preview("main", st, frames, 640, 400, 100, m.Viewport(1.5))
        assert _collect(service)[0].path == "full"
        service.request_preview("main", st, frames, 640, 400, 100, m.Viewport(1.9))
        assert _collect(service)[0].path == "crop"
        service.request_preview("main", st, frames, 640, 400, 100, m.Viewport(2.5))
        assert _collect(service)[0].path == "full"

    def test_a_cropped_pan_matches_drawing_that_region_from_scratch(self, service, state,
                                                                    tmp_path):
        """The same guarantee the restyle fast path gets: cheap must mean identical.

        A fresh service has no raster to crop, so it draws the region; the first one crops
        one it already had. If those ever diverge, this fails rather than looking slightly
        wrong on screen.
        """
        st, frames = state
        target = m.Viewport(2.0, 0.28, 0.66)
        service.request_preview("main", st, frames, 620, 380, 100, m.Viewport(2.0, 0.5, 0.5))
        _collect(service)
        service.request_preview("main", st, frames, 620, 380, 100, target)
        cropped = _collect(service)[0]
        assert cropped.path == "crop"

        other = sv.RenderService()
        try:
            other.request_preview("main", st, frames, 620, 380, 100, target)
            drawn = _collect(other)[0]
        finally:
            other.stop()
        assert drawn.path == "full"
        assert cropped.ppm == drawn.ppm

    def test_zooming_actually_changes_the_picture(self, service, state):
        st, frames = state
        service.request_preview("main", st, frames, 640, 400, 100)
        whole = _collect(service)[0]
        service.request_preview("main", st, frames, 640, 400, 100, m.Viewport(3, 0.5, 0.5))
        close = _collect(service)[0]
        assert _ppm_size(close.ppm) == _ppm_size(whole.ppm)
        assert close.ppm != whole.ppm

    def test_an_edit_while_zoomed_keeps_the_zoom(self, service, state):
        """Changing the plot must not throw you back to the whole figure."""
        st, frames = state
        vp = m.Viewport(2.0, 0.3, 0.3)
        service.request_preview("main", st, frames, 640, 400, 100, vp)
        _collect(service)
        after = m.PlotState(**{**st.__dict__, "theme": "dark"})
        service.request_preview("main", after, frames, 640, 400, 100, vp)
        result = _collect(service)[0]
        assert result.path == "full" and _ppm_size(result.ppm) == (640, 400)

    def test_a_style_change_while_zoomed_does_not_take_the_crop_path(self, service, state):
        """Same viewport, different colour: cropping the old raster would show the old one."""
        st, frames = state
        vp = m.Viewport(2.0, 0.5, 0.5)
        service.request_preview("main", st, frames, 640, 400, 100, vp)
        _collect(service)
        after = m.PlotState(**{**st.__dict__,
                               "styles": {"u-pscq": m.SeriesStyle(color="#4a3aa7")}})
        service.request_preview("main", after, frames, 640, 400, 100, vp)
        assert _collect(service)[0].path == "restyle"
