"""Full pipeline workflow tests: EVENT -> VECSET -> SIM."""

import pytest


@pytest.mark.workflows
class TestFullPipeline:
    def test_event_vecset_sim_pipeline(self, nvecd):
        """Full workflow: add events, set vectors, search."""
        # 1. Add events
        for i in range(5):
            resp = nvecd.event("pipe_ctx", f"pipe_item_{i}", 100 - i * 10)
            assert resp.startswith("OK"), f"EVENT failed: {resp}"

        # 2. Set vectors
        vectors = [
            [1.0, 0.0, 0.0],
            [0.9, 0.1, 0.0],
            [0.8, 0.2, 0.0],
            [0.0, 1.0, 0.0],
            [0.0, 0.0, 1.0],
        ]
        for i, vec in enumerate(vectors):
            resp = nvecd.vecset(f"pipe_item_{i}", vec)
            assert resp.startswith("OK"), f"VECSET failed: {resp}"

        # 3. Search by events
        resp = nvecd.sim("pipe_item_0", 10, mode="events")
        assert resp is not None
        assert "OK" in resp

        # 4. Search by vectors
        resp = nvecd.sim("pipe_item_0", 10, mode="vectors")
        assert resp is not None
        assert "OK" in resp
        assert "RESULTS" in resp

        # 5. Search by fusion
        resp = nvecd.sim("pipe_item_0", 10, mode="fusion")
        assert resp is not None
        assert "OK" in resp

        # 6. Verify INFO shows data
        info = nvecd.info()
        assert info.get("vector_count", 0) > 0
        assert info.get("event_count", 0) > 0

    def test_simv_after_vecset(self, nvecd):
        """SIMV should find vectors set via VECSET."""
        nvecd.vecset("simv_pipe_a", [1.0, 0.0, 0.0])
        nvecd.vecset("simv_pipe_b", [0.0, 1.0, 0.0])

        resp = nvecd.simv([1.0, 0.0, 0.0], top_k=10)
        assert resp is not None
        assert "OK" in resp
        assert "RESULTS" in resp

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
        assert "cache_entries" in stats

        # Clear
        assert nvecd.cache_clear()

        # Disable and re-enable
        assert nvecd.cache_disable()
        assert nvecd.cache_enable()
