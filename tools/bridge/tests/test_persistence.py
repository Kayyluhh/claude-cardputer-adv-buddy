from __future__ import annotations

import json
from pathlib import Path

import pytest

from claude_buddy import persistence


class TestAtomicWriteJson:
    def test_creates_file(self, tmp_path):
        target = tmp_path / "state.json"
        persistence.atomic_write_json(target, {"x": 1})
        assert json.loads(target.read_text()) == {"x": 1}

    def test_overwrites_atomically(self, tmp_path):
        target = tmp_path / "state.json"
        target.write_text('{"old": true}')
        persistence.atomic_write_json(target, {"new": True})
        assert json.loads(target.read_text()) == {"new": True}

    def test_no_tmp_left_behind(self, tmp_path):
        target = tmp_path / "state.json"
        persistence.atomic_write_json(target, {"x": 1})
        assert not (tmp_path / "state.json.tmp").exists()


class TestStateFile:
    def test_load_missing_returns_default(self, tmp_path):
        path = tmp_path / "state.json"
        state = persistence.load_state(path)
        assert state.tokens_today == 0
        assert state.tokens_today_date == ""
        assert state.tokens_lifetime == 0

    def test_load_existing(self, tmp_path):
        path = tmp_path / "state.json"
        path.write_text(json.dumps({
            "tokens_today": 100,
            "tokens_today_date": "2026-05-07",
            "tokens_lifetime": 5000,
        }))
        state = persistence.load_state(path)
        assert state.tokens_today == 100
        assert state.tokens_today_date == "2026-05-07"
        assert state.tokens_lifetime == 5000

    def test_save_round_trip(self, tmp_path):
        path = tmp_path / "state.json"
        original = persistence.PersistedState(tokens_today=42, tokens_today_date="2026-05-07", tokens_lifetime=1000)
        persistence.save_state(path, original)
        loaded = persistence.load_state(path)
        assert loaded == original


class TestConfigFile:
    def test_load_missing_returns_empty(self, tmp_path):
        path = tmp_path / "config.json"
        cfg = persistence.load_config(path)
        assert cfg.device_address is None
        assert cfg.device_name is None
        assert cfg.owner_name is None
        assert cfg.permission_timeout_ms == 5000
        assert cfg.device_idle_timeout_ms == 600000

    def test_load_existing(self, tmp_path):
        path = tmp_path / "config.json"
        path.write_text(json.dumps({
            "device_address": "AA:BB:CC:DD:EE:FF",
            "device_name": "Clawd",
            "owner_name": "Kayla",
            "permission_timeout_ms": 7000,
        }))
        cfg = persistence.load_config(path)
        assert cfg.device_address == "AA:BB:CC:DD:EE:FF"
        assert cfg.device_name == "Clawd"
        assert cfg.owner_name == "Kayla"
        assert cfg.permission_timeout_ms == 7000

    def test_save_round_trip(self, tmp_path):
        path = tmp_path / "config.json"
        original = persistence.Config(
            device_address="AA:BB:CC:DD:EE:FF",
            device_name="Clawd",
            owner_name="Kayla",
            permission_timeout_ms=7000,
            device_idle_timeout_ms=300000,
        )
        persistence.save_config(path, original)
        loaded = persistence.load_config(path)
        assert loaded == original


class TestMutedSessions:
    def test_load_missing_returns_empty(self, tmp_path):
        path = tmp_path / "muted-sessions.json"
        assert persistence.load_muted_sessions(path) == set()

    def test_save_load_round_trip(self, tmp_path):
        path = tmp_path / "muted-sessions.json"
        persistence.save_muted_sessions(path, {"sess_a", "sess_b"})
        assert persistence.load_muted_sessions(path) == {"sess_a", "sess_b"}


class TestRobustness:
    def test_load_state_corrupt_json_returns_default(self, tmp_path):
        path = tmp_path / "state.json"
        path.write_text("{not json")
        assert persistence.load_state(path) == persistence.PersistedState()

    def test_load_state_wrong_root_returns_default(self, tmp_path):
        path = tmp_path / "state.json"
        path.write_text("null")
        assert persistence.load_state(path) == persistence.PersistedState()

    def test_load_state_wrong_field_type_uses_default_for_that_field(self, tmp_path):
        path = tmp_path / "state.json"
        path.write_text(json.dumps({
            "tokens_today": "100",
            "tokens_today_date": "2026-05-07",
            "tokens_lifetime": 5,
        }))
        state = persistence.load_state(path)
        assert state.tokens_today == 0           # wrong type → default
        assert state.tokens_today_date == "2026-05-07"  # correct type → kept
        assert state.tokens_lifetime == 5        # correct type → kept

    def test_load_config_corrupt_json_returns_default(self, tmp_path):
        path = tmp_path / "config.json"
        path.write_text("{not json")
        assert persistence.load_config(path) == persistence.Config()

    def test_load_muted_sessions_skips_non_strings(self, tmp_path):
        path = tmp_path / "muted-sessions.json"
        path.write_text(json.dumps(["s1", 42, "s2"]))
        assert persistence.load_muted_sessions(path) == {"s1", "s2"}
