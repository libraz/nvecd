"""Tests for invalid/unknown commands."""

import pytest


@pytest.mark.edge_cases
class TestInvalidCommands:
    def test_unknown_command(self, nvecd):
        resp = nvecd.tcp_command("FOOBAR")
        assert resp is not None
        assert "ERROR" in resp or "ERR" in resp

    def test_empty_command(self, nvecd):
        resp = nvecd.tcp_command("")
        # Empty command may return error or close connection (None)
        if resp is not None:
            assert "ERROR" in resp or "ERR" in resp

    def test_partial_command(self, nvecd):
        resp = nvecd.tcp_command("EVENT")
        assert resp is not None
        assert "ERROR" in resp or "ERR" in resp

    def test_sim_missing_args(self, nvecd):
        resp = nvecd.tcp_command("SIM")
        assert resp is not None
        assert "ERROR" in resp or "ERR" in resp
