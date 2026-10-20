"""Tests for DUMP commands."""

import pytest

from lib.assertions import assert_block, block_header, result_ids


@pytest.mark.commands
class TestDump:
    def test_dump_save(self, nvecd):
        nvecd.event("dump_ctx", "dump_item", 100)
        resp = nvecd.tcp_command("DUMP SAVE e2e_test.dmp")
        assert resp is not None
        # snapshot.mode is "lock", so the file is complete when the reply arrives.
        assert resp.startswith("OK DUMP_SAVED ")
        assert resp.endswith("e2e_test.dmp")

    def test_dump_info(self, nvecd):
        assert nvecd.tcp_command("DUMP SAVE e2e_info.dmp").startswith("OK DUMP_SAVED ")
        resp = nvecd.tcp_command("DUMP INFO e2e_info.dmp")
        assert resp is not None
        header = block_header(resp)
        assert header.startswith("OK DUMP_INFO ")
        assert header.endswith("e2e_info.dmp")
        body = assert_block(resp, header)
        assert any(line.startswith("version: ") for line in body)

    def test_dump_verify(self, nvecd):
        assert nvecd.tcp_command("DUMP SAVE e2e_verify.dmp").startswith("OK DUMP_SAVED ")
        resp = nvecd.tcp_command("DUMP VERIFY e2e_verify.dmp")
        assert resp is not None
        assert resp.startswith("OK DUMP_VERIFIED ")
        assert resp.endswith("e2e_verify.dmp")

    def test_dump_load_restores_deleted_vector(self, nvecd):
        vector = [0.267, 0.534, 0.802]
        assert nvecd.vecset("dump_roundtrip", vector) == "OK VECSET"
        assert nvecd.tcp_command("DUMP SAVE e2e_roundtrip.dmp").startswith("OK DUMP_SAVED ")

        assert nvecd.tcp_command("VECDEL dump_roundtrip") == "OK VECDEL"
        assert "dump_roundtrip" not in result_ids(nvecd.simv(vector, top_k=100))

        resp = nvecd.tcp_command("DUMP LOAD e2e_roundtrip.dmp")
        assert resp is not None
        assert resp.startswith("OK DUMP_LOADED ")
        assert resp.endswith("e2e_roundtrip.dmp")
        assert result_ids(nvecd.simv(vector, top_k=100))[0] == "dump_roundtrip"

    def test_dump_load_nonexistent(self, nvecd):
        resp = nvecd.tcp_command("DUMP LOAD nonexistent_file.dmp")
        assert resp is not None
        assert resp.startswith("ERROR Failed to load snapshot from ")
        assert "nonexistent_file.dmp" in resp
