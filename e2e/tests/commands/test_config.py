"""Tests for CONFIG commands."""

import pytest


@pytest.mark.commands
class TestConfig:
    def test_config_help_root(self, nvecd):
        resp = nvecd.tcp_command("CONFIG HELP")
        assert resp is not None
        assert "+OK" in resp or "OK" in resp

    def test_config_help_path(self, nvecd):
        resp = nvecd.tcp_command("CONFIG HELP api")
        assert resp is not None
        # May return +OK or -ERR depending on path validity
        assert "+OK" in resp or "-ERR" in resp or "OK" in resp

    def test_config_show(self, nvecd):
        resp = nvecd.tcp_command("CONFIG SHOW")
        assert resp is not None
        assert "+OK" in resp or "OK" in resp
