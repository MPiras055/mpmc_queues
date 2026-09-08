"""Throughput and scalability plots, driven from the command line.

Replaces the previous ``plot.py``, which was a script you edited in place to change which
queues or sizes were plotted.
"""

from __future__ import annotations

import argparse
import logging
import sys
from dataclasses import dataclass
from pathlib import Path

import matplotlib.pyplot as plt

from . import data as dataio
from . import theme as theming
from .styles import assign_styles
from .theme import LIGHT, Theme

logger = logging.getLogger("mpmc.plot")

__all__ = [
    "PlotConfig", "plot_throughput", "plot_scalability",
    "plot_slot_efficiency", "plot_segments_per_item", "plot_backoff_grid", "main",
]


@dataclass
class PlotConfig:
    title: str = "Benchmark results"
    xlabel: str = "Total threads"
    ylabel: str = "Throughput (ops/sec)"
    scale: float = 1.0
    logx: bool = False
    show: bool = True
    save_path: str | Path | None = None
    theme: Theme = LIGHT
    #: Where to write the plotted values as CSV. On the light theme three palette slots
    #: measure below 3:1 against the surface, and the relief rule for that answers with
    #: visible labels *or* a table -- so this is the relief, not a nicety.
    table_path: str | Path | None = None


def write_table(config: PlotConfig, records: list[dict]) -> Path | None:
    """Write the values a chart actually drew, using its own axis names as headers."""
    if not config.table_path or not records:
        return None
    import pandas as pd

    frame = pd.DataFrame(records).rename(columns={"X": config.xlabel, "Y": config.ylabel})
    path = Path(config.table_path)
    frame.to_csv(path, index=False)
    logger.info("saved %s", path)
    return path


def _legend(ax_or_fig, theme: Theme, handles=None, labels=None, **kwargs):
    """One legend, in text ink.

    A lone series needs no legend box -- the title already names it -- but two or more
    always do, because identity must never rest on colour alone.
    """
    if handles is None:
        handles, labels = ax_or_fig.get_legend_handles_labels()
    if len(handles) < 2:
        return None
    leg = ax_or_fig.legend(handles, labels, **kwargs)
    for text in leg.get_texts():
        text.set_color(theme.text_secondary)
    return leg


def _finalise(fig, ax, config: PlotConfig, records: list[dict] | None = None) -> None:
    ax.set_title(config.title)
    ax.set_xlabel(config.xlabel)
    ax.set_ylabel(config.ylabel)
    if config.logx:
        ax.set_xscale("log", base=2)
    theming.style_axes(ax, config.theme)
    _legend(ax, config.theme)
    fig.tight_layout()

    write_table(config, records or [])
    if config.save_path:
        fig.savefig(config.save_path, dpi=150, facecolor=config.theme.surface)
        logger.info("saved %s", config.save_path)
    if config.show:
        plt.show()
    else:
        plt.close(fig)


def plot_throughput(df, config: PlotConfig, stat: str = "median"):
    """Throughput against total thread count, one line per implementation.

    Total threads here, not producers: this chart is about offered load and the contention it
    creates, which every thread contributes to. Only plot_scalability normalises on producers.
    """
    if df.empty:
        raise ValueError("nothing to plot: the filters matched no rows")

    col = dataio.stat_column(df, stat)
    names = dataio.queues_in(df)
    styles = assign_styles(names, config.theme)
    records: list[dict] = []
    fig, ax = plt.subplots(figsize=(9, 5.5))
    for name in names:
        group = df[df["Queue"] == name].sort_values("Total_Threads")
        # Several rows can share a thread count (repeats, or sizes left unfiltered);
        # average them rather than drawing a zigzag between duplicate x values.
        agg = group.groupby("Total_Threads", as_index=False).agg(
            {col: "mean", "Throughput_StdDev": "mean"}
        )
        st = styles[str(name)]
        ax.errorbar(
            agg["Total_Threads"],
            agg[col] / config.scale,
            yerr=agg["Throughput_StdDev"] / config.scale,
            label=st.label, color=st.color, marker=st.marker, linestyle=st.linestyle,
            capsize=3, markersize=5, linewidth=1.5,
        )
        records += [
            {"Series": st.label, "X": x, "Y": y, "YErr": e}
            for x, y, e in zip(agg["Total_Threads"], agg[col] / config.scale,
                               agg["Throughput_StdDev"] / config.scale)
        ]
    _finalise(fig, ax, config, records)
    return fig


