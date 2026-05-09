#!/usr/bin/env python3
"""Standalone uninstall for the claude-buddy bridge. Stdlib only."""
from __future__ import annotations

import json
import os
import shutil
import sys
from pathlib import Path


HOOK_EVENTS = [
    "PreToolUse", "PostToolUse", "UserPromptSubmit",
    "Stop", "SessionStart", "SessionEnd", "Notification",
]


def _atomic_write(path: Path, content: str) -> None:
    backup = path.with_suffix(path.suffix + ".buddy-bak")
    if path.exists():
        shutil.copy2(path, backup)
    tmp = path.with_suffix(path.suffix + ".tmp")
    with open(tmp, "w", encoding="utf-8") as f:
        f.write(content)
        f.flush()
        os.fsync(f.fileno())
    os.replace(tmp, path)


def remove_hook_entries(settings_path: Path) -> None:
    if not settings_path.exists():
        return
    data = json.loads(settings_path.read_text())
    hooks = data.get("hooks", {})
    for evt in HOOK_EVENTS:
        if evt not in hooks:
            continue
        hooks[evt] = [e for e in hooks[evt] if not (isinstance(e, dict) and e.get("_buddy"))]
        if not hooks[evt]:
            del hooks[evt]
    _atomic_write(settings_path, json.dumps(data, indent=2))


def remove_skills(skills_dir: Path) -> list[Path]:
    removed: list[Path] = []
    for link in skills_dir.glob("buddy-*"):
        if link.is_symlink():
            link.unlink()
            removed.append(link)
        else:
            print(f"refusing to delete real directory {link}", file=sys.stderr)
    return removed


def main() -> int:
    home = Path(os.path.expanduser("~"))
    print("[1/3] removing _buddy hook entries from ~/.claude/settings.json")
    remove_hook_entries(home / ".claude" / "settings.json")
    print("[2/3] deleting ~/.claude/hooks/buddy_hook.py")
    shim = home / ".claude" / "hooks" / "buddy_hook.py"
    shim.unlink(missing_ok=True)
    print("[3/3] removing skill symlinks from ~/.claude/skills")
    removed = remove_skills(home / ".claude" / "skills")
    for r in removed:
        print(f"  - {r}")
    print()
    print("uninstall complete.")
    print("To fully purge, also: rm -rf ~/.claude-buddy/")
    print("(this preserves token history, mute state, and BLE bond)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
