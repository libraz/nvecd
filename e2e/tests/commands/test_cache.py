"""Tests for CACHE commands."""

import pytest


@pytest.mark.commands
class TestCache:
    def test_cache_stats(self, nvecd):
        resp = nvecd.cache_stats()
        assert resp is not None
        assert "OK" in resp
        assert "cache_entries" in resp
        assert "cache_hits" in resp
        assert "cache_misses" in resp

    def test_cache_clear(self, nvecd):
        assert nvecd.cache_clear()

    def test_cache_enable(self, nvecd):
        assert nvecd.cache_enable()
        resp = nvecd.cache_stats()
        assert "cache_enabled: true" in resp

    def test_cache_disable(self, nvecd):
        assert nvecd.cache_disable()
        resp = nvecd.cache_stats()
        assert "cache_enabled: false" in resp
        # Re-enable for other tests
        assert nvecd.cache_enable()

    def test_cache_stats_has_hit_rate(self, nvecd):
        resp = nvecd.cache_stats()
        assert "cache_hit_rate" in resp
