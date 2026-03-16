"""Tests for SIMV command."""

import pytest


@pytest.mark.commands
class TestSimv:
    @pytest.fixture(autouse=True)
    def setup_vectors(self, nvecd):
        nvecd.vecset("simv_a", [1.0, 0.0, 0.0])
        nvecd.vecset("simv_b", [0.0, 1.0, 0.0])
        nvecd.vecset("simv_c", [0.0, 0.0, 1.0])

    def test_simv_basic(self, nvecd):
        resp = nvecd.simv([1.0, 0.0, 0.0], top_k=10)
        assert resp is not None
        assert resp.startswith("OK")
        assert "RESULTS" in resp
