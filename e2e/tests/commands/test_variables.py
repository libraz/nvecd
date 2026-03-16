"""Tests for SET/GET/SHOW VARIABLES commands."""

import pytest


@pytest.mark.commands
class TestVariables:
    def test_set_get_variable(self, nvecd):
        resp = nvecd.tcp_command("SET cache.ttl_seconds 120")
        assert resp is not None
        assert "+OK" in resp

        resp = nvecd.tcp_command("GET cache.ttl_seconds")
        assert resp is not None
        assert "120" in resp

    def test_show_variables(self, nvecd):
        resp = nvecd.tcp_command("SHOW VARIABLES")
        assert resp is not None
        # SHOW VARIABLES returns Redis array format (*N\r\n...)
        assert resp.startswith("*") or "cache." in resp
