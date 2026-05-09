"""Interactive device discovery and config writing for /buddy-run first-run pairing."""
from __future__ import annotations

import argparse
import asyncio
import json
import sys
from pathlib import Path

from . import persistence


def filter_candidates(items: list[tuple[str, str | None]]) -> list[tuple[str, str]]:
    """Keep entries whose advertised name starts with 'Claude'."""
    out: list[tuple[str, str]] = []
    for addr, name in items:
        if name and name.startswith("Claude"):
            out.append((addr, name))
    return out


def save_choice(config_path: Path, *, address: str, name: str) -> None:
    cfg = persistence.load_config(config_path)
    cfg.device_address = address
    cfg.device_name = name
    persistence.save_config(config_path, cfg)


async def scan(timeout_s: float = 5.0) -> list[tuple[str, str]]:
    """Scan via bleak. Returns [(address, name)]."""
    from bleak import BleakScanner
    devices = await BleakScanner.discover(timeout=timeout_s)
    return filter_candidates([(d.address, d.name) for d in devices])


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--timeout", type=float, default=5.0)
    parser.add_argument("--save", help="address to save once chosen")
    parser.add_argument("--name", help="name to save with --save")
    parser.add_argument("--config", default=str(Path.home() / ".claude-buddy" / "config.json"))
    args = parser.parse_args()

    if args.save:
        if not args.name:
            print("--name required with --save", file=sys.stderr)
            return 2
        save_choice(Path(args.config), address=args.save, name=args.name)
        print(json.dumps({"ok": True, "saved": {"address": args.save, "name": args.name}}))
        return 0

    candidates = asyncio.run(scan(timeout_s=args.timeout))
    print(json.dumps([{"address": a, "name": n} for a, n in candidates]))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
