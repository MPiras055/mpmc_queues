"""Loading and filtering benchmark result CSVs."""

from __future__ import annotations

import logging
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Iterable

import pandas as pd

logger = logging.getLogger(__name__)

__all__ = ["Filters", "load_results", "apply_filters", "REQUIRED_COLUMNS"]

REQUIRED_COLUMNS = [
    "Queue", "Producers", "Consumers", "Size", "Pinning",
    "ProdDelay_NS", "ConsDelay_NS", "Throughput_Mean",
]


@dataclass
class Filters:
    """Which subset of rows to plot. ``None`` means "do not filter on this"."""

    queues: str | list[str] | None = None
    size: int | list[int] | None = None
    pinning: bool | list[bool] | None = None
    prod_delay_ns: int | list[int] | None = None
    cons_delay_ns: int | list[int] | None = None


def _as_list(value: Any) -> list | None:
    if value is None:
        return None
    return list(value) if isinstance(value, (list, tuple, set)) else [value]


def load_results(csv_path: str | Path) -> pd.DataFrame:
    """Read a results CSV and prepare the derived columns plots rely on.

    Rows that failed to measure are dropped rather than silently plotted as zero. The
    older schema had no Status column, so absence is treated as success.
    """
    path = Path(csv_path)
    if not path.is_file():
        raise FileNotFoundError(f"results file not found: {path}")

    df = pd.read_csv(path)

    # The oldest results record delays as tick counts, later ones as nanoseconds. Accept
    # both so historical files still plot, but do not pretend the units match: a tick
    # count is machine-specific, so the delay column is only comparable within one file.
    for ns_col, ticks_col in (("ProdDelay_NS", "ProdDelay_Ticks"), ("ConsDelay_NS", "ConsDelay_Ticks")):
        if ns_col not in df.columns and ticks_col in df.columns:
            df[ns_col] = df[ticks_col]
            logger.warning(
                "%s: '%s' holds tick counts, not nanoseconds; delay values are not "
                "comparable with newer result files",
                path.name, ticks_col,
            )

    missing = [c for c in REQUIRED_COLUMNS if c not in df.columns]
    if missing:
        raise ValueError(f"{path}: missing expected column(s): {missing}")

    if "Status" in df.columns:
        df = df[df["Status"] == "OK"]

    # Older CSVs wrote the literal "FAILED" into the throughput column.
    df = df[pd.to_numeric(df["Throughput_Mean"], errors="coerce").notna()].copy()
    df["Throughput_Mean"] = df["Throughput_Mean"].astype(float)
    # Median/min/max arrived later than mean/stdev, so a CSV recorded before them has only the
    # mean. Coerce whichever are present; stat_column() decides which one a plot may use.
    for col in ("Throughput_Median", "Throughput_Min", "Throughput_Max"):
        if col in df.columns:
            df[col] = pd.to_numeric(df[col], errors="coerce")
    if "Throughput_StdDev" in df.columns:
        df["Throughput_StdDev"] = pd.to_numeric(
            df["Throughput_StdDev"], errors="coerce"
        ).fillna(0.0)
    else:
        df["Throughput_StdDev"] = 0.0

    df["Total_Threads"] = df["Producers"] + df["Consumers"]
    return df


def apply_filters(df: pd.DataFrame, filters: Filters) -> pd.DataFrame:
    """Narrow @p df. Returns a copy; never mutates the input."""
    out = df
    for column, wanted in (
        ("Queue", _as_list(filters.queues)),
        ("Size", _as_list(filters.size)),
        ("Pinning", _as_list(filters.pinning)),
        ("ProdDelay_NS", _as_list(filters.prod_delay_ns)),
        ("ConsDelay_NS", _as_list(filters.cons_delay_ns)),
    ):
        if wanted is not None:
            out = out[out[column].isin(wanted)]
    return out.copy()


def queues_in(df: pd.DataFrame) -> list[str]:
    """Implementation names present, in first-appearance order."""
    return list(dict.fromkeys(df["Queue"].tolist()))


def stat_column(df: pd.DataFrame, stat: str = "median") -> str:
    """Which throughput column a plot should read.

    `median` is the right estimator -- the runner records it for that reason, since a clock
    that ramps drags the mean -- but it only exists in CSVs written after it was added. Falling
    back keeps older result files plottable instead of raising at them.
    """
    wanted = {"median": "Throughput_Median", "mean": "Throughput_Mean"}[stat]
    if wanted in df.columns and df[wanted].notna().any():
        return wanted
    if wanted != "Throughput_Mean":
        logger.warning("%s is not in this CSV; falling back to Throughput_Mean", wanted)
    return "Throughput_Mean"


def scalability(
    df: pd.DataFrame, baseline_producers: int, stat: str = "median"
) -> Iterable[tuple[str, pd.DataFrame]]:
    """Yield (queue, frame) with a Scalability column relative to @p baseline_producers.

    **Normalised on producers, not on total threads.** Consumers add no production capacity, so
    counting them as scaling makes the ideal line unreachable by construction: at 64 total
    threads a balanced sweep has 32 producers and a 1:3 sweep has 16, and crediting both with 64
    asks the same speedup of two configurations that cannot deliver the same work.

    A queue with no measurement at the baseline is skipped, with its name reported by the
    caller -- normalising against a missing point would silently invent a speedup.
    """
    col = stat_column(df, stat)
    for name, group in df.groupby("Queue", sort=False):
        group = group.sort_values("Producers")
        base = group[group["Producers"] == baseline_producers][col]
        if base.empty or base.iloc[0] <= 0:
            continue
        group = group.copy()
        group["Scalability"] = group[col] / base.iloc[0]
        yield str(name), group
