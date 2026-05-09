"""End-to-end: start a daemon, simulate a CC session, verify device sees the snapshot."""
from __future__ import annotations

import asyncio
import json

import pytest

from claude_buddy import daemon as daemon_mod
from claude_buddy import persistence
from .ble_fake import BleFake


class _StubBleakClient:
    def __init__(self, address, fake: BleFake):
        self.address = address
        self.is_connected = False
        self._fake = fake
        self._cb = None
        fake.set_notify_callback(lambda d: self._cb and self._cb(0, bytearray(d)))
        self._sent_to_device: list[dict] = []

    async def connect(self): self.is_connected = True; return True
    async def disconnect(self): self.is_connected = False; return True
    async def pair(self): return True
    async def start_notify(self, u, c): self._cb = c
    async def stop_notify(self, u): self._cb = None
    async def write_gatt_char(self, u, d, response=False):
        # Capture daemon -> device traffic.
        for raw in d.split(b"\n"):
            if not raw.strip():
                continue
            try:
                self._sent_to_device.append(json.loads(raw))
            except json.JSONDecodeError:
                pass
        await self._fake.write_rx(bytes(d))


@pytest.fixture
async def daemon_and_capture(tmp_path, monkeypatch):
    monkeypatch.setenv("HOME", str(tmp_path / "home"))
    state_dir = tmp_path / "home" / ".claude-buddy"
    state_dir.mkdir(parents=True)
    persistence.save_config(state_dir / "config.json", persistence.Config(
        device_address="AA", device_name="Buddy",
    ))
    fake = BleFake()
    captured: list[dict] = []

    def factory(addr):
        client = _StubBleakClient(addr, fake)
        client._sent_to_device = captured
        return client

    sock_path = tmp_path / "buddy.sock"
    d = daemon_mod.Daemon(state_dir=state_dir, sock_path=sock_path, ble_factory=factory)
    task = asyncio.create_task(d.run())
    await asyncio.sleep(0.1)
    yield d, captured, sock_path
    d.shutdown_event.set()
    await asyncio.wait_for(task, timeout=2.0)


async def _send(sock_path, msg):
    reader, writer = await asyncio.open_unix_connection(str(sock_path))
    writer.write(json.dumps(msg).encode() + b"\n")
    await writer.drain()
    writer.close()
    await writer.wait_closed()


class TestEndToEnd:
    async def test_session_lifecycle_emits_heartbeats(self, daemon_and_capture):
        d, captured, sock_path = daemon_and_capture
        await _send(sock_path, {"op": "event", "event": "SessionStart",
                                "session_id": "s1", "transcript_path": ""})
        await _send(sock_path, {"op": "event", "event": "UserPromptSubmit",
                                "session_id": "s1", "prompt": "fix it"})
        await asyncio.sleep(0.2)
        # At least one heartbeat with msg matching the user prompt.
        assert any(m.get("msg") == "user: fix it" for m in captured)
