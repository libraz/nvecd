"""Tests for EVENT command."""

import pytest


@pytest.mark.commands
class TestEvent:
    def test_event_add_basic(self, nvecd):
        resp = nvecd.event("ctx_test", "item1", 100)
        assert resp is not None
        assert resp.startswith("OK")

    def test_event_add_multiple(self, nvecd):
        for i in range(5):
            resp = nvecd.event("ctx_multi", f"item_{i}", 100 - i * 10)
            assert resp is not None
            assert resp.startswith("OK")

    def test_event_invalid_score(self, nvecd):
        resp = nvecd.tcp_command("EVENT ctx1 ADD item1 notanumber")
        assert resp is not None
        assert "ERROR" in resp or "ERR" in resp

    def test_event_missing_args(self, nvecd):
        resp = nvecd.tcp_command("EVENT ctx1 ADD")
        assert resp is not None
        assert "ERROR" in resp or "ERR" in resp

    def test_event_empty_context(self, nvecd):
        resp = nvecd.tcp_command("EVENT")
        assert resp is not None
        assert "ERROR" in resp or "ERR" in resp
