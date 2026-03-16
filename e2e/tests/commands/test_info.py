"""Tests for INFO command."""

import pytest


@pytest.mark.commands
class TestInfo:
    def test_info_data_section(self, nvecd):
        """INFO should report data counts."""
        # Seed some data first
        nvecd.event("info_ctx", "info_item1", 100)
        nvecd.vecset("info_item1", [1.0, 2.0, 3.0])

        info = nvecd.info()
        assert "vector_count" in info
        assert "event_count" in info
        assert "ctx_count" in info
        assert "id_count" in info

    def test_info_memory_section(self, nvecd):
        info = nvecd.info()
        assert "used_memory_bytes" in info
        assert "used_memory_human" in info

    def test_info_cache_section(self, nvecd):
        resp = nvecd.tcp_command("INFO")
        assert resp is not None
        assert "# Cache" in resp
        assert "cache_entries" in resp
