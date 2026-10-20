"""Tests for SIM command."""

import pytest

from lib.assertions import assert_error, parse_results, result_ids


@pytest.mark.commands
class TestSim:
    @pytest.fixture(autouse=True)
    def setup_data(self, nvecd):
        """Seed test data for similarity searches."""
        for item, score in (("sim_a", 100), ("sim_b", 90), ("sim_c", 80)):
            assert nvecd.event("sim_ctx", item, score) == "OK EVENT"
        assert nvecd.vecset("sim_a", [1.0, 0.0, 0.0]) == "OK VECSET"
        assert nvecd.vecset("sim_b", [0.9, 0.1, 0.0]) == "OK VECSET"
        assert nvecd.vecset("sim_c", [0.5, 0.5, 0.0]) == "OK VECSET"

    def test_sim_events(self, nvecd):
        # sim_a pairs with sim_b at 100*90 and with sim_c at 100*80.
        resp = nvecd.sim("sim_a", 10, mode="events")
        assert result_ids(resp) == ["sim_b", "sim_c"]

    def test_sim_vectors(self, nvecd):
        rows = dict(parse_results(nvecd.sim("sim_a", 100, mode="vectors")))
        ids = list(rows)
        assert "sim_a" not in ids
        assert ids.index("sim_b") < ids.index("sim_c")
        assert rows["sim_b"] == pytest.approx(0.9939, abs=1e-3)
        assert rows["sim_c"] == pytest.approx(0.7071, abs=1e-3)

    def test_sim_fusion(self, nvecd):
        ids = result_ids(nvecd.sim("sim_a", 100, mode="fusion"))
        assert "sim_a" not in ids
        assert ids.index("sim_b") < ids.index("sim_c")

    def test_sim_events_unknown_id_is_empty(self, nvecd):
        resp = nvecd.sim("nonexistent_item_xyz", 10, mode="events")
        assert resp == "OK RESULTS 0"

    @pytest.mark.parametrize("mode", ["vectors", "fusion"])
    def test_sim_unknown_id_is_an_error(self, nvecd, mode):
        resp = nvecd.sim("nonexistent_item_xyz", 10, mode=mode)
        assert_error(resp, "Query vector not found: nonexistent_item_xyz")
