"""Polling wait utilities."""

from __future__ import annotations

import time
from typing import Callable


class WaitTimeout(Exception):
    """Raised when a wait operation times out."""

    def __init__(self, description: str, timeout: float) -> None:
        super().__init__(f"Timed out waiting for {description} after {timeout}s")
        self.description = description
        self.timeout = timeout


def wait_until(
    predicate: Callable[[], bool],
    *,
    timeout: float = 30.0,
    interval: float = 1.0,
    description: str = "condition",
) -> None:
    """Poll until predicate returns True or timeout is reached."""
    deadline = time.monotonic() + timeout
    last_error: Exception | None = None

    while time.monotonic() < deadline:
        try:
            if predicate():
                return
        except Exception as e:
            last_error = e
        time.sleep(interval)

    msg = f"Timed out waiting for {description} after {timeout}s"
    if last_error:
        msg += f" (last error: {last_error})"
    raise WaitTimeout(description, timeout)