def plot_scalability(df, config: PlotConfig, baseline_producers: int | None = None,
                     stat: str = "median"):
    """Speedup relative to @p baseline_producers, against ideal linear scaling.

    Scaled on **producers**, not on total threads: consumers do not add production capacity, so
    an axis that counts them asks for speedup the configuration cannot deliver. See
    dataio.scalability.
    """
    if df.empty:
        raise ValueError("nothing to plot: the filters matched no rows")

    if baseline_producers is None:
        # The smallest producer count actually measured. A fixed default cannot work across
        # sweeps -- [1,1] starts at 1 producer, [3,1] at 3 -- and erroring by default only
        # trains people to pass a flag without thinking about it.
        baseline_producers = int(df["Producers"].min())
        logger.info("normalising against %d producer(s), the smallest in this data",
                    baseline_producers)

    fig, ax = plt.subplots(figsize=(9, 5.5))
    styles = assign_styles(dataio.queues_in(df), config.theme)
    records: list[dict] = []
    plotted, skipped = 0, []
    present = set(dataio.queues_in(df))

    for name, group in dataio.scalability(df, baseline_producers, stat):
        st = styles[name]
        ax.plot(
            group["Producers"], group["Scalability"],
            label=st.label, color=st.color, marker=st.marker, linestyle=st.linestyle,
            markersize=5, linewidth=1.5,
        )
        records += [
            {"Series": st.label, "X": x, "Y": y}
            for x, y in zip(group["Producers"], group["Scalability"])
        ]
        plotted += 1
        present.discard(name)

    skipped = sorted(present)
    if skipped:
        # Say so rather than quietly omitting lines: without a baseline point there is no
        # honest way to normalise, and an absent line is easy to miss.
        logger.warning(
            "no %d-producer baseline for %s; omitted from the scalability plot",
            baseline_producers, ", ".join(skipped),
        )
    if not plotted:
        raise ValueError(
            f"no implementation has a {baseline_producers}-producer measurement to normalise "
            f"against (--baseline counts producers, not threads)"
        )

    producers = sorted(df["Producers"].unique())
    # A reference line, not a series: it takes the muted ink so it never reads as one more
    # implementation, and so it never eats a palette slot.
    ax.plot(
        producers, [p / baseline_producers for p in producers],
        label="Ideal (linear)", color=config.theme.text_muted, linestyle="--", linewidth=1,
    )
    cfg = PlotConfig(**{**config.__dict__,
                        "xlabel": "Producers",
                        "ylabel": f"Speedup vs {baseline_producers} producer(s)"})
    _finalise(fig, ax, cfg, records)
    return fig


def _require(df, columns: list[str], what: str):
    """Metrics columns only exist when the sweep ran with `metrics: true`."""
    missing = [c for c in columns if c not in df.columns]
    if missing:
        raise ValueError(
            f"{what} needs {', '.join(missing)}, which this CSV does not have. "
            "Re-run the experiment with \"metrics\": true (it drives the benchmark's "
            "--metrics mode)."
        )
    if df.empty:
        raise ValueError("nothing to plot: the filters matched no rows")


def plot_slot_efficiency(df, config: PlotConfig):
    """Slot efficiency against thread count, one line per implementation.

    eta = i / (S*n): the fraction of provisioned cells that actually carried an item. This is
    the headline for the HybridQueue claim -- FAAArray without backoff should collapse while HQ
    stays flat, because HQ's slowDequeue stops consumers invalidating cells a producer is still
    working on.
    """
    _require(df, ["SlotEfficiency", "Total_Threads"], "plot_slot_efficiency")

    fig, ax = plt.subplots(figsize=(9, 5.5))
    styles = assign_styles(dataio.queues_in(df), config.theme)
    records: list[dict] = []
    for name in dataio.queues_in(df):
        group = df[df["Queue"] == name].dropna(subset=["SlotEfficiency"])
        if group.empty:
            continue
        agg = group.groupby("Total_Threads", as_index=False).agg({"SlotEfficiency": "median"})
        st = styles[str(name)]
        ax.plot(agg["Total_Threads"], agg["SlotEfficiency"], label=st.label, color=st.color,
                marker=st.marker, linestyle=st.linestyle, markersize=5, linewidth=1.5)
        records += [{"Series": st.label, "X": x, "Y": y}
                    for x, y in zip(agg["Total_Threads"], agg["SlotEfficiency"])]
    ax.set_ylim(0, 1.05)
    _finalise(fig, ax, config, records)
    return fig


