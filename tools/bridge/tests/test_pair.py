from __future__ import annotations

from pathlib import Path

import pytest

from claude_buddy import pair, persistence


class TestSaveChoice:
    def test_writes_config(self, tmp_path):
        cfg_path = tmp_path / "config.json"
        pair.save_choice(cfg_path, address="AA:BB:CC:DD:EE:FF", name="Clawd")
        cfg = persistence.load_config(cfg_path)
        assert cfg.device_address == "AA:BB:CC:DD:EE:FF"
        assert cfg.device_name == "Clawd"

    def test_preserves_other_fields(self, tmp_path):
        cfg_path = tmp_path / "config.json"
        persistence.save_config(cfg_path, persistence.Config(owner_name="Kayla", permission_timeout_ms=7000))
        pair.save_choice(cfg_path, address="AA:BB:CC:DD:EE:FF", name="Clawd")
        cfg = persistence.load_config(cfg_path)
        assert cfg.owner_name == "Kayla"
        assert cfg.permission_timeout_ms == 7000


class TestFilter:
    def test_keeps_claude_prefix(self):
        items = [
            ("AA", "Claude-Cardputer"),
            ("BB", "MyHeadphones"),
            ("CC", "Claude"),
            ("DD", None),
        ]
        out = pair.filter_candidates(items)
        assert {addr for addr, _ in out} == {"AA", "CC"}
