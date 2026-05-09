#!/usr/bin/env python3
"""Standalone install for the claude-buddy bridge. Stdlib only."""
from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

HOOK_EVENTS = [
    "PreToolUse", "PostToolUse", "UserPromptSubmit",
    "Stop", "SessionStart", "SessionEnd", "Notification",
]


def repo_root() -> Path:
    return Path(__file__).resolve().parents[2]  # claude-cardputer-adv-buddy/


def bridge_dir() -> Path:
    return Path(__file__).resolve().parent  # claude-cardputer-adv-buddy/tools/bridge/


def _atomic_write(path: Path, content: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    backup = path.with_suffix(path.suffix + ".buddy-bak")
    if path.exists():
        shutil.copy2(path, backup)
    tmp = path.with_suffix(path.suffix + ".tmp")
    with open(tmp, "w", encoding="utf-8") as f:
        f.write(content)
        f.flush()
        os.fsync(f.fileno())
    os.replace(tmp, path)


def setup_venv(state_dir: Path, bridge: Path) -> Path:
    """Create ~/.claude-buddy/venv if missing and pip install the bridge editable."""
    state_dir.mkdir(parents=True, exist_ok=True)
    venv = state_dir / "venv"
    if not venv.exists():
        subprocess.check_call([sys.executable, "-m", "venv", str(venv)])
    pip = venv / "bin" / "pip"
    subprocess.check_call([str(pip), "install", "-e", str(bridge)])
    py_link = state_dir / "python"
    target = venv / "bin" / "python"
    if py_link.exists() or py_link.is_symlink():
        py_link.unlink()
    py_link.symlink_to(target)
    return py_link


def write_hook_shim(state_dir: Path, hooks_dir: Path) -> Path:
    hooks_dir.mkdir(parents=True, exist_ok=True)
    shim = hooks_dir / "buddy_hook.py"
    py = state_dir / "python"
    body = (
        "#!/bin/bash\n"
        f'exec "{py}" -m claude_buddy.hook "$@"\n'
    )
    shim.write_text(body)
    shim.chmod(0o755)
    return shim


def symlink_skills(repo: Path, skills_dst_dir: Path) -> list[Path]:
    skills_dst_dir.mkdir(parents=True, exist_ok=True)
    src_dir = repo / ".claude" / "skills"
    created: list[Path] = []
    for src in src_dir.glob("buddy-*"):
        if not src.is_dir():
            continue
        link = skills_dst_dir / src.name
        if link.exists() or link.is_symlink():
            if link.is_symlink() and link.resolve() == src.resolve():
                continue
            print(f"refusing to overwrite existing {link}", file=sys.stderr)
            continue
        link.symlink_to(src)
        created.append(link)
    return created


def merge_hook_entries(settings_path: Path, *, hook_path: str) -> None:
    settings_path.parent.mkdir(parents=True, exist_ok=True)
    if settings_path.exists():
        try:
            data = json.loads(settings_path.read_text())
        except json.JSONDecodeError:
            print("settings.json unparseable; aborting merge", file=sys.stderr)
            raise
    else:
        data = {}

    hooks = data.setdefault("hooks", {})
    for evt in HOOK_EVENTS:
        entries = hooks.setdefault(evt, [])
        if any(isinstance(e, dict) and e.get("_buddy") is True for e in entries):
            continue
        entries.append({
            "_buddy": True,
            "matcher": "*",
            "hooks": [{"type": "command", "command": hook_path, "timeout": 60}],
        })

    _atomic_write(settings_path, json.dumps(data, indent=2))


def main() -> int:
    home = Path(os.path.expanduser("~"))
    state_dir = home / ".claude-buddy"
    repo = repo_root()
    bridge = bridge_dir()

    if not (bridge / "pyproject.toml").exists():
        print(f"pyproject.toml not found in {bridge}; abort", file=sys.stderr)
        return 1

    print("[1/5] setting up venv at ~/.claude-buddy/venv")
    setup_venv(state_dir, bridge)

    print("[2/5] writing hook shim to ~/.claude/hooks/buddy_hook.py")
    shim = write_hook_shim(state_dir, home / ".claude" / "hooks")

    print("[3/5] symlinking skills into ~/.claude/skills")
    created = symlink_skills(repo, home / ".claude" / "skills")
    for c in created:
        print(f"  + {c}")

    print("[4/5] merging hook entries into ~/.claude/settings.json")
    merge_hook_entries(home / ".claude" / "settings.json", hook_path=str(shim))

    print("[5/5] persisting repo path to ~/.claude-buddy/config.json")
    cfg_path = state_dir / "config.json"
    if cfg_path.exists():
        cfg = json.loads(cfg_path.read_text())
    else:
        cfg = {}
    cfg["repo_root"] = str(repo)
    _atomic_write(cfg_path, json.dumps(cfg, indent=2))

    print()
    print("install complete.")
    print()
    print("⚠️  First /buddy-run will trigger a macOS Bluetooth permission dialog tied to")
    print("   ~/.claude-buddy/python — click Allow.")
    print()
    print("Next: run /buddy-run from any CC session inside the firmware repo, then")
    print("pick your Cardputer from the scan list.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
