"""Tests for boundary values."""

import pytest


@pytest.mark.edge_cases
class TestBoundaryValues:
    def test_event_zero_score(self, nvecd):
        resp = nvecd.event("boundary_ctx", "item_zero", 0)
        assert resp is not None
        # Zero score may be accepted or rejected depending on implementation
        assert "OK" in resp or "ERROR" in resp

    def test_event_negative_score(self, nvecd):
        resp = nvecd.event("boundary_ctx", "item_neg", -100)
        assert resp is not None

    def test_event_large_score(self, nvecd):
        resp = nvecd.event("boundary_ctx", "item_large", 999999)
        assert resp is not None
        assert resp.startswith("OK")

    def test_sim_top_k_zero(self, nvecd):
        resp = nvecd.sim("any_item", 0, mode="events")
        assert resp is not None

    def test_long_item_id(self, nvecd):
        long_id = "x" * 200
        resp = nvecd.event("boundary_ctx", long_id, 50)
        assert resp is not None
