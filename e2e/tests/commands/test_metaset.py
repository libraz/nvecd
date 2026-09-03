"""Tests for METASET and the filter= option it feeds."""

import pytest


def result_ids(response: str) -> list[str]:
    """Extract the item IDs from an ``OK RESULTS <n>`` response body."""
    assert response is not None
    lines = response.split("\n")
    assert lines[0].startswith("OK RESULTS "), response
    count = int(lines[0].split()[2])
    ids = [line.split()[0] for line in lines[1 : 1 + count] if line.strip()]
    assert len(ids) == count, response
    return ids


def filtered_ids(nvecd, expr: str, top_k: int = 10) -> list[str]:
    """Vector-search neighbours of meta_a that satisfy the filter expression."""
    return result_ids(nvecd.sim("meta_a", top_k, mode="vectors", filter_expr=expr))


@pytest.mark.commands
class TestMetaset:
    @pytest.fixture(autouse=True)
    def setup_data(self, nvecd):
        """Seed three vectors, all similar to meta_a, with distinct metadata."""
        nvecd.vecset("meta_a", [1.0, 0.0, 0.0])
        nvecd.vecset("meta_b", [0.9, 0.1, 0.0])
        nvecd.vecset("meta_c", [0.8, 0.2, 0.0])
        nvecd.metaset("meta_a", {"category": "electronics", "rank": 10, "active": True})
        nvecd.metaset("meta_b", {"category": "electronics", "rank": 20, "active": False})
        nvecd.metaset("meta_c", {"category": "books", "rank": 30, "active": True})

    def test_metaset_acknowledges_the_write(self, nvecd):
        resp = nvecd.metaset("meta_a", {"category": "electronics", "rank": 10, "active": True})
        assert resp == "OK METASET", resp

    def test_metaset_rejects_an_unknown_item(self, nvecd):
        """Metadata must attach to an existing vector, never create one."""
        resp = nvecd.metaset("meta_never_vecset", {"category": "electronics"})
        assert resp is not None
        assert resp.startswith("ERROR"), resp
        assert "Vector not found" in resp

        # The rejected write leaves no entry behind for filters to match.
        assert filtered_ids(nvecd, "category:electronics") == ["meta_b"]

    def test_metaset_requires_an_id_and_a_pair_list(self, nvecd):
        resp = nvecd.tcp_command("METASET meta_a")
        assert resp is not None
        assert resp.startswith("ERROR"), resp
        assert "METASET requires 2 arguments" in resp

    def test_metaset_rejects_a_malformed_pair(self, nvecd):
        resp = nvecd.metaset("meta_a", "categoryelectronics")
        assert resp is not None
        assert resp.startswith("ERROR"), resp
        assert "Invalid filter condition" in resp

    def test_filter_equality_selects_only_matching_items(self, nvecd):
        # meta_a is the query and is excluded from its own results.
        assert filtered_ids(nvecd, "category:books") == ["meta_c"]
        assert filtered_ids(nvecd, "category:electronics") == ["meta_b"]

    def test_filter_without_a_match_returns_no_results(self, nvecd):
        assert filtered_ids(nvecd, "category:groceries") == []

    def test_filter_excludes_items_that_carry_no_metadata(self, nvecd):
        """A non-empty filter never matches an item that was never METASET."""
        nvecd.vecset("meta_unlabelled", [0.95, 0.05, 0.0])
        assert "meta_unlabelled" not in filtered_ids(nvecd, "category:electronics")

        # Without a filter the same item is a legitimate neighbour, so its
        # absence above is the filter's doing and not a missing vector.
        unfiltered = result_ids(nvecd.sim("meta_a", 100, mode="vectors"))
        assert "meta_unlabelled" in unfiltered

    def test_filter_numeric_comparison(self, nvecd):
        assert filtered_ids(nvecd, "rank>15") == ["meta_b", "meta_c"]
        assert filtered_ids(nvecd, "rank>=30") == ["meta_c"]
        assert filtered_ids(nvecd, "rank<20") == []

    def test_filter_boolean_value(self, nvecd):
        assert filtered_ids(nvecd, "active:true") == ["meta_c"]
        assert filtered_ids(nvecd, "active:false") == ["meta_b"]

    def test_filter_membership_and_inequality(self, nvecd):
        assert filtered_ids(nvecd, "category=in(books|toys)") == ["meta_c"]
        assert filtered_ids(nvecd, "category!=books") == ["meta_b"]

    def test_filter_conditions_are_anded(self, nvecd):
        assert filtered_ids(nvecd, "category:electronics,rank>15") == ["meta_b"]
        assert filtered_ids(nvecd, "category:books,rank>15") == ["meta_c"]
        assert filtered_ids(nvecd, "category:books,rank<15") == []

    def test_metaset_replaces_the_previous_metadata(self, nvecd):
        assert nvecd.metaset("meta_c", {"category": "toys"}) == "OK METASET"
        # The replaced value no longer matches, and the old key is gone.
        assert filtered_ids(nvecd, "category:books") == []
        assert filtered_ids(nvecd, "category:toys") == ["meta_c"]
        assert filtered_ids(nvecd, "rank>15") == ["meta_b"]

    def test_simv_accepts_the_same_filter(self, nvecd):
        resp = nvecd.simv([1.0, 0.0, 0.0], top_k=10, filter_expr="category:books")
        assert result_ids(resp) == ["meta_c"]
