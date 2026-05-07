"""In-process fake of the Cardputer's NUS side. Used by daemon integration tests."""
from __future__ import annotations

import asyncio
import json
from typing import Callable


class BleFake:
    """Simulates the device side of the NUS protocol.

    The host writes to the RX characteristic via write_rx(); BleFake parses lines
    and (for {"cmd": "status"}) auto-replies with a canned ack. The host reads
    notifications via the callback set by set_notify_callback().
    """

    def __init__(self) -> None:
        self._from_host: asyncio.Queue[dict] = asyncio.Queue()
        self._notify_cb: Callable[[bytes], None] | None = None
        self._rx_buffer = bytearray()

    def set_notify_callback(self, cb: Callable[[bytes], None]) -> None:
        self._notify_cb = cb

    async def write_rx(self, data: bytes) -> None:
        """Host -> device: line-buffer the bytes, parse complete lines, react to certain cmds."""
        self._rx_buffer.extend(data)
        while b"\n" in self._rx_buffer:
            idx = self._rx_buffer.index(b"\n")
            line = bytes(self._rx_buffer[:idx])
            del self._rx_buffer[: idx + 1]
            if not line:
                continue
            try:
                msg = json.loads(line)
            except json.JSONDecodeError:
                continue
            await self._from_host.put(msg)
            await self._react(msg)

    async def received_from_host(self) -> dict:
        return await self._from_host.get()

    def history_size(self) -> int:
        return self._from_host.qsize()

    async def notify(self, payload: dict) -> None:
        """Device -> host."""
        if self._notify_cb is None:
            return
        self._notify_cb((json.dumps(payload) + "\n").encode("utf-8"))

    async def _react(self, msg: dict) -> None:
        if msg.get("cmd") == "status":
            await self.notify({
                "ack": "status",
                "ok": True,
                "data": {"name": "FakeBuddy", "sec": True,
                         "bat": {"pct": 100, "mV": 4200, "mA": 0, "usb": True},
                         "sys": {"up": 0, "heap": 100000},
                         "stats": {"appr": 0, "deny": 0, "vel": 0, "nap": 0, "lvl": 0}},
            })