def plot_segments_per_item(df, config: PlotConfig):
    """S/i -- allocation pressure. PRQ and an untuned FAAArray should stand out."""
    _require(df, ["Segments", "Produced", "Total_Threads"], "plot_segments_per_item")

    fig, ax = plt.subplots(figsize=(9, 5.5))
    styles = assign_styles(dataio.queues_in(df), config.theme)
    records: list[dict] = []
    for name in dataio.queues_in(df):
        group = df[df["Queue"] == name].dropna(subset=["Segments", "Produced"]).copy()
        if group.empty:
            continue
        group["SegPerItem"] = group["Segments"] / group["Produced"]
        agg = group.groupby("Total_Threads", as_index=False).agg({"SegPerItem": "median"})
        st = styles[str(name)]
        ax.plot(agg["Total_Threads"], agg["SegPerItem"], label=st.label, color=st.color,
                marker=st.marker, linestyle=st.linestyle, markersize=5, linewidth=1.5)
        records += [{"Series": st.label, "X": x, "Y": y}
                    for x, y in zip(agg["Total_Threads"], agg["SegPerItem"])]
    ax.set_yscale("log")
    _finalise(fig, ax, config, records)
    return fig


def plot_backoff_grid(df, config: PlotConfig, value: str = "Throughput_Median"):
    """Heatmap over patience x thread count, for the backoff grid-search.

    Queue names carry the value (`u-faa-p1024`), so the patience axis is recovered from the
    name rather than needing a column. Rows are sorted numerically, not lexically, or 1024
    sorts before 16.
    """
    _require(df, [value, "Total_Threads"], "plot_backoff_grid")

    rows = []
    for name in dataio.queues_in(df):
        base, _, suffix = str(name).rpartition("-p")
        if not suffix.isdigit():
            continue      # not a backoff variant; skip rather than guess
        group = df[df["Queue"] == name]
        for threads, sub in group.groupby("Total_Threads"):
            rows.append((base, int(suffix), int(threads), sub[value].median()))
    if not rows:
        raise ValueError(
            "no backoff variants found. Names must end in '-p<N>', which the "
            "registry::Tuning entries do (u-faa-p0, u-hq-p1024, ...)."
        )

    families = sorted({r[0] for r in rows})
    fig, axes = plt.subplots(1, len(families), figsize=(6 * len(families), 4.5), squeeze=False)
    fig.set_facecolor(config.theme.surface)
    for ax, fam in zip(axes[0], families):
        sub = [r for r in rows if r[0] == fam]
        patiences = sorted({r[1] for r in sub})
        threads = sorted({r[2] for r in sub})
        grid = [[next((r[3] for r in sub if r[1] == p and r[2] == t), float("nan"))
                 for t in threads] for p in patiences]
        im = ax.imshow(grid, aspect="auto", origin="lower", cmap="viridis")
        ax.set_xticks(range(len(threads)), [str(t) for t in threads])
        ax.set_yticks(range(len(patiences)), [str(p) for p in patiences])
        ax.set_xlabel("Total threads")
        ax.set_ylabel("patience")
        ax.set_title(fam)
        # A heatmap is a continuous field; a grid drawn over the cells is only noise.
        ax.set_facecolor(config.theme.surface)
        ax.grid(False)
        ax.tick_params(colors=config.theme.axis, labelcolor=config.theme.text_muted)
        ax.xaxis.label.set_color(config.theme.text_secondary)
        ax.yaxis.label.set_color(config.theme.text_secondary)
        ax.title.set_color(config.theme.text_primary)
        bar = fig.colorbar(im, ax=ax, label=value)
        bar.ax.yaxis.label.set_color(config.theme.text_secondary)
        bar.ax.tick_params(colors=config.theme.axis, labelcolor=config.theme.text_muted)
    fig.suptitle(config.title, color=config.theme.text_primary)
    fig.tight_layout()
    write_table(config, [{"Series": r[0], "Patience": r[1], "X": r[2], "Y": r[3]}
                         for r in rows])
    if config.save_path:
        fig.savefig(config.save_path, dpi=150, facecolor=config.theme.surface)
        logger.info("saved %s", config.save_path)
    if config.show:
        plt.show()
    else:
        plt.close(fig)
    return fig


#: Per-kind y scaling and axis name. compare.py reads this too, so the panels are labelled
#: exactly like the single-chart version of the same kind.
DEFAULTS = {
    "throughput": (1e6, "Millions of ops/sec"),
    "scalability": (1.0, "Speedup vs baseline"),
    "slot-efficiency": (1.0, "Slot efficiency  i / (S*n)"),
    "segments-per-item": (1.0, "Segments per item  S / i"),
    "backoff-grid": (1.0, "Throughput (median)"),
}


