"""Tests for DEBUG commands."""

import pytest


@pytest.mark.commands
class TestDebug:
    def test_debug_on(self, nvecd):
        assert nvecd.tcp_command("DEBUG ON") == "OK DEBUG_ON"
        assert nvecd.tcp_command("DEBUG OFF") == "OK DEBUG_OFF"

    def test_debug_off(self, nvecd):
        assert nvecd.tcp_command("DEBUG OFF") == "OK DEBUG_OFF"
