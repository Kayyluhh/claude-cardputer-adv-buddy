from __future__ import annotations

import asyncio
import json
import subprocess
import sys
from pathlib import Path

import pytest


HOOK_CMD = [sys.executable, "-m", "claude_buddy.hook"]


def _run_hook(stdin_payload: dict, env_overrides: dict[str, str]) -> subprocess.CompletedProcess:
    import os
    env = os.environ.copy()
    env.update(env_overrides)
    return subprocess.run(
        HOOK_CMD,
        input=json.dumps(stdin_payload).encode(),
        capture_output=True,
        env=env,
        timeout=10,
    )


class TestNoDaemon:
    def test_pretooluse_no_daemon_returns_ask(self, tmp_path):
        sock = tmp_path / "no-such.sock"
        result = _run_hook(
            {"hook_event_name": "PreToolUse", "session_id": "s1",
             "tool_name": "Bash", "tool_input": {}, "tool_use_id": "t1"},
            {"BUDDY_SOCKET": str(sock)},
        )
        assert result.returncode == 0
        out = json.loads(result.stdout)
        assert out["hookSpecificOutput"]["permissionDecision"] == "ask"

    def test_other_event_no_daemon_silent(self, tmp_path):
        sock = tmp_path / "no-such.sock"
        result = _run_hook(
            {"hook_event_name": "Stop", "session_id": "s1"},
            {"BUDDY_SOCKET": str(sock)},
        )
        assert result.returncode == 0
        assert result.stdout == b""


class TestProjectDisabled:
    def test_settings_local_disable_short_circuits(self, tmp_path):
        cwd = tmp_path / "proj"
        (cwd / ".claude").mkdir(parents=True)
        (cwd / ".claude" / "settings.local.json").write_text(json.dumps({"_buddy_disabled": True}))
        result = subprocess.run(
            HOOK_CMD,
            input=json.dumps({"hook_event_name": "PreToolUse", "session_id": "s1",
                              "tool_name": "Bash", "tool_input": {}, "tool_use_id": "t1",
                              "cwd": str(cwd)}).encode(),
            capture_output=True,
            cwd=str(cwd),
            timeout=10,
        )
        assert result.returncode == 0
        # No stdout = CC behaves as if hook didn't fire.
        assert result.stdout == b""


class TestWithFakeDaemon:
    async def test_pretooluse_round_trip(self, tmp_path):
        sock = tmp_path / "buddy.sock"

        async def serve():
            srv = await asyncio.start_unix_server(_echo_allow, path=str(sock))
            await srv.serve_forever()

        async def _echo_allow(reader, writer):
            await reader.readline()
            writer.write(b'{"decision":"allow","reason":"ok"}\n')
            await writer.drain()
            writer.close()
            await writer.wait_closed()

        server_task = asyncio.create_task(serve())
        await asyncio.sleep(0.05)

        try:
            proc = await asyncio.create_subprocess_exec(
                *HOOK_CMD,
                stdin=asyncio.subprocess.PIPE,
                stdout=asyncio.subprocess.PIPE,
                stderr=asyncio.subprocess.PIPE,
                env={**__import__("os").environ, "BUDDY_SOCKET": str(sock)},
            )
            stdout, _ = await proc.communicate(json.dumps({
                "hook_event_name": "PreToolUse",
                "session_id": "s1", "tool_name": "Bash",
                "tool_input": {"command": "ls"}, "tool_use_id": "t1",
            }).encode())
            assert proc.returncode == 0
            out = json.loads(stdout)
            assert out["hookSpecificOutput"]["permissionDecision"] == "allow"
        finally:
            server_task.cancel()
            try:
                await server_task
            except asyncio.CancelledError:
                pass


