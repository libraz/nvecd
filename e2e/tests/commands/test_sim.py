"""Tests for SIM command."""

import pytest


@pytest.mark.commands
class TestSim:
    @pytest.fixture(autouse=True)
    def setup_data(self, nvecd):
        """Seed test data for similarity searches."""
        nvecd.event("sim_ctx", "sim_a", 100)
        nvecd.event("sim_ctx", "sim_b", 90)
        nvecd.event("sim_ctx", "sim_c", 80)
        nvecd.vecset("sim_a", [1.0, 0.0, 0.0])
        nvecd.vecset("sim_b", [0.9, 0.1, 0.0])
        nvecd.vecset("sim_c", [0.0, 1.0, 0.0])

    def test_sim_events(self, nvecd):
        resp = nvecd.sim("sim_a", 10, mode="events")
        assert resp is not None
        assert resp.startswith("OK")

    def test_sim_vectors(self, nvecd):
        resp = nvecd.sim("sim_a", 10, mode="vectors")
        assert resp is not None
        assert resp.startswith("OK")
        assert "RESULTS" in resp

    def test_sim_fusion(self, nvecd):
        resp = nvecd.sim("sim_a", 10, mode="fusion")
        assert resp is not None
        assert resp.startswith("OK")

    def test_sim_not_found(self, nvecd):
        resp = nvecd.sim("nonexistent_item_xyz", 10, mode="events")
        assert resp is not None
        # Should return OK with 0 results or an error
        assert "OK" in resp or "ERROR" in resp
