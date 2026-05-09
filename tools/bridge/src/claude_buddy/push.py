"""Folder push CLI client. Talks to the daemon over the Unix socket."""
from __future__ import annotations

import argparse
import asyncio
import json
import sys
from pathlib import Path


async def run(folder: Path, sock_path: Path) -> int:
    if not folder.is_dir():
        print(json.dumps({"ok": False, "error": f"not a directory: {folder}"}))
        return 2
    if not sock_path.exists():
        print(json.dumps({"ok": False, "error": "daemon not running; start with /buddy-run"}))
        return 1
    reader, writer = await asyncio.open_unix_connection(str(sock_path))
    try:
        writer.write(json.dumps({"op": "push", "path": str(folder.resolve())}).encode() + b"\n")
        await writer.drain()
        last = {}
        while True:
            line = await asyncio.wait_for(reader.readline(), timeout=30.0)
            if not line:
                break
            stage = json.loads(line)
            sys.stdout.write(json.dumps(stage) + "\n")
            sys.stdout.flush()
            last = stage
            if stage.get("stage") in ("done", "error"):
                break
        return 0 if last.get("stage") == "done" else 1
    finally:
        writer.close()
        await writer.wait_closed()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("folder")
    parser.add_argument("--socket", default="/tmp/claude-buddy.sock")
    args = parser.parse_args()
    return asyncio.run(run(Path(args.folder), Path(args.socket)))


if __name__ == "__main__":
    raise SystemExit(main())
