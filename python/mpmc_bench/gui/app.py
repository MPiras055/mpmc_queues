"""The plotting window.

    mpmc-plot-ui [results.csv ...] [--session figure.json]
    python -m mpmc_bench.gui [results.csv ...]

Tk, because it ships with Python: the tool needs no dependency the CLI does not already have.

**Nothing is drawn on this thread.** Edits update a :class:`model.PlotState`, Apply hands a
copy to :mod:`service`, and the only work left here is uploading the finished pixels -- about
90 ms, against the ~900 ms a full redraw takes. That is the whole reason the window stays
responsive while a plot is being made, and why a progress bar can animate during one.
"""

from __future__ import annotations

import argparse
import copy
import logging
import re
import sys
import time
import tkinter as tk
from pathlib import Path
from tkinter import colorchooser, filedialog, messagebox, ttk

from ..plotting import theme as theming
from . import model as m
from .service import PreviewResult, RenderService, SaveResult

logger = logging.getLogger("mpmc.plot-ui")

_HUES = ["blue", "orange", "aqua", "yellow", "magenta", "green", "violet", "red"]
#: How often finished renders are collected. Small enough to feel immediate, large enough to
#: cost nothing: the loop is a queue poll.
_POLL_MS = 40
_RESIZE_MS = 180
#: Smallest gap between redraw requests while a pan drag is in progress.
_DRAG_MS = 30

TICK, UNTICK = "☑", "☐"


def _inverse(d: dict) -> dict:
    return {v: k for k, v in d.items()}


def _float_or_none(text: str) -> float | None:
    text = text.strip().replace(",", ".")
    if not text:
        return None
    try:
        return float(text)
    except ValueError:
        return None


# --------------------------------------------------------------------------------------
# Small widgets
# --------------------------------------------------------------------------------------

class ScrollFrame(ttk.Frame):
    """A vertically scrolling container. Put children in ``.inner``."""

    def __init__(self, parent, **kw):
        super().__init__(parent, **kw)
        bg = ttk.Style().lookup("TFrame", "background") or None
        self.canvas = tk.Canvas(self, highlightthickness=0, borderwidth=0, background=bg)
        bar = ttk.Scrollbar(self, orient="vertical", command=self.canvas.yview)
        self.inner = ttk.Frame(self.canvas, padding=(10, 8))
        win = self.canvas.create_window((0, 0), window=self.inner, anchor="nw")
        self.canvas.configure(yscrollcommand=bar.set)
        self.canvas.pack(side="left", fill="both", expand=True)
        bar.pack(side="right", fill="y")
        self.inner.bind("<Configure>",
                        lambda e: self.canvas.configure(scrollregion=self.canvas.bbox("all")))
        self.canvas.bind("<Configure>", lambda e: self.canvas.itemconfigure(win, width=e.width))

    def scroll(self, units: int) -> None:
        if self.inner.winfo_height() > self.canvas.winfo_height():
            self.canvas.yview_scroll(units, "units")


def section(parent, text: str) -> ttk.Frame:
    """A titled group: a bold heading and a padded body."""
    ttk.Label(parent, text=text, style="Heading.TLabel").pack(anchor="w", pady=(12, 4))
    body = ttk.Frame(parent)
    body.pack(fill="x")
    body.columnconfigure(1, weight=1)
    return body


def labelled(body, row: int, text: str, widget, **grid) -> None:
    ttk.Label(body, text=text).grid(row=row, column=0, sticky="w", padx=(0, 8), pady=2)
    widget.grid(row=row, column=1, sticky="ew", pady=2, **grid)


class Choice(ttk.Combobox):
    """A read-only combobox over ``{value: display name}``."""

    def __init__(self, parent, options: dict[str, str], on_change, width=22):
        self._choices = dict(options)
        super().__init__(parent, state="readonly", width=width,
                         values=list(self._choices.values()))
        self._on_change = on_change
        self.bind("<<ComboboxSelected>>", lambda e: self._on_change(self.value))

    def set_options(self, options: dict[str, str]) -> None:
        self._choices = dict(options)
        self.configure(values=list(options.values()))

    @property
    def value(self) -> str:
        return _inverse(self._choices).get(self.get(), "")

    @value.setter
    def value(self, key: str) -> None:
        self.set(self._choices.get(key, ""))


class ImageView(ttk.Frame):
    """Shows whatever the render thread last produced, and lets you zoom and pan it.

    Deliberately not matplotlib's Tk canvas: that one draws on the calling thread, which is
    the thing this rewrite exists to stop. The cost here is one ``PhotoImage`` per frame.

    Zoom and pan are therefore ours rather than the navigation toolbar's. The view owns a
    :class:`model.Viewport` and asks for a redraw of that region; the render thread magnifies
    by drawing at a higher DPI and re-crops its stored raster for a pan, so a drag costs no
    draw at all. Wheel to zoom about the pointer, drag to pan, double-click or Ctrl+0 to fit.
    """

    def __init__(self, parent, on_resize, on_viewport=None):
        super().__init__(parent)
        self.label = tk.Label(self, borderwidth=0)
        self.label.pack(fill="both", expand=True)
        self._photo = None
        self._pending = None
        self._on_resize = on_resize
        self._on_viewport = on_viewport or (lambda: None)
        self._size = (0, 0)
        self.viewport = m.Viewport()
        self._drag: tuple[int, int] | None = None
        self._last_sent = 0.0
        self.label.bind("<Configure>", self._resized)
        for seq in ("<MouseWheel>", "<Button-4>", "<Button-5>"):
            self.label.bind(seq, self._wheel)
        self.label.bind("<ButtonPress-1>", self._press)
        self.label.bind("<B1-Motion>", self._drag_to)
        self.label.bind("<ButtonRelease-1>", self._release)
        self.label.bind("<Double-Button-1>", lambda e: self.fit())

    def _resized(self, event) -> None:
        if (event.width, event.height) == self._size:
            return
        self._size = (event.width, event.height)
        if self._pending:
            self.after_cancel(self._pending)
        self._pending = self.after(_RESIZE_MS, self._fire)

    def _fire(self) -> None:
        self._pending = None
        self._on_resize()

    @property
    def size(self) -> tuple[int, int]:
        return self.label.winfo_width(), self.label.winfo_height()

    def show(self, ppm: bytes, background: str) -> None:
        self._photo = tk.PhotoImage(data=ppm)
        self.label.configure(image=self._photo, background=background)

    def message(self, text: str, background: str = "#fcfcfb") -> None:
        self._photo = None
        self.label.configure(image="", text=text, background=background,
                             foreground="#6f6e69", font=("TkDefaultFont", 12))

    # -- zoom and pan --------------------------------------------------------------------

    def set_viewport(self, vp: m.Viewport, throttle: bool = False) -> None:
        """Move the view, and ask for the pixels unless a drag is still in flight.

        @p throttle drops requests during a drag to about one per _DRAG_MS. The service
        coalesces anyway, but there is no point queueing frames faster than they can be
        cropped and uploaded.
        """
        vp = vp.clamped()
        if vp == self.viewport:
            return
        self.viewport = vp
        now = time.perf_counter()
        if throttle and (now - self._last_sent) * 1000 < _DRAG_MS:
            return
        self._last_sent = now
        self._on_viewport()

    def fit(self) -> None:
        """Back to the whole figure."""
        if self.viewport.whole:
            return
        self.set_viewport(m.Viewport())

    def zoom_by(self, factor: float, ax: float = 0.5, ay: float = 0.5) -> None:
        self.set_viewport(self.viewport.zoomed(factor, ax, ay))

    def _wheel(self, event):
        up = getattr(event, "num", 0) == 4 or getattr(event, "delta", 0) > 0
        w, h = self.size
        if w < 2 or h < 2:
            return "break"
        # About the pointer, so the wheel works like a magnifying glass. 1.25 per notch is
        # roughly ten notches end to end over the 1x-8x range.
        self.zoom_by(1.25 if up else 1 / 1.25, event.x / w, event.y / h)
        return "break"              # stop the sidebar's bind_all scroll handler

    def _press(self, event) -> None:
        self.label.focus_set()
        if not self.viewport.whole:
            self._drag = (event.x, event.y)
            self.label.configure(cursor="fleur")

    def _drag_to(self, event) -> None:
        if self._drag is None:
            return
        w, h = self.size
        x, y = self._drag
        self._drag = (event.x, event.y)
        self.set_viewport(self.viewport.panned((event.x - x) / max(w, 1),
                                               (event.y - y) / max(h, 1)), throttle=True)

    def _release(self, event) -> None:
        if self._drag is None:
            return
        self._drag = None
        self.label.configure(cursor="")
        self._last_sent = 0.0
        self._on_viewport()          # make sure the final position is the one drawn


