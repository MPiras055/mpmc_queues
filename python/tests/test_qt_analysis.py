"""Baselines, rankings and summaries -- the arithmetic behind the new panels.

No Qt here: :mod:`qt.analysis` is deliberately pure, so the claims the window makes about
the numbers can be checked without a display.
"""

import matplotlib

matplotlib.use("Agg")

import pytest  # noqa: E402

from mpmc_bench.gui import model as m  # noqa: E402
from mpmc_bench.qt import analysis  # noqa: E402


def series(key: str, x, y, lo=None, hi=None) -> m.Series:
    return m.Series(key, key, (), list(x), list(y), lo, hi)


@pytest.fixture
def built() -> m.Built:
    return m.Built([{
        "base": series("base", [1, 2, 4], [10.0, 20.0, 40.0]),
        "fast": series("fast", [1, 2, 4], [20.0, 50.0, 40.0]),
        "slow": series("slow", [1, 2, 4], [5.0, 10.0, 10.0]),
    }])


def visible(*keys) -> dict[str, m.Resolved]:
    return {k: m.Resolved(k, "#000000", "o", "-", 2.0, True) for k in keys}


class TestBaseline:
    def test_off_is_the_identity(self, built):
        assert analysis.apply_baseline(built, "base", "off") is built

    def test_ratio_divides_by_the_baseline(self, built):
        out = analysis.apply_baseline(built, "base", "ratio")
        assert out.panels[0]["fast"].y == [2.0, 2.5, 1.0]
        assert out.panels[0]["slow"].y == [0.5, 0.5, 0.25]

    def test_the_baseline_stays_as_the_reference_line(self, built):
        """Dropping it would leave the reader guessing where the axis crosses."""
        assert analysis.apply_baseline(built, "base", "ratio").panels[0]["base"].y == [1, 1, 1]
        assert analysis.apply_baseline(built, "base", "percent").panels[0]["base"].y == [0, 0, 0]

    def test_percent_is_the_difference(self, built):
        out = analysis.apply_baseline(built, "base", "percent")
        assert out.panels[0]["fast"].y == [100.0, 150.0, 0.0]
        assert out.panels[0]["slow"].y == pytest.approx([-50.0, -50.0, -75.0])

    def test_error_bars_are_converted_too(self):
        built = m.Built([{"base": series("base", [1], [10.0], [9.0], [11.0]),
                          "other": series("other", [1], [20.0], [18.0], [22.0])}])
        out = analysis.apply_baseline(built, "base", "ratio")
        assert out.panels[0]["other"].lo == [1.8]
        assert out.panels[0]["other"].hi == [2.2]

    def test_x_values_the_baseline_lacks_are_dropped(self):
        """A ratio at an x the baseline never measured is a number with no denominator."""
        built = m.Built([{"base": series("base", [1, 2], [10.0, 20.0]),
                          "other": series("other", [1, 2, 4], [10.0, 10.0, 10.0])}])
        out = analysis.apply_baseline(built, "base", "ratio")
        assert out.panels[0]["other"].x == [1, 2]

    def test_a_zero_baseline_point_is_skipped_not_divided_by(self):
        built = m.Built([{"base": series("base", [1, 2], [0.0, 20.0]),
                          "other": series("other", [1, 2], [5.0, 10.0])}])
        out = analysis.apply_baseline(built, "base", "ratio")
        assert out.panels[0]["other"].x == [2]

    def test_a_missing_baseline_empties_the_plot_and_says_so(self, built):
        out = analysis.apply_baseline(built, "nope", "ratio")
        assert out.panels[0] == {}
        assert any("nope" in n for n in out.notes)

    def test_panel_scope_compares_a_run_against_another_run(self):
        """The before/after case: the same queue in two CSVs."""
        built = m.Built([{"q": series("q", [1, 2], [10.0, 20.0])},
                         {"q": series("q", [1, 2], [20.0, 20.0])}])
        out = analysis.apply_baseline(built, "0", "percent", scope="panel")
        assert out.panels[0]["q"].y == [0.0, 0.0]
        assert out.panels[1]["q"].y == [100.0, 0.0]

    def test_panel_scope_drops_series_the_baseline_plot_lacks(self):
        built = m.Built([{"q": series("q", [1], [10.0])},
                         {"q": series("q", [1], [20.0]), "extra": series("extra", [1], [5.0])}])
        out = analysis.apply_baseline(built, "0", "ratio", scope="panel")
        assert "extra" not in out.panels[1]
        assert any("not in the baseline plot" in n for n in out.notes)

    def test_an_out_of_range_baseline_plot_is_reported_not_raised(self, built):
        out = analysis.apply_baseline(built, "9", "ratio", scope="panel")
        assert any("does not exist" in n for n in out.notes)


class TestRanking:
    def test_names_the_winner_and_the_margin_at_each_x(self, built):
        ranks = analysis.ranking(built, visible("base", "fast", "slow"))
        first = ranks[0]
        assert (first.x, first.winner, first.runner_up) == (1, "fast", "base")
        assert first.margin == pytest.approx(100.0)

    def test_a_tie_still_reports_a_runner_up(self):
        built = m.Built([{"a": series("a", [1], [5.0]), "b": series("b", [1], [5.0])}])
        rank = analysis.ranking(built, visible("a", "b"))[0]
        assert rank.margin == pytest.approx(0.0)

    def test_a_lone_series_has_no_runner_up_and_no_margin(self):
        built = m.Built([{"a": series("a", [1], [5.0])}])
        rank = analysis.ranking(built, visible("a"))[0]
        assert rank.runner_up is None and rank.margin is None

    def test_hidden_series_do_not_compete(self, built):
        styles = visible("base", "fast", "slow")
        styles["fast"] = m.Resolved("fast", "#000000", "o", "-", 2.0, False)
        assert {r.winner for r in analysis.ranking(built, styles)} == {"base"}

    def test_lower_is_better_flips_it(self, built):
        ranks = analysis.ranking(built, visible("base", "fast", "slow"),
                                 higher_is_better=False)
        assert ranks[0].winner == "slow"


class TestSummary:
    def test_reports_the_peak_and_where_it_happens(self, built):
        peaks = {p.key: p for p in analysis.summary(built, visible("base", "fast", "slow"))}
        assert (peaks["fast"].peak, peaks["fast"].peak_at) == (50.0, 2)
        assert peaks["fast"].final == 40.0
        assert peaks["base"].points == 3

    def test_hidden_series_are_left_out(self, built):
        styles = visible("base", "fast", "slow")
        styles["slow"] = m.Resolved("slow", "#000000", "o", "-", 2.0, False)
        assert "slow" not in {p.key for p in analysis.summary(built, styles)}

    def test_an_empty_series_is_skipped_rather_than_crashing(self):
        built = m.Built([{"a": series("a", [], [])}])
        assert analysis.summary(built, visible("a")) == []
