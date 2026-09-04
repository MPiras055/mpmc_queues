"""Experiment configuration: one explicit schema, parsed strictly.

Replaces ``benchmark_loader.py``. Two things are deliberately different.

**Nothing is guessed.** The old ``_normalize_delays`` inspected the shape of the input
to decide whether it had been handed one delay pair or a list of them, with comments
that openly reasoned in circles about the cases. Delays are now named objects, so there
is no shape to infer.

**Nothing fails quietly.** The old loader's dict branch never assigned
``raw_experiments`` from ``data["experiments"]``, so the documented
``{"experiments": [...]}` form parsed to *zero* runs and the runner then exited
successfully having measured nothing. Unknown keys are rejected too: a mistyped
``repetition`` silently meant "1" before.
"""

from __future__ import annotations

import json
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

__all__ = ["Delay", "DelayPair", "Experiment", "load", "ConfigError"]


class ConfigError(ValueError):
    """The configuration is malformed. The message says where and how."""


@dataclass(frozen=True)
class Delay:
    """Simulated per-operation work, in nanoseconds, plus a randomisation amplitude."""

    ns: int = 0
    amplitude: float = 0.0

    @staticmethod
    def parse(raw: Any, where: str) -> "Delay":
        if raw is None:
            return Delay()
        if isinstance(raw, dict):
            _reject_unknown(raw, {"ns", "amplitude"}, where)
            return Delay(int(raw.get("ns", 0)), float(raw.get("amplitude", 0.0)))
        # Accept the legacy [ns, amplitude] pair so old configs still load.
        if isinstance(raw, (list, tuple)) and len(raw) == 2:
            return Delay(int(raw[0]), float(raw[1]))
        raise ConfigError(
            f"{where}: expected {{'ns': .., 'amplitude': ..}} or [ns, amplitude], got {raw!r}"
        )


@dataclass(frozen=True)
class DelayPair:
    producer: Delay = field(default_factory=Delay)
    consumer: Delay = field(default_factory=Delay)

    @staticmethod
    def parse(raw: Any, where: str) -> "DelayPair":
        if raw is None:
            return DelayPair()
        if isinstance(raw, dict):
            _reject_unknown(raw, {"producer", "consumer"}, where)
            return DelayPair(
                Delay.parse(raw.get("producer"), f"{where}.producer"),
                Delay.parse(raw.get("consumer"), f"{where}.consumer"),
            )
        # Legacy: [[pns, pamp], [cns, camp]]
        if isinstance(raw, (list, tuple)) and len(raw) == 2:
            return DelayPair(
                Delay.parse(raw[0], f"{where}[0]"), Delay.parse(raw[1], f"{where}[1]")
            )
        raise ConfigError(f"{where}: expected a producer/consumer delay pair, got {raw!r}")

    @property
    def is_zero(self) -> bool:
        return self.producer.ns == 0 and self.consumer.ns == 0


_EXPERIMENT_KEYS = {
    "metrics",
    "executable",
    "name",
    "output_file",
    "repetitions",
    "queues",
    "items",
    "queue_sizes",
    "threads",
    "pinning",
    "delays",
    "timeout_s",
}


@dataclass
class Experiment:
    output_file: str
    queues: list[str]
    queue_sizes: list[int]
    threads: list[tuple[int, int]]
    items: int = 1_000_000
    repetitions: int = 1
    pinning: list[bool] = field(default_factory=lambda: [False])
    delays: list[DelayPair] = field(default_factory=lambda: [DelayPair()])
    name: str = ""
    timeout_s: float = 300.0
    #: Ask the benchmark for `key=value` output and record the counter columns. Off by default,
    #: because the instrumented entries put atomics on the link path -- a counter run and a
    #: throughput run must be separate passes.
    metrics: bool = False
    #: Which binary to sweep: "benchmark" (the 47 headline entries) or "mpmc_tune" (the
    #: instrumented and backoff variants). See registry::Tuning.
    executable: str = "benchmark"

    @property
    def grid_size(self) -> int:
        return (
            len(self.queues)
            * len(self.threads)
            * len(self.queue_sizes)
            * len(self.pinning)
            * len(self.delays)
        )

    @staticmethod
    def parse(raw: Any, where: str) -> "Experiment":
        if not isinstance(raw, dict):
            raise ConfigError(f"{where}: expected an object, got {type(raw).__name__}")
        _reject_unknown(raw, _EXPERIMENT_KEYS, where)

        for required in ("output_file", "queues", "queue_sizes", "threads"):
            if required not in raw:
                raise ConfigError(f"{where}: missing required key '{required}'")

        threads: list[tuple[int, int]] = []
        for i, t in enumerate(raw["threads"]):
            if not isinstance(t, (list, tuple)) or len(t) != 2:
                raise ConfigError(f"{where}.threads[{i}]: expected [producers, consumers]")
            p, c = int(t[0]), int(t[1])
            if p < 1 or c < 1:
                raise ConfigError(
                    f"{where}.threads[{i}]: need at least one producer and one consumer, got {p}P/{c}C"
                )
            threads.append((p, c))

        queues = list(raw["queues"])
        if not queues:
            raise ConfigError(f"{where}.queues: must name at least one implementation")

        sizes = [int(s) for s in raw["queue_sizes"]]
        if any(s < 2 for s in sizes):
            raise ConfigError(f"{where}.queue_sizes: capacities must be >= 2")

        raw_delays = raw.get("delays")
        delays = (
            [DelayPair.parse(d, f"{where}.delays[{i}]") for i, d in enumerate(raw_delays)]
            if raw_delays
            else [DelayPair()]
        )

        return Experiment(
            output_file=str(raw["output_file"]),
            queues=queues,
            queue_sizes=sizes,
            threads=threads,
            items=int(raw.get("items", 1_000_000)),
            repetitions=int(raw.get("repetitions", 1)),
            pinning=[bool(p) for p in raw.get("pinning", [False])],
            delays=delays,
            name=str(raw.get("name", "")),
            timeout_s=float(raw.get("timeout_s", 300.0)),
            metrics=bool(raw.get("metrics", False)),
            executable=str(raw.get("executable", "benchmark")),
        )


def _reject_unknown(raw: dict, allowed: set[str], where: str) -> None:
    unknown = set(raw) - allowed
    if unknown:
        raise ConfigError(
            f"{where}: unknown key(s) {sorted(unknown)}; allowed: {sorted(allowed)}"
        )


def load(path: str | Path) -> list[Experiment]:
    """Parse a config file into experiments.

    Accepts either a bare list of experiment objects or
    ``{"experiments": [...]}``. Both forms are tested; the second used to parse to
    nothing at all.
    """
    p = Path(path)
    if not p.is_file():
        raise ConfigError(f"config file not found: {p}")

    try:
        data = json.loads(p.read_text())
    except json.JSONDecodeError as exc:
        raise ConfigError(f"{p}: invalid JSON: {exc}") from exc

    return loads(data, str(p))


def loads(data: Any, where: str = "<config>") -> list[Experiment]:
    """Parse already-decoded JSON. Split out so tests need no temp files."""
    if isinstance(data, list):
        raw = data
    elif isinstance(data, dict) and "experiments" in data:
        raw = data["experiments"]
        if not isinstance(raw, list):
            raise ConfigError(f"{where}.experiments: expected a list")
    elif isinstance(data, dict):
        raw = [data]  # a single experiment written directly
    else:
        raise ConfigError(f"{where}: expected a list or an object, got {type(data).__name__}")

    if not raw:
        raise ConfigError(f"{where}: contains no experiments")

    return [Experiment.parse(e, f"{where}[{i}]") for i, e in enumerate(raw)]
