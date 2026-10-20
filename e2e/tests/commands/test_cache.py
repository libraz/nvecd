"""Tests for CACHE commands."""

import pytest

from lib.assertions import assert_block


@pytest.mark.commands
class TestCache:
    def test_cache_stats(self, nvecd):
        resp = nvecd.cache_stats()
        body = assert_block(resp, "OK CACHE_STATS")
        fields = {line.partition(": ")[0] for line in body}
        assert {"cache_enabled", "cache_entries", "cache_hits", "cache_misses"} <= fields

    def test_cache_clear(self, nvecd):
        assert nvecd.tcp_command("CACHE CLEAR") == "OK CACHE_CLEARED"

    def test_cache_enable(self, nvecd):
        assert nvecd.tcp_command("CACHE ENABLE") == "OK CACHE_ENABLED"
        resp = nvecd.cache_stats()
        assert "cache_enabled: true" in resp

    def test_cache_disable(self, nvecd):
        assert nvecd.tcp_command("CACHE DISABLE") == "OK CACHE_DISABLED"
        resp = nvecd.cache_stats()
        assert "cache_enabled: false" in resp
        # Re-enable for other tests
        assert nvecd.tcp_command("CACHE ENABLE") == "OK CACHE_ENABLED"

    def test_cache_stats_has_hit_rate(self, nvecd):
        resp = nvecd.cache_stats()
        assert "cache_hit_rate" in resp
