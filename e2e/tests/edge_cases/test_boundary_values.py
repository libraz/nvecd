"""Tests for boundary values at the edges of the documented parameter ranges."""

import pytest


@pytest.mark.edge_cases
class TestBoundaryValues:
    """EVENT scores are documented as integers in the inclusive range 0-100."""

    def test_event_score_lower_bound_accepted(self, nvecd):
        resp = nvecd.event("boundary_ctx", "item_zero", 0)
        assert resp is not None
        assert resp.startswith("OK"), resp

    def test_event_score_upper_bound_accepted(self, nvecd):
        resp = nvecd.event("boundary_ctx", "item_hundred", 100)
        assert resp is not None
        assert resp.startswith("OK"), resp

    def test_event_negative_score(self, nvecd):
        resp = nvecd.event("boundary_ctx", "item_neg", -100)
        assert resp is not None
        assert resp.startswith("ERROR"), resp
        assert "Score must be in range [0, 100]" in resp

    def test_event_score_just_above_upper_bound(self, nvecd):
        resp = nvecd.event("boundary_ctx", "item_101", 101)
        assert resp is not None
        assert resp.startswith("ERROR"), resp
        assert "Score must be in range [0, 100]" in resp

    def test_event_large_score(self, nvecd):
        resp = nvecd.event("boundary_ctx", "item_large", 999999)
        assert resp is not None
        assert resp.startswith("ERROR"), resp

    def test_sim_top_k_zero(self, nvecd):
        """top_k is documented as > 0, so zero is rejected at parse time."""
        resp = nvecd.sim("any_item", 0, mode="events")
        assert resp is not None
        assert resp.startswith("ERROR"), resp
        assert "top_k must be positive" in resp

    def test_sim_top_k_negative(self, nvecd):
        resp = nvecd.sim("any_item", -1, mode="events")
        assert resp is not None
        assert resp.startswith("ERROR"), resp
        assert "top_k must be positive" in resp

    def test_long_item_id(self, nvecd):
        """Item IDs carry no documented length limit, so a long one is stored."""
        long_id = "x" * 200
        resp = nvecd.event("long_id_ctx", long_id, 50)
        assert resp is not None
        assert resp.startswith("OK"), resp

        # The ID must survive intact rather than being silently truncated: a
        # co-occurring item in the same context lists it back verbatim.
        assert nvecd.event("long_id_ctx", "long_id_peer", 50).startswith("OK")
        results = nvecd.sim("long_id_peer", 10, mode="events")
        assert results is not None
        assert results.startswith("OK RESULTS"), results
        assert long_id in results.split()
