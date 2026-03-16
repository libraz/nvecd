"""nvecd TCP client for E2E tests."""

from __future__ import annotations

import socket
from typing import Any


class NvecdClient:
    """nvecd client supporting TCP protocol."""

    def __init__(self, host: str = "127.0.0.1", tcp_port: int = 11117) -> None:
        self.host = host
        self.tcp_port = tcp_port

    def ping(self) -> bool:
        """Check if nvecd accepts TCP connections."""
        try:
            resp = self.tcp_command("INFO")
            return resp is not None
        except Exception:
            return False

    def tcp_command(self, cmd: str, timeout: float = 30.0) -> str | None:
        """Send a TCP command and return the response string."""
        try:
            sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            sock.settimeout(timeout)
            sock.connect((self.host, self.tcp_port))
            sock.sendall((cmd + "\r\n").encode("utf-8"))

            data = b""
            while True:
                chunk = sock.recv(65536)
                if not chunk:
                    break
                data += chunk
                if data.endswith(b"\r\n") or data.endswith(b"\n"):
                    break

            sock.close()
            return data.decode("utf-8", errors="ignore").strip()
        except Exception:
            return None

    def event(self, ctx: str, item: str, score: int | float, action: str = "ADD") -> str | None:
        """Send an EVENT command."""
        return self.tcp_command(f"EVENT {ctx} {action} {item} {score}")

    def vecset(self, item: str, vector: list[float]) -> str | None:
        """Send a VECSET command."""
        vec_str = " ".join(str(v) for v in vector)
        return self.tcp_command(f"VECSET {item} {vec_str}")

    def sim(self, item: str, top_k: int = 10, mode: str = "fusion") -> str | None:
        """Send a SIM command."""
        return self.tcp_command(f"SIM {item} {top_k} using={mode}")

    def simv(self, vector: list[float], top_k: int = 10) -> str | None:
        """Send a SIMV command."""
        vec_str = " ".join(str(v) for v in vector)
        return self.tcp_command(f"SIMV {top_k} {vec_str}")

    def info(self) -> dict[str, Any]:
        """Execute INFO command and parse result."""
        resp = self.tcp_command("INFO")
        if not resp:
            return {}
        result: dict[str, Any] = {}
        for line in resp.split("\n"):
            line = line.strip()
            if ":" in line and not line.startswith("#"):
                key, _, value = line.partition(":")
                key = key.strip()
                value = value.strip()
                try:
                    result[key] = int(value)
                except ValueError:
                    try:
                        result[key] = float(value)
                    except ValueError:
                        result[key] = value
        return result

    def cache_stats(self) -> str | None:
        """Get cache statistics."""
        return self.tcp_command("CACHE STATS")

    def cache_clear(self) -> bool:
        """Clear the cache."""
        resp = self.tcp_command("CACHE CLEAR")
        return resp is not None and "OK" in resp

    def cache_enable(self) -> bool:
        """Enable the cache."""
        resp = self.tcp_command("CACHE ENABLE")
        return resp is not None and "OK" in resp

    def cache_disable(self) -> bool:
        """Disable the cache."""
        resp = self.tcp_command("CACHE DISABLE")
        return resp is not None and "OK" in resp
