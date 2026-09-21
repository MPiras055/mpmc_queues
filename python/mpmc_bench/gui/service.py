"""Rendering, off the Tk main thread.

The window must never block. Measured on the real result files, one refresh costs about
900 ms -- 115 ms of pandas, 105 ms building artists, 250 ms drawing, and the rest encoding an
image -- and all of it used to run inside the Tk event loop, so every keystroke froze the
window for most of a second. Here it runs on one worker thread and the main thread only
uploads the finished pixels (~90 ms).

**Exactly one thread may render.** matplotlib's ``rcParams`` is process-wide and
``theming.apply`` writes it, so a second renderer would corrupt a figure mid-draw. That is why
exports run here too rather than inline: it keeps the rule to one place. Nothing
matplotlib-shaped crosses back -- the worker returns bytes plus the plain-data
:class:`model.Rendered` -- and nothing Tk-shaped is ever touched from this side.

Four things make a redraw cheap, in the order they are tried:

1. when only the **viewport** moved -- a pan, or a zoom step that stays within the same
   supersampled raster -- nothing is drawn at all: the stored raster is re-cropped, which is
   a numpy slice and a buffer copy;
2. an unchanged :func:`model.data_signature` reuses the previous ``Built``, skipping pandas;
3. an unchanged :func:`model.structure_signature` takes :func:`model.restyle`, mutating the
   artists already on the figure instead of building new ones;
4. after a full render the layout is frozen (:func:`model.freeze_layout`), because constrained
   layout is about half the cost of a draw and recomputes the same answer.

Zoom is why each view keeps its last raster. The figure is drawn at ``dpi * raster_zoom`` so
a magnified plot is genuinely redrawn rather than upsampled, and because ``raster_zoom`` only
ever takes powers of two, most wheel steps land on path 1.

Pixels go out as a PPM buffer, not a PNG: Tk reads both, but PNG costs 290 ms to encode and
another 60 ms to decode, and a benchmark plot is not worth compressing.
"""

from __future__ import annotations

import logging
import queue
import threading
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

import numpy as np
from matplotlib.backends.backend_agg import FigureCanvasAgg
from matplotlib.figure import Figure

from . import model as m

logger = logging.getLogger(__name__)

__all__ = ["RenderService", "PreviewResult", "SaveResult", "to_ppm", "pack_ppm"]


def pack_ppm(rgb: np.ndarray) -> bytes:
    """An (h, w, 3) uint8 array as a binary PPM, which ``tk.PhotoImage(data=...)`` reads."""
    return (b"P6\n%d %d\n255\n" % (rgb.shape[1], rgb.shape[0])
            + np.ascontiguousarray(rgb).tobytes())


def to_ppm(fig: Figure) -> bytes:
    """The whole drawn figure as a PPM."""
    return pack_ppm(np.asarray(fig.canvas.buffer_rgba())[:, :, :3])


@dataclass
class PreviewResult:
    view: str
    seq: int
    ppm: bytes | None = None
    rendered: m.Rendered | None = None
    error: str | None = None
    ms: int = 0
    #: "full" | "restyle" | "crop" -- which path drew it, which the status line reports.
    path: str = "full"
    facecolor: str = "#ffffff"

    @property
    def fast(self) -> bool:
        """True when no artists were rebuilt."""
        return self.path != "full"


@dataclass
class SaveResult:
    path: Path | None = None
    table: Path | None = None
    error: str | None = None
    ms: int = 0


@dataclass
class _Job:
    kind: str                  # "preview" | "save"
    view: str = "main"
    seq: int = 0
    state: m.PlotState | None = None
    frames: list = field(default_factory=list)
    width: int = 0
    height: int = 0
    dpi: float = 96.0
    path: str = ""
    fmt: str = "png"
    viewport: m.Viewport = field(default_factory=m.Viewport)
    transparent: bool = False
    with_data: bool = False


@dataclass
class _View:
    """One on-screen figure, and what it was last drawn from."""

    figure: Figure
    key: tuple | None = None          # size + structure signature of the last full render
    rendered: m.Rendered | None = None
    #: What the last draw produced, kept so a pan or a small zoom step needs no draw at all.
    raster: np.ndarray | None = None
    cosmetic: tuple = ()


