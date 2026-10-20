"""Tests for SIMV command."""

import pytest

from lib.assertions import parse_results


@pytest.mark.commands
class TestSimv:
    @pytest.fixture(autouse=True)
    def setup_vectors(self, nvecd):
        assert nvecd.vecset("simv_a", [0.6, 0.8, 0.0]) == "OK VECSET"
        assert nvecd.vecset("simv_b", [0.0, 0.6, 0.8]) == "OK VECSET"
        assert nvecd.vecset("simv_c", [0.8, 0.0, 0.6]) == "OK VECSET"

    def test_simv_ranks_the_matching_vector_first(self, nvecd):
        rows = parse_results(nvecd.simv([0.6, 0.8, 0.0], top_k=10))
        assert rows[0][0] == "simv_a"
        assert rows[0][1] == pytest.approx(1.0, abs=1e-3)

    def test_simv_top_k_limits_rows(self, nvecd):
        assert len(parse_results(nvecd.simv([0.6, 0.8, 0.0], top_k=2))) == 2
