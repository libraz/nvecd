"""Tests for EVENT command."""

import pytest

from lib.assertions import assert_error

_ARITY = "EVENT requires at least 3 arguments: <ctx> <type> <id> [<score>]"


@pytest.mark.commands
class TestEvent:
    def test_event_add_basic(self, nvecd):
        assert nvecd.event("ctx_test", "item1", 100) == "OK EVENT"

    def test_event_add_multiple(self, nvecd):
        for i in range(5):
            assert nvecd.event("ctx_multi", f"item_{i}", 100 - i * 10) == "OK EVENT"

    def test_event_invalid_score(self, nvecd):
        resp = nvecd.tcp_command("EVENT ctx1 ADD item1 notanumber")
        assert_error(resp, "Failed to parse integer: notanumber")

    def test_event_missing_args(self, nvecd):
        assert_error(nvecd.tcp_command("EVENT ctx1 ADD"), _ARITY)

    def test_event_empty_context(self, nvecd):
        assert_error(nvecd.tcp_command("EVENT"), _ARITY)
