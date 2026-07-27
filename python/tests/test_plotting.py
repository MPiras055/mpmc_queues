"""Styling and result loading."""

from __future__ import annotations

import subprocess
import sys
import textwrap

import pytest

pytest.importorskip("pandas")

from mpmc_bench.plotting import data as dataio
from mpmc_bench.plotting.styles import pretty_label, style_for

CSV = """Queue,Producers,Consumers,Size,Items,Pinning,ProdDelay_NS,ProdDelay_Amp,ConsDelay_NS,ConsDelay_Amp,Throughput_Mean,Throughput_StdDev,Samples,Status
vyukov,1,1,1024,1000,True,0,0.0,0,0.0,100.0,1.0,3,OK
vyukov,2,2,1024,1000,True,0,0.0,0,0.0,180.0,2.0,3,OK
u-prq,1,1,1024,1000,True,0,0.0,0,0.0,90.0,1.0,3,OK
u-prq,2,2,1024,1000,True,0,0.0,0,0.0,150.0,2.0,3,OK
mutex,1,1,1024,1000,True,0,0.0,0,0.0,,,0,EXIT_3: boom
"""

LEGACY_CSV = """Queue,Producers,Consumers,Size,Items,Pinning,ProdDelay_NS,ProdDelay_Amp,ConsDelay_NS,ConsDelay_Amp,Throughput_Mean,Throughput_StdDev
VyukovBuffer,1,1,1024,1000,True,0,0.0,0,0.0,100.0,1.0
PSCQ,1,1,1024,1000,True,0,0.0,0,0.0,FAILED,0.0
"""


@pytest.fixture
def csv_file(tmp_path):
    p = tmp_path / "r.csv"; p.write_text(CSV); return p


class TestLoading:
    def test_failed_rows_are_dropped(self, csv_file):
        df = dataio.load_results(csv_file)
        assert "mutex" not in set(df["Queue"])
        assert len(df) == 4

    def test_legacy_csv_without_status_still_loads(self, tmp_path):
        """Old files have no Status column and wrote the literal 'FAILED'."""
        p = tmp_path / "old.csv"; p.write_text(LEGACY_CSV)
        df = dataio.load_results(p)
        assert list(df["Queue"]) == ["VyukovBuffer"]

    def test_total_threads_is_derived(self, csv_file):
        assert set(dataio.load_results(csv_file)["Total_Threads"]) == {2, 4}

    def test_missing_column_is_reported(self, tmp_path):
        p = tmp_path / "bad.csv"; p.write_text("Queue,Producers\nvyukov,1\n")
        with pytest.raises(ValueError, match="missing expected column"):
            dataio.load_results(p)

    def test_missing_file(self, tmp_path):
        with pytest.raises(FileNotFoundError):
            dataio.load_results(tmp_path / "nope.csv")


class TestFilters:
    def test_filters_compose(self, csv_file):
        df = dataio.load_results(csv_file)
        out = dataio.apply_filters(df, dataio.Filters(queues="vyukov", size=1024))
        assert set(out["Queue"]) == {"vyukov"}

    def test_no_filter_keeps_everything(self, csv_file):
        df = dataio.load_results(csv_file)
        assert len(dataio.apply_filters(df, dataio.Filters())) == len(df)

    def test_scalability_skips_queues_without_a_baseline(self, csv_file):
        df = dataio.load_results(csv_file)
        df = df[df["Total_Threads"] != 2]           # remove every baseline point
        assert list(dataio.scalability(df, baseline_threads=2)) == []


class TestStyles:
    def test_every_registry_name_gets_a_style(self):
        for name in ["vyukov", "u-prq", "chunk-faaarray", "mem-scq", "pscq", "brand-new"]:
            st = style_for(name)
            assert st.color.startswith("#") and st.marker and st.label

    def test_styles_are_stable_across_processes(self):
        """Derived from a salted hash() this would differ per run, so pin it."""
        code = textwrap.dedent("""
            import sys; sys.path.insert(0, %r)
            from mpmc_bench.plotting.styles import style_for
            print(style_for("u-prq").color, style_for("u-prq").marker)
        """ % str(__import__("pathlib").Path(__file__).resolve().parents[1]))
        outs = {
            subprocess.run([sys.executable, "-c", code], capture_output=True, text=True,
                           check=True).stdout.strip()
            for _ in range(2)
        }
        assert len(outs) == 1

    def test_labels_reflect_the_naming_scheme(self):
        assert pretty_label("u-prq") == "Unbounded / PRQ"
        assert pretty_label("mem-scq").startswith("Pool-bounded")
        assert pretty_label("mutex") == "Mutex (baseline)"
