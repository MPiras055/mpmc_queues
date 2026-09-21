"""The persistent style library: what it remembers, and what it must not overrule."""

import json

import matplotlib

matplotlib.use("Agg")

import pytest  # noqa: E402

from mpmc_bench.gui import model as m  # noqa: E402
from mpmc_bench.qt.stylelib import Pinned, StyleLibrary, default_path  # noqa: E402


@pytest.fixture
def library(tmp_path) -> StyleLibrary:
    return StyleLibrary(tmp_path / "styles.json")


@pytest.fixture
def built() -> m.Built:
    return m.Built([{
        "u-pscq": m.Series("u-pscq", "u-pscq", (), [1], [1.0]),
        "u-pscq-@Size=1024": m.Series("u-pscq-@Size=1024", "u-pscq",
                                      (("Size", "1024"),), [1], [1.0]),
        "u-prq": m.Series("u-prq", "u-prq", (), [1], [1.0]),
    }])


class TestPersistence:
    def test_round_trips(self, library, tmp_path):
        library.entries["u-pscq"] = Pinned(color="#123456", marker="s")
        library.save()
        again = StyleLibrary(tmp_path / "styles.json")
        assert again.entries["u-pscq"].color == "#123456"
        assert again.entries["u-pscq"].marker == "s"

    def test_a_missing_file_is_simply_empty(self, tmp_path):
        assert StyleLibrary(tmp_path / "nothing.json").entries == {}

    def test_a_corrupt_file_is_ignored_rather_than_fatal(self, tmp_path):
        path = tmp_path / "styles.json"
        path.write_text("{not json")
        assert StyleLibrary(path).entries == {}

    def test_unknown_fields_from_a_newer_version_are_dropped(self, tmp_path):
        path = tmp_path / "styles.json"
        path.write_text(json.dumps({"queues": {"q": {"color": "#fff", "glow": True}}}))
        assert StyleLibrary(path).entries["q"].color == "#fff"

    def test_empty_entries_are_not_written(self, library):
        library.entries["q"] = Pinned()
        library.save()
        assert json.loads(library.path.read_text())["queues"] == {}

    def test_the_default_path_follows_xdg(self, monkeypatch, tmp_path):
        monkeypatch.setenv("XDG_CONFIG_HOME", str(tmp_path))
        assert default_path() == tmp_path / "mpmc-bench" / "styles.json"


class TestApplying:
    def test_fills_in_what_the_user_has_not_chosen(self, library, built):
        library.entries["u-pscq"] = Pinned(color="#abcdef", label="PSCQ")
        state = m.PlotState()
        assert library.apply(state, built) == 2         # both u-pscq series
        assert state.styles["u-pscq"].color == "#abcdef"
        assert state.styles["u-pscq"].label == "PSCQ"
        assert "u-prq" not in state.styles

    def test_never_overrules_a_choice_made_in_this_session(self, library, built):
        library.entries["u-pscq"] = Pinned(color="#abcdef")
        state = m.PlotState(styles={"u-pscq": m.SeriesStyle(color="#ff0000")})
        library.apply(state, built)
        assert state.styles["u-pscq"].color == "#ff0000"

    def test_reaches_every_split_of_the_same_queue(self, library, built):
        """A colour is for the implementation, not for one slice of it."""
        library.entries["u-pscq"] = Pinned(marker="D")
        state = m.PlotState()
        library.apply(state, built)
        assert state.styles["u-pscq-@Size=1024"].marker == "D"


class TestRemembering:
    def test_pins_overrides_by_queue_discarding_the_split(self, library, built):
        state = m.PlotState(styles={"u-pscq-@Size=1024": m.SeriesStyle(color="#0f0f0f")})
        assert library.remember(state, built) == 1
        assert library.entries["u-pscq"].color == "#0f0f0f"

    def test_ignores_series_that_are_not_on_the_chart(self, library, built):
        state = m.PlotState(styles={"gone": m.SeriesStyle(color="#111111")})
        assert library.remember(state, built) == 0
        assert library.entries == {}

    def test_writes_through_to_disk(self, library, built):
        state = m.PlotState(styles={"u-prq": m.SeriesStyle(label="PRQ")})
        library.remember(state, built)
        assert json.loads(library.path.read_text())["queues"]["u-prq"]["label"] == "PRQ"

    def test_forget_and_clear(self, library, built):
        state = m.PlotState(styles={"u-prq": m.SeriesStyle(label="PRQ"),
                                    "u-pscq": m.SeriesStyle(label="PSCQ")})
        library.remember(state, built)
        library.forget("u-prq")
        assert set(library.entries) == {"u-pscq"}
        library.clear()
        assert library.entries == {}
        assert json.loads(library.path.read_text())["queues"] == {}

    def test_a_round_trip_is_stable(self, library, built, tmp_path):
        """Pin, reload, apply: the figure must come back the way it was left."""
        state = m.PlotState(styles={"u-prq": m.SeriesStyle(color="#334455", marker="*")})
        library.remember(state, built)
        fresh = StyleLibrary(tmp_path / "styles.json")
        restored = m.PlotState()
        fresh.apply(restored, built)
        assert restored.styles["u-prq"].color == "#334455"
        assert restored.styles["u-prq"].marker == "*"
