from __future__ import annotations

import asyncio
import json

import pytest

from claude_buddy import status


async def _fake_daemon(sock_path):
    async def handle(reader, writer):
        await reader.readline()
        writer.write(json.dumps({"ok": True, "data": {"ble": "connected", "sessions": 0}}).encode() + b"\n")
        await writer.drain()
        writer.close()
        await writer.wait_closed()
    server = await asyncio.start_unix_server(handle, path=str(sock_path))
    return server


class TestStatus:
    async def test_query_running_daemon(self, tmp_path, capsys):
        sock_path = tmp_path / "buddy.sock"
        server = await _fake_daemon(sock_path)
        try:
            await status.query(sock_path)
        finally:
            server.close()
            await server.wait_closed()
        captured = capsys.readouterr()
        out = json.loads(captured.out)
        assert out["data"]["ble"] == "connected"

    async def test_no_daemon(self, tmp_path, capsys):
        sock_path = tmp_path / "no.sock"
        await status.query(sock_path)
        captured = capsys.readouterr()
        out = json.loads(captured.out)
        assert out["ok"] is False
        assert out["error"] == "not_running"

    async def test_daemon_hangs_returns_no_reply(self, tmp_path, capsys, monkeypatch):
        sock_path = tmp_path / "buddy.sock"

        async def stall_handler(reader, writer):
            await reader.readline()
            # Never reply — wait for client to disconnect instead of sleeping a
            # fixed duration, so server.wait_closed() returns promptly.
            try:
                await reader.read()  # returns b"" on EOF when client closes
            except (asyncio.CancelledError, ConnectionError):
                pass
            writer.close()
            await writer.wait_closed()

        # Patch asyncio.wait_for in status to use a tiny timeout.
        original_wait_for = asyncio.wait_for
        async def short_wait_for(coro, timeout):
            return await original_wait_for(coro, 0.3)
        monkeypatch.setattr("claude_buddy.status.asyncio.wait_for", short_wait_for)

        server = await asyncio.start_unix_server(stall_handler, path=str(sock_path))
        try:
            await status.query(sock_path)
        finally:
            server.close()
            await server.wait_closed()
        captured = capsys.readouterr()
        out = json.loads(captured.out)
        assert out["ok"] is False
        assert out["error"] == "no_reply"
