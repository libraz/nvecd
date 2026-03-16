"""Tests for DUMP commands."""

import pytest


@pytest.mark.commands
class TestDump:
    def test_dump_save(self, nvecd):
        nvecd.event("dump_ctx", "dump_item", 100)
        resp = nvecd.tcp_command("DUMP SAVE e2e_test.dmp")
        assert resp is not None
        assert resp.startswith("OK")

    def test_dump_info(self, nvecd):
        nvecd.tcp_command("DUMP SAVE e2e_info.dmp")
        resp = nvecd.tcp_command("DUMP INFO e2e_info.dmp")
        assert resp is not None
        assert "OK" in resp
        assert "version" in resp

    def test_dump_verify(self, nvecd):
        nvecd.tcp_command("DUMP SAVE e2e_verify.dmp")
        resp = nvecd.tcp_command("DUMP VERIFY e2e_verify.dmp")
        assert resp is not None
        assert "OK" in resp

    def test_dump_load_nonexistent(self, nvecd):
        resp = nvecd.tcp_command("DUMP LOAD nonexistent_file.dmp")
        assert resp is not None
        assert "ERROR" in resp or "ERR" in resp
