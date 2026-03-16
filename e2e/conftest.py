"""Global pytest fixtures for nvecd E2E tests."""

from __future__ import annotations

import os
import signal
import subprocess
from pathlib import Path
from typing import Generator

import pytest

from lib.nvecd_client import NvecdClient
from lib.wait import wait_until

# Constants
NVECD_HOST = "127.0.0.1"
NVECD_TCP_PORT = 11117

PROJECT_ROOT = Path(__file__).parent.parent
NVECD_BINARY = PROJECT_ROOT / "build" / "bin" / "nvecd"
NVECD_CONFIG = Path(__file__).parent / "nvecd-test.yaml"
NVECD_LOG = Path("/tmp/nvecd-e2e.log")
NVECD_DUMP_DIR = Path("/tmp/nvecd-e2e-dumps")


@pytest.fixture(scope="session")
def nvecd_process() -> Generator[subprocess.Popen, None, None]:
    """Start nvecd binary and yield the process. Teardown sends SIGTERM."""
    if not NVECD_BINARY.exists():
        pytest.fail(
            f"nvecd binary not found at {NVECD_BINARY}. "
            f"Run 'make build' first."
        )
    if not NVECD_CONFIG.exists():
        pytest.fail(f"nvecd config not found at {NVECD_CONFIG}")

    NVECD_DUMP_DIR.mkdir(parents=True, exist_ok=True)

    log_file = open(NVECD_LOG, "w")
    proc = subprocess.Popen(
        [str(NVECD_BINARY), "-c", str(NVECD_CONFIG)],
        stdout=log_file,
        stderr=subprocess.STDOUT,
        cwd=str(PROJECT_ROOT),
    )

    yield proc

    # Teardown: graceful shutdown
    if proc.poll() is None:
        proc.send_signal(signal.SIGTERM)
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
    log_file.close()


@pytest.fixture(scope="session")
def nvecd(nvecd_process: subprocess.Popen) -> NvecdClient:
    """Session-scoped nvecd client."""
    client = NvecdClient(host=NVECD_HOST, tcp_port=NVECD_TCP_PORT)
    wait_until(
        lambda: client.ping(),
        timeout=30,
        interval=1,
        description="nvecd to accept connections",
    )
    return client