class RenderService:
    """A render thread with a one-deep preview queue.

    Preview jobs are **coalesced**: only the newest per view survives, because an
    intermediate frame nobody will see is wasted work. Save jobs are never dropped and keep
    their order relative to each other.
    """

    def __init__(self) -> None:
        self._jobs: queue.Queue[_Job | None] = queue.Queue()
        self._results: queue.Queue[Any] = queue.Queue()
        self._views: dict[str, _View] = {}
        self._built: tuple[tuple | None, m.Built | None] = (None, None)
        self._seq = 0
        self._outstanding = 0
        self._lock = threading.Lock()
        self._thread = threading.Thread(target=self._run, name="mpmc-render", daemon=True)
        self._thread.start()

    # -- main thread ---------------------------------------------------------------------

    @property
    def busy(self) -> bool:
        with self._lock:
            return self._outstanding > 0

    def request_preview(self, view: str, state: m.PlotState, frames: list,
                        width: int, height: int, dpi: float,
                        viewport: m.Viewport | None = None) -> int:
        """Queue a redraw of @p view. Returns the sequence number to expect back."""
        with self._lock:
            self._seq += 1
            self._outstanding += 1
            seq = self._seq
        self._jobs.put(_Job("preview", view=view, seq=seq, state=state, frames=frames,
                            width=width, height=height, dpi=dpi,
                            viewport=(viewport or m.Viewport()).clamped()))
        return seq

    def request_save(self, state: m.PlotState, frames: list, path: str, fmt: str,
                     transparent: bool = False, with_data: bool = False) -> None:
        with self._lock:
            self._outstanding += 1
        self._jobs.put(_Job("save", state=state, frames=frames, path=path, fmt=fmt,
                            transparent=transparent, with_data=with_data))

    def drain(self) -> list:
        """Every finished result, oldest first. Never blocks."""
        out = []
        while True:
            try:
                out.append(self._results.get_nowait())
            except queue.Empty:
                return out

    def forget(self, view: str) -> None:
        """Drop a closed window's figure."""
        self._jobs.put(_Job("forget", view=view))

    def stop(self) -> None:
        self._jobs.put(None)

    # -- render thread -------------------------------------------------------------------

    def _run(self) -> None:
        while True:
            job = self._jobs.get()
            if job is None:
                return
            batch = [job]
            while True:                      # take everything queued up behind it
                try:
                    nxt = self._jobs.get_nowait()
                except queue.Empty:
                    break
                if nxt is None:
                    self._finish_batch(batch)
                    return
                batch.append(nxt)
            self._finish_batch(batch)

    def _finish_batch(self, batch: list[_Job]) -> None:
        # Saves first and in order -- they are explicit acts and must not be dropped -- then
        # only the newest preview per view.
        latest: dict[str, _Job] = {}
        for job in batch:
            if job.kind == "save":
                self._do_save(job)
            elif job.kind == "forget":
                self._views.pop(job.view, None)
                self._done()
            else:
                superseded = latest.get(job.view)
                if superseded is not None:
                    self._done()             # count it off; nobody will see it
                latest[job.view] = job
        for job in latest.values():
            self._do_preview(job)

    def _done(self) -> None:
        with self._lock:
            self._outstanding -= 1

    def _do_preview(self, job: _Job) -> None:
        start = time.perf_counter()
        try:
            result = self._render(job)
        except Exception as exc:                       # never kill the thread
            logger.exception("render failed")
            result = PreviewResult(job.view, job.seq, error=str(exc))
        result.ms = round(1000 * (time.perf_counter() - start))
        self._results.put(result)
        self._done()

    def _render(self, job: _Job) -> PreviewResult:
        state, frames = job.state, job.frames
        view = self._views.get(job.view)
        vp = job.viewport
        zoom = m.raster_zoom(vp.zoom, job.width, job.height)
        size = (job.width, job.height, round(job.dpi, 3), zoom)
        key = (size, m.structure_signature(state))
        cosmetic = m.cosmetic_signature(state)

        # Nothing about the picture changed, so only the viewport can have: re-crop.
        if (view is not None and view.key == key and view.raster is not None
                and view.cosmetic == cosmetic):
            pixels = m.viewport_pixels(view.raster, vp, job.width, job.height)
            return PreviewResult(job.view, job.seq, ppm=pack_ppm(pixels),
                                 rendered=view.rendered, path="crop",
                                 facecolor=_hex(view.figure.get_facecolor()))

        fast = view is not None and view.key == key and view.rendered is not None
        if fast:
            fig = view.figure
        else:
            # A fresh figure rather than clear(): a reused one carries the last render's axis
            # state, and restoring linear limits onto a now-logarithmic shared axis warns
            # about a non-positive ylim nobody asked for. Building one costs about a
            # millisecond against the ~250 ms draw.
            fig = Figure(layout="constrained")
            FigureCanvasAgg(fig)
            view = self._views[job.view] = _View(fig)
        # Supersample by raising the DPI, not the size in inches: every point-sized thing --
        # fonts, line widths, marker sizes -- then magnifies with the plot, and the layout is
        # the identical one the export would get.
        fig.set_dpi(job.dpi * zoom)
        fig.set_size_inches(job.width / job.dpi, job.height / job.dpi)

        if fast:
            rendered = m.restyle(fig, frames, state, view.rendered)
        else:
            sig = m.data_signature(state)
            built = self._built[1] if self._built[0] == sig else None
            rendered = m.render(fig, frames, state, built=built)
            self._built = (sig, rendered.built)
        fig.canvas.draw()
        if not fast:
            # Positions are settled now; skip the layout pass on every later draw.
            m.freeze_layout(fig)
        # A copy: buffer_rgba() is a window onto the canvas, which the next draw overwrites.
        view.raster = np.asarray(fig.canvas.buffer_rgba())[:, :, :3].copy()
        view.key, view.rendered, view.cosmetic = key, rendered, cosmetic
        pixels = m.viewport_pixels(view.raster, vp, job.width, job.height)
        return PreviewResult(job.view, job.seq, ppm=pack_ppm(pixels), rendered=rendered,
                             path="restyle" if fast else "full",
                             facecolor=_hex(fig.get_facecolor()))

    def _do_save(self, job: _Job) -> None:
        start = time.perf_counter()
        try:
            path, rendered = m.save_figure(job.frames, job.state, job.path, job.fmt,
                                           transparent=job.transparent)
            table = m.export_table(rendered, path.with_suffix(".csv")) if job.with_data else None
            result = SaveResult(path=path, table=table)
        except Exception as exc:
            logger.exception("save failed")
            result = SaveResult(error=str(exc))
        result.ms = round(1000 * (time.perf_counter() - start))
        self._results.put(result)
        self._done()


def _hex(rgba) -> str:
    from matplotlib.colors import to_hex
    return to_hex(rgba)
