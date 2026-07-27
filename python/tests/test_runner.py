"""The execution engine: command construction and how failures are recorded."""

from __future__ import annotations

import csv

import pytest

from mpmc_bench import runner, schema
from mpmc_bench.discovery import UnknownQueue
from mpmc_bench.runner import Point, _command
from mpmc_bench.schema import Delay, DelayPair
from mpmc_bench.ticks import TickConverter


def _point(**kw):
    base = dict(
        queue="vyukov", producers=2, consumers=2, size=1024,
        pinning=False, delays=DelayPair(), samples=[],
    )
    base.update(kw)
    return Point(**base)


class TestCommandConstruction:
    def test_queue_name_is_the_first_argument(self, fake_benchmark):
        """It used to be an integer from QUEUE_MAP; the binary now wants the name."""
        cmd = _command(fake_benchmark, _point(), items=1000, p_ticks=0, c_ticks=0)
        assert cmd[1] == "vyukov"
        assert cmd[2:6] == ["2", "2", "1000", "1024"]

    def test_pin_only_when_requested(self, fake_benchmark):
        assert "pin" not in _command(fake_benchmark, _point(), 10, 0, 0)
        assert "pin" in _command(fake_benchmark, _point(pinning=True), 10, 0, 0)

    def test_delay_args_omitted_when_zero(self, fake_benchmark):
        assert len(_command(fake_benchmark, _point(), 10, 0, 0)) == 6

    def test_delay_args_appended_when_nonzero(self, fake_benchmark):
        pt = _point(delays=DelayPair(Delay(100, 0.5), Delay(200, 0.25)))
        cmd = _command(fake_benchmark, pt, 10, 300, 600)
        assert cmd[-4:] == ["300", "0.5", "600", "0.25"]

    def test_pin_precedes_delays(self, fake_benchmark):
        pt = _point(pinning=True, delays=DelayPair(Delay(1), Delay(1)))
        cmd = _command(fake_benchmark, pt, 10, 3, 3)
        assert cmd.index("pin") < len(cmd) - 4


def _run(tmp_path, exe, ticks_exe, queues=("vyukov",), reps=1, timeout=30.0):
    (exp,) = schema.loads(
        {
            "output_file": "r.csv", "queues": list(queues), "queue_sizes": [1024],
            "threads": [[1, 1]], "items": 1000, "repetitions": reps, "timeout_s": timeout,
        }
    )
    out = runner.run_experiment(exp, exe, TickConverter(ticks_exe), tmp_path)
    with out.open() as fh:
        return list(csv.DictReader(fh))


class TestOutcomeRecording:
    def test_successful_run_records_samples(self, tmp_path, fake_benchmark, fake_timeticks):
        rows = _run(tmp_path, fake_benchmark, fake_timeticks, reps=3)
        assert len(rows) == 1
        assert rows[0]["Status"] == "OK"
        assert float(rows[0]["Throughput_Mean"]) == pytest.approx(1234567.5)
        assert rows[0]["Samples"] == "3"

    def test_nonzero_exit_records_the_reason(self, tmp_path, failing_benchmark, fake_timeticks):
        """Previously every failure mode collapsed into the single string 'FAILED'."""
        rows = _run(tmp_path, failing_benchmark, fake_timeticks)
        assert rows[0]["Status"].startswith("EXIT_3")
        assert rows[0]["Throughput_Mean"] == ""

    def test_unparseable_output_records_the_reason(self, tmp_path, garbage_benchmark, fake_timeticks):
        rows = _run(tmp_path, garbage_benchmark, fake_timeticks)
        assert rows[0]["Status"].startswith("BAD_OUTPUT")

    def test_timeout_records_the_reason(self, tmp_path, fake_timeticks):
        slow = tmp_path / "slow"
        slow.write_text(
            "#!/usr/bin/env python3\n"
            "import sys,time\n"
            "if len(sys.argv)==2 and sys.argv[1]=='--list': print('vyukov'); sys.exit(0)\n"
            "time.sleep(30)\n"
        )
        slow.chmod(0o755)
        rows = _run(tmp_path, slow, fake_timeticks, timeout=0.5)
        assert rows[0]["Status"].startswith("TIMEOUT")


def test_unknown_queue_fails_before_measuring(tmp_path, fake_benchmark, fake_timeticks):
    """Validation is up front: an unusable config must not burn a whole sweep first."""
    (exp,) = schema.loads(
        {"output_file": "r.csv", "queues": ["PSCQ"], "queue_sizes": [1024], "threads": [[1, 1]]}
    )
    with pytest.raises(UnknownQueue):
        runner.run_experiment(exp, fake_benchmark, TickConverter(fake_timeticks), tmp_path)
    assert not (tmp_path / "r.csv").exists()


def test_results_land_beside_the_config_not_the_cwd(tmp_path, fake_benchmark, fake_timeticks, monkeypatch):
    """The old runner opened output_file relative to the working directory."""
    monkeypatch.chdir(tmp_path.parent)
    nested = tmp_path / "deep"
    nested.mkdir()
    (exp,) = schema.loads(
        {"output_file": "sub/r.csv", "queues": ["vyukov"], "queue_sizes": [1024],
         "threads": [[1, 1]], "items": 10}
    )
    out = runner.run_experiment(exp, fake_benchmark, TickConverter(fake_timeticks), nested)
    assert out == nested / "sub" / "r.csv" and out.is_file()


def test_tick_calibration_failure_is_not_silently_zero(tmp_path, fake_benchmark):
    """Returning 0 ticks would turn a delayed experiment into an undelayed one."""
    conv = TickConverter(tmp_path / "missing-timeticks")
    with pytest.raises(RuntimeError, match="calibration failed"):
        conv.ticks_for(500)
