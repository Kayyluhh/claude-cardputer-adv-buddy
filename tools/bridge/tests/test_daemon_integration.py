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

    async def connect(self): self.is_connected = True; return True
    async def disconnect(self): self.is_connected = False; return True
    async def pair(self): return True
    async def start_notify(self, u, c): self._cb = c
    async def stop_notify(self, u): self._cb = None
    async def write_gatt_char(self, u, d, response=False):
        await self._fake.write_rx(bytes(d))


@pytest.fixture
async def running_daemon(tmp_path, monkeypatch):
    monkeypatch.setenv("HOME", str(tmp_path / "home"))
    state_dir = tmp_path / "home" / ".claude-buddy"
    state_dir.mkdir(parents=True)
    persistence.save_config(state_dir / "config.json", persistence.Config(
        device_address="AA:BB:CC:DD:EE:FF", device_name="FakeBuddy",
    ))

    fake = BleFake()
    sock_path = tmp_path / "buddy.sock"
    d = daemon_mod.Daemon(
        state_dir=state_dir,
        sock_path=sock_path,
        ble_factory=lambda addr: _StubBleakClient(addr, fake),
    )
    task = asyncio.create_task(d.run())
    await asyncio.sleep(0.1)  # give it time to start
    yield d, fake, sock_path
    d.shutdown_event.set()
    await asyncio.wait_for(task, timeout=2.0)


async def _hook_send(sock_path, msg, expect_reply=False):
    reader, writer = await asyncio.open_unix_connection(str(sock_path))
    writer.write(json.dumps(msg).encode() + b"\n")
    await writer.drain()
    if expect_reply:
        line = await asyncio.wait_for(reader.readline(), timeout=2.0)
        writer.close()
        await writer.wait_closed()
        return json.loads(line)
    writer.close()
    await writer.wait_closed()
    return None


class TestDaemon:
    async def test_session_start_emits_snapshot(self, running_daemon):
        _, fake, sock_path = running_daemon
        await _hook_send(sock_path, {"op": "event", "event": "SessionStart",
                                     "session_id": "s1", "transcript_path": "/tmp/x.jsonl"})
        # Wait for snapshot to land on the wire.
        await asyncio.sleep(0.1)
        # Daemon -> device traffic lands in fake._from_host.
        # Expect at minimum: time sync + at least one heartbeat snapshot triggered
        # by the SessionStart event. The heartbeat has a "total" key.
        msgs = []
        while fake.history_size():
            msgs.append(await fake.received_from_host())
        snapshots = [m for m in msgs if "total" in m]
        assert len(snapshots) >= 1, f"expected at least one heartbeat, got {msgs}"
        # The snapshot should reflect that we have one session.
        assert snapshots[-1]["total"] == 1
        assert snapshots[-1]["msg"] == "1 sessions"

    async def test_pretooluse_round_trip_via_device_ack(self, running_daemon):
        d, fake, sock_path = running_daemon
        # Register the session first.
        await _hook_send(sock_path, {"op": "event", "event": "SessionStart",
                                     "session_id": "s1", "transcript_path": "/tmp/x.jsonl"})
        await asyncio.sleep(0.05)

        # In parallel: hook awaits decision, device sends ack.
        async def hook_call():
            return await _hook_send(sock_path, {
                "op": "prehook", "session_id": "s1", "tool_name": "Bash",
                "tool_input": {"command": "ls"}, "tool_use_id": "t1",
            }, expect_reply=True)

        async def device_ack():
            await asyncio.sleep(0.1)  # ensure prehook is registered
            await fake.notify({"cmd": "permission", "id": "t1", "decision": "once"})

        results = await asyncio.gather(hook_call(), device_ack())
        assert results[0]["decision"] == "allow"

    async def test_pretooluse_timeout_falls_through(self, running_daemon):
        d, fake, sock_path = running_daemon
        await _hook_send(sock_path, {"op": "event", "event": "SessionStart",
                                     "session_id": "s1", "transcript_path": "/tmp/x.jsonl"})
        await asyncio.sleep(0.05)

        # Override timeout to short value for the test.
        d.permission_timeout_s = 0.5

        reply = await _hook_send(sock_path, {
            "op": "prehook", "session_id": "s1", "tool_name": "Bash",
            "tool_input": {}, "tool_use_id": "t-timeout",
        }, expect_reply=True)
        assert reply["decision"] == "ask"

    async def test_muted_session_skips_device_for_prehook(self, running_daemon):
        d, fake, sock_path = running_daemon
        await _hook_send(sock_path, {"op": "event", "event": "SessionStart",
                                     "session_id": "s1", "transcript_path": "/tmp/x.jsonl"})
        await asyncio.sleep(0.05)
        d.state.muted_sessions.add("s1")

        reply = await _hook_send(sock_path, {
            "op": "prehook", "session_id": "s1", "tool_name": "Bash",
            "tool_input": {}, "tool_use_id": "t1",
        }, expect_reply=True)
        # Muted -> immediate "ask".
        assert reply["decision"] == "ask"
