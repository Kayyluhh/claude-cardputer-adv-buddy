from __future__ import annotations

import asyncio
import base64
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


class _AcksAllFake(BleFake):
    """Auto-acks every char_begin/file/chunk/file_end/char_end with ok:true."""
    async def _react(self, msg: dict) -> None:
        cmd = msg.get("cmd")
        if cmd in ("char_begin", "file", "file_end", "char_end"):
            await self.notify({"ack": cmd, "ok": True})
        elif cmd == "chunk":
            await self.notify({"ack": "chunk", "ok": True, "n": len(base64.b64decode(msg["d"]))})
        else:
            await super()._react(msg)


class _DeclinesCharBeginFake(BleFake):
    """Replies ok:false to char_begin to test rejection."""
    async def _react(self, msg: dict) -> None:
        cmd = msg.get("cmd")
        if cmd == "char_begin":
            await self.notify({"ack": "char_begin", "ok": False})
        else:
            await super()._react(msg)


class _DeclinesFileFake(BleFake):
    """Acks char_begin ok but rejects file with ok:false."""
    async def _react(self, msg: dict) -> None:
        cmd = msg.get("cmd")
        if cmd == "char_begin":
            await self.notify({"ack": "char_begin", "ok": True})
        elif cmd == "file":
            await self.notify({"ack": "file", "ok": False})
        else:
            await super()._react(msg)


@pytest.fixture
async def running_daemon_with_push(tmp_path, monkeypatch):
    monkeypatch.setenv("HOME", str(tmp_path / "home"))
    state_dir = tmp_path / "home" / ".claude-buddy"
    state_dir.mkdir(parents=True)
    persistence.save_config(state_dir / "config.json", persistence.Config(
        device_address="AA", device_name="FakeBuddy",
    ))
    fake = _AcksAllFake()
    sock_path = tmp_path / "buddy.sock"
    d = daemon_mod.Daemon(
        state_dir=state_dir, sock_path=sock_path,
        ble_factory=lambda addr: _StubBleakClient(addr, fake),
    )
    task = asyncio.create_task(d.run())
    await asyncio.sleep(0.1)
    yield d, fake, sock_path, tmp_path
    d.shutdown_event.set()
    await asyncio.wait_for(task, timeout=2.0)


