"""Tests for SET/GET/SHOW VARIABLES commands."""

import pytest

from lib.assertions import assert_error


@pytest.mark.commands
class TestVariables:
    def test_set_get_variable(self, nvecd):
        assert nvecd.tcp_command("SET cache.ttl_seconds 120") == "+OK"
        assert nvecd.tcp_command("GET cache.ttl_seconds") == "$3\r\n120"

    def test_show_variables(self, nvecd):
        assert nvecd.tcp_command("SET cache.ttl_seconds 130") == "+OK"
        resp = nvecd.tcp_command("SHOW VARIABLES LIKE cache.%")
        assert resp is not None
        lines = resp.split("\r\n")
        # RESP array: "*N" then N "$<len>" / "<name>=<value> (mutable)" pairs.
        count = int(lines[0][1:])
        assert count > 0
        assert len(lines) == 1 + 2 * count
        entries = lines[2::2]
        for length, entry in zip(lines[1::2], entries):
            assert length == f"${len(entry)}"
            assert entry.startswith("cache.")
        assert "cache.ttl_seconds=130 (mutable)" in entries

    def test_show_variables_requires_like(self, nvecd):
        resp = nvecd.tcp_command("SHOW VARIABLES cache.%")
        assert_error(resp, "SHOW VARIABLES accepts only: [LIKE <pattern>]")
