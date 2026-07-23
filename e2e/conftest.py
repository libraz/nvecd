"""Isolated pytest fixtures for nvecd E2E tests."""

from __future__ import annotations

import signal
import socket
import subprocess
from pathlib import Path
from typing import Generator

import pytest

from lib.nvecd_client import NvecdClient
from lib.wait import wait_until

NVECD_HOST = "127.0.0.1"
E2E_PASSWORD = "isolated-e2e-password"
PROJECT_ROOT = Path(__file__).parent.parent
NVECD_BINARY = PROJECT_ROOT / "build" / "bin" / "nvecd"
NVECD_CONFIG_TEMPLATE = Path(__file__).parent / "nvecd-test.yaml"


def _unused_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind((NVECD_HOST, 0))
        return int(sock.getsockname()[1])


@pytest.fixture(scope="session")
def nvecd_process(tmp_path_factory: pytest.TempPathFactory) -> Generator[tuple[subprocess.Popen, int], None, None]:
    """Start one owned process on per-run ports and storage paths."""
    if not NVECD_BINARY.exists():
        pytest.fail(f"nvecd binary not found at {NVECD_BINARY}. Run 'make build' first.")
    if not NVECD_CONFIG_TEMPLATE.exists():
        pytest.fail(f"nvecd config not found at {NVECD_CONFIG_TEMPLATE}")

    runtime_dir = tmp_path_factory.mktemp("nvecd-e2e")
    dump_dir = runtime_dir / "dumps"
    dump_dir.mkdir(mode=0o700)
    tcp_port = _unused_port()
    http_port = _unused_port()
    config_text = NVECD_CONFIG_TEMPLATE.read_text(encoding="utf-8")
    config_text = config_text.replace("port: 11117", f"port: {tcp_port}", 1)
    config_text = config_text.replace("port: 18180", f"port: {http_port}", 1)
    config_text = config_text.replace('/tmp/nvecd-e2e-dumps', str(dump_dir))
    config_text += f'\nsecurity:\n  requirepass: "{E2E_PASSWORD}"\n'
    config_path = runtime_dir / "config.yaml"
    config_path.write_text(config_text, encoding="utf-8")

    log_path = runtime_dir / "nvecd.log"
    with log_path.open("w", encoding="utf-8") as log_file:
        proc = subprocess.Popen(
            [str(NVECD_BINARY), "-c", str(config_path)],
            stdout=log_file,
            stderr=subprocess.STDOUT,
            cwd=str(PROJECT_ROOT),
        )
        try:
            yield proc, tcp_port
        finally:
            if proc.poll() is None:
                proc.send_signal(signal.SIGTERM)
                try:
                    proc.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait()


@pytest.fixture(scope="session")
def nvecd(nvecd_process: tuple[subprocess.Popen, int]) -> Generator[NvecdClient, None, None]:
    """Persistent authenticated client owned by the spawned process fixture."""
    proc, tcp_port = nvecd_process
    client = NvecdClient(host=NVECD_HOST, tcp_port=tcp_port, password=E2E_PASSWORD)

    def owned_process_is_ready() -> bool:
        if proc.poll() is not None:
            pytest.fail(f"owned nvecd process exited during startup with status {proc.returncode}")
        return client.ping()

    wait_until(owned_process_is_ready, timeout=30, interval=0.2, description="owned nvecd process to become ready")
    try:
        yield client
    finally:
        client.close()
