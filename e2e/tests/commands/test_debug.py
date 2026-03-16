"""Tests for DEBUG commands."""

import pytest


@pytest.mark.commands
class TestDebug:
    def test_debug_on(self, nvecd):
        resp = nvecd.tcp_command("DEBUG ON")
        assert resp is not None
        assert "OK" in resp

    def test_debug_off(self, nvecd):
        resp = nvecd.tcp_command("DEBUG OFF")
        assert resp is not None
        assert "OK" in resp

    def test_debug_toggle(self, nvecd):
        resp = nvecd.tcp_command("DEBUG ON")
        assert "OK" in resp
        resp = nvecd.tcp_command("DEBUG OFF")
        assert "OK" in resp
