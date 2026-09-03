"""Tests for invalid/unknown commands."""

import socket

import pytest


def _read_until(sock: socket.socket, terminator: bytes) -> bytes:
    """Read from the socket until the terminator appears in the stream."""
    buffer = b""
    while terminator not in buffer:
        chunk = sock.recv(65536)
        if not chunk:
            raise AssertionError(f"connection closed before {terminator!r}; got {buffer!r}")
        buffer += chunk
    return buffer


def _raw_session(nvecd, timeout: float = 10.0) -> socket.socket:
    """Open an authenticated raw socket that bypasses the client's framing."""
    sock = socket.create_connection((nvecd.host, nvecd.tcp_port), timeout=timeout)
    sock.settimeout(timeout)
    sock.sendall(f"AUTH {nvecd.password}\r\n".encode())
    response = _read_until(sock, b"\r\n")
    assert response.startswith((b"OK", b"+OK")), response
    return sock


@pytest.mark.edge_cases
class TestInvalidCommands:
    def test_unknown_command(self, nvecd):
        resp = nvecd.tcp_command("FOOBAR")
        assert resp is not None
        assert "ERROR" in resp or "ERR" in resp

    def test_blank_line_is_skipped_without_a_response(self, nvecd):
        """A bare line terminator carries no command and is consumed silently.

        The line framer drops empty frames before the parser sees them, so the
        connection stays usable and the next command answers in its place.
        """
        with _raw_session(nvecd) as sock:
            sock.sendall(b"\r\nINFO\r\n")
            response = _read_until(sock, b"\nEND\r\n")
        assert response.startswith(b"OK INFO"), response
        assert b"ERROR" not in response, response

    def test_whitespace_only_command_is_rejected(self, nvecd):
        """A non-empty line that tokenizes to nothing reaches the parser."""
        with _raw_session(nvecd) as sock:
            sock.sendall(b"   \r\n")
            response = _read_until(sock, b"\r\n")
        assert response == b"ERROR Empty command\r\n", response

    def test_partial_command(self, nvecd):
        resp = nvecd.tcp_command("EVENT")
        assert resp is not None
        assert "ERROR" in resp or "ERR" in resp

    def test_sim_missing_args(self, nvecd):
        resp = nvecd.tcp_command("SIM")
        assert resp is not None
        assert "ERROR" in resp or "ERR" in resp
