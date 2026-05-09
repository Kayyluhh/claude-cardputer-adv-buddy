"""/buddy-mute and /buddy-unmute helper. Discovers session id, sends to daemon."""
from __future__ import annotations

import argparse
import asyncio
import json
import os
import sys
from pathlib import Path


async def _open(sock_path: Path):
    if not sock_path.exists():
        return None
    try:
        return await asyncio.open_unix_connection(str(sock_path))
    except (OSError, ConnectionRefusedError):
        return None


async def _send_recv(reader, writer, msg: dict) -> dict:
    writer.write(json.dumps(msg).encode() + b"\n")
    await writer.drain()
    line = await asyncio.wait_for(reader.readline(), timeout=2.0)
    return json.loads(line) if line else {}


async def run(*, action: str, sock_path: Path, cwd: str) -> int:
    pair = await _open(sock_path)
    if pair is None:
        print(json.dumps({"ok": False, "error": "daemon not running"}))
        return 1
    reader, writer = pair
    try:
        # Discover session id.
        session_id = os.environ.get("CLAUDE_SESSION_ID", "")
        if not session_id:
            ans = await _send_recv(reader, writer, {"op": "current_session", "cwd": cwd})
            session_id = ans.get("session_id") or ""
            if not session_id:
                print(json.dumps({"ok": False, "error": "no active session"}))
                return 1
            # Reopen socket because the daemon closes after one op currently? No — the
            # daemon's current implementation handles one msg per connection. Reopen.
            writer.close()
            await writer.wait_closed()
            pair = await _open(sock_path)
            if pair is None:
                print(json.dumps({"ok": False, "error": "daemon disconnected"}))
                return 1
            reader, writer = pair

        op = "mute" if action == "mute" else "unmute"
        result = await _send_recv(reader, writer, {"op": op, "session_id": session_id})
        out = {"ok": result.get("ok", False), "session_id": session_id, "action": action}
        print(json.dumps(out))
        return 0 if out["ok"] else 1
    finally:
        try:
            writer.close()
            await writer.wait_closed()
        except Exception:
            pass


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--unmute", action="store_true")
    parser.add_argument("--socket", default="/tmp/claude-buddy.sock")
    parser.add_argument("--cwd", default=os.getcwd())
    args = parser.parse_args()
    action = "unmute" if args.unmute else "mute"
    return asyncio.run(run(action=action, sock_path=Path(args.socket), cwd=args.cwd))


if __name__ == "__main__":
    raise SystemExit(main())