class TestFolderPush:
    async def test_pushes_two_files(self, running_daemon_with_push):
        d, fake, sock_path, tmp_path = running_daemon_with_push
        folder = tmp_path / "char"
        folder.mkdir()
        (folder / "manifest.json").write_text(json.dumps({"name": "bufo"}))
        (folder / "idle.gif").write_bytes(b"GIF89a fake")

        # Send push request, collect streamed responses.
        reader, writer = await asyncio.open_unix_connection(str(sock_path))
        writer.write(json.dumps({"op": "push", "path": str(folder)}).encode() + b"\n")
        await writer.drain()
        stages = []
        while True:
            line = await asyncio.wait_for(reader.readline(), timeout=5.0)
            if not line:
                break
            stages.append(json.loads(line))
            if stages[-1].get("stage") in ("done", "error"):
                break
        writer.close()
        await writer.wait_closed()
        assert stages[-1] == {"stage": "done"}
        # Sequence: begin, file, chunks*, file_end, file, chunks*, file_end, end
        assert stages[0]["stage"] == "begin"
        assert any(s.get("stage") == "file" and s.get("name") == "manifest.json" for s in stages)
        assert any(s.get("stage") == "file" and s.get("name") == "idle.gif" for s in stages)

    async def test_rejects_oversize_folder(self, running_daemon_with_push):
        d, fake, sock_path, tmp_path = running_daemon_with_push
        folder = tmp_path / "big"
        folder.mkdir()
        (folder / "huge.bin").write_bytes(b"x" * (2 * 1024 * 1024))  # 2 MB > 1.8 MB cap

        reader, writer = await asyncio.open_unix_connection(str(sock_path))
        writer.write(json.dumps({"op": "push", "path": str(folder)}).encode() + b"\n")
        await writer.drain()
        line = await asyncio.wait_for(reader.readline(), timeout=2.0)
        writer.close()
        await writer.wait_closed()
        msg = json.loads(line)
        assert msg["stage"] == "error"
        assert "1.8" in msg["msg"] or "size" in msg["msg"].lower()

    async def test_device_declines_char_begin(self, tmp_path, monkeypatch):
        monkeypatch.setenv("HOME", str(tmp_path / "home"))
        state_dir = tmp_path / "home" / ".claude-buddy"
        state_dir.mkdir(parents=True)
        persistence.save_config(state_dir / "config.json", persistence.Config(
            device_address="AA", device_name="FakeBuddy",
        ))
        fake = _DeclinesCharBeginFake()
        sock_path = tmp_path / "buddy.sock"
        d = daemon_mod.Daemon(
            state_dir=state_dir, sock_path=sock_path,
            ble_factory=lambda addr: _StubBleakClient(addr, fake),
        )
        task = asyncio.create_task(d.run())
        await asyncio.sleep(0.1)
        try:
            folder = tmp_path / "char"
            folder.mkdir()
            (folder / "a.txt").write_bytes(b"x")

            reader, writer = await asyncio.open_unix_connection(str(sock_path))
            writer.write(json.dumps({"op": "push", "path": str(folder)}).encode() + b"\n")
            await writer.drain()
            line = await asyncio.wait_for(reader.readline(), timeout=5.0)
            writer.close()
            await writer.wait_closed()
            msg = json.loads(line)
            assert msg["stage"] == "error"
            assert "declined" in msg["msg"].lower()
        finally:
            d.shutdown_event.set()
            await asyncio.wait_for(task, timeout=2.0)

    async def test_device_silent_after_char_begin_timeout(self, tmp_path, monkeypatch):
        monkeypatch.setenv("HOME", str(tmp_path / "home"))
        state_dir = tmp_path / "home" / ".claude-buddy"
        state_dir.mkdir(parents=True)
        persistence.save_config(state_dir / "config.json", persistence.Config(
            device_address="AA", device_name="FakeBuddy",
        ))

        class _SilentAfterCharBegin(BleFake):
            """Acks char_begin then never replies again."""
            async def _react(self, msg: dict) -> None:
                cmd = msg.get("cmd")
                if cmd == "char_begin":
                    await self.notify({"ack": "char_begin", "ok": True})
                # Drop everything else - simulates device hang.

        fake = _SilentAfterCharBegin()
        sock_path = tmp_path / "buddy.sock"
        d = daemon_mod.Daemon(
            state_dir=state_dir, sock_path=sock_path,
            ble_factory=lambda addr: _StubBleakClient(addr, fake),
        )
        task = asyncio.create_task(d.run())
        await asyncio.sleep(0.1)
        try:
            folder = tmp_path / "char"
            folder.mkdir()
            (folder / "a.txt").write_bytes(b"x" * 50)

            reader, writer = await asyncio.open_unix_connection(str(sock_path))
            writer.write(json.dumps({"op": "push", "path": str(folder)}).encode() + b"\n")
            await writer.drain()
            stages = []
            # Read until we see done or error. The daemon's wait_ack default timeout
            # is 5s, so error will land within ~6s.
            while True:
                line = await asyncio.wait_for(reader.readline(), timeout=8.0)
                if not line:
                    break
                stages.append(json.loads(line))
                if stages[-1].get("stage") in ("done", "error"):
                    break
            writer.close()
            await writer.wait_closed()
            assert stages[-1]["stage"] == "error"
            assert "timeout" in stages[-1]["msg"].lower()
        finally:
            d.shutdown_event.set()
            await asyncio.wait_for(task, timeout=2.0)

    async def test_device_declines_file(self, tmp_path, monkeypatch):
        monkeypatch.setenv("HOME", str(tmp_path / "home"))
        state_dir = tmp_path / "home" / ".claude-buddy"
        state_dir.mkdir(parents=True)
        persistence.save_config(state_dir / "config.json", persistence.Config(
            device_address="AA", device_name="FakeBuddy",
        ))
        fake = _DeclinesFileFake()
        sock_path = tmp_path / "buddy.sock"
        d = daemon_mod.Daemon(
            state_dir=state_dir, sock_path=sock_path,
            ble_factory=lambda addr: _StubBleakClient(addr, fake),
        )
        task = asyncio.create_task(d.run())
        await asyncio.sleep(0.1)
        try:
            folder = tmp_path / "char"
            folder.mkdir()
            (folder / "a.txt").write_bytes(b"x")

            reader, writer = await asyncio.open_unix_connection(str(sock_path))
            writer.write(json.dumps({"op": "push", "path": str(folder)}).encode() + b"\n")
            await writer.drain()
            stages = []
            while True:
                line = await asyncio.wait_for(reader.readline(), timeout=5.0)
                if not line:
                    break
                stages.append(json.loads(line))
                if stages[-1].get("stage") in ("done", "error"):
                    break
            writer.close()
            await writer.wait_closed()
            assert stages[-1]["stage"] == "error"
            assert "rejected" in stages[-1]["msg"].lower()
        finally:
            d.shutdown_event.set()
            await asyncio.wait_for(task, timeout=2.0)
