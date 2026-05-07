from __future__ import annotations

import asyncio
import json
from pathlib import Path

import pytest

from claude_buddy import hook_server


@pytest.fixture
async def server_and_handler(tmp_path):
    sock_path = tmp_path / "buddy.sock"
    received: list[dict] = []

    async def handler(msg: dict, respond):
        received.append(msg)
        if msg.get("op") == "prehook":
            await respond({"decision": "allow", "reason": "test"})

    server = hook_server.HookServer(sock_path, handler)
    await server.start()
    yield sock_path, received
    await server.stop()


async def _send_one(sock_path: Path, msg: dict) -> dict | None:
    reader, writer = await asyncio.open_unix_connection(str(sock_path))
    writer.write(json.dumps(msg).encode() + b"\n")
    await writer.drain()
    line = await asyncio.wait_for(reader.readline(), timeout=1.0)
    writer.close()
    await writer.wait_closed()
    return json.loads(line) if line else None


class TestHookServer:
    async def test_fire_and_forget_event(self, server_and_handler):
        sock_path, received = server_and_handler
        reader, writer = await asyncio.open_unix_connection(str(sock_path))
        writer.write(json.dumps({"op": "event", "event": "Stop", "session_id": "s1"}).encode() + b"\n")
        await writer.drain()
        writer.close()
        await writer.wait_closed()
        await asyncio.sleep(0.05)
        assert received == [{"op": "event", "event": "Stop", "session_id": "s1"}]

    async def test_prehook_round_trip(self, server_and_handler):
        sock_path, _ = server_and_handler
        reply = await _send_one(sock_path, {
            "op": "prehook", "session_id": "s1",
            "tool_name": "Bash", "tool_input": {}, "tool_use_id": "t1",
        })
        assert reply == {"decision": "allow", "reason": "test"}

    async def test_socket_file_removed_on_stop(self, tmp_path):
        sock_path = tmp_path / "buddy.sock"
        async def noop(msg, respond): pass
        server = hook_server.HookServer(sock_path, noop)
        await server.start()
        assert sock_path.exists()
        await server.stop()
        assert not sock_path.exists()

    async def test_concurrent_connections(self, server_and_handler):
        sock_path, _ = server_and_handler
        replies = await asyncio.gather(*[
            _send_one(sock_path, {"op": "prehook", "session_id": f"s{i}",
                                  "tool_name": "Bash", "tool_input": {}, "tool_use_id": f"t{i}"})
            for i in range(5)
        ])
        for r in replies:
            assert r == {"decision": "allow", "reason": "test"}

    async def test_handler_exception_does_not_crash_server(self, tmp_path):
        sock_path = tmp_path / "buddy.sock"

        async def handler(msg, respond):
            raise RuntimeError("intentional test failure")

        server = hook_server.HookServer(sock_path, handler)
        await server.start()
        try:
            # Send one message that triggers the raising handler.
            reader, writer = await asyncio.open_unix_connection(str(sock_path))
            writer.write(json.dumps({"op": "event", "event": "Stop"}).encode() + b"\n")
            await writer.drain()
            writer.close()
            await writer.wait_closed()
            # Server should still accept new connections.
            reader2, writer2 = await asyncio.open_unix_connection(str(sock_path))
            writer2.write(json.dumps({"op": "event", "event": "Stop"}).encode() + b"\n")
            await writer2.drain()
            writer2.close()
            await writer2.wait_closed()
        finally:
            await server.stop()
