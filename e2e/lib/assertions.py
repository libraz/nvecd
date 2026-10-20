"""Exact-shape assertions for nvecd wire replies."""

from __future__ import annotations

import re

_RESULT_LINE = re.compile(r"^(\S+) (-?\d+\.\d{4})$")


def assert_error(resp: str | None, message: str) -> None:
    """Assert the reply is exactly ``ERROR <message>``."""
    assert resp == f"ERROR {message}", resp


def assert_status(resp: str | None, status: str) -> None:
    """Assert the reply is exactly the status line ``<status>``."""
    assert resp == status, resp


def block_header(resp: str) -> str:
    """First line of a reply, without its line terminator."""
    return resp.replace("\r\n", "\n").split("\n", 1)[0]


def assert_block(resp: str | None, header: str) -> list[str]:
    """Assert an END-terminated block opened by ``header``; return its body lines."""
    assert resp is not None
    lines = resp.replace("\r\n", "\n").split("\n")
    assert lines[0] == header, resp
    assert lines[-1] == "END", resp
    return lines[1:-1]


def parse_results(resp: str | None) -> list[tuple[str, float]]:
    """Parse ``OK RESULTS <n>`` and its ``<id> <score>`` lines, checking n against the rows."""
    assert resp is not None
    lines = resp.replace("\r\n", "\n").split("\n")
    header = re.fullmatch(r"OK RESULTS (\d+)", lines[0])
    assert header, resp
    rows = []
    for line in lines[1:]:
        match = _RESULT_LINE.match(line)
        assert match, f"malformed result line {line!r} in {resp!r}"
        rows.append((match.group(1), float(match.group(2))))
    assert len(rows) == int(header.group(1)), resp
    return rows


def result_ids(resp: str | None) -> list[str]:
    """Ids of a SIM/SIMV reply in rank order."""
    return [item for item, _ in parse_results(resp)]
