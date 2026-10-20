"""Tests for VECSET command."""

import pytest

from lib.assertions import assert_error, parse_results


@pytest.mark.commands
class TestVecset:
    def test_vecset_basic(self, nvecd):
        assert nvecd.vecset("vec_test1", [1.0, 2.0, 3.0]) == "OK VECSET"

    def test_vecset_overwrite(self, nvecd):
        first = [1.0, 0.0, 0.0]
        second = [0.5, 0.5, 0.707]
        assert nvecd.vecset("vec_overwrite", first) == "OK VECSET"
        count_before = nvecd.info()["vector_count"]
        assert nvecd.vecset("vec_overwrite", second) == "OK VECSET"
        assert nvecd.info()["vector_count"] == count_before

        # The stored vector is the second one: it matches itself exactly and
        # sits at cos = 0.5 from the first.
        by_second = dict(parse_results(nvecd.simv(second, top_k=100)))
        assert by_second["vec_overwrite"] == pytest.approx(1.0, abs=1e-3)
        by_first = dict(parse_results(nvecd.simv(first, top_k=100)))
        assert by_first["vec_overwrite"] == pytest.approx(0.5, abs=1e-2)

    def test_vecset_dimension_mismatch(self, nvecd):
        """Sending wrong dimension should fail."""
        assert nvecd.vecset("vec_dim_ok", [1.0, 2.0, 3.0]) == "OK VECSET"
        resp = nvecd.vecset("vec_dim_bad", [1.0, 2.0])
        assert_error(resp, "Vector dimension mismatch: expected 3, got 2")
