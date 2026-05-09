"""Print daemon state by querying it over the Unix socket."""
from __future__ import annotations

import argparse
import asyncio
import json
import sys
from pathlib import Path


async def query(sock_path: Path) -> None:
    if not sock_path.exists():
        print(json.dumps({"ok": False, "error": "not_running"}))
        return
    try:
        reader, writer = await asyncio.open_unix_connection(str(sock_path))
    except (OSError, ConnectionRefusedError):
        print(json.dumps({"ok": False, "error": "not_running"}))
        return
    try:
        writer.write(b'{"op":"status"}\n')
        await writer.drain()
        try:
            line = await asyncio.wait_for(reader.readline(), timeout=2.0)
        except asyncio.TimeoutError:
            print(json.dumps({"ok": False, "error": "no_reply"}))
            return
        if not line:
            print(json.dumps({"ok": False, "error": "no_reply"}))
            return
        sys.stdout.write(line.decode("utf-8"))
    finally:
        writer.close()
        await writer.wait_closed()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--socket", default="/tmp/claude-buddy.sock")
    args = parser.parse_args()
    asyncio.run(query(Path(args.socket)))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
