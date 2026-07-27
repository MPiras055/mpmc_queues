"""Config parsing: the shapes accepted, and the failures that must be loud."""

from __future__ import annotations

import json

import pytest

from mpmc_bench import schema
from mpmc_bench.schema import ConfigError, Delay, DelayPair

MINIMAL = {
    "output_file": "out.csv",
    "queues": ["vyukov"],
    "queue_sizes": [1024],
    "threads": [[1, 1]],
}


class TestAcceptedShapes:
    def test_experiments_object_parses(self):
        """Regression: this form used to parse to *zero* experiments.

        The old loader's dict branch never assigned raw_experiments from
        data["experiments"], so the runner exited successfully having measured nothing.
        """
        runs = schema.loads({"experiments": [MINIMAL]})
        assert len(runs) == 1
        assert runs[0].queues == ["vyukov"]

    def test_bare_list_parses(self):
        assert len(schema.loads([MINIMAL, MINIMAL])) == 2

    def test_single_object_parses(self):
        assert len(schema.loads(MINIMAL)) == 1

    def test_load_from_file(self, tmp_path):
        p = tmp_path / "c.json"
        p.write_text(json.dumps({"experiments": [MINIMAL]}))
        assert len(schema.load(p)) == 1


class TestRejections:
    def test_unknown_key_rejected(self):
        """A mistyped key silently took its default before."""
        with pytest.raises(ConfigError, match="unknown key"):
            schema.loads({**MINIMAL, "repetition": 5})

    def test_missing_required_key(self):
        bad = {k: v for k, v in MINIMAL.items() if k != "queues"}
        with pytest.raises(ConfigError, match="queues"):
            schema.loads(bad)

    def test_empty_config_rejected(self):
        with pytest.raises(ConfigError, match="no experiments"):
            schema.loads([])

    def test_empty_queue_list_rejected(self):
        with pytest.raises(ConfigError, match="at least one"):
            schema.loads({**MINIMAL, "queues": []})

    @pytest.mark.parametrize("threads", [[[0, 1]], [[1, 0]], [[1]], [[1, 2, 3]]])
    def test_bad_thread_pairs_rejected(self, threads):
        with pytest.raises(ConfigError):
            schema.loads({**MINIMAL, "threads": threads})

    @pytest.mark.parametrize("size", [0, 1, -4])
    def test_capacity_below_two_rejected(self, size):
        with pytest.raises(ConfigError, match=">= 2"):
            schema.loads({**MINIMAL, "queue_sizes": [size]})

    def test_bad_json_names_the_file(self, tmp_path):
        p = tmp_path / "c.json"
        p.write_text("{not json")
        with pytest.raises(ConfigError, match="invalid JSON"):
            schema.load(p)

    def test_missing_file(self, tmp_path):
        with pytest.raises(ConfigError, match="not found"):
            schema.load(tmp_path / "nope.json")


class TestDelays:
    def test_explicit_form(self):
        d = DelayPair.parse(
            {"producer": {"ns": 100, "amplitude": 0.5}, "consumer": {"ns": 7}}, "x"
        )
        assert d.producer == Delay(100, 0.5)
        assert d.consumer == Delay(7, 0.0)
        assert not d.is_zero

    def test_legacy_pair_form_still_loads(self):
        """Old configs wrote [[pns, pamp], [cns, camp]]; they should still work."""
        d = DelayPair.parse([[100, 0.5], [200, 0.25]], "x")
        assert d.producer == Delay(100, 0.5)
        assert d.consumer == Delay(200, 0.25)

    def test_absent_delays_default_to_zero(self):
        (run,) = schema.loads(MINIMAL)
        assert len(run.delays) == 1 and run.delays[0].is_zero

    def test_malformed_delay_rejected(self):
        with pytest.raises(ConfigError):
            DelayPair.parse("fast", "x")

    def test_unknown_delay_key_rejected(self):
        with pytest.raises(ConfigError, match="unknown key"):
            DelayPair.parse({"producer": {"nanoseconds": 5}}, "x")


def test_grid_size_is_the_product():
    (run,) = schema.loads(
        {
            **MINIMAL,
            "queues": ["a", "b"],
            "queue_sizes": [64, 128, 256],
            "threads": [[1, 1], [2, 2]],
            "pinning": [True, False],
        }
    )
    assert run.grid_size == 2 * 3 * 2 * 2 * 1
