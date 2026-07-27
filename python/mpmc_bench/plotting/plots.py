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
from .styles import style_for

logger = logging.getLogger("mpmc.plot")

__all__ = ["PlotConfig", "plot_throughput", "plot_scalability", "main"]


@dataclass
class PlotConfig:
    title: str = "Benchmark results"
    xlabel: str = "Total threads"
    ylabel: str = "Throughput (ops/sec)"
    scale: float = 1.0
    logx: bool = False
    show: bool = True
    save_path: str | Path | None = None


def _finalise(fig, ax, config: PlotConfig) -> None:
    ax.set_title(config.title)
    ax.set_xlabel(config.xlabel)
    ax.set_ylabel(config.ylabel)
    if config.logx:
        ax.set_xscale("log", base=2)
    ax.grid(True, alpha=0.3)
    ax.legend(fontsize="small")
    fig.tight_layout()

    if config.save_path:
        fig.savefig(config.save_path, dpi=150)
        logger.info("saved %s", config.save_path)
    if config.show:
        plt.show()
    else:
        plt.close(fig)


def plot_throughput(df, config: PlotConfig):
    """Throughput against total thread count, one line per implementation."""
    if df.empty:
        raise ValueError("nothing to plot: the filters matched no rows")

    fig, ax = plt.subplots(figsize=(9, 5.5))
    for name in dataio.queues_in(df):
        group = df[df["Queue"] == name].sort_values("Total_Threads")
        # Several rows can share a thread count (repeats, or sizes left unfiltered);
        # average them rather than drawing a zigzag between duplicate x values.
        agg = group.groupby("Total_Threads", as_index=False).agg(
            {"Throughput_Mean": "mean", "Throughput_StdDev": "mean"}
        )
        st = style_for(str(name))
        ax.errorbar(
            agg["Total_Threads"],
            agg["Throughput_Mean"] / config.scale,
            yerr=agg["Throughput_StdDev"] / config.scale,
            label=st.label, color=st.color, marker=st.marker, linestyle=st.linestyle,
            capsize=3, markersize=5, linewidth=1.5,
        )
    _finalise(fig, ax, config)
    return fig


def plot_scalability(df, config: PlotConfig, baseline_threads: int = 2):
    """Speedup relative to @p baseline_threads, against ideal linear scaling."""
    if df.empty:
        raise ValueError("nothing to plot: the filters matched no rows")

    fig, ax = plt.subplots(figsize=(9, 5.5))
    plotted, skipped = 0, []
    present = set(dataio.queues_in(df))

    for name, group in dataio.scalability(df, baseline_threads):
        st = style_for(name)
        ax.plot(
            group["Total_Threads"], group["Scalability"],
            label=st.label, color=st.color, marker=st.marker, linestyle=st.linestyle,
            markersize=5, linewidth=1.5,
        )
        plotted += 1
        present.discard(name)

    skipped = sorted(present)
    if skipped:
        # Say so rather than quietly omitting lines: without a baseline point there is no
        # honest way to normalise, and an absent line is easy to miss.
        logger.warning(
            "no %d-thread baseline for %s; omitted from the scalability plot",
            baseline_threads, ", ".join(skipped),
        )
    if not plotted:
        raise ValueError(
            f"no implementation has a {baseline_threads}-thread measurement to normalise against"
        )

    threads = sorted(df["Total_Threads"].unique())
    ax.plot(
        threads, [t / baseline_threads for t in threads],
        label="Ideal (linear)", color="#777777", linestyle="--", linewidth=1,
    )
    cfg = PlotConfig(**{**config.__dict__, "ylabel": f"Speedup vs {baseline_threads} threads"})
    _finalise(fig, ax, cfg)
    return fig


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="mpmc-plot", description="Plot benchmark results.")
    parser.add_argument("csv", help="results CSV produced by mpmc-run")
    parser.add_argument("--kind", choices=["throughput", "scalability"], default="throughput")
    parser.add_argument("--queues", nargs="*", help="implementations to include (default: all)")
    parser.add_argument("--size", type=int, nargs="*", help="queue capacities to include")
    parser.add_argument("--pin", dest="pin", action="store_true", default=None)
    parser.add_argument("--no-pin", dest="pin", action="store_false")
    parser.add_argument("--prod-delay", type=int, nargs="*", help="producer delays (ns)")
    parser.add_argument("--cons-delay", type=int, nargs="*", help="consumer delays (ns)")
    parser.add_argument("--baseline", type=int, default=2, help="threads to normalise scalability against")
    parser.add_argument("--scale", type=float, default=1e6)
    parser.add_argument("--ylabel", default="Millions of ops/sec")
    parser.add_argument("--title", default=None)
    parser.add_argument("--logx", action="store_true")
    parser.add_argument("--save", help="write the figure here instead of showing it")
    parser.add_argument("--list", action="store_true", help="list implementations in the CSV and exit")
    args = parser.parse_args(argv)

    logging.basicConfig(level=logging.INFO, format="%(levelname)s: %(message)s")

    try:
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

        cfg = PlotConfig(
            title=args.title or f"{Path(args.csv).stem} ({args.kind})",
            ylabel=args.ylabel, scale=args.scale, logx=args.logx,
            show=args.save is None, save_path=args.save,
        )
        if args.kind == "throughput":
            plot_throughput(df, cfg)
        else:
            plot_scalability(df, cfg, args.baseline)
        return 0

    except (FileNotFoundError, ValueError) as exc:
        logger.critical("%s", exc)
        return 1


if __name__ == "__main__":
    sys.exit(main())