def _table_path_for(requested: str | None, save_path: str | None) -> Path | None:
    """Resolve ``--table [PATH]``: explicit path, else beside ``--save``."""
    if requested is None:
        return None
    if requested:
        return Path(requested)
    if not save_path:
        raise ValueError("--table needs a path, or a --save to sit beside")
    return Path(save_path).with_suffix(".csv")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="mpmc-plot", description="Plot benchmark results.")
    parser.add_argument("csv", nargs="?", help="results CSV produced by mpmc-run")
    parser.add_argument(
        "--kind",
        choices=["throughput", "scalability", "slot-efficiency", "segments-per-item",
                 "backoff-grid"],
        default="throughput",
        help="the last three need a CSV produced with \"metrics\": true",
    )
    parser.add_argument("--queues", nargs="*", help="implementations to include (default: all)")
    parser.add_argument("--size", type=int, nargs="*", help="queue capacities to include")
    parser.add_argument("--pin", dest="pin", action="store_true", default=None)
    parser.add_argument("--no-pin", dest="pin", action="store_false")
    parser.add_argument("--prod-delay", type=int, nargs="*", help="producer delays (ns)")
    parser.add_argument("--cons-delay", type=int, nargs="*", help="consumer delays (ns)")
    parser.add_argument("--baseline", type=int, default=None,
                        help="PRODUCERS to normalise scalability against "
                             "(default: the smallest present). Counts producers, not threads: "
                             "consumers add no production capacity")
    parser.add_argument("--stat", choices=["median", "mean"], default="median",
                        help="throughput estimator (default: median; the mean is dragged by "
                             "clock-ramp outliers)")
    parser.add_argument("--scale", type=float, default=None,
                        help="divide the y values (default: 1e6 for throughput, else 1)")
    parser.add_argument("--ylabel", default=None, help="default depends on --kind")
    parser.add_argument("--title", default=None)
    parser.add_argument("--logx", action="store_true")
    parser.add_argument("--theme", choices=sorted(theming.THEMES), default=None,
                        help=f"colour theme (default: light, or ${theming.ENV_VAR})")
    parser.add_argument("--label", action="append", metavar="NAME=LABEL",
                        help="rename one series in the legend; repeatable")
    parser.add_argument("--labels", metavar="FILE",
                        help="JSON object of {queue name: legend label}")
    parser.add_argument("--table", nargs="?", const="", default=None, metavar="FILE",
                        help="also write the plotted values as CSV (default: beside --save). "
                             "On the light theme this is the relief the sub-3:1 palette "
                             "slots oblige, not an extra")
    parser.add_argument("--compare", metavar="SPEC.json",
                        help="render side-by-side panels sharing one y-axis; see compare.py")
    parser.add_argument("--save", help="write the figure here instead of showing it")
    parser.add_argument("--list", action="store_true", help="list implementations in the CSV and exit")
    args = parser.parse_args(argv)

    logging.basicConfig(level=logging.INFO, format="%(levelname)s: %(message)s")

    try:
        from .styles import load_labels, parse_label_assignments, set_labels

        active = theming.apply(theming.resolve(args.theme))
        set_labels(load_labels(args.labels) if args.labels else None)
        set_labels(parse_label_assignments(args.label))
        table_path = _table_path_for(args.table, args.save)

        if args.compare:
            from .compare import load_spec, plot_compare

            spec = load_spec(args.compare, base_csv=args.csv)
            scale, ylabel = DEFAULTS[spec.kind]
            cfg = PlotConfig(
                title=args.title or spec.title,
                ylabel=args.ylabel or ylabel,
                scale=args.scale if args.scale is not None else scale,
                logx=args.logx, theme=active,
                show=args.save is None, save_path=args.save, table_path=table_path,
            )
            plot_compare(spec, cfg, args.stat)
            return 0

        if not args.csv:
            parser.error("a results CSV is required (or --compare with one in the spec)")

        df = dataio.load_results(args.csv)
        if args.list:
            for name in dataio.queues_in(df):
                print(name)
            return 0

        df = dataio.apply_filters(
            df,
            dataio.Filters(
                queues=args.queues, size=args.size, pinning=args.pin,
                prod_delay_ns=args.prod_delay, cons_delay_ns=args.cons_delay,
            ),
        )

        scale, ylabel = DEFAULTS[args.kind]
        cfg = PlotConfig(
            title=args.title or f"{Path(args.csv).stem} ({args.kind})",
            ylabel=args.ylabel or ylabel,
            scale=args.scale if args.scale is not None else scale,
            logx=args.logx,
            theme=active,
            show=args.save is None, save_path=args.save, table_path=table_path,
        )
        kinds = {
            "throughput": lambda: plot_throughput(df, cfg, args.stat),
            "scalability": lambda: plot_scalability(df, cfg, args.baseline, args.stat),
            "slot-efficiency": lambda: plot_slot_efficiency(df, cfg),
            "segments-per-item": lambda: plot_segments_per_item(df, cfg),
            "backoff-grid": lambda: plot_backoff_grid(
                df, cfg, dataio.stat_column(df, args.stat)),
        }
        kinds[args.kind]()
        return 0

    except (FileNotFoundError, ValueError) as exc:
        logger.critical("%s", exc)
        return 1


if __name__ == "__main__":
    sys.exit(main())
