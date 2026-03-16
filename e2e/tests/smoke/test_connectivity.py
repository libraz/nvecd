"""Smoke tests: basic connectivity and health checks."""

import pytest


@pytest.mark.smoke
class TestConnectivity:
    def test_tcp_connection(self, nvecd):
        """Server accepts TCP connections."""
        assert nvecd.ping()

    def test_info_command(self, nvecd):
        """INFO command returns server information."""
        info = nvecd.info()
        assert "version" in info
        assert info["version"] == "0.1.0"

    def test_info_has_sections(self, nvecd):
        """INFO response includes all expected sections."""
        resp = nvecd.tcp_command("INFO")
        assert resp is not None
        assert "# Server" in resp
        assert "# Stats" in resp
        assert "# Memory" in resp
        assert "# Data" in resp
