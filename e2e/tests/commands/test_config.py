"""Tests for CONFIG commands."""

import pytest

from lib.assertions import assert_error


@pytest.mark.commands
class TestConfig:
    def test_config_help_root(self, nvecd):
        resp = nvecd.tcp_command("CONFIG HELP")
        assert resp is not None
        assert resp.startswith("+OK\r\n")
        assert resp.endswith("\r\nEND")
        assert "api" in resp

    def test_config_help_path(self, nvecd):
        resp = nvecd.tcp_command("CONFIG HELP similarity.index_type")
        assert resp is not None
        assert resp.startswith("+OK\r\nsimilarity.index_type\r\n")
        assert resp.endswith("\r\nEND")
        assert "Type: string (enum)" in resp

    def test_config_help_unknown_path(self, nvecd):
        resp = nvecd.tcp_command("CONFIG HELP no.such.path")
        assert_error(resp, "Configuration path not found: no.such.path")

    def test_config_show(self, nvecd):
        resp = nvecd.tcp_command("CONFIG SHOW")
        assert resp is not None
        assert resp.startswith("+OK\r\n")
        assert resp.endswith("\r\nEND")
        assert "***" in resp
        assert nvecd.password not in resp
