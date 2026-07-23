"""Persistent, length-framed nvecd TCP client for E2E tests."""

from __future__ import annotations

import socket
from typing import Any


_END_TERMINATED = ("INFO", "CONFIG", "CACHE STATS", "DUMP INFO", "DUMP STATUS")


def _line_end(data: bytes, start: int = 0) -> int | None:
    position = data.find(b"\n", start)
    return None if position < 0 else position + 1


def _line(data: bytes, start: int, end: int) -> bytes:
    return data[start : end - 1].removesuffix(b"\r")


def _decimal(value: bytes) -> int | None:
    try:
        return int(value.decode("ascii"))
    except (UnicodeDecodeError, ValueError):
        return None


def _complete_value_length(data: bytes, start: int = 0, depth: int = 0) -> int | None:
    line_end = _line_end(data, start)
    if line_end is None:
        return None
    header = _line(data, start, line_end)
    if not header or depth > 8:
        return line_end
    if header.startswith(b"$"):
        length = _decimal(header[1:])
        if length is None or length < -1:
            return line_end
        if length == -1:
            return line_end
        payload_end = line_end + length
        if payload_end >= len(data):
            return None
        if data[payload_end : payload_end + 2] == b"\r\n":
            return payload_end + 2
        if data[payload_end : payload_end + 1] == b"\n":
            return payload_end + 1
        return line_end
    if header.startswith(b"*"):
        count = _decimal(header[1:])
        if count is None or count < -1:
            return line_end
        cursor = line_end
        for _ in range(max(0, count)):
            cursor = _complete_value_length(data, cursor, depth + 1)
            if cursor is None:
                return None
        return cursor
    return line_end


def _complete_response_length(data: bytes, command: str, debug_mode: bool = False) -> int | None:
    first_end = _line_end(data)
    if first_end is None:
        return None
    header = _line(data, 0, first_end)
    is_error = header.startswith((b"ERROR", b"-ERR", b"(error)"))
    upper = command.strip().upper()
    end_terminated = any(upper == prefix or upper.startswith(prefix + " ") for prefix in _END_TERMINATED)
    if end_terminated and not is_error:
        for terminator in (b"\nEND\r\n", b"\nEND\n"):
            position = data.find(terminator)
            if position >= 0:
                return position + len(terminator)
        return None
    if header.startswith(b"OK RESULTS "):
        count = _decimal(header[len(b"OK RESULTS ") :])
        if count is None or count < 0:
            return first_end
        cursor = first_end
        for _ in range(count):
            cursor = _line_end(data, cursor)
            if cursor is None:
                return None
        if not debug_mode:
            return cursor
        debug_end = _line_end(data, cursor)
        if debug_end is None:
            return None
        debug_header = _line(data, cursor, debug_end)
        if not debug_header.startswith(b"# DEBUG "):
            return debug_end
        field_count = _decimal(debug_header[len(b"# DEBUG ") :])
        if field_count is None or field_count < 0:
            return debug_end
        cursor = debug_end
        for _ in range(field_count):
            cursor = _line_end(data, cursor)
            if cursor is None:
                return None
        return cursor
    return _complete_value_length(data)


class NvecdClient:
    """Persistent nvecd client with command-aware response framing."""

    def __init__(
        self,
        host: str = "127.0.0.1",
        tcp_port: int = 11117,
        password: str | None = None,
    ) -> None:
        self.host = host
        self.tcp_port = tcp_port
        self.password = password
        self._socket: socket.socket | None = None
        self._buffer = b""
        self._debug_mode = False

    def connect(self, timeout: float = 30.0) -> None:
        if self._socket is not None:
            return
        sock = socket.create_connection((self.host, self.tcp_port), timeout=timeout)
        sock.settimeout(timeout)
        self._socket = sock
        if self.password is not None:
            response = self._round_trip(f"AUTH {self.password}")
            if response.startswith(("ERROR", "-ERR", "(error)")):
                self.close()
                raise PermissionError("nvecd authentication failed")

    def close(self) -> None:
        if self._socket is not None:
            self._socket.close()
        self._socket = None
        self._buffer = b""
        self._debug_mode = False

    def ping(self) -> bool:
        """Check both process protocol readiness and response framing."""
        try:
            return self.tcp_command("INFO") is not None
        except Exception:
            return False

    def _round_trip(self, command: str, timeout: float = 30.0) -> str:
        if self._socket is None:
            raise ConnectionError("not connected")
        self._socket.settimeout(timeout)
        self._socket.sendall((command + "\r\n").encode("utf-8"))
        while True:
            length = _complete_response_length(self._buffer, command, self._debug_mode)
            if length is not None:
                response, self._buffer = self._buffer[:length], self._buffer[length:]
                upper = command.strip().upper()
                if upper == "DEBUG ON" and response.startswith((b"OK", b"+OK")):
                    self._debug_mode = True
                elif upper == "DEBUG OFF" and response.startswith((b"OK", b"+OK")):
                    self._debug_mode = False
                return response.decode("utf-8", errors="replace").strip()
            chunk = self._socket.recv(65536)
            if not chunk:
                raise ConnectionError("server closed an incomplete response")
            self._buffer += chunk

    def tcp_command(self, cmd: str, timeout: float = 30.0) -> str | None:
        """Send a command on the persistent authenticated connection."""
        if not cmd.strip():
            return None
        try:
            self.connect(timeout)
            return self._round_trip(cmd, timeout)
        except (OSError, ConnectionError, PermissionError):
            self.close()
            return None

    def event(self, ctx: str, item: str, score: int | float, action: str = "ADD") -> str | None:
        return self.tcp_command(f"EVENT {ctx} {action} {item} {score}")

    def vecset(self, item: str, vector: list[float]) -> str | None:
        vec_str = " ".join(str(v) for v in vector)
        return self.tcp_command(f"VECSET {item} {vec_str}")

    def sim(self, item: str, top_k: int = 10, mode: str = "fusion") -> str | None:
        return self.tcp_command(f"SIM {item} {top_k} using={mode}")

    def simv(self, vector: list[float], top_k: int = 10) -> str | None:
        vec_str = " ".join(str(v) for v in vector)
        return self.tcp_command(f"SIMV {top_k} {vec_str}")

    def info(self) -> dict[str, Any]:
        resp = self.tcp_command("INFO")
        if not resp:
            return {}
        result: dict[str, Any] = {}
        for line in resp.split("\n"):
            line = line.strip()
            if ":" in line and not line.startswith("#"):
                key, _, value = line.partition(":")
                try:
                    result[key.strip()] = int(value.strip())
                except ValueError:
                    try:
                        result[key.strip()] = float(value.strip())
                    except ValueError:
                        result[key.strip()] = value.strip()
        return result

    def cache_stats(self) -> str | None:
        return self.tcp_command("CACHE STATS")

    def cache_clear(self) -> bool:
        resp = self.tcp_command("CACHE CLEAR")
        return resp is not None and "OK" in resp

    def cache_enable(self) -> bool:
        resp = self.tcp_command("CACHE ENABLE")
        return resp is not None and "OK" in resp

    def cache_disable(self) -> bool:
        resp = self.tcp_command("CACHE DISABLE")
        return resp is not None and "OK" in resp
