"""Smoke tests: basic connectivity and health checks."""

import re

import pytest

SEMVER = re.compile(r"^\d+\.\d+\.\d+(?:[-+][0-9A-Za-z.\-]+)?$")


@pytest.mark.smoke
class TestConnectivity:
    def test_tcp_connection(self, nvecd):
        """Server accepts TCP connections."""
        assert nvecd.ping()

    def test_info_command(self, nvecd):
        """INFO reports a semver server version."""
        info = nvecd.info()
        assert "version" in info
        version = str(info["version"])
        assert SEMVER.match(version), f"version is not semver: {version!r}"

    def test_info_has_sections(self, nvecd):
        """INFO response includes all expected sections."""
        resp = nvecd.tcp_command("INFO")
        assert resp is not None
        assert "# Server" in resp
        assert "# Stats" in resp
        assert "# Memory" in resp
        assert "# Data" in resp