# --------------------------------------------------------------------------------------
# The application
# --------------------------------------------------------------------------------------

class PlotApp:
    def __init__(self, root: tk.Tk, csvs: list[str], session: str | None = None):
        self.root = root
        self.state = m.PlotState()
        self.cache: dict[str, tuple[float, object]] = {}
        self.links: dict[str, dict[str, dict[str, str]]] = {}
        self.last_dir = str(Path.cwd())
        self.rendered: m.Rendered | None = None
        self.service = RenderService()

        self._series_keys: list[str] = []
        self._dims_signature: tuple = ()
        self._rows: dict[str, dict] = {}
        self._filter_vars: dict[str, dict[str, tk.BooleanVar]] = {}
        self._loading = False
        self._edits = 0                 # bumped by every state change; how "pending" is known
        self._requested_at = 0
        self._request_seq = 0
        self._sent: tuple | None = None
        self._busy_since: float | None = None
        self._windows: dict[str, tuple[tk.Toplevel, ImageView]] = {}
        self._window_seq = 0

        # Tk scales fonts by the display's DPI but not pixel sizes; scale those ourselves so
        # a 2x screen does not get a sidebar half as wide as its text.
        self.f = max(1.0, root.winfo_fpixels("1i") / 96)
        root.title("mpmc plot studio")
        w = min(self.px(1500), int(root.winfo_screenwidth() * 0.92))
        h = min(self.px(900), int(root.winfo_screenheight() * 0.88))
        root.geometry(f"{w}x{h}")
        root.minsize(min(self.px(1000), w), min(self.px(640), h))
        self._style()
        self._build()
        self._shortcuts()
        # Seed every control from the default state: without this the comboboxes start blank,
        # since _reconcile only ever touches the two whose options depend on the data.
        self._sync_widgets()
        root.protocol("WM_DELETE_WINDOW", self._quit)
        self.root.after(_POLL_MS, self._poll)

        if session:
            self.load_session(session)
        elif csvs:
            self.add_csvs(csvs)
        else:
            self._show_status()

    def px(self, n: float) -> int:
        return int(n * self.f)

    def _quit(self) -> None:
        self.service.stop()
        self.root.destroy()

    # -- look ----------------------------------------------------------------------------

    def _style(self) -> None:
        style = ttk.Style()
        if "clam" in style.theme_names():
            style.theme_use("clam")
        from tkinter import font as tkfont
        base = ("TkDefaultFont",)
        line = tkfont.nametofont("TkDefaultFont").metrics("linespace")
        style.configure("Heading.TLabel", font=(*base, 10, "bold"))
        style.configure("Muted.TLabel", foreground="#6f6e69", font=(*base, 8))
        style.configure("Treeview", rowheight=line + self.px(6))
        style.configure("Accent.TButton", padding=(10, 4))
        style.configure("Status.TLabel", font=(*base, 9))

    # -- layout --------------------------------------------------------------------------

    def _build(self) -> None:
        bar = ttk.Frame(self.root, padding=(8, 6))
        bar.pack(side="top", fill="x")
        for text, cmd in [("Add CSV…", self.ask_add_csv),
                          ("Open session…", self.ask_load_session),
                          ("Save session…", self.ask_save_session)]:
            ttk.Button(bar, text=text, command=cmd).pack(side="left", padx=2)
        ttk.Separator(bar, orient="vertical").pack(side="left", fill="y", padx=8)
        ttk.Button(bar, text="Save figure…", command=self.ask_save_figure).pack(side="left", padx=2)
        ttk.Button(bar, text="Export data…", command=self.ask_export_data).pack(side="left", padx=2)
        ttk.Button(bar, text="Show in window", command=self.show_window).pack(side="left", padx=2)
        ttk.Separator(bar, orient="vertical").pack(side="left", fill="y", padx=8)
        ttk.Button(bar, text="−", width=2,
                   command=lambda: self.view.zoom_by(1 / 1.5)).pack(side="left")
        self.zoom_var = tk.StringVar(value="100%")
        ttk.Label(bar, textvariable=self.zoom_var, width=6, anchor="center").pack(side="left")
        ttk.Button(bar, text="+", width=2,
                   command=lambda: self.view.zoom_by(1.5)).pack(side="left")
        ttk.Button(bar, text="Fit", width=4,
                   command=lambda: self.view.fit()).pack(side="left", padx=(4, 0))
        ttk.Separator(bar, orient="vertical").pack(side="left", fill="y", padx=8)
        ttk.Label(bar, text="Theme").pack(side="left", padx=(0, 4))
        self.theme_choice = Choice(bar, {"light": "Light", "dark": "Dark"},
                                   lambda v: self._set("theme", v), width=9)
        self.theme_choice.pack(side="left")

        # Apply sits at the right, where the eye ends up after editing, with the state of the
        # figure right beside it.
        self.apply_button = ttk.Button(bar, text="Apply", style="Accent.TButton",
                                       command=self.apply)
        self.apply_button.pack(side="right", padx=(6, 0))
        self.auto = tk.BooleanVar(value=True)
        ttk.Checkbutton(bar, text="Apply on change", variable=self.auto,
                        command=self._auto_toggled).pack(side="right")
        self.progress = ttk.Progressbar(bar, mode="indeterminate", length=self.px(90))
        self.status_var = tk.StringVar()
        ttk.Label(bar, textvariable=self.status_var, style="Status.TLabel",
                  width=34, anchor="e").pack(side="right", padx=8)

        paned = ttk.PanedWindow(self.root, orient="horizontal")
        paned.pack(fill="both", expand=True)
        left = ttk.Notebook(paned, width=self.px(450))
        paned.add(left, weight=0)
        self.scrollers: list[ScrollFrame] = []
        tabs = {}
        for name in ("Data", "Series", "Axes & layout"):
            sf = ScrollFrame(left)
            left.add(sf, text=name)
            self.scrollers.append(sf)
            tabs[name] = sf.inner
        self._build_data_tab(tabs["Data"])
        self._build_series_tab(tabs["Series"])
        self._build_axes_tab(tabs["Axes & layout"])

        right = ttk.Frame(paned)
        paned.add(right, weight=1)
        self.view = ImageView(right, self.apply, lambda: self.refresh_viewport("main"))
        self.notes = tk.Text(right, height=4, wrap="word", relief="flat", padx=8, pady=6,
                             font=("TkDefaultFont", 9), background="#f4f3ef")
        self.notes.tag_configure("fail", foreground="#a4262c")
        self.notes.tag_configure("warn", foreground="#7a4d00")
        self.notes.tag_configure("note", foreground="#52514e")
        self.notes.pack(side="bottom", fill="x")
        self.view.pack(side="top", fill="both", expand=True)
        self.view.message("Add a results CSV to start  (Ctrl+O)")

        self.root.bind_all("<MouseWheel>", self._wheel, add="+")
        self.root.bind_all("<Button-4>", self._wheel, add="+")
        self.root.bind_all("<Button-5>", self._wheel, add="+")

    def _wheel(self, event) -> None:
        widget = self.root.winfo_containing(event.x_root, event.y_root)
        units = -2 if getattr(event, "num", 0) == 4 or getattr(event, "delta", 0) > 0 else 2
        while widget is not None:
            for sf in self.scrollers:
                if widget in (sf, sf.canvas, sf.inner):
                    sf.scroll(units)
                    return
            widget = widget.master

    # -- Data tab ------------------------------------------------------------------------

    def _build_data_tab(self, tab) -> None:
        files = section(tab, "Loaded files — tick the ones to plot")
        self.tree = ttk.Treeview(files, columns=("plot", "file", "title"), show="headings",
                                 height=6, selectmode="browse")
        self.tree.heading("plot", text="")
        self.tree.heading("file", text="CSV")
        self.tree.heading("title", text="Plot title")
        self.tree.column("plot", width=self.px(30), anchor="center", stretch=False)
        self.tree.column("file", width=self.px(170))
        self.tree.column("title", width=self.px(150))
        self.tree.grid(row=0, column=0, columnspan=2, sticky="ew")
        self.tree.bind("<<TreeviewSelect>>", lambda e: self._on_file_select())
        self.tree.bind("<Button-1>", self._tree_click)
        self.tree.bind("<Double-1>", self._tree_double)
        self.tree.bind("<space>", lambda e: self._toggle_plotted(self._selected_file()))
        ttk.Label(files, style="Muted.TLabel", wraplength=self.px(410), justify="left",
                  text="Files stay loaded whether or not they are ticked, so switching is "
                       "instant. Double-click a file to show only it.").grid(
            row=1, column=0, columnspan=2, sticky="w", pady=(2, 0))
        buttons = ttk.Frame(files)
        buttons.grid(row=2, column=0, columnspan=2, sticky="w", pady=4)
        for text, cmd, width in [("Add…", self.ask_add_csv, 7),
                                 ("Only this", self.only_selected, 9),
                                 ("Remove", self.remove_file, 8),
                                 ("↑", lambda: self.move_file(-1), 3),
                                 ("↓", lambda: self.move_file(1), 3)]:
            ttk.Button(buttons, text=text, command=cmd, width=width).pack(side="left", padx=(0, 4))
        self.file_info = ttk.Label(files, text="", style="Muted.TLabel",
                                   wraplength=self.px(410), justify="left")
        self.file_info.grid(row=3, column=0, columnspan=2, sticky="w")

        titles = section(tab, "Titles")
        self.fig_title = tk.StringVar()
        self._entry(titles, 0, "Figure title", self.fig_title, self._commit_fig_title,
                    auto_button=lambda: self._reset_title(None))
        self.panel_title = tk.StringVar()
        self._entry(titles, 1, "Selected plot", self.panel_title, self._commit_panel_title,
                    auto_button=lambda: self._reset_title("panel"))

        what = section(tab, "What to plot")
        self.kind = Choice(what, m.KINDS, lambda v: self._set("kind", v))
        labelled(what, 0, "Plot type", self.kind)
        self.metric = Choice(what, {}, lambda v: self._set("metric", v))
        labelled(what, 1, "Y: metric", self.metric)
        self.stat = Choice(what, {"median": "Median of runs", "mean": "Mean of runs"},
                           lambda v: self._set("stat", v))
        labelled(what, 2, "Statistic", self.stat)
        self.xaxis = Choice(what, {}, lambda v: self._set("x", v))
        labelled(what, 3, "X axis", self.xaxis)
        self.errorbars = Choice(what, m.ERRORBARS, lambda v: self._set("errorbars", v))
        labelled(what, 4, "Error bars", self.errorbars)

        ttk.Label(tab, text="Filters", style="Heading.TLabel").pack(anchor="w", pady=(12, 0))
        ttk.Label(tab, style="Muted.TLabel", wraplength=self.px(410), justify="left",
                  text="Tick several values of one parameter to compare them: the lines are "
                       "split per value instead of being averaged together.").pack(anchor="w")
        self.link_filters = tk.BooleanVar(value=True)
        ttk.Checkbutton(tab, text="Link related filters", variable=self.link_filters,
                        command=self._relabel_links).pack(anchor="w", pady=(4, 0))
        self.link_note = ttk.Label(tab, style="Muted.TLabel", wraplength=self.px(410),
                                   justify="left", text="")
        self.link_note.pack(anchor="w")
        self.filter_box = ttk.Frame(tab)
        self.filter_box.pack(fill="x")

    def _entry(self, body, row: int, text: str, var: tk.StringVar, commit, auto_button=None):
        """A text field that reaches the state on Return or focus-out -- never per keystroke.

        Typing used to schedule a redraw per character, which is what made the window feel
        stuck. Nothing here reads the variable until the edit is finished.
        """
        e = ttk.Entry(body, textvariable=var)
        labelled(body, row, text, e)
        e.bind("<Return>", lambda ev: commit())
        e.bind("<FocusOut>", lambda ev: commit())
        if auto_button:
            ttk.Button(body, text="Auto", width=5, command=auto_button).grid(
                row=row, column=2, padx=(4, 0))
        return e

    def _rebuild_filters(self, dims: dict[str, list[str]]) -> None:
        for child in self.filter_box.winfo_children():
            child.destroy()
        self._filter_vars = {}
        for col, values in dims.items():
            if len(values) < 2:
                continue
            box = ttk.LabelFrame(self.filter_box, text=m.AXIS_NAMES.get(col, col), padding=(8, 4))
            box.pack(fill="x", pady=4)
            selected = set(self.state.filters.get(col, values))
            per_row = 4 if len(values) > 4 else len(values)
            self._filter_vars[col] = {}
            for i, v in enumerate(values):
                var = tk.BooleanVar(value=v in selected)
                text = {"True": "pinned", "False": "unpinned"}.get(v, v) if col == "Pinning" else v
                ttk.Checkbutton(box, text=text, variable=var,
                                command=lambda c=col: self._filter_changed(c)
                                ).grid(row=i // per_row, column=i % per_row, sticky="w", padx=(0, 10))
                self._filter_vars[col][v] = var
            row = (len(values) - 1) // per_row + 1
            links = ttk.Frame(box)
            links.grid(row=row, column=0, columnspan=max(1, per_row), sticky="w", pady=(2, 0))
            ttk.Button(links, text="All", width=5,
                       command=lambda c=col: self._filter_all(c, True)).pack(side="left")
            ttk.Button(links, text="None", width=5,
                       command=lambda c=col: self._filter_all(c, False)).pack(side="left", padx=4)
            note = ttk.Label(box, style="Muted.TLabel")
            note.grid(row=row + 1, column=0, columnspan=max(1, per_row), sticky="w")
            self._filter_vars[col]["__note__"] = note   # keyed out of band; values are strings
        self._relabel_links()

    def _relabel_links(self) -> None:
        """Say, next to each group, which other filters move with it."""
        linked_any = []
        for col, vars_ in self._filter_vars.items():
            note = vars_.get("__note__")
            if note is None:
                continue
            others = [m.AXIS_NAMES.get(c, c).lower() for c in self.links.get(col, {})
                      if c in self._filter_vars]
            if others and self.link_filters.get():
                note.configure(text="also sets: " + ", ".join(others))
                linked_any.append(col)
            else:
                note.configure(text="")
        if linked_any and self.link_filters.get():
            self.link_note.configure(
                text="Parameters that were swept together move together; untick to control "
                     "them separately.")
        else:
            self.link_note.configure(text="")

    def _filter_changed(self, col: str) -> None:
        self.state.filters[col] = [v for v, var in self._filter_vars[col].items()
                                   if v != "__note__" and var.get()]
        if self.link_filters.get():
            for other in m.propagate(self.state.filters, col, self.links):
                wanted = set(self.state.filters[other])
                for value, var in self._filter_vars.get(other, {}).items():
                    if value != "__note__":
                        var.set(value in wanted)
        self._changed()

    def _filter_all(self, col: str, on: bool) -> None:
        for value, var in self._filter_vars[col].items():
            if value != "__note__":
                var.set(on)
        self._filter_changed(col)

    # -- Series tab ----------------------------------------------------------------------

    def _build_series_tab(self, tab) -> None:
        top = ttk.Frame(tab)
        top.pack(fill="x", pady=(4, 6))
        self.series_filter = tk.StringVar()
        ttk.Label(top, text="Find").pack(side="left")
        ttk.Entry(top, textvariable=self.series_filter, width=16).pack(side="left", padx=4)
        self.series_filter.trace_add("write", lambda *a: self._apply_series_search())
        ttk.Button(top, text="Show all", command=lambda: self._all_visible(True)).pack(side="left", padx=2)
        ttk.Button(top, text="Hide all", command=lambda: self._all_visible(False)).pack(side="left", padx=2)
        ttk.Button(top, text="Reset styles", command=self._reset_styles).pack(side="left", padx=2)
        ttk.Label(tab, style="Muted.TLabel", wraplength=self.px(410), justify="left",
                  text="Click a swatch for the theme's validated colours or a custom one. "
                       "A legend name takes effect on Enter.").pack(anchor="w")
        self.series_box = ttk.Frame(tab)
        self.series_box.pack(fill="x", pady=(6, 0))
        self.series_box.columnconfigure(2, weight=1)

    def _rebuild_series(self) -> None:
        for child in self.series_box.winfo_children():
            child.destroy()
        self._rows = {}
        if not self.rendered:
            return
        by_key = self.rendered.built.by_key()
        markers = {"default": "(default)", **m.MARKERS}
        lines = {"default": "(default)", **m.LINESTYLES}
        for r, key in enumerate(self._series_keys):
            st = self.rendered.styles[key]
            o = self.state.styles.get(key, m.SeriesStyle())
            row = r * 2
            vis = tk.BooleanVar(value=o.visible)
            cb = ttk.Checkbutton(self.series_box, variable=vis,
                                 command=lambda k=key, v=vis: self._style_set(k, visible=v.get()))
            cb.grid(row=row, column=0, sticky="w")
            sw = tk.Label(self.series_box, width=3, background=st.color, relief="groove",
                          cursor="hand2")
            sw.grid(row=row, column=1, sticky="ns", padx=(2, 6), pady=3)
            sw.bind("<Button-1>", lambda e, k=key: self._color_menu(k, e))
            label = tk.StringVar(value=st.label)
            ent = ttk.Entry(self.series_box, textvariable=label, width=24)
            ent.grid(row=row, column=2, columnspan=3, sticky="ew", pady=(4, 1))
            ent.bind("<Return>", lambda e, k=key: self._commit_label(k))
            ent.bind("<FocusOut>", lambda e, k=key: self._commit_label(k))
            key_label = ttk.Label(self.series_box, text=by_key[key].key, style="Muted.TLabel")
            key_label.grid(row=row + 1, column=2, sticky="w", pady=(0, 6))
            mk = Choice(self.series_box, markers,
                        lambda v, k=key: self._style_set(k, marker=None if v == "default" else v),
                        width=11)
            mk.value = o.marker if o.marker is not None else "default"
            mk.grid(row=row + 1, column=3, padx=(6, 2), pady=(0, 6))
            ls = Choice(self.series_box, lines,
                        lambda v, k=key: self._style_set(k, linestyle=None if v == "default" else v),
                        width=10)
            ls.value = o.linestyle if o.linestyle is not None else "default"
            ls.grid(row=row + 1, column=4, padx=(2, 0), pady=(0, 6))
            self._rows[key] = {"widgets": [cb, sw, ent, mk, ls, key_label], "swatch": sw,
                               "label": label, "visible": vis}
        self._apply_series_search()

    def _apply_series_search(self) -> None:
        needle = self.series_filter.get().strip().lower()
        for key, row in self._rows.items():
            show = not needle or needle in f"{key} {row['label'].get()}".lower()
            for w in row["widgets"]:
                w.grid() if show else w.grid_remove()

    def _refresh_swatches(self) -> None:
        if not self.rendered:
            return
        for key, row in self._rows.items():
            if key in self.rendered.styles:
                row["swatch"].configure(background=self.rendered.styles[key].color)

    def _style_set(self, key: str, **changes) -> None:
        o = self.state.styles.setdefault(key, m.SeriesStyle())
        for k, v in changes.items():
            setattr(o, k, v)
        self._changed()

    def _commit_label(self, key: str) -> None:
        if self._loading or not self.rendered or key not in self._rows:
            return
        text = self._rows[key]["label"].get().strip()
        default = self.rendered.built.by_key()[key].default_label
        new = None if text in ("", default) else text
        if new != self.state.styles.get(key, m.SeriesStyle()).label:
            self._style_set(key, label=new)

    def _color_menu(self, key: str, event) -> None:
        theme = theming.resolve(self.state.theme)
        menu = tk.Menu(self.root, tearoff=False)
        for name, hexv in zip(_HUES, theme.series):
            menu.add_command(label=f"■■■  {name}  {hexv}", foreground=hexv,
                             activeforeground=hexv,
                             command=lambda c=hexv: self._style_set(key, color=c))
        menu.add_separator()
        menu.add_command(label="Custom colour…", command=lambda: self._custom_color(key))
        menu.add_command(label="Default", command=lambda: self._style_set(key, color=None))
        menu.tk_popup(event.x_root, event.y_root)

    def _custom_color(self, key: str) -> None:
        current = self.rendered.styles[key].color if self.rendered else "#2a78d6"
        _, hexv = colorchooser.askcolor(color=current, parent=self.root, title="Series colour")
        if hexv:
            self._style_set(key, color=hexv)

    def _all_visible(self, on: bool) -> None:
        needle = self.series_filter.get().strip().lower()
        for key, row in self._rows.items():
            if needle and needle not in f"{key} {row['label'].get()}".lower():
                continue
            row["visible"].set(on)
            self.state.styles.setdefault(key, m.SeriesStyle()).visible = on
        self._changed()

    def _reset_styles(self) -> None:
        self.state.styles.clear()
        self._series_keys = []      # force the rows to rebuild with default text
        self._changed()

    # -- Axes tab ------------------------------------------------------------------------

    def _build_axes_tab(self, tab) -> None:
        labels = section(tab, "Axis labels (blank = automatic)")
        self.xlabel, self.ylabel = tk.StringVar(), tk.StringVar()
        self._entry(labels, 0, "X label", self.xlabel,
                    lambda: self._set("xlabel", self.xlabel.get() or None))
        self._entry(labels, 1, "Y label", self.ylabel,
                    lambda: self._set("ylabel", self.ylabel.get() or None))

        xs = section(tab, "X axis")
        self.xlog = tk.BooleanVar()
        ttk.Checkbutton(xs, text="Log scale (base 2)", variable=self.xlog,
                        command=lambda: self._set("xlog", self.xlog.get())
                        ).grid(row=0, column=0, columnspan=2, sticky="w")
        self.xtick_mode = Choice(xs, m.X_TICK_MODES, lambda v: self._tick("x"))
        labelled(xs, 1, "Ticks", self.xtick_mode)
        self.xtick_value = tk.StringVar(value="1")
        self._entry(xs, 2, "N / spacing", self.xtick_value, lambda: self._tick("x"))

        ys = section(tab, "Y axis")
        self.ylog, self.yzero = tk.BooleanVar(), tk.BooleanVar(value=True)
        ttk.Checkbutton(ys, text="Log scale", variable=self.ylog,
                        command=lambda: self._set("ylog", self.ylog.get())
                        ).grid(row=0, column=0, sticky="w")
        ttk.Checkbutton(ys, text="Start at zero", variable=self.yzero,
                        command=lambda: self._set("y_from_zero", self.yzero.get())
                        ).grid(row=0, column=1, sticky="w")
        self.ytick_mode = Choice(ys, m.Y_TICK_MODES, lambda v: self._tick("y"))
        labelled(ys, 1, "Ticks", self.ytick_mode)
        self.ytick_value = tk.StringVar(value="5")
        self._entry(ys, 2, "N / spacing", self.ytick_value, lambda: self._tick("y"))
        self.ymin, self.ymax, self.yscale = tk.StringVar(), tk.StringVar(), tk.StringVar()
        self._entry(ys, 3, "Min (blank = auto)", self.ymin,
                    lambda: self._set("ymin", _float_or_none(self.ymin.get())))
        self._entry(ys, 4, "Max (blank = auto)", self.ymax,
                    lambda: self._set("ymax", _float_or_none(self.ymax.get())))
        self._entry(ys, 5, "Divide values by", self.yscale,
                    lambda: self._set("yscale", _float_or_none(self.yscale.get())))
        ttk.Label(ys, text="blank = automatic (1e6 for throughput)", style="Muted.TLabel").grid(
            row=6, column=1, sticky="w")

        lay = section(tab, "Several plots")
        self.share_y, self.share_x = tk.BooleanVar(value=True), tk.BooleanVar()
        ttk.Checkbutton(lay, text="Same Y axis on every plot (the largest range)",
                        variable=self.share_y,
                        command=lambda: self._set("share_y", self.share_y.get())
                        ).grid(row=0, column=0, columnspan=2, sticky="w")
        ttk.Checkbutton(lay, text="Same X axis on every plot", variable=self.share_x,
                        command=lambda: self._set("share_x", self.share_x.get())
                        ).grid(row=1, column=0, columnspan=2, sticky="w")
        self.columns = tk.StringVar(value="0")
        self._entry(lay, 2, "Plots per row (0 = auto)", self.columns,
                    lambda: self._set("columns", int(_float_or_none(self.columns.get()) or 0)))

        look = section(tab, "Legend & grid")
        self.legend = Choice(look, m.LEGEND_PLACES, lambda v: self._set("legend", v))
        labelled(look, 0, "Legend", self.legend)
        self.grid = Choice(look, m.GRIDS, lambda v: self._set("grid", v))
        labelled(look, 1, "Grid lines", self.grid)

        out = section(tab, "Export size")
        self.width, self.height, self.dpi = tk.StringVar(), tk.StringVar(), tk.StringVar()
        self._entry(out, 0, "Width (inches)", self.width, lambda: self._size("width", float))
        self._entry(out, 1, "Height (inches)", self.height, lambda: self._size("height", float))
        self._entry(out, 2, "DPI (PNG)", self.dpi, lambda: self._size("dpi", int))

    def _size(self, name: str, cast) -> None:
        var = {"width": self.width, "height": self.height, "dpi": self.dpi}[name]
        value = _float_or_none(var.get())
        if value and value > 0:
            setattr(self.state, name, cast(value))
        # Export size does not change the preview, so nothing is re-rendered here.

    def _tick(self, axis: str) -> None:
        if self._loading:
            return
        mode = (self.xtick_mode if axis == "x" else self.ytick_mode).value
        value = _float_or_none((self.xtick_value if axis == "x" else self.ytick_value).get()) or 1.0
        setattr(self.state, f"{axis}ticks", m.TickSpec(mode or "auto", value))
        self._changed()

    # -- state plumbing ------------------------------------------------------------------

    def _set(self, name: str, value) -> None:
        if self._loading:
            return
        setattr(self.state, name, value)
        self._changed()

    def _changed(self) -> None:
        """One edit. Draws now if "Apply on change" is on, otherwise waits for Apply."""
        if self._loading:
            return
        self._edits += 1
        if self.auto.get():
            self.apply()
        else:
            self._show_status()

    def _auto_toggled(self) -> None:
        if self.auto.get() and self._edits != self._requested_at:
            self.apply()
        else:
            self._show_status()

    def _sync_widgets(self) -> None:
        """Push self.state into every static widget (after loading a session)."""
        s = self.state
        self._loading = True
        try:
            self.theme_choice.value = s.theme
            self.kind.value = s.kind
            self.stat.value = s.stat
            self.errorbars.value = s.errorbars
            self.xlabel.set(s.xlabel or "")
            self.ylabel.set(s.ylabel or "")
            self.xlog.set(s.xlog)
            self.ylog.set(s.ylog)
            self.yzero.set(s.y_from_zero)
            self.xtick_mode.value, self.ytick_mode.value = s.xticks.mode, s.yticks.mode
            self.xtick_value.set(f"{s.xticks.value:g}")
            self.ytick_value.set(f"{s.yticks.value:g}")
            self.ymin.set("" if s.ymin is None else f"{s.ymin:g}")
            self.ymax.set("" if s.ymax is None else f"{s.ymax:g}")
            self.yscale.set("" if s.yscale is None else f"{s.yscale:g}")
            self.share_y.set(s.share_y)
            self.share_x.set(s.share_x)
            self.columns.set(str(s.columns))
            self.legend.value, self.grid.value = s.legend, s.grid
            self.width.set(f"{s.width:g}")
            self.height.set(f"{s.height:g}")
            self.dpi.set(str(s.dpi))
            self.fig_title.set(s.figure_title() if s.panels else "")
        finally:
            self._loading = False

    # -- data ----------------------------------------------------------------------------

    def frame(self, path: str):
        """Load @p path, reloading when the file changed on disk (a sweep still running)."""
        mtime = Path(path).stat().st_mtime
        hit = self.cache.get(path)
        if hit and hit[0] == mtime:
            return hit[1]
        df = m.load_frame(path)
        self.cache[path] = (mtime, df)
        return df

    def frames(self):
        out = []
        for p in self.state.panels:
            try:
                out.append(self.frame(p.csv))
            except (OSError, ValueError) as exc:
                messagebox.showerror("Cannot read CSV", f"{p.csv}\n\n{exc}", parent=self.root)
                raise
        return out

    def reload(self) -> None:
        self.cache.clear()
        self._dims_signature = ()
        self.apply()

    def add_csvs(self, paths) -> None:
        added = False
        for p in paths:
            p = str(Path(p).expanduser().resolve())
            if any(f.csv == p for f in self.state.library):
                continue
            try:
                self.frame(p)
            except (OSError, ValueError) as exc:
                messagebox.showerror("Cannot read CSV", f"{p}\n\n{exc}", parent=self.root)
                continue
            self.state.library.append(m.Panel(p))
            self.last_dir = str(Path(p).parent)
            added = True
        if added:
            self._refresh_files()
            self._changed()
            if not self.auto.get():
                self.apply()          # a new file is worth drawing even without auto-apply

    def _refresh_files(self) -> None:
        sel = self._selected_file()
        self.tree.delete(*self.tree.get_children())
        plotted = self.state.panels
        for i, f in enumerate(self.state.library):
            title = self.state.panel_title(plotted.index(f)) if f in plotted else ""
            self.tree.insert("", "end", iid=str(i),
                             values=(TICK if f.plotted else UNTICK, Path(f.csv).name, title))
        if self.state.library:
            idx = sel if sel is not None and sel < len(self.state.library) else 0
            self.tree.selection_set(str(idx))
        self._loading = True
        try:
            self.fig_title.set(self.state.figure_title() if self.state.panels else "")
        finally:
            self._loading = False

    def _selected_file(self) -> int | None:
        sel = self.tree.selection()
        return int(sel[0]) if sel else None

    def _tree_click(self, event) -> None:
        """Clicking the tick column toggles; anywhere else selects, as usual."""
        if self.tree.identify_region(event.x, event.y) != "cell":
            return
        if self.tree.identify_column(event.x) != "#1":
            return
        row = self.tree.identify_row(event.y)
        if row:
            self._toggle_plotted(int(row))

    def _tree_double(self, event) -> None:
        if self.tree.identify_column(event.x) != "#1":
            self.only_selected()

    def _toggle_plotted(self, index: int | None) -> None:
        if index is None:
            return
        f = self.state.library[index]
        if f.plotted and len(self.state.panels) == 1:
            return                      # never end up with nothing to draw
        f.plotted = not f.plotted
        self._refresh_files()
        self._changed()

    def only_selected(self) -> None:
        i = self._selected_file()
        if i is None:
            return
        for j, f in enumerate(self.state.library):
            f.plotted = (j == i)
        self._refresh_files()
        self._changed()

    def _on_file_select(self) -> None:
        i = self._selected_file()
        if i is None:
            return
        f = self.state.library[i]
        self._loading = True
        try:
            plotted = self.state.panels
            self.panel_title.set(self.state.panel_title(plotted.index(f)) if f in plotted
                                 else (f.title or ""))
        finally:
            self._loading = False
        df = self.cache.get(f.csv, (0, None))[1]
        if df is not None:
            dims = m.dimension_values([df])
            varying = ", ".join(f"{m.AXIS_NAMES.get(c, c).lower()} {len(v)}"
                                for c, v in dims.items() if len(v) > 1)
            metrics = ", ".join(x.name.lower() for x in m.available_metrics([df]))
            self.file_info.configure(
                text=f"{len(df)} rows · {df['Queue'].nunique()} queues · varies: "
                     f"{varying}\nmetrics: {metrics}")

    def remove_file(self) -> None:
        i = self._selected_file()
        if i is None:
            return
        self.cache.pop(self.state.library[i].csv, None)
        del self.state.library[i]
        self._refresh_files()
        self._changed()

    def move_file(self, step: int) -> None:
        i = self._selected_file()
        if i is None or not 0 <= i + step < len(self.state.library):
            return
        lib = self.state.library
        lib[i], lib[i + step] = lib[i + step], lib[i]
        self._refresh_files()
        self.tree.selection_set(str(i + step))
        self._changed()

    def _commit_fig_title(self) -> None:
        if not self._loading and self.state.panels:
            self._set("title", self.fig_title.get())

    def _commit_panel_title(self) -> None:
        i = self._selected_file()
        if self._loading or i is None:
            return
        self.state.library[i].title = self.panel_title.get()
        self.tree.set(str(i), "title", self.panel_title.get())
        self._changed()

    def _reset_title(self, which) -> None:
        if which is None:
            self.state.title = None
        else:
            i = self._selected_file()
            if i is None:
                return
            self.state.library[i].title = None
        self._refresh_files()
        self._on_file_select()
        self._changed()

    # -- rendering -----------------------------------------------------------------------

    def _reconcile(self, frames) -> None:
        """Keep the state valid for the data now plotted, and rebuild what depends on it."""
        s = self.state
        dims = m.dimension_values(frames)
        signature = tuple((c, tuple(v)) for c, v in dims.items())
        if signature != self._dims_signature:
            self.links = m.correlations(frames)
        defaults = m.default_filters(dims, self.links)
        for col, values in dims.items():
            chosen = s.filters.get(col)
            if chosen is None:
                s.filters[col] = defaults.get(col, list(values))
            else:
                kept = [v for v in chosen if v in values]
                s.filters[col] = kept if kept else defaults.get(col, list(values))
        if signature != self._dims_signature:
            for col in ("Producers", "Consumers"):
                if col in dims and self._dims_signature:
                    old = dict(self._dims_signature).get(col, ())
                    if set(s.filters.get(col, [])) >= set(old):
                        s.filters[col] = list(dims[col])
            self._dims_signature = signature
            self._rebuild_filters(dims)

        metrics = {x.key: x.name for x in m.available_metrics(frames)}
        self.metric.set_options(metrics)
        if metrics and s.metric not in metrics:
            s.metric = next(iter(metrics))
        self.metric.value = s.metric
        xs = {c: m.AXIS_NAMES.get(c, c) for c in m.x_candidates(frames)}
        self.xaxis.set_options(xs)
        if xs and s.x not in xs:
            s.x = "Total_Threads" if "Total_Threads" in xs else next(iter(xs))
        self.xaxis.value = s.effective_x
        self.xaxis.configure(state="disabled" if s.kind == "speedup" else "readonly")
        errors_ok = s.metric == "throughput" and s.kind != "speedup"
        self.errorbars.configure(state="readonly" if errors_ok else "disabled")

    def apply(self) -> None:
        """Hand the current state to the render thread. Returns immediately."""
        if not self.state.panels:
            self.view.message("Add a results CSV to start  (Ctrl+O)")
            self._requested_at = self._edits
            self._show_status()
            return
        try:
            frames = self.frames()
        except (OSError, ValueError):
            return
        self._reconcile(frames)
        w, h = self.view.size
        if w < 50 or h < 50:
            return                      # not laid out yet; the first <Configure> will call back
        dpi = self.root.winfo_fpixels("1i")
        # A copy, because the worker reads it while the window keeps editing the original.
        state = copy.deepcopy(self.state)
        # Kept so a pan or a zoom can be requested without walking the widgets again: the
        # plot is unchanged, only the region being looked at.
        self._sent = (state, frames)
        self._request_seq = self.service.request_preview("main", state, frames, w, h, dpi,
                                                         self.view.viewport)
        self._requested_at = self._edits
        for view_id, (win, view) in list(self._windows.items()):
            if not win.winfo_exists():
                self._windows.pop(view_id)
                self.service.forget(view_id)
                continue
            vw, vh = view.size
            if vw > 50 and vh > 50:
                self.service.request_preview(view_id, state, frames, vw, vh, dpi,
                                             view.viewport)
        self._busy_since = self._busy_since or time.perf_counter()
        self._zoom_label()
        self._show_status()

    def refresh_viewport(self, view_id: str = "main") -> None:
        """Redraw one view after a pan or zoom, reusing the state the worker already has.

        No deep copy, no widget reconciliation and no pandas: the figure is identical and
        only the crop moved, which is the whole reason a drag stays smooth.
        """
        view = self.view if view_id == "main" else (self._windows.get(view_id, (None, None))[1])
        if view is None:
            return
        if self._sent is None:
            self.apply()
            return
        w, h = view.size
        if w < 50 or h < 50:
            return
        state, frames = self._sent
        seq = self.service.request_preview(view_id, state, frames, w, h,
                                           self.root.winfo_fpixels("1i"), view.viewport)
        if view_id == "main":
            self._request_seq = seq
            self._zoom_label()
        self._busy_since = self._busy_since or time.perf_counter()
        self._show_status()

    def _zoom_label(self) -> None:
        self.zoom_var.set(f"{round(self.view.viewport.zoom * 100)}%")

    def _poll(self) -> None:
        """Collect finished work. The only place a result touches Tk."""
        for result in self.service.drain():
            if isinstance(result, SaveResult):
                self._saved(result)
            else:
                self._preview_done(result)
        if self.service.busy:
            # Only once it is actually slow: a crop finishes in about ten milliseconds, and a
            # bar that flashes on every pan step is noise, not information.
            slow = time.perf_counter() - (self._busy_since or time.perf_counter()) > 0.15
            if slow and not self.progress.winfo_ismapped():
                self.progress.pack(side="right", padx=4)
                self.progress.start(12)
            self._show_status()
        elif self._busy_since is not None:
            if self.progress.winfo_ismapped():
                self.progress.stop()
                self.progress.pack_forget()
            self._busy_since = None
            self._show_status()
        self.root.after(_POLL_MS, self._poll)

    def _preview_done(self, result: PreviewResult) -> None:
        if result.view != "main":
            pair = self._windows.get(result.view)
            if pair and pair[0].winfo_exists() and result.ppm:
                pair[1].show(result.ppm, result.facecolor)
            return
        if result.seq != self._request_seq:
            return                      # superseded while it was drawing
        self._last_ms, self._last_path = result.ms, result.path
        if result.error:
            self._show_status(error=result.error)
            return
        self.rendered = result.rendered
        self.view.show(result.ppm, result.facecolor)
        keys = self.rendered.built.keys()
        if keys != self._series_keys:
            self._series_keys = keys
            self._rebuild_series()
        else:
            self._refresh_swatches()
        self._show_notes(self.rendered.notes, self.rendered.clashes)
        self._show_status()

    def _show_status(self, error: str | None = None) -> None:
        pending = self._edits != self._requested_at
        if error:
            text = f"✖ {error[:40]}"
        elif self.service.busy:
            elapsed = time.perf_counter() - (self._busy_since or time.perf_counter())
            text = f"◐ Drawing… {elapsed:0.1f} s"
        elif pending:
            text = "▲ Changes pending — press Apply"
        elif getattr(self, "_last_ms", None) is not None:
            text = f"● Ready · {self._last_ms} ms"
            suffix = {"restyle": " (restyle)", "crop": " (crop)"}.get(self._last_path, "")
            text += suffix
        else:
            text = "● Ready"
        self.status_var.set(text)
        self.apply_button.state(["!disabled"] if pending else ["disabled"])

    def _show_notes(self, notes, clashes) -> None:
        self.notes.configure(state="normal")
        self.notes.delete("1.0", "end")
        for c in clashes:
            self.notes.insert("end", f"{'✖' if c.severity == 'fail' else '⚠'} "
                                     f"{c.message}\n", c.severity)
        for n in notes:
            self.notes.insert("end", f"ℹ {n}\n", "note")
        self.notes.configure(state="disabled")

    # -- files ---------------------------------------------------------------------------

    def ask_add_csv(self) -> None:
        paths = filedialog.askopenfilenames(parent=self.root, initialdir=self.last_dir,
                                            title="Add results CSV",
                                            filetypes=[("CSV", "*.csv"), ("All files", "*")])
        if paths:
            self.add_csvs(paths)

    def ask_save_session(self) -> None:
        path = filedialog.asksaveasfilename(parent=self.root, initialdir=self.last_dir,
                                            defaultextension=".json", title="Save session",
                                            filetypes=[("Plot session", "*.json")])
        if path:
            Path(path).write_text(self.state.to_json())
            self.last_dir = str(Path(path).parent)

    def ask_load_session(self) -> None:
        path = filedialog.askopenfilename(parent=self.root, initialdir=self.last_dir,
                                          title="Open session",
                                          filetypes=[("Plot session", "*.json")])
        if path:
            self.load_session(path)

    def load_session(self, path: str) -> None:
        try:
            self.state = m.PlotState.from_json(Path(path).read_text())
        except (OSError, ValueError, TypeError) as exc:
            messagebox.showerror("Cannot open session", f"{path}\n\n{exc}", parent=self.root)
            return
        self.last_dir = str(Path(path).parent)
        self._dims_signature = ()
        self._series_keys = []
        self._sync_widgets()
        self._refresh_files()
        self.apply()

    def ask_export_data(self) -> None:
        if not self.rendered or not self.rendered.records:
            messagebox.showinfo("Nothing to export", "Draw something first.", parent=self.root)
            return
        path = filedialog.asksaveasfilename(parent=self.root, initialdir=self.last_dir,
                                            initialfile=self._slug() + ".csv",
                                            defaultextension=".csv",
                                            title="Export plotted values",
                                            filetypes=[("CSV", "*.csv")])
        if path:
            m.export_table(self.rendered, path)

    def _slug(self) -> str:
        base = self.state.figure_title() or (Path(self.state.panels[0].csv).stem
                                             if self.state.panels else "figure")
        return re.sub(r"[^A-Za-z0-9._-]+", "_", base).strip("_") or "figure"

    def ask_save_figure(self) -> None:
        if not self.state.panels:
            messagebox.showinfo("Nothing to save", "Add a CSV first.", parent=self.root)
            return
        SaveDialog(self)

    def _saved(self, result: SaveResult) -> None:
        if result.error:
            messagebox.showerror("Could not save", result.error, parent=self.root)
            return
        extra = f" (+ {result.table.name})" if result.table else ""
        self._show_notes([f"saved {result.path}{extra} in {result.ms} ms"], [])

    def show_window(self) -> None:
        """The same figure in its own window, kept up to date with every edit."""
        if not self.state.panels:
            return
        self._window_seq += 1
        view_id = f"window-{self._window_seq}"
        win = tk.Toplevel(self.root)
        win.title(self.state.figure_title() or "plot")
        dpi = self.root.winfo_fpixels("1i")
        win.geometry(f"{min(int(self.state.width * dpi), int(self.root.winfo_screenwidth() * 0.9))}x"
                     f"{min(int(self.state.height * dpi), int(self.root.winfo_screenheight() * 0.85))}")
        view = ImageView(win, self.apply, lambda: self.refresh_viewport(view_id))
        view.pack(fill="both", expand=True)
        self._windows[view_id] = (win, view)
        win.protocol("WM_DELETE_WINDOW", lambda: self._close_window(view_id))
        self.apply()

    def _close_window(self, view_id: str) -> None:
        win, _ = self._windows.pop(view_id, (None, None))
        self.service.forget(view_id)
        if win is not None:
            win.destroy()

    def _shortcuts(self) -> None:
        for seq, cmd in [("<Control-o>", self.ask_add_csv), ("<Control-s>", self.ask_save_figure),
                         ("<Control-e>", self.ask_export_data), ("<Control-r>", self.reload),
                         ("<Control-S>", self.ask_save_session), ("<Control-w>", self.show_window),
                         ("<Control-Return>", self.apply),
                         ("<Control-plus>", lambda: self.view.zoom_by(1.5)),
                         ("<Control-equal>", lambda: self.view.zoom_by(1.5)),
                         ("<Control-minus>", lambda: self.view.zoom_by(1 / 1.5)),
                         ("<Control-Key-0>", lambda: self.view.fit())]:
            self.root.bind(seq, lambda e, c=cmd: c())


class SaveDialog(tk.Toplevel):
    """Format, size and background, then the file chooser."""

    FORMATS = {"png": "PNG (raster)", "svg": "SVG (vector, editable text)", "pdf": "PDF (vector)"}

    def __init__(self, app: PlotApp):
        super().__init__(app.root)
        self.app = app
        self.title("Save figure")
        self.transient(app.root)
        self.resizable(False, False)
        body = ttk.Frame(self, padding=14)
        body.pack(fill="both", expand=True)
        s = app.state

        ttk.Label(body, text="Format", style="Heading.TLabel").grid(row=0, column=0, sticky="w")
        self.fmt = tk.StringVar(value="png")
        for i, (k, v) in enumerate(self.FORMATS.items()):
            ttk.Radiobutton(body, text=v, value=k, variable=self.fmt,
                            command=self._fmt_changed).grid(row=1 + i, column=0, columnspan=2,
                                                            sticky="w")

        ttk.Label(body, text="Size", style="Heading.TLabel").grid(row=4, column=0, sticky="w",
                                                                  pady=(10, 0))
        self.w = tk.StringVar(value=f"{s.width:g}")
        self.h = tk.StringVar(value=f"{s.height:g}")
        self.dpi = tk.StringVar(value=str(s.dpi))
        for r, (text, var) in enumerate([("Width (in)", self.w), ("Height (in)", self.h),
                                         ("DPI", self.dpi)]):
            ttk.Label(body, text=text).grid(row=5 + r, column=0, sticky="w")
            e = ttk.Entry(body, textvariable=var, width=8)
            e.grid(row=5 + r, column=1, sticky="w", pady=2)
            if var is self.dpi:
                self.dpi_entry = e
        self.pixels = ttk.Label(body, style="Muted.TLabel")
        self.pixels.grid(row=8, column=0, columnspan=2, sticky="w")
        for var in (self.w, self.h, self.dpi):
            var.trace_add("write", lambda *a: self._update_pixels())

        self.transparent = tk.BooleanVar()
        ttk.Checkbutton(body, text="Transparent background", variable=self.transparent).grid(
            row=9, column=0, columnspan=2, sticky="w", pady=(10, 0))
        self.with_data = tk.BooleanVar()
        ttk.Checkbutton(body, text="Also write the plotted values as CSV beside it",
                        variable=self.with_data).grid(row=10, column=0, columnspan=2, sticky="w")

        buttons = ttk.Frame(body)
        buttons.grid(row=11, column=0, columnspan=2, sticky="e", pady=(14, 0))
        ttk.Button(buttons, text="Cancel", command=self.destroy).pack(side="right")
        ttk.Button(buttons, text="Choose file & save…", style="Accent.TButton",
                   command=self._save).pack(side="right", padx=6)
        self._update_pixels()
        self.bind("<Escape>", lambda e: self.destroy())
        self.bind("<Return>", lambda e: self._save())
        self.wait_visibility()
        self.grab_set()

    def _fmt_changed(self) -> None:
        self.dpi_entry.configure(state="normal" if self.fmt.get() == "png" else "disabled")
        self._update_pixels()

    def _update_pixels(self) -> None:
        w, h, d = (_float_or_none(v.get()) for v in (self.w, self.h, self.dpi))
        if self.fmt.get() == "png" and w and h and d:
            self.pixels.configure(text=f"{int(w * d)} × {int(h * d)} px")
        else:
            self.pixels.configure(text="vector: size sets proportions and text scale")

    def _save(self) -> None:
        fmt = self.fmt.get()
        w, h, d = (_float_or_none(v.get()) for v in (self.w, self.h, self.dpi))
        if not (w and h and w > 0 and h > 0):
            messagebox.showerror("Invalid size", "Width and height must be positive numbers.",
                                 parent=self)
            return
        app = self.app
        path = filedialog.asksaveasfilename(
            parent=self, initialdir=app.last_dir, initialfile=f"{app._slug()}.{fmt}",
            defaultextension=f".{fmt}", title="Save figure",
            filetypes=[(self.FORMATS[fmt], f"*.{fmt}")])
        if not path:
            return
        app.state.width, app.state.height = w, h
        if d and d > 0:
            app.state.dpi = int(d)
        app._sync_widgets()
        app.last_dir = str(Path(path).parent)
        # Queued, not run here: saving re-renders at the export size, which is the same
        # half-second of work as a preview and has no business blocking the window.
        app.service.request_save(copy.deepcopy(app.state), app.frames(), path, fmt,
                                 transparent=self.transparent.get(),
                                 with_data=self.with_data.get())
        app._show_notes([f"saving {Path(path).name}…"], [])
        self.destroy()


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="mpmc-plot-ui",
                                     description="Interactive benchmark plotting.")
    parser.add_argument("csv", nargs="*", help="results CSVs to open")
    parser.add_argument("--session", help="a session saved from the app")
    args = parser.parse_args(argv)
    logging.basicConfig(level=logging.WARNING, format="%(levelname)s: %(message)s")
    root = tk.Tk()
    PlotApp(root, args.csv, args.session)
    root.mainloop()
    return 0


if __name__ == "__main__":
    sys.exit(main())
