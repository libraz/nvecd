"""Full pipeline workflow tests: EVENT -> VECSET -> SIM."""

import pytest

from lib.assertions import result_ids


@pytest.mark.workflows
class TestFullPipeline:
    def test_event_vecset_sim_pipeline(self, nvecd):
        """Full workflow: add events, set vectors, search."""
        # 1. Add events
        for i in range(5):
            resp = nvecd.event("pipe_ctx", f"pipe_item_{i}", 100 - i * 10)
            assert resp == "OK EVENT"

        # 2. Set vectors
        vectors = [
            [1.0, 0.0, 0.0],
            [0.9, 0.1, 0.0],
            [0.8, 0.2, 0.0],
            [0.0, 1.0, 0.0],
            [0.0, 0.0, 1.0],
        ]
        for i, vec in enumerate(vectors):
            assert nvecd.vecset(f"pipe_item_{i}", vec) == "OK VECSET"

        # 3. Search by events: every other item shares the context, strongest
        # partner first (the product of the two scores).
        ids = result_ids(nvecd.sim("pipe_item_0", 10, mode="events"))
        assert ids == ["pipe_item_1", "pipe_item_2", "pipe_item_3", "pipe_item_4"]

        # 4. Search by vectors: the closer vector ranks first.
        ids = result_ids(nvecd.sim("pipe_item_0", 100, mode="vectors"))
        assert "pipe_item_0" not in ids
        assert ids.index("pipe_item_1") < ids.index("pipe_item_2")

        # 5. Search by fusion
        ids = result_ids(nvecd.sim("pipe_item_0", 100, mode="fusion"))
        assert "pipe_item_0" not in ids
        assert ids.index("pipe_item_1") < ids.index("pipe_item_4")

        # 6. Verify INFO shows data
        info = nvecd.info()
        assert info.get("vector_count", 0) >= 5
        assert info.get("event_count", 0) >= 5

    def test_simv_after_vecset(self, nvecd):
        """SIMV should find vectors set via VECSET."""
        assert nvecd.vecset("simv_pipe_a", [0.0, 0.28, 0.96]) == "OK VECSET"
        assert nvecd.vecset("simv_pipe_b", [0.96, 0.28, 0.0]) == "OK VECSET"

        ids = result_ids(nvecd.simv([0.0, 0.28, 0.96], top_k=10))
        assert ids[0] == "simv_pipe_a"
        assert "simv_pipe_b" in result_ids(nvecd.simv([0.0, 0.28, 0.96], top_k=100))

    def test_cache_workflow(self, nvecd):
        """Cache commands work in sequence."""
        # Start enabled
        assert nvecd.cache_enable()

        # Do some operations that might use cache
        nvecd.event("cache_wf_ctx", "cache_item", 100)
        nvecd.vecset("cache_item", [1.0, 0.0, 0.0])
        nvecd.sim("cache_item", 10, mode="vectors")

        # Check stats
        stats = nvecd.cache_stats()
        assert stats is not None
        assert stats.startswith("OK CACHE_STATS")
        assert "cache_entries" in stats

        # Clear
        assert nvecd.cache_clear()

        # Disable and re-enable
        assert nvecd.cache_disable()
        assert nvecd.cache_enable()
