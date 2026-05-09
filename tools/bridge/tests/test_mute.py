from __future__ import annotations

import asyncio
import json
import os

import pytest

from claude_buddy import mute


async def _fake_daemon(sock_path):
    async def handle(reader, writer):
        line = await reader.readline()
        msg = json.loads(line)
        if msg.get("op") == "current_session":
            writer.write(json.dumps({"session_id": "from-daemon"}).encode() + b"\n")
        elif msg.get("op") == "mute":
            writer.write(json.dumps({"ok": True}).encode() + b"\n")
        elif msg.get("op") == "unmute":
            writer.write(json.dumps({"ok": True}).encode() + b"\n")
        await writer.drain()
        writer.close()
        await writer.wait_closed()
    return await asyncio.start_unix_server(handle, path=str(sock_path))


class TestMute:
    async def test_uses_env_var_when_set(self, tmp_path, monkeypatch, capsys):
        monkeypatch.setenv("CLAUDE_SESSION_ID", "from-env")
        sock_path = tmp_path / "buddy.sock"
        server = await _fake_daemon(sock_path)
        try:
            rc = await mute.run(action="mute", sock_path=sock_path, cwd="/tmp")
        finally:
            server.close()
            await server.wait_closed()
        assert rc == 0
        captured = capsys.readouterr()
        out = json.loads(captured.out)
        assert out["session_id"] == "from-env"

    async def test_falls_back_to_daemon_lookup(self, tmp_path, monkeypatch, capsys):
        monkeypatch.delenv("CLAUDE_SESSION_ID", raising=False)
        sock_path = tmp_path / "buddy.sock"
        server = await _fake_daemon(sock_path)
        try:
            rc = await mute.run(action="mute", sock_path=sock_path, cwd="/tmp")
        finally:
            server.close()
            await server.wait_closed()
        assert rc == 0
        out = json.loads(capsys.readouterr().out)
        assert out["session_id"] == "from-daemon"

    async def test_unmute_path(self, tmp_path, monkeypatch, capsys):
        monkeypatch.setenv("CLAUDE_SESSION_ID", "x")
        sock_path = tmp_path / "buddy.sock"
        server = await _fake_daemon(sock_path)
        try:
            rc = await mute.run(action="unmute", sock_path=sock_path, cwd="/tmp")
        finally:
            server.close()
            await server.wait_closed()
        assert rc == 0

    async def test_no_daemon(self, tmp_path, monkeypatch, capsys):
        monkeypatch.setenv("CLAUDE_SESSION_ID", "x")
        sock_path = tmp_path / "no.sock"
        rc = await mute.run(action="mute", sock_path=sock_path, cwd="/tmp")
        assert rc == 1
        out = json.loads(capsys.readouterr().out)
        assert out["ok"] is False
