"""Tests for VECSET command."""

import pytest


@pytest.mark.commands
class TestVecset:
    def test_vecset_basic(self, nvecd):
        resp = nvecd.vecset("vec_test1", [1.0, 2.0, 3.0])
        assert resp is not None
        assert resp.startswith("OK")

    def test_vecset_overwrite(self, nvecd):
        nvecd.vecset("vec_overwrite", [1.0, 0.0, 0.0])
        resp = nvecd.vecset("vec_overwrite", [0.0, 1.0, 0.0])
        assert resp is not None
        assert resp.startswith("OK")

    def test_vecset_dimension_mismatch(self, nvecd):
        """Sending wrong dimension should fail."""
        nvecd.vecset("vec_dim_ok", [1.0, 2.0, 3.0])
        resp = nvecd.vecset("vec_dim_bad", [1.0, 2.0])
        assert resp is not None
        assert "ERROR" in resp or "ERR" in resp
