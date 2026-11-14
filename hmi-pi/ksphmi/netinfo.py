"""This display's network identity, for the waiting screen: which address the PC (and SSH) can reach it at."""
from __future__ import annotations

import socket
import subprocess
import time


def route_address(target: str) -> str:
    """The local IPv4 address used to reach ``target`` (a UDP connect sends nothing). Empty when unknown."""
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
            s.connect((target, 9))
            return s.getsockname()[0]
    except OSError:
        return ""


def all_addresses() -> list[str]:
    """Every global IPv4 address of this machine (``hostname -I`` on Linux), loopback excluded."""
    try:
        out = subprocess.run(["hostname", "-I"], capture_output=True, text=True, timeout=2).stdout
        found = [a for a in out.split() if a.count(".") == 3]
    except (OSError, subprocess.SubprocessError):
        found = []
    if not found:  # Windows / no hostname -I: whatever the name resolves to
        try:
            found = [a for a in socket.gethostbyname_ex(socket.gethostname())[2] if a.count(".") == 3]
        except OSError:
            found = []
    return [a for a in found if not a.startswith("127.")]


class NetInfo:
    """Cached, refreshed every ``every`` seconds (DHCP may change the address while the HMI runs)."""

    def __init__(self, server: str, every: float = 15.0) -> None:
        self.server = server
        self.every = every
        self._at = -1e9
        self.hostname = socket.gethostname()
        self.primary = ""
        self.others: list[str] = []

    def refresh(self, now: float | None = None) -> None:
        now = time.monotonic() if now is None else now
        if now - self._at < self.every:
            return
        self._at = now
        primary = route_address(self.server)
        everything = all_addresses()
        if not primary or primary.startswith("127."):  # server is localhost (PC development)
            primary = everything[0] if everything else ""
        self.primary = primary
        self.others = [a for a in everything if a != primary]

    def line(self) -> str:
        """``This display: 192.168.100.48 (kspKontrollerPi), also 192.168.100.56``"""
        self.refresh()
        text = "This display: %s (%s)" % (self.primary or "no network address", self.hostname)
        if self.others:
            text += ", also " + ", ".join(self.others)
        return text