class TestDaemonResponseHandling:
    async def test_daemon_returns_garbage_falls_through_to_ask(self, tmp_path):
        sock = tmp_path / "buddy.sock"

        async def garbage_handler(reader, writer):
            await reader.readline()
            writer.write(b"not json\n")
            await writer.drain()
            writer.close()
            await writer.wait_closed()

        srv = await asyncio.start_unix_server(garbage_handler, path=str(sock))
        try:
            proc = await asyncio.create_subprocess_exec(
                *HOOK_CMD,
                stdin=asyncio.subprocess.PIPE,
                stdout=asyncio.subprocess.PIPE,
                stderr=asyncio.subprocess.PIPE,
                env={**__import__("os").environ, "BUDDY_SOCKET": str(sock)},
            )
            stdout, _ = await proc.communicate(json.dumps({
                "hook_event_name": "PreToolUse", "session_id": "s1",
                "tool_name": "Bash", "tool_input": {}, "tool_use_id": "t1",
            }).encode())
            assert proc.returncode == 0
            out = json.loads(stdout)
            assert out["hookSpecificOutput"]["permissionDecision"] == "ask"
        finally:
            srv.close()
            await srv.wait_closed()

    async def test_daemon_invalid_decision_value_falls_through_to_ask(self, tmp_path):
        sock = tmp_path / "buddy.sock"

        async def invalid_handler(reader, writer):
            await reader.readline()
            writer.write(b'{"decision":"maybe","reason":"vague"}\n')
            await writer.drain()
            writer.close()
            await writer.wait_closed()

        srv = await asyncio.start_unix_server(invalid_handler, path=str(sock))
        try:
            proc = await asyncio.create_subprocess_exec(
                *HOOK_CMD,
                stdin=asyncio.subprocess.PIPE,
                stdout=asyncio.subprocess.PIPE,
                stderr=asyncio.subprocess.PIPE,
                env={**__import__("os").environ, "BUDDY_SOCKET": str(sock)},
            )
            stdout, _ = await proc.communicate(json.dumps({
                "hook_event_name": "PreToolUse", "session_id": "s1",
                "tool_name": "Bash", "tool_input": {}, "tool_use_id": "t1",
            }).encode())
            assert proc.returncode == 0
            out = json.loads(stdout)
            assert out["hookSpecificOutput"]["permissionDecision"] == "ask"
        finally:
            srv.close()
            await srv.wait_closed()

    async def test_daemon_supplies_reason_threads_through(self, tmp_path):
        sock = tmp_path / "buddy.sock"

        async def reason_handler(reader, writer):
            await reader.readline()
            writer.write(b'{"decision":"deny","reason":"away from desk"}\n')
            await writer.drain()
            writer.close()
            await writer.wait_closed()

        srv = await asyncio.start_unix_server(reason_handler, path=str(sock))
        try:
            proc = await asyncio.create_subprocess_exec(
                *HOOK_CMD,
                stdin=asyncio.subprocess.PIPE,
                stdout=asyncio.subprocess.PIPE,
                stderr=asyncio.subprocess.PIPE,
                env={**__import__("os").environ, "BUDDY_SOCKET": str(sock)},
            )
            stdout, _ = await proc.communicate(json.dumps({
                "hook_event_name": "PreToolUse", "session_id": "s1",
                "tool_name": "Bash", "tool_input": {}, "tool_use_id": "t1",
            }).encode())
            assert proc.returncode == 0
            out = json.loads(stdout)
            assert out["hookSpecificOutput"]["permissionDecision"] == "deny"
            assert out["hookSpecificOutput"]["permissionDecisionReason"] == "away from desk"
        finally:
            srv.close()
            await srv.wait_closed()


class TestSendEvent:
    async def test_event_payload_shape_reaches_daemon(self, tmp_path):
        sock = tmp_path / "buddy.sock"
        received: list[dict] = []

        async def event_handler(reader, writer):
            line = await reader.readline()
            received.append(json.loads(line))
            writer.close()
            await writer.wait_closed()

        srv = await asyncio.start_unix_server(event_handler, path=str(sock))
        try:
            proc = await asyncio.create_subprocess_exec(
                *HOOK_CMD,
                stdin=asyncio.subprocess.PIPE,
                stdout=asyncio.subprocess.PIPE,
                stderr=asyncio.subprocess.PIPE,
                env={**__import__("os").environ, "BUDDY_SOCKET": str(sock)},
            )
            stdout, _ = await proc.communicate(json.dumps({
                "hook_event_name": "Stop", "session_id": "s1",
                "stop_reason": "end_turn", "transcript_path": "/tmp/t.jsonl",
            }).encode())
            assert proc.returncode == 0
            assert stdout == b""
            # Wait briefly for daemon receipt.
            await asyncio.sleep(0.05)
            assert len(received) == 1
            msg = received[0]
            assert msg["op"] == "event"
            assert msg["event"] == "Stop"
            assert msg["session_id"] == "s1"
            assert msg["stop_reason"] == "end_turn"
        finally:
            srv.close()
            await srv.wait_closed()
