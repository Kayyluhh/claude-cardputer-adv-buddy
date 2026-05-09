from __future__ import annotations

import json
from pathlib import Path

import pytest

# Import directly via path because install.py is at tools/bridge/install.py, not in the package.
import importlib.util
import sys

INSTALL_PATH = Path(__file__).resolve().parents[1] / "install.py"
spec = importlib.util.spec_from_file_location("buddy_install", INSTALL_PATH)
install_mod = importlib.util.module_from_spec(spec)
sys.modules["buddy_install"] = install_mod
spec.loader.exec_module(install_mod)


class TestSettingsMerge:
    def test_creates_settings_file(self, tmp_path):
        settings = tmp_path / "settings.json"
        install_mod.merge_hook_entries(settings, hook_path="/tmp/buddy_hook.py")
        data = json.loads(settings.read_text())
        for evt in install_mod.HOOK_EVENTS:
            assert any(e.get("_buddy") for e in data["hooks"][evt])

    def test_idempotent(self, tmp_path):
        settings = tmp_path / "settings.json"
        install_mod.merge_hook_entries(settings, hook_path="/tmp/buddy_hook.py")
        first = settings.read_text()
        install_mod.merge_hook_entries(settings, hook_path="/tmp/buddy_hook.py")
        second = settings.read_text()
        assert first == second

    def test_preserves_user_entries(self, tmp_path):
        settings = tmp_path / "settings.json"
        settings.write_text(json.dumps({
            "hooks": {
                "PreToolUse": [
                    {"matcher": "Edit", "hooks": [{"type": "command", "command": "/usr/bin/my-tool"}]}
                ]
            }
        }))
        install_mod.merge_hook_entries(settings, hook_path="/tmp/buddy_hook.py")
        data = json.loads(settings.read_text())
        commands = [
            h["command"]
            for entry in data["hooks"]["PreToolUse"]
            for h in entry.get("hooks", [])
        ]
        assert "/usr/bin/my-tool" in commands

    def test_writes_atomically(self, tmp_path):
        settings = tmp_path / "settings.json"
        install_mod.merge_hook_entries(settings, hook_path="/tmp/buddy_hook.py")
        assert not (tmp_path / "settings.json.tmp").exists()
        assert (tmp_path / "settings.json.buddy-bak").exists() or settings.exists()
