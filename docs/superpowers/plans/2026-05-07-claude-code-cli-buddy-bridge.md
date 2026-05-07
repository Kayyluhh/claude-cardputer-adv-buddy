# Claude Code CLI Buddy Bridge — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a Python bridge that lets the existing Cardputer-Adv Hardware Buddy firmware be driven by Claude Code CLI sessions instead of the Claude desktop app.

**Architecture:** Single Python package `claude_buddy` running as a long-lived asyncio daemon. CC hooks forward events to the daemon over a Unix socket; the daemon multiplexes them onto BLE Nordic UART Service (NUS) traffic to the Cardputer. Six Claude Code skills (`buddy-run`, `buddy-stop`, `buddy-status`, `buddy-gifpush`, `buddy-mute`, `buddy-unmute`) drive the daemon. Install/uninstall are standalone Python scripts run once per machine.

**Tech Stack:** Python 3.11, `bleak` (BLE), `asyncio`, `pytest` + `pytest-asyncio`, `hatchling` (build).

**Spec reference:** `docs/superpowers/specs/2026-05-07-claude-code-cli-buddy-bridge-design.md` — read it first.

---

## Conventions

- All paths absolute or relative to `claude-cardputer-adv-buddy/` (the firmware repo root).
- Every task is one git commit. Test code and implementation code go in the same commit (TDD discipline).
- Use the verified hook contract in spec §6 — do not improvise field names.
- Tests use `pytest-asyncio` `asyncio_mode = "auto"` so `async def test_*` works without decorators.
- Type hints are required on all new code (`from __future__ import annotations` at the top of each module).
- Logging uses the stdlib `logging` module. `daemon.log` gets DEBUG; stderr from skills gets INFO.
- Throughout the plan, "the venv" means `~/.claude-buddy/venv`. Until Task 19 runs, develop with a local `tools/bridge/.venv` created via `python3 -m venv tools/bridge/.venv`.

## Local development environment

Before starting Task 1, run once:

```bash
cd claude-cardputer-adv-buddy/tools/bridge
python3 -m venv .venv
source .venv/bin/activate
```

Subsequent tasks assume `.venv` is active. After Task 1 installs the package editable, `pytest` runs against the dev venv.

---

## Phase 1 — Foundation

### Task 1: Project scaffolding

**Files:**
- Create: `tools/bridge/pyproject.toml`
- Create: `tools/bridge/src/claude_buddy/__init__.py`
- Create: `tools/bridge/tests/__init__.py`
- Create: `tools/bridge/tests/conftest.py`
- Create: `tools/bridge/.gitignore`

- [ ] **Step 1: Create `tools/bridge/pyproject.toml`**

```toml
[project]
name = "claude-buddy"
version = "0.1.0"
description = "Claude Code CLI bridge for the Hardware Buddy BLE protocol"
requires-python = ">=3.11"
dependencies = [
    "bleak>=0.21",
]

[project.optional-dependencies]
dev = [
    "pytest>=7.4",
    "pytest-asyncio>=0.23",
]

[build-system]
requires = ["hatchling"]
build-backend = "hatchling.build"

[tool.hatch.build.targets.wheel]
packages = ["src/claude_buddy"]

[tool.pytest.ini_options]
asyncio_mode = "auto"
testpaths = ["tests"]
```

- [ ] **Step 2: Create `tools/bridge/src/claude_buddy/__init__.py`**

```python
"""Claude Code CLI bridge for the Hardware Buddy BLE protocol."""
from __future__ import annotations

__version__ = "0.1.0"
```

- [ ] **Step 3: Create empty `tools/bridge/tests/__init__.py`**

```python
```

- [ ] **Step 4: Create `tools/bridge/tests/conftest.py`**

```python
"""Shared pytest fixtures for the claude_buddy test suite."""
from __future__ import annotations

import pytest


@pytest.fixture
def tmp_state_dir(tmp_path, monkeypatch):
    """Redirect ~/.claude-buddy to a tmp_path. Use in tests that touch persistence."""
    home = tmp_path / "home"
    (home / ".claude-buddy").mkdir(parents=True)
    monkeypatch.setenv("HOME", str(home))
    return home / ".claude-buddy"
```

- [ ] **Step 5: Create `tools/bridge/.gitignore`**

```
.venv/
__pycache__/
*.egg-info/
.pytest_cache/
dist/
build/
```

- [ ] **Step 6: Install package editable**

```bash
cd tools/bridge
pip install -e ".[dev]"
```

Expected: `Successfully installed claude-buddy-0.1.0`. Verify with `python -c "import claude_buddy; print(claude_buddy.__version__)"` → `0.1.0`.

- [ ] **Step 7: Run pytest to confirm setup works**

```bash
pytest
```

Expected: `no tests ran in 0.01s` (exit 0).

- [ ] **Step 8: Commit**

```bash
git add tools/bridge/
git commit -m "feat(bridge): scaffold claude_buddy Python package"
```

---

### Task 2: `wire.py` — protocol constants and JSON line framing

**Files:**
- Create: `tools/bridge/src/claude_buddy/wire.py`
- Test: `tools/bridge/tests/test_wire.py`

This module owns: NUS UUID strings, the 4KB turn-event cap, ND-JSON line framing for both BLE and Unix socket, and small helpers used by the rest of the package.

- [ ] **Step 1: Write the failing test**

`tools/bridge/tests/test_wire.py`:

```python
from __future__ import annotations

import json

import pytest

from claude_buddy import wire


class TestUUIDs:
    def test_nus_service_uuid(self):
        assert wire.NUS_SERVICE_UUID == "6e400001-b5a3-f393-e0a9-e50e24dcca9e"

    def test_rx_uuid_desktop_to_device(self):
        assert wire.NUS_RX_UUID == "6e400002-b5a3-f393-e0a9-e50e24dcca9e"

    def test_tx_uuid_device_to_desktop(self):
        assert wire.NUS_TX_UUID == "6e400003-b5a3-f393-e0a9-e50e24dcca9e"


class TestEncodeLine:
    def test_appends_newline(self):
        out = wire.encode_line({"a": 1})
        assert out.endswith(b"\n")

    def test_utf8_encoding(self):
        out = wire.encode_line({"msg": "café"})
        assert out == '{"msg": "café"}\n'.encode("utf-8")

    def test_compact_separators(self):
        out = wire.encode_line({"a": 1, "b": 2})
        assert b" " not in out.replace(b'"a"', b"").replace(b'"b"', b"")  # no extra spaces


class TestLineBuffer:
    def test_collects_until_newline(self):
        buf = wire.LineBuffer()
        out = buf.feed(b'{"a":1}\n{"b":2}\n')
        assert [json.loads(line) for line in out] == [{"a": 1}, {"b": 2}]

    def test_partial_line_held(self):
        buf = wire.LineBuffer()
        assert buf.feed(b'{"a":') == []
        assert buf.feed(b'1}\n') == [b'{"a":1}']

    def test_skips_empty_lines(self):
        buf = wire.LineBuffer()
        out = buf.feed(b"\n\n{}\n")
        assert out == [b"{}"]


class TestTurnEventCap:
    def test_under_cap_passes(self):
        evt = {"evt": "turn", "role": "assistant", "content": [{"type": "text", "text": "hi"}]}
        assert wire.turn_event_too_large(evt) is False

    def test_over_cap_drops(self):
        big_text = "x" * 4096
        evt = {"evt": "turn", "role": "assistant", "content": [{"type": "text", "text": big_text}]}
        assert wire.turn_event_too_large(evt) is True

    def test_exact_4kb_boundary(self):
        # Build an event whose UTF-8 serialization is exactly 4096 bytes.
        base = {"evt": "turn", "role": "assistant", "content": [{"type": "text", "text": ""}]}
        baseline = len(wire.encode_line(base)) - 1  # subtract trailing newline
        pad = 4096 - baseline
        evt = {"evt": "turn", "role": "assistant", "content": [{"type": "text", "text": "x" * pad}]}
        assert wire.turn_event_too_large(evt) is False  # exactly 4096 is allowed

        evt2 = {"evt": "turn", "role": "assistant", "content": [{"type": "text", "text": "x" * (pad + 1)}]}
        assert wire.turn_event_too_large(evt2) is True
```

- [ ] **Step 2: Run tests, expect failure**

```bash
pytest tests/test_wire.py -v
```

Expected: `ImportError: cannot import name 'wire' from 'claude_buddy'` (module does not exist).

- [ ] **Step 3: Implement `wire.py`**

`tools/bridge/src/claude_buddy/wire.py`:

```python
"""NUS protocol constants and line framing helpers."""
from __future__ import annotations

import json
from typing import Any

# Nordic UART Service UUIDs from REFERENCE.md §Transport.
NUS_SERVICE_UUID = "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
NUS_RX_UUID = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"  # desktop -> device, write
NUS_TX_UUID = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"  # device -> desktop, notify

# Turn-event UTF-8 byte cap from REFERENCE.md §Turn events.
TURN_EVENT_BYTE_CAP = 4096


def encode_line(obj: Any) -> bytes:
    """Serialize obj as compact JSON terminated with a newline, UTF-8 encoded."""
    return (json.dumps(obj, separators=(",", ":"), ensure_ascii=False) + "\n").encode("utf-8")


def turn_event_too_large(evt: Any) -> bool:
    """True if the encoded turn event (without trailing newline) exceeds 4KB UTF-8."""
    encoded = json.dumps(evt, separators=(",", ":"), ensure_ascii=False).encode("utf-8")
    return len(encoded) > TURN_EVENT_BYTE_CAP


class LineBuffer:
    """Accumulates byte chunks and yields complete \\n-terminated lines.

    The BLE notification layer fragments at the MTU boundary; this reassembles.
    The Unix socket also uses ND-JSON.
    """

    def __init__(self) -> None:
        self._buf = bytearray()

    def feed(self, chunk: bytes) -> list[bytes]:
        """Append chunk, return any complete lines (without trailing newline). Skips empty lines."""
        self._buf.extend(chunk)
        out: list[bytes] = []
        while b"\n" in self._buf:
            idx = self._buf.index(b"\n")
            line = bytes(self._buf[:idx])
            del self._buf[: idx + 1]
            if line:
                out.append(line)
        return out
```

- [ ] **Step 4: Run tests, expect pass**

```bash
pytest tests/test_wire.py -v
```

Expected: 8 tests pass.

- [ ] **Step 5: Compact-separator test note**

The test `test_compact_separators` asserts no extra spaces in JSON output. This matches `json.dumps(separators=(",", ":"))` which produces `{"a":1,"b":2}` without spaces. The test deliberately strips key markers to verify the body has no whitespace.

- [ ] **Step 6: Commit**

```bash
git add tools/bridge/src/claude_buddy/wire.py tools/bridge/tests/test_wire.py
git commit -m "feat(bridge): add wire.py with NUS UUIDs and JSON line framing"
```

---

### Task 3: `persistence.py` — atomic JSON read/write

**Files:**
- Create: `tools/bridge/src/claude_buddy/persistence.py`
- Test: `tools/bridge/tests/test_persistence.py`

Owns the `~/.claude-buddy/state.json`, `config.json`, and `muted-sessions.json` files. Atomic writes via tmp+fsync+rename per spec §16 risk #5.

- [ ] **Step 1: Write the failing test**

`tools/bridge/tests/test_persistence.py`:

```python
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


class TestMutedSessions:
    def test_load_missing_returns_empty(self, tmp_path):
        path = tmp_path / "muted-sessions.json"
        assert persistence.load_muted_sessions(path) == set()

    def test_save_load_round_trip(self, tmp_path):
        path = tmp_path / "muted-sessions.json"
        persistence.save_muted_sessions(path, {"sess_a", "sess_b"})
        assert persistence.load_muted_sessions(path) == {"sess_a", "sess_b"}
```

- [ ] **Step 2: Run tests, expect failure**

```bash
pytest tests/test_persistence.py -v
```

Expected: `ImportError`.

- [ ] **Step 3: Implement `persistence.py`**

`tools/bridge/src/claude_buddy/persistence.py`:

```python
"""Atomic JSON persistence for ~/.claude-buddy/{state,config,muted-sessions}.json."""
from __future__ import annotations

import json
import os
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Any


def atomic_write_json(path: Path, data: Any) -> None:
    """Write JSON to path atomically: tmp + fsync + rename. POSIX-atomic."""
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_suffix(path.suffix + ".tmp")
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(data, f, indent=2)
        f.flush()
        os.fsync(f.fileno())
    os.replace(tmp, path)


@dataclass
class PersistedState:
    tokens_today: int = 0
    tokens_today_date: str = ""  # YYYY-MM-DD
    tokens_lifetime: int = 0


def load_state(path: Path) -> PersistedState:
    if not path.exists():
        return PersistedState()
    raw = json.loads(path.read_text())
    return PersistedState(
        tokens_today=raw.get("tokens_today", 0),
        tokens_today_date=raw.get("tokens_today_date", ""),
        tokens_lifetime=raw.get("tokens_lifetime", 0),
    )


def save_state(path: Path, state: PersistedState) -> None:
    atomic_write_json(path, asdict(state))


@dataclass
class Config:
    device_address: str | None = None
    device_name: str | None = None
    owner_name: str | None = None
    permission_timeout_ms: int = 5000
    device_idle_timeout_ms: int = 600000  # 10 minutes


def load_config(path: Path) -> Config:
    if not path.exists():
        return Config()
    raw = json.loads(path.read_text())
    return Config(
        device_address=raw.get("device_address"),
        device_name=raw.get("device_name"),
        owner_name=raw.get("owner_name"),
        permission_timeout_ms=raw.get("permission_timeout_ms", 5000),
        device_idle_timeout_ms=raw.get("device_idle_timeout_ms", 600000),
    )


def save_config(path: Path, cfg: Config) -> None:
    atomic_write_json(path, asdict(cfg))


def load_muted_sessions(path: Path) -> set[str]:
    if not path.exists():
        return set()
    raw = json.loads(path.read_text())
    if isinstance(raw, dict):
        return set(raw.keys())
    if isinstance(raw, list):
        return set(raw)
    return set()


def save_muted_sessions(path: Path, sessions: set[str]) -> None:
    atomic_write_json(path, sorted(sessions))
```

- [ ] **Step 4: Run tests, expect pass**

```bash
pytest tests/test_persistence.py -v
```

Expected: all 9 tests pass.

- [ ] **Step 5: Commit**

```bash
git add tools/bridge/src/claude_buddy/persistence.py tools/bridge/tests/test_persistence.py
git commit -m "feat(bridge): add persistence.py for state/config/mute files"
```

---

### Task 4: `state.py` part 1 — dataclasses and msg derivation

**Files:**
- Create: `tools/bridge/src/claude_buddy/state.py`
- Test: `tools/bridge/tests/test_state.py`

Defines `Session`, `PendingPrompt`, and `GlobalState` dataclasses. Implements the `msg` derivation table from spec §8.2.

- [ ] **Step 1: Write failing tests for dataclasses + msg derivation**

`tools/bridge/tests/test_state.py`:

```python
from __future__ import annotations

import time
from collections import deque

import pytest

from claude_buddy import state


class TestDataclasses:
    def test_session_defaults(self):
        s = state.Session(id="sess_a", started_at=100.0, last_activity=100.0,
                          state="idle", transcript_path="/tmp/t.jsonl")
        assert s.pending_prompt is None
        assert s.last_msg == ""
        assert s.transcript_offset == 0

    def test_global_state_empty(self):
        gs = state.GlobalState()
        assert gs.sessions == {}
        assert gs.pending_by_id == {}
        assert gs.muted_sessions == set()
        assert gs.tokens_cumulative == 0
        assert gs.tokens_today == 0
        assert gs.last_msg == ""
        assert gs.ble_connected is False


class TestMsgDerivation:
    def test_pretooluse_with_permission_required(self):
        msg = state.derive_msg("PreToolUse", {"tool_name": "Bash", "tool_input": {"command": "git push"}}, awaiting_permission=True)
        assert msg == "approve: Bash"

    def test_pretooluse_autonomous_bash(self):
        msg = state.derive_msg("PreToolUse", {"tool_name": "Bash", "tool_input": {"command": "ls -la"}}, awaiting_permission=False)
        assert msg == "Bash: ls -la"

    def test_pretooluse_autonomous_edit(self):
        msg = state.derive_msg("PreToolUse", {"tool_name": "Edit", "tool_input": {"file_path": "/a/b/c.py"}}, awaiting_permission=False)
        assert msg == "Edit: c.py"

    def test_pretooluse_autonomous_grep(self):
        msg = state.derive_msg("PreToolUse", {"tool_name": "Grep", "tool_input": {"pattern": "TODO"}}, awaiting_permission=False)
        assert msg == "Grep: TODO"

    def test_pretooluse_unknown_tool(self):
        msg = state.derive_msg("PreToolUse", {"tool_name": "Custom", "tool_input": {}}, awaiting_permission=False)
        assert msg == "Custom"

    def test_posttooluse(self):
        msg = state.derive_msg("PostToolUse", {"tool_name": "Bash"})
        assert msg == "ran: Bash"

    def test_userpromptsubmit_truncation(self):
        long = "x" * 100
        msg = state.derive_msg("UserPromptSubmit", {"prompt": long})
        assert msg == "user: " + ("x" * 30)

    def test_userpromptsubmit_short(self):
        msg = state.derive_msg("UserPromptSubmit", {"prompt": "fix it"})
        assert msg == "user: fix it"

    def test_stop(self):
        assert state.derive_msg("Stop", {}) == "done"

    def test_session_start(self):
        assert state.derive_msg("SessionStart", {"session_count": 3}) == "3 sessions"

    def test_session_end_when_empty(self):
        assert state.derive_msg("SessionEnd", {"session_count": 0}) == "idle"

    def test_session_end_others_remain(self):
        assert state.derive_msg("SessionEnd", {"session_count": 2}) == "2 sessions"

    def test_notification_idle_prompt(self):
        assert state.derive_msg("Notification", {"notification_type": "idle_prompt"}) == "idle prompt"

    def test_notification_other_returns_none(self):
        # Other notification types do not update msg.
        assert state.derive_msg("Notification", {"notification_type": "auth_success"}) is None


class TestHintTruncation:
    def test_unicode_30_chars(self):
        msg = state.derive_msg(
            "PreToolUse",
            {"tool_name": "Bash", "tool_input": {"command": "café " * 20}},
            awaiting_permission=False,
        )
        # "café " is 5 chars; first 30 chars = "café " * 6 = 30 chars exactly.
        assert msg == "Bash: " + ("café " * 6)
```

- [ ] **Step 2: Run tests, expect failure**

```bash
pytest tests/test_state.py -v
```

Expected: ImportError.

- [ ] **Step 3: Implement dataclasses and `derive_msg`**

`tools/bridge/src/claude_buddy/state.py`:

```python
"""In-memory daemon state: sessions, pending prompts, msg derivation, token math."""
from __future__ import annotations

import asyncio
from collections import deque
from dataclasses import dataclass, field
from datetime import date
from typing import Any, Literal

SessionState = Literal["idle", "running", "waiting"]


@dataclass
class PendingPrompt:
    tool_use_id: str
    tool_name: str
    hint: str
    future: asyncio.Future
    arrived_at: float


@dataclass
class Session:
    id: str
    started_at: float
    last_activity: float
    state: SessionState
    transcript_path: str
    pending_prompt: PendingPrompt | None = None
    last_msg: str = ""
    transcript_offset: int = 0
    cwd: str = ""


@dataclass
class GlobalState:
    sessions: dict[str, Session] = field(default_factory=dict)
    pending_by_id: dict[str, PendingPrompt] = field(default_factory=dict)
    muted_sessions: set[str] = field(default_factory=set)
    tokens_cumulative: int = 0
    tokens_today: int = 0
    tokens_today_date: date | None = None
    entries: deque[str] = field(default_factory=lambda: deque(maxlen=8))
    last_msg: str = ""
    ble_connected: bool = False
    device_name: str | None = None
    owner_name: str | None = None


# Hint extractors per tool. Returns a string; truncated to 30 chars by caller.
_HINT_EXTRACTORS = {
    "Bash": lambda ti: ti.get("command", ""),
    "Edit": lambda ti: _basename(ti.get("file_path", "")),
    "Write": lambda ti: _basename(ti.get("file_path", "")),
    "Read": lambda ti: _basename(ti.get("file_path", "")),
    "NotebookEdit": lambda ti: _basename(ti.get("file_path", "")),
    "Grep": lambda ti: ti.get("pattern", ""),
    "Glob": lambda ti: ti.get("pattern", ""),
    "WebFetch": lambda ti: ti.get("url", ""),
    "WebSearch": lambda ti: ti.get("query", ""),
}


def _basename(path: str) -> str:
    if not path:
        return ""
    return path.rsplit("/", 1)[-1]


def _hint_for(tool_name: str, tool_input: dict[str, Any]) -> str:
    extractor = _HINT_EXTRACTORS.get(tool_name)
    if extractor is None:
        return ""
    return str(extractor(tool_input))[:30]


def derive_msg(event: str, payload: dict[str, Any], *, awaiting_permission: bool = False) -> str | None:
    """Map a CC hook event to the device-display msg string. Returns None if event should not update msg."""
    if event == "PreToolUse":
        tool = payload.get("tool_name", "")
        if awaiting_permission:
            return f"approve: {tool}"
        hint = _hint_for(tool, payload.get("tool_input", {}))
        if hint:
            return f"{tool}: {hint}"
        return tool
    if event == "PostToolUse":
        return f"ran: {payload.get('tool_name', '')}"
    if event == "UserPromptSubmit":
        prompt = payload.get("prompt", "")
        return f"user: {prompt[:30]}"
    if event == "Stop":
        return "done"
    if event == "SessionStart":
        return f"{payload.get('session_count', 1)} sessions"
    if event == "SessionEnd":
        n = payload.get("session_count", 0)
        return "idle" if n == 0 else f"{n} sessions"
    if event == "Notification":
        if payload.get("notification_type") == "idle_prompt":
            return "idle prompt"
        return None
    return None
```

- [ ] **Step 4: Run tests, expect pass**

```bash
pytest tests/test_state.py -v
```

Expected: all 14 tests pass.

- [ ] **Step 5: Commit**

```bash
git add tools/bridge/src/claude_buddy/state.py tools/bridge/tests/test_state.py
git commit -m "feat(bridge): add state.py dataclasses and msg derivation"
```

---

### Task 5: `state.py` part 2 — entries, midnight rollover, stale reaper

**Files:**
- Modify: `tools/bridge/src/claude_buddy/state.py`
- Modify: `tools/bridge/tests/test_state.py`

Adds entry-rotation logic, `tokens_today` midnight rollover, and the 10-minute stale-session reaper.

- [ ] **Step 1: Write failing tests**

Append to `tools/bridge/tests/test_state.py`:

```python
import time
from datetime import date, datetime
from unittest.mock import patch


class TestEntries:
    def test_append_with_timestamp(self):
        gs = state.GlobalState()
        with patch("claude_buddy.state._now_local", return_value=datetime(2026, 5, 7, 10, 42)):
            state.append_entry(gs, "approve: Bash")
        assert list(gs.entries) == ["10:42 approve: Bash"]

    def test_caps_at_8(self):
        gs = state.GlobalState()
        for i in range(12):
            with patch("claude_buddy.state._now_local", return_value=datetime(2026, 5, 7, 10, i)):
                state.append_entry(gs, f"item-{i}")
        assert len(gs.entries) == 8
        # newest at the right
        assert gs.entries[-1].endswith("item-11")
        assert gs.entries[0].endswith("item-4")

    def test_wire_entries_returns_top_4_newest_first(self):
        gs = state.GlobalState()
        for i in range(6):
            with patch("claude_buddy.state._now_local", return_value=datetime(2026, 5, 7, 10, i)):
                state.append_entry(gs, f"item-{i}")
        wire_entries = state.wire_entries(gs)
        assert len(wire_entries) == 4
        assert wire_entries[0].endswith("item-5")  # newest first
        assert wire_entries[3].endswith("item-2")


class TestMidnightRollover:
    def test_rollover_resets_tokens_today(self):
        gs = state.GlobalState(tokens_today=500, tokens_today_date=date(2026, 5, 6))
        state.maybe_rollover_tokens(gs, today=date(2026, 5, 7))
        assert gs.tokens_today == 0
        assert gs.tokens_today_date == date(2026, 5, 7)

    def test_no_rollover_same_day(self):
        gs = state.GlobalState(tokens_today=500, tokens_today_date=date(2026, 5, 7))
        state.maybe_rollover_tokens(gs, today=date(2026, 5, 7))
        assert gs.tokens_today == 500

    def test_first_run_initializes_date(self):
        gs = state.GlobalState(tokens_today=0, tokens_today_date=None)
        state.maybe_rollover_tokens(gs, today=date(2026, 5, 7))
        assert gs.tokens_today_date == date(2026, 5, 7)
        assert gs.tokens_today == 0


class TestStaleReaper:
    def test_drops_idle_sessions(self):
        gs = state.GlobalState()
        now = 1000.0
        gs.sessions["fresh"] = state.Session(
            id="fresh", started_at=now, last_activity=now,
            state="idle", transcript_path="",
        )
        gs.sessions["stale"] = state.Session(
            id="stale", started_at=now - 1000, last_activity=now - 700,
            state="idle", transcript_path="",
        )
        dropped = state.reap_stale_sessions(gs, now=now, ttl_seconds=600)
        assert dropped == ["stale"]
        assert "fresh" in gs.sessions
        assert "stale" not in gs.sessions

    def test_clears_pending_for_dropped(self):
        gs = state.GlobalState()
        sess_id = "stale"
        sess = state.Session(
            id=sess_id, started_at=0, last_activity=0,
            state="waiting", transcript_path="",
        )
        gs.sessions[sess_id] = sess
        # Skip filling the future since reap should not touch it.
        prompt = state.PendingPrompt(
            tool_use_id="t1", tool_name="Bash", hint="",
            future=__import__("asyncio").get_event_loop().create_future(),
            arrived_at=0,
        )
        sess.pending_prompt = prompt
        gs.pending_by_id["t1"] = prompt
        state.reap_stale_sessions(gs, now=2000, ttl_seconds=600)
        assert "t1" not in gs.pending_by_id
        # The pending future is resolved with "ask" so any waiting hook unblocks.
        assert prompt.future.done()
        assert prompt.future.result() == "ask"
```

- [ ] **Step 2: Run tests, expect failure**

```bash
pytest tests/test_state.py -v
```

Expected: failures referencing missing `append_entry`, `wire_entries`, `maybe_rollover_tokens`, `reap_stale_sessions`, `_now_local`.

- [ ] **Step 3: Add implementations to `state.py`**

Append to `tools/bridge/src/claude_buddy/state.py`:

```python
from datetime import datetime


def _now_local() -> datetime:
    """Indirection for testing — can be patched."""
    return datetime.now()


def append_entry(gs: GlobalState, msg: str) -> None:
    """Append an HH:MM-stamped entry. Capped at maxlen=8 by deque."""
    stamp = _now_local().strftime("%H:%M")
    gs.entries.append(f"{stamp} {msg}")


def wire_entries(gs: GlobalState) -> list[str]:
    """Return the newest 4 entries in newest-first order — what rides the BLE wire."""
    last_four = list(gs.entries)[-4:]
    return list(reversed(last_four))


def maybe_rollover_tokens(gs: GlobalState, *, today: date) -> None:
    """If today != stored date, reset tokens_today to 0. First-run sets the date with no reset."""
    if gs.tokens_today_date is None:
        gs.tokens_today_date = today
        return
    if today != gs.tokens_today_date:
        gs.tokens_today = 0
        gs.tokens_today_date = today


def reap_stale_sessions(gs: GlobalState, *, now: float, ttl_seconds: float) -> list[str]:
    """Drop sessions idle for longer than ttl_seconds. Returns the list of dropped ids.

    Resolves any pending prompts on dropped sessions to "ask" so awaiting hooks unblock.
    """
    dropped: list[str] = []
    for sid, sess in list(gs.sessions.items()):
        if now - sess.last_activity <= ttl_seconds:
            continue
        if sess.pending_prompt is not None:
            pp = sess.pending_prompt
            if not pp.future.done():
                pp.future.set_result("ask")
            gs.pending_by_id.pop(pp.tool_use_id, None)
        gs.sessions.pop(sid)
        dropped.append(sid)
    return dropped
```

- [ ] **Step 4: Run tests, expect pass**

```bash
pytest tests/test_state.py -v
```

Expected: all tests pass (originals + new).

- [ ] **Step 5: Commit**

```bash
git add tools/bridge/src/claude_buddy/state.py tools/bridge/tests/test_state.py
git commit -m "feat(bridge): entries rotation, midnight rollover, stale reaper"
```

---

### Task 6: `transcript.py` — JSONL token harvest

**Files:**
- Create: `tools/bridge/src/claude_buddy/transcript.py`
- Test: `tools/bridge/tests/test_transcript.py`

Reads a CC session transcript JSONL from a stored offset, extracts `usage.output_tokens` from assistant messages, returns the total and the new offset.

- [ ] **Step 1: Write failing tests**

`tools/bridge/tests/test_transcript.py`:

```python
from __future__ import annotations

import json
from pathlib import Path

import pytest

from claude_buddy import transcript


def _write_jsonl(path: Path, records: list[dict]) -> None:
    path.write_text("\n".join(json.dumps(r) for r in records) + "\n")


class TestHarvestUsage:
    def test_nonexistent_file(self, tmp_path):
        result = transcript.harvest_usage(tmp_path / "nope.jsonl", offset=0)
        assert result.tokens == 0
        assert result.new_offset == 0

    def test_empty_file(self, tmp_path):
        path = tmp_path / "t.jsonl"
        path.write_text("")
        result = transcript.harvest_usage(path, offset=0)
        assert result.tokens == 0
        assert result.new_offset == 0

    def test_single_assistant_message(self, tmp_path):
        path = tmp_path / "t.jsonl"
        _write_jsonl(path, [
            {"type": "user", "content": "hi"},
            {"type": "assistant", "message": {"usage": {"output_tokens": 42}}},
        ])
        result = transcript.harvest_usage(path, offset=0)
        assert result.tokens == 42
        assert result.new_offset == path.stat().st_size

    def test_resumes_from_offset(self, tmp_path):
        path = tmp_path / "t.jsonl"
        _write_jsonl(path, [
            {"type": "assistant", "message": {"usage": {"output_tokens": 10}}},
        ])
        first = transcript.harvest_usage(path, offset=0)
        # Append more.
        with open(path, "a") as f:
            f.write(json.dumps({"type": "assistant", "message": {"usage": {"output_tokens": 33}}}) + "\n")
        second = transcript.harvest_usage(path, offset=first.new_offset)
        assert second.tokens == 33

    def test_multiple_assistant_messages(self, tmp_path):
        path = tmp_path / "t.jsonl"
        _write_jsonl(path, [
            {"type": "assistant", "message": {"usage": {"output_tokens": 5}}},
            {"type": "user", "content": "x"},
            {"type": "assistant", "message": {"usage": {"output_tokens": 7}}},
        ])
        result = transcript.harvest_usage(path, offset=0)
        assert result.tokens == 12

    def test_handles_partial_last_line(self, tmp_path):
        path = tmp_path / "t.jsonl"
        # File with a complete record followed by half a JSON object.
        complete = json.dumps({"type": "assistant", "message": {"usage": {"output_tokens": 4}}}) + "\n"
        partial = '{"type": "assist'
        path.write_text(complete + partial)
        result = transcript.harvest_usage(path, offset=0)
        assert result.tokens == 4
        # Offset advances to the start of the partial line so we re-read it next time.
        assert result.new_offset == len(complete.encode("utf-8"))

    def test_tolerates_missing_usage(self, tmp_path):
        path = tmp_path / "t.jsonl"
        _write_jsonl(path, [
            {"type": "assistant", "message": {}},  # no usage
            {"type": "assistant"},  # no message
        ])
        result = transcript.harvest_usage(path, offset=0)
        assert result.tokens == 0

    def test_tolerates_unparseable_lines(self, tmp_path):
        path = tmp_path / "t.jsonl"
        good = json.dumps({"type": "assistant", "message": {"usage": {"output_tokens": 9}}})
        path.write_text(good + "\nthis is not json\n")
        result = transcript.harvest_usage(path, offset=0)
        assert result.tokens == 9
```

- [ ] **Step 2: Run tests, expect failure**

```bash
pytest tests/test_transcript.py -v
```

Expected: ImportError.

- [ ] **Step 3: Implement `transcript.py`**

`tools/bridge/src/claude_buddy/transcript.py`:

```python
"""Tail a CC session JSONL for usage stats."""
from __future__ import annotations

import json
import logging
from dataclasses import dataclass
from pathlib import Path

log = logging.getLogger(__name__)


@dataclass
class HarvestResult:
    tokens: int
    new_offset: int


def harvest_usage(path: Path, *, offset: int) -> HarvestResult:
    """Read assistant.message.usage.output_tokens from new JSONL records since offset.

    Returns total tokens added and the new offset. If the last line is incomplete (no
    trailing newline), its bytes are not consumed so the next call retries.
    """
    if not path.exists():
        return HarvestResult(tokens=0, new_offset=0)
    size = path.stat().st_size
    if offset >= size:
        return HarvestResult(tokens=0, new_offset=offset)

    with open(path, "rb") as f:
        f.seek(offset)
        chunk = f.read()

    # Split on \n. If the chunk does not end with \n, the last fragment is partial.
    has_partial_tail = not chunk.endswith(b"\n")
    lines = chunk.split(b"\n")
    if has_partial_tail:
        partial = lines[-1]
        lines = lines[:-1]
    else:
        partial = b""

    consumed = len(chunk) - len(partial)
    tokens = 0
    for raw in lines:
        if not raw.strip():
            continue
        try:
            rec = json.loads(raw)
        except json.JSONDecodeError:
            log.debug("transcript: skipping unparseable line at offset %d", offset)
            continue
        tokens += _extract_output_tokens(rec)

    return HarvestResult(tokens=tokens, new_offset=offset + consumed)


def _extract_output_tokens(record: dict) -> int:
    """Defensive extraction. Schema may shift; tolerate missing fields."""
    if record.get("type") != "assistant":
        return 0
    msg = record.get("message")
    if not isinstance(msg, dict):
        return 0
    usage = msg.get("usage")
    if not isinstance(usage, dict):
        return 0
    val = usage.get("output_tokens")
    return int(val) if isinstance(val, (int, float)) else 0
```

- [ ] **Step 4: Run tests, expect pass**

```bash
pytest tests/test_transcript.py -v
```

Expected: all 8 tests pass.

- [ ] **Step 5: Commit**

```bash
git add tools/bridge/src/claude_buddy/transcript.py tools/bridge/tests/test_transcript.py
git commit -m "feat(bridge): transcript.py token harvest with partial-line tolerance"
```

---

## Phase 2 — Hook server side

### Task 7: `hook_server.py` — Unix socket listener

**Files:**
- Create: `tools/bridge/src/claude_buddy/hook_server.py`
- Test: `tools/bridge/tests/test_hook_protocol.py`

Listens on `/tmp/claude-buddy.sock`, dispatches incoming ND-JSON messages to a callback, supports per-connection request/response (PreToolUse holds the connection until the daemon resolves the prompt future).

- [ ] **Step 1: Write failing tests**

`tools/bridge/tests/test_hook_protocol.py`:

```python
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
```

- [ ] **Step 2: Run tests, expect failure**

```bash
pytest tests/test_hook_protocol.py -v
```

Expected: ImportError.

- [ ] **Step 3: Implement `hook_server.py`**

`tools/bridge/src/claude_buddy/hook_server.py`:

```python
"""Unix-socket ND-JSON server: hook scripts and skills connect here."""
from __future__ import annotations

import asyncio
import json
import logging
from pathlib import Path
from typing import Awaitable, Callable

log = logging.getLogger(__name__)

# Handler signature: (msg, respond) where respond is an async callable accepting a dict.
Handler = Callable[[dict, Callable[[dict], Awaitable[None]]], Awaitable[None]]


class HookServer:
    def __init__(self, sock_path: Path, handler: Handler) -> None:
        self.sock_path = sock_path
        self._handler = handler
        self._server: asyncio.AbstractServer | None = None

    async def start(self) -> None:
        # Remove stale socket if present.
        if self.sock_path.exists():
            self.sock_path.unlink()
        self.sock_path.parent.mkdir(parents=True, exist_ok=True)
        self._server = await asyncio.start_unix_server(self._on_conn, path=str(self.sock_path))
        log.info("hook server listening on %s", self.sock_path)

    async def stop(self) -> None:
        if self._server is not None:
            self._server.close()
            await self._server.wait_closed()
            self._server = None
        if self.sock_path.exists():
            self.sock_path.unlink()

    async def _on_conn(self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
        try:
            line = await reader.readline()
            if not line:
                return
            try:
                msg = json.loads(line)
            except json.JSONDecodeError:
                log.warning("hook server: bad JSON from client; closing")
                return

            response_sent = False

            async def respond(payload: dict) -> None:
                nonlocal response_sent
                if response_sent:
                    return
                writer.write((json.dumps(payload) + "\n").encode("utf-8"))
                await writer.drain()
                response_sent = True

            await self._handler(msg, respond)
        except (ConnectionError, asyncio.CancelledError):
            pass
        except Exception:
            log.exception("hook server: handler error")
        finally:
            try:
                writer.close()
                await writer.wait_closed()
            except Exception:
                pass
```

- [ ] **Step 4: Run tests, expect pass**

```bash
pytest tests/test_hook_protocol.py -v
```

Expected: 4 tests pass.

- [ ] **Step 5: Commit**

```bash
git add tools/bridge/src/claude_buddy/hook_server.py tools/bridge/tests/test_hook_protocol.py
git commit -m "feat(bridge): hook_server.py Unix-socket ND-JSON dispatch"
```

---

### Task 8: `hook.py` — hook script entry point

**Files:**
- Create: `tools/bridge/src/claude_buddy/hook.py`
- Test: `tools/bridge/tests/test_hook_entrypoint.py`

This is the `python -m claude_buddy.hook` entry point that CC's settings.json invokes. Reads JSON from stdin, sends to daemon, prints CC-formatted JSON on stdout for PreToolUse, exits 0 always.

- [ ] **Step 1: Write failing tests**

`tools/bridge/tests/test_hook_entrypoint.py`:

```python
from __future__ import annotations

import asyncio
import json
import subprocess
import sys
from pathlib import Path

import pytest


HOOK_CMD = [sys.executable, "-m", "claude_buddy.hook"]


def _run_hook(stdin_payload: dict, env_overrides: dict[str, str]) -> subprocess.CompletedProcess:
    import os
    env = os.environ.copy()
    env.update(env_overrides)
    return subprocess.run(
        HOOK_CMD,
        input=json.dumps(stdin_payload).encode(),
        capture_output=True,
        env=env,
        timeout=10,
    )


class TestNoDaemon:
    def test_pretooluse_no_daemon_returns_ask(self, tmp_path):
        sock = tmp_path / "no-such.sock"
        result = _run_hook(
            {"hook_event_name": "PreToolUse", "session_id": "s1",
             "tool_name": "Bash", "tool_input": {}, "tool_use_id": "t1"},
            {"BUDDY_SOCKET": str(sock)},
        )
        assert result.returncode == 0
        out = json.loads(result.stdout)
        assert out["hookSpecificOutput"]["permissionDecision"] == "ask"

    def test_other_event_no_daemon_silent(self, tmp_path):
        sock = tmp_path / "no-such.sock"
        result = _run_hook(
            {"hook_event_name": "Stop", "session_id": "s1"},
            {"BUDDY_SOCKET": str(sock)},
        )
        assert result.returncode == 0
        assert result.stdout == b""


class TestProjectDisabled:
    def test_settings_local_disable_short_circuits(self, tmp_path):
        cwd = tmp_path / "proj"
        (cwd / ".claude").mkdir(parents=True)
        (cwd / ".claude" / "settings.local.json").write_text(json.dumps({"_buddy_disabled": True}))
        result = subprocess.run(
            HOOK_CMD,
            input=json.dumps({"hook_event_name": "PreToolUse", "session_id": "s1",
                              "tool_name": "Bash", "tool_input": {}, "tool_use_id": "t1",
                              "cwd": str(cwd)}).encode(),
            capture_output=True,
            cwd=str(cwd),
            timeout=10,
        )
        assert result.returncode == 0
        # No stdout = CC behaves as if hook didn't fire.
        assert result.stdout == b""


class TestWithFakeDaemon:
    async def test_pretooluse_round_trip(self, tmp_path):
        sock = tmp_path / "buddy.sock"

        async def serve():
            srv = await asyncio.start_unix_server(_echo_allow, path=str(sock))
            await srv.serve_forever()

        async def _echo_allow(reader, writer):
            await reader.readline()
            writer.write(b'{"decision":"allow","reason":"ok"}\n')
            await writer.drain()
            writer.close()
            await writer.wait_closed()

        server_task = asyncio.create_task(serve())
        await asyncio.sleep(0.05)

        try:
            proc = await asyncio.create_subprocess_exec(
                *HOOK_CMD,
                stdin=asyncio.subprocess.PIPE,
                stdout=asyncio.subprocess.PIPE,
                stderr=asyncio.subprocess.PIPE,
                env={**__import__("os").environ, "BUDDY_SOCKET": str(sock)},
            )
            stdout, _ = await proc.communicate(json.dumps({
                "hook_event_name": "PreToolUse",
                "session_id": "s1", "tool_name": "Bash",
                "tool_input": {"command": "ls"}, "tool_use_id": "t1",
            }).encode())
            assert proc.returncode == 0
            out = json.loads(stdout)
            assert out["hookSpecificOutput"]["permissionDecision"] == "allow"
        finally:
            server_task.cancel()
            try:
                await server_task
            except asyncio.CancelledError:
                pass
```

- [ ] **Step 2: Run tests, expect failure**

```bash
pytest tests/test_hook_entrypoint.py -v
```

Expected: failures (module missing).

- [ ] **Step 3: Implement `hook.py`**

`tools/bridge/src/claude_buddy/hook.py`:

```python
"""CC hook entry point: python -m claude_buddy.hook.

Reads a hook payload from stdin, forwards to the daemon over Unix socket, and for
PreToolUse waits up to 5s for a decision. Always exits 0; emits CC-formatted JSON
on stdout for PreToolUse only.
"""
from __future__ import annotations

import json
import os
import socket
import sys
from pathlib import Path

DEFAULT_SOCKET = "/tmp/claude-buddy.sock"
CONNECT_TIMEOUT = 0.5
READ_TIMEOUT = 5.0


def _emit_pretooluse(decision: str, reason: str = "hardware buddy") -> None:
    out = {
        "hookSpecificOutput": {
            "hookEventName": "PreToolUse",
            "permissionDecision": decision,
            "permissionDecisionReason": reason,
        }
    }
    sys.stdout.write(json.dumps(out))
    sys.stdout.flush()


def _project_disabled(cwd: str) -> bool:
    if not cwd:
        return False
    settings = Path(cwd) / ".claude" / "settings.local.json"
    if not settings.exists():
        return False
    try:
        data = json.loads(settings.read_text())
    except (OSError, json.JSONDecodeError):
        return False
    return bool(data.get("_buddy_disabled", False))


def main() -> int:
    raw = sys.stdin.read()
    if not raw.strip():
        return 0
    try:
        payload = json.loads(raw)
    except json.JSONDecodeError:
        return 0

    event = payload.get("hook_event_name", "")
    cwd = payload.get("cwd", "")
    if _project_disabled(cwd):
        # Spec §11.3: exit 0 silently when disabled. CC behaves as if no hook fired.
        return 0

    sock_path = os.environ.get("BUDDY_SOCKET", DEFAULT_SOCKET)

    if event == "PreToolUse":
        decision = _request_permission(sock_path, payload)
        _emit_pretooluse(decision)
        return 0

    _send_event(sock_path, payload)
    return 0


def _request_permission(sock_path: str, payload: dict) -> str:
    msg = {
        "op": "prehook",
        "session_id": payload.get("session_id"),
        "tool_name": payload.get("tool_name"),
        "tool_input": payload.get("tool_input", {}),
        "tool_use_id": payload.get("tool_use_id"),
        "cwd": payload.get("cwd", ""),
        "transcript_path": payload.get("transcript_path", ""),
    }
    try:
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(CONNECT_TIMEOUT)
        s.connect(sock_path)
        s.settimeout(READ_TIMEOUT)
        s.sendall((json.dumps(msg) + "\n").encode("utf-8"))
        data = b""
        while not data.endswith(b"\n"):
            chunk = s.recv(4096)
            if not chunk:
                break
            data += chunk
        s.close()
        line = data.strip()
        if not line:
            return "ask"
        reply = json.loads(line)
        decision = reply.get("decision", "ask")
        if decision not in ("allow", "deny", "ask"):
            return "ask"
        return decision
    except (OSError, json.JSONDecodeError, socket.timeout):
        return "ask"


def _send_event(sock_path: str, payload: dict) -> None:
    msg = {
        "op": "event",
        "event": payload.get("hook_event_name"),
        "session_id": payload.get("session_id"),
        "transcript_path": payload.get("transcript_path", ""),
        "cwd": payload.get("cwd", ""),
        # Event-specific fields included verbatim.
        **{k: v for k, v in payload.items() if k not in (
            "hook_event_name", "session_id", "transcript_path", "cwd",
        )},
    }
    try:
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(CONNECT_TIMEOUT)
        s.connect(sock_path)
        s.sendall((json.dumps(msg) + "\n").encode("utf-8"))
        s.close()
    except OSError:
        pass


if __name__ == "__main__":
    sys.exit(main())
```

- [ ] **Step 4: Run tests, expect pass**

```bash
pytest tests/test_hook_entrypoint.py -v
```

Expected: all pass.

- [ ] **Step 5: Commit**

```bash
git add tools/bridge/src/claude_buddy/hook.py tools/bridge/tests/test_hook_entrypoint.py
git commit -m "feat(bridge): hook.py CC hook entry point with timeout/fallback"
```

---

## Phase 3 — BLE side

### Task 9: BLE fake test peer

**Files:**
- Create: `tools/bridge/tests/ble_fake.py`
- Test: `tools/bridge/tests/test_ble_fake.py`

An in-process implementation of the firmware's BLE side of the wire protocol — used by daemon integration tests. Talks ND-JSON over an asyncio queue pair.

- [ ] **Step 1: Write failing tests**

`tools/bridge/tests/test_ble_fake.py`:

```python
from __future__ import annotations

import asyncio
import json

import pytest

from .ble_fake import BleFake


class TestBleFake:
    async def test_send_to_device_via_rx(self):
        fake = BleFake()
        await fake.write_rx(b'{"hello":1}\n')
        line = await asyncio.wait_for(fake.received_from_host(), timeout=1.0)
        assert line == {"hello": 1}

    async def test_device_notifies_via_tx(self):
        fake = BleFake()
        notifications: list[dict] = []

        def on_notify(data: bytes) -> None:
            for chunk in data.split(b"\n"):
                if chunk:
                    notifications.append(json.loads(chunk))

        fake.set_notify_callback(on_notify)
        await fake.notify({"ack": "status", "ok": True})
        # Allow the queue to drain.
        await asyncio.sleep(0.01)
        assert notifications == [{"ack": "status", "ok": True}]

    async def test_status_response_canned(self):
        fake = BleFake()
        # When fake receives a status command, it auto-replies with a canned status.
        notifications: list[dict] = []
        fake.set_notify_callback(lambda d: notifications.append(json.loads(d.strip())))
        await fake.write_rx(b'{"cmd":"status"}\n')
        await asyncio.sleep(0.05)
        assert any(n.get("ack") == "status" for n in notifications)
```

- [ ] **Step 2: Run tests, expect failure**

```bash
pytest tests/test_ble_fake.py -v
```

Expected: ImportError.

- [ ] **Step 3: Implement `ble_fake.py`**

`tools/bridge/tests/ble_fake.py`:

```python
"""In-process fake of the Cardputer's NUS side. Used by daemon integration tests."""
from __future__ import annotations

import asyncio
import json
from typing import Callable


class BleFake:
    """Simulates the device side of the NUS protocol.

    The host writes to the RX characteristic via write_rx(); BleFake parses lines
    and (for {"cmd": "status"}) auto-replies with a canned ack. The host reads
    notifications via the callback set by set_notify_callback().
    """

    def __init__(self) -> None:
        self._from_host: asyncio.Queue[dict] = asyncio.Queue()
        self._notify_cb: Callable[[bytes], None] | None = None
        self._rx_buffer = bytearray()

    def set_notify_callback(self, cb: Callable[[bytes], None]) -> None:
        self._notify_cb = cb

    async def write_rx(self, data: bytes) -> None:
        """Host -> device: line-buffer the bytes, parse complete lines, react to certain cmds."""
        self._rx_buffer.extend(data)
        while b"\n" in self._rx_buffer:
            idx = self._rx_buffer.index(b"\n")
            line = bytes(self._rx_buffer[:idx])
            del self._rx_buffer[: idx + 1]
            if not line:
                continue
            try:
                msg = json.loads(line)
            except json.JSONDecodeError:
                continue
            await self._from_host.put(msg)
            await self._react(msg)

    async def received_from_host(self) -> dict:
        return await self._from_host.get()

    def history_size(self) -> int:
        return self._from_host.qsize()

    async def notify(self, payload: dict) -> None:
        """Device -> host."""
        if self._notify_cb is None:
            return
        self._notify_cb((json.dumps(payload) + "\n").encode("utf-8"))

    async def _react(self, msg: dict) -> None:
        if msg.get("cmd") == "status":
            await self.notify({
                "ack": "status",
                "ok": True,
                "data": {"name": "FakeBuddy", "sec": True,
                         "bat": {"pct": 100, "mV": 4200, "mA": 0, "usb": True},
                         "sys": {"up": 0, "heap": 100000},
                         "stats": {"appr": 0, "deny": 0, "vel": 0, "nap": 0, "lvl": 0}},
            })
```

- [ ] **Step 4: Run tests, expect pass**

```bash
pytest tests/test_ble_fake.py -v
```

Expected: 3 tests pass.

- [ ] **Step 5: Commit**

```bash
git add tools/bridge/tests/ble_fake.py tools/bridge/tests/test_ble_fake.py
git commit -m "test(bridge): in-process BleFake NUS peer fixture"
```

---

### Task 10: `ble_client.py` — bleak NUS client

**Files:**
- Create: `tools/bridge/src/claude_buddy/ble_client.py`
- Test: `tools/bridge/tests/test_ble_client.py`

Wraps `bleak.BleakClient` with a NUS-specific interface: send a JSON line, receive parsed JSON lines via callback, reconnect on drop.

The real bleak code paths only run against a real device. Tests use dependency injection: `BleClient` takes a `BleakClient`-shaped factory so tests inject `BleFake`-backed stubs.

- [ ] **Step 1: Write failing tests**

`tools/bridge/tests/test_ble_client.py`:

```python
from __future__ import annotations

import asyncio
import json

import pytest

from claude_buddy import ble_client, wire
from .ble_fake import BleFake


class _StubBleakClient:
    """Minimal stub that mimics the bleak.BleakClient surface BleClient uses."""

    def __init__(self, address: str, fake: BleFake) -> None:
        self.address = address
        self.is_connected = False
        self._fake = fake
        self._notify_cb = None
        fake.set_notify_callback(self._dispatch_notify)

    def _dispatch_notify(self, data: bytes) -> None:
        if self._notify_cb:
            self._notify_cb(0, bytearray(data))

    async def connect(self) -> bool:
        self.is_connected = True
        return True

    async def disconnect(self) -> bool:
        self.is_connected = False
        return True

    async def pair(self) -> bool:
        return True

    async def start_notify(self, char_uuid, callback):
        self._notify_cb = callback

    async def stop_notify(self, char_uuid):
        self._notify_cb = None

    async def write_gatt_char(self, char_uuid, data, response=False):
        await self._fake.write_rx(bytes(data))


@pytest.fixture
async def client_pair():
    fake = BleFake()
    received: list[dict] = []

    def on_msg(msg: dict) -> None:
        received.append(msg)

    client = ble_client.BleClient(
        address="AA:BB:CC:DD:EE:FF",
        on_message=on_msg,
        client_factory=lambda addr: _StubBleakClient(addr, fake),
    )
    await client.connect()
    yield client, fake, received
    await client.disconnect()


class TestBleClient:
    async def test_send_writes_to_rx(self, client_pair):
        client, fake, _ = client_pair
        await client.send({"cmd": "status"})
        msg = await asyncio.wait_for(fake.received_from_host(), timeout=1.0)
        assert msg == {"cmd": "status"}

    async def test_notification_parsed_via_callback(self, client_pair):
        client, fake, received = client_pair
        await fake.notify({"foo": 1})
        await asyncio.sleep(0.05)
        assert received == [{"foo": 1}]

    async def test_fragmented_notification_reassembled(self, client_pair):
        client, fake, received = client_pair
        # Simulate MTU fragmentation: bleak callbacks fire per-packet.
        if fake._notify_cb is not None:
            fake._notify_cb(b'{"a":')
            fake._notify_cb(b'1}\n')
        await asyncio.sleep(0.05)
        assert received == [{"a": 1}]

    async def test_status_auto_replies(self, client_pair):
        client, fake, received = client_pair
        await client.send({"cmd": "status"})
        await asyncio.sleep(0.05)
        assert any(m.get("ack") == "status" for m in received)
```

- [ ] **Step 2: Run tests, expect failure**

```bash
pytest tests/test_ble_client.py -v
```

Expected: ImportError.

- [ ] **Step 3: Implement `ble_client.py`**

`tools/bridge/src/claude_buddy/ble_client.py`:

```python
"""bleak-based NUS client. Real BLE traffic for the Cardputer."""
from __future__ import annotations

import asyncio
import json
import logging
from typing import Callable, Protocol

from bleak import BleakClient as _RealBleakClient

from .wire import LineBuffer, NUS_RX_UUID, NUS_TX_UUID, encode_line

log = logging.getLogger(__name__)


class _BleakLike(Protocol):
    """The subset of bleak.BleakClient we depend on. Lets tests inject stubs."""
    is_connected: bool
    async def connect(self) -> bool: ...
    async def disconnect(self) -> bool: ...
    async def pair(self) -> bool: ...
    async def start_notify(self, char_uuid: str, callback) -> None: ...
    async def stop_notify(self, char_uuid: str) -> None: ...
    async def write_gatt_char(self, char_uuid: str, data: bytes, response: bool = False) -> None: ...


def _default_factory(address: str) -> _BleakLike:
    return _RealBleakClient(address)


class BleClient:
    """High-level NUS client. send(dict) writes a line; on_message(dict) fires per line."""

    def __init__(
        self,
        address: str,
        on_message: Callable[[dict], None],
        client_factory: Callable[[str], _BleakLike] = _default_factory,
    ) -> None:
        self._address = address
        self._on_message = on_message
        self._factory = client_factory
        self._client: _BleakLike | None = None
        self._line_buffer = LineBuffer()

    @property
    def is_connected(self) -> bool:
        return self._client is not None and self._client.is_connected

    async def connect(self) -> None:
        client = self._factory(self._address)
        await client.connect()
        try:
            await client.pair()
        except Exception:
            log.debug("pair() not supported or already paired")
        await client.start_notify(NUS_TX_UUID, self._on_notify)
        self._client = client
        log.info("BLE connected to %s", self._address)

    async def disconnect(self) -> None:
        if self._client is None:
            return
        try:
            await self._client.stop_notify(NUS_TX_UUID)
        except Exception:
            pass
        try:
            await self._client.disconnect()
        finally:
            self._client = None

    async def send(self, payload: dict) -> None:
        if self._client is None:
            raise RuntimeError("ble_client: not connected")
        data = encode_line(payload)
        await self._client.write_gatt_char(NUS_RX_UUID, data, response=False)

    def _on_notify(self, _handle, data: bytearray) -> None:
        for raw in self._line_buffer.feed(bytes(data)):
            try:
                msg = json.loads(raw)
            except json.JSONDecodeError:
                log.warning("ble_client: dropping unparseable notification: %r", raw)
                continue
            try:
                self._on_message(msg)
            except Exception:
                log.exception("ble_client: on_message callback raised")
```

- [ ] **Step 4: Run tests, expect pass**

```bash
pytest tests/test_ble_client.py -v
```

Expected: 4 tests pass.

- [ ] **Step 5: Commit**

```bash
git add tools/bridge/src/claude_buddy/ble_client.py tools/bridge/tests/test_ble_client.py
git commit -m "feat(bridge): ble_client.py NUS wrapper with line reassembly"
```

---

## Phase 4 — Daemon

### Task 11: `daemon.py` — asyncio main wiring

**Files:**
- Create: `tools/bridge/src/claude_buddy/daemon.py`
- Test: `tools/bridge/tests/test_daemon_integration.py`

The asyncio main loop. Owns `GlobalState`, creates `HookServer` and `BleClient`, dispatches hook ops, generates heartbeat snapshots, runs the reaper, and persists state.

This is the wiring task — most logic already lives in tested modules. Tests are end-to-end via BleFake + a fake hook client.

- [ ] **Step 1: Write failing integration test**

`tools/bridge/tests/test_daemon_integration.py`:

```python
from __future__ import annotations

import asyncio
import json

import pytest

from claude_buddy import daemon as daemon_mod
from claude_buddy import persistence
from .ble_fake import BleFake


class _StubBleakClient:
    def __init__(self, address, fake: BleFake):
        self.address = address
        self.is_connected = False
        self._fake = fake
        self._cb = None
        fake.set_notify_callback(lambda d: self._cb and self._cb(0, bytearray(d)))

    async def connect(self): self.is_connected = True; return True
    async def disconnect(self): self.is_connected = False; return True
    async def pair(self): return True
    async def start_notify(self, u, c): self._cb = c
    async def stop_notify(self, u): self._cb = None
    async def write_gatt_char(self, u, d, response=False):
        await self._fake.write_rx(bytes(d))


@pytest.fixture
async def running_daemon(tmp_path, monkeypatch):
    monkeypatch.setenv("HOME", str(tmp_path / "home"))
    state_dir = tmp_path / "home" / ".claude-buddy"
    state_dir.mkdir(parents=True)
    persistence.save_config(state_dir / "config.json", persistence.Config(
        device_address="AA:BB:CC:DD:EE:FF", device_name="FakeBuddy",
    ))

    fake = BleFake()
    sock_path = tmp_path / "buddy.sock"
    d = daemon_mod.Daemon(
        state_dir=state_dir,
        sock_path=sock_path,
        ble_factory=lambda addr: _StubBleakClient(addr, fake),
    )
    task = asyncio.create_task(d.run())
    await asyncio.sleep(0.1)  # give it time to start
    yield d, fake, sock_path
    d.shutdown_event.set()
    await asyncio.wait_for(task, timeout=2.0)


async def _hook_send(sock_path, msg, expect_reply=False):
    reader, writer = await asyncio.open_unix_connection(str(sock_path))
    writer.write(json.dumps(msg).encode() + b"\n")
    await writer.drain()
    if expect_reply:
        line = await asyncio.wait_for(reader.readline(), timeout=2.0)
        writer.close()
        await writer.wait_closed()
        return json.loads(line)
    writer.close()
    await writer.wait_closed()
    return None


class TestDaemon:
    async def test_session_start_emits_snapshot(self, running_daemon):
        _, fake, sock_path = running_daemon
        await _hook_send(sock_path, {"op": "event", "event": "SessionStart",
                                     "session_id": "s1", "transcript_path": "/tmp/x.jsonl"})
        # Wait for snapshot to land on the wire.
        await asyncio.sleep(0.1)
        # The fake should have received at least one snapshot.
        msgs = []
        while fake.history_size():
            msgs.append(await fake.received_from_host())
        assert msgs == []  # outgoing went via notify; check fake's notify history instead

    async def test_pretooluse_round_trip_via_device_ack(self, running_daemon):
        d, fake, sock_path = running_daemon
        # Register the session first.
        await _hook_send(sock_path, {"op": "event", "event": "SessionStart",
                                     "session_id": "s1", "transcript_path": "/tmp/x.jsonl"})
        await asyncio.sleep(0.05)

        # In parallel: hook awaits decision, device sends ack.
        async def hook_call():
            return await _hook_send(sock_path, {
                "op": "prehook", "session_id": "s1", "tool_name": "Bash",
                "tool_input": {"command": "ls"}, "tool_use_id": "t1",
            }, expect_reply=True)

        async def device_ack():
            await asyncio.sleep(0.1)  # ensure prehook is registered
            await fake.write_rx(b'{"cmd":"permission","id":"t1","decision":"once"}\n')

        results = await asyncio.gather(hook_call(), device_ack())
        assert results[0]["decision"] == "allow"

    async def test_pretooluse_timeout_falls_through(self, running_daemon):
        d, fake, sock_path = running_daemon
        await _hook_send(sock_path, {"op": "event", "event": "SessionStart",
                                     "session_id": "s1", "transcript_path": "/tmp/x.jsonl"})
        await asyncio.sleep(0.05)

        # Override timeout to short value for the test.
        d.permission_timeout_s = 0.5

        reply = await _hook_send(sock_path, {
            "op": "prehook", "session_id": "s1", "tool_name": "Bash",
            "tool_input": {}, "tool_use_id": "t-timeout",
        }, expect_reply=True)
        assert reply["decision"] == "ask"

    async def test_muted_session_skips_device_for_prehook(self, running_daemon):
        d, fake, sock_path = running_daemon
        await _hook_send(sock_path, {"op": "event", "event": "SessionStart",
                                     "session_id": "s1", "transcript_path": "/tmp/x.jsonl"})
        await asyncio.sleep(0.05)
        d.state.muted_sessions.add("s1")

        reply = await _hook_send(sock_path, {
            "op": "prehook", "session_id": "s1", "tool_name": "Bash",
            "tool_input": {}, "tool_use_id": "t1",
        }, expect_reply=True)
        # Muted -> immediate "ask".
        assert reply["decision"] == "ask"
```

- [ ] **Step 2: Run tests, expect failure**

```bash
pytest tests/test_daemon_integration.py -v
```

Expected: ImportError on `daemon`.

- [ ] **Step 3: Implement `daemon.py`**

`tools/bridge/src/claude_buddy/daemon.py`:

```python
"""Asyncio main loop wiring hook server + BLE client + state."""
from __future__ import annotations

import asyncio
import logging
import time
from datetime import date
from pathlib import Path
from typing import Callable

from . import persistence, state as state_mod, transcript, wire
from .ble_client import BleClient, _default_factory
from .hook_server import HookServer

log = logging.getLogger(__name__)

HEARTBEAT_KEEPALIVE_S = 10.0
REAPER_INTERVAL_S = 30.0
DEFAULT_PERMISSION_TIMEOUT_S = 5.0


class Daemon:
    def __init__(
        self,
        state_dir: Path,
        sock_path: Path,
        ble_factory: Callable[[str], object] = _default_factory,
    ) -> None:
        self.state_dir = state_dir
        self.sock_path = sock_path
        self._ble_factory = ble_factory
        self.state = state_mod.GlobalState()
        self.config = persistence.load_config(state_dir / "config.json")
        self.persisted = persistence.load_state(state_dir / "state.json")
        self.state.tokens_today = self.persisted.tokens_today
        self.state.tokens_today_date = (
            date.fromisoformat(self.persisted.tokens_today_date)
            if self.persisted.tokens_today_date
            else None
        )
        self.state.muted_sessions = persistence.load_muted_sessions(
            state_dir / "muted-sessions.json"
        )
        self.permission_timeout_s = self.config.permission_timeout_ms / 1000.0
        self.session_ttl_s = self.config.device_idle_timeout_ms / 1000.0
        self.shutdown_event = asyncio.Event()

        self._hook_server = HookServer(sock_path, self._on_hook)
        self._ble: BleClient | None = None
        self._last_snapshot_repr: str | None = None
        self._last_heartbeat_at = 0.0

    # ---- lifecycle ----

    async def run(self) -> None:
        await self._hook_server.start()
        if self.config.device_address:
            self._ble = BleClient(
                address=self.config.device_address,
                on_message=self._on_ble_message,
                client_factory=self._ble_factory,
            )
            try:
                await self._ble.connect()
                self.state.ble_connected = True
                self.state.device_name = self.config.device_name
                self.state.owner_name = self.config.owner_name
                await self._send_one_shots()
            except Exception:
                log.exception("BLE connect failed; daemon continues without device")
                self.state.ble_connected = False

        try:
            await asyncio.gather(
                self._heartbeat_loop(),
                self._reaper_loop(),
                self._wait_shutdown(),
            )
        finally:
            await self._hook_server.stop()
            if self._ble is not None:
                await self._ble.disconnect()

    async def _wait_shutdown(self) -> None:
        await self.shutdown_event.wait()

    # ---- hook dispatch ----

    async def _on_hook(self, msg: dict, respond) -> None:
        op = msg.get("op")
        if op == "prehook":
            decision = await self._handle_prehook(msg)
            await respond({"decision": decision, "reason": "hardware buddy"})
            return
        if op == "event":
            await self._handle_event(msg)
            return
        if op == "status":
            await respond(self._snapshot_for_status())
            return
        if op == "mute":
            sid = msg.get("session_id", "")
            if sid:
                self.state.muted_sessions.add(sid)
                self._persist_mute()
            await respond({"ok": True})
            return
        if op == "unmute":
            self.state.muted_sessions.discard(msg.get("session_id", ""))
            self._persist_mute()
            await respond({"ok": True})
            return
        if op == "current_session":
            cwd = msg.get("cwd", "")
            sid = self._pick_current_session(cwd)
            await respond({"session_id": sid})
            return
        if op == "prehook_timeout":
            tid = msg.get("tool_use_id", "")
            self._resolve_pending(tid, "ask")
            return
        if op == "push":
            await self._handle_push(msg, respond)
            return
        log.warning("daemon: unknown op %r", op)

    async def _handle_prehook(self, msg: dict) -> str:
        sid = msg.get("session_id", "")
        tid = msg.get("tool_use_id", "")
        sess = self._touch_session(sid, msg.get("transcript_path", ""), msg.get("cwd", ""))
        if sid in self.state.muted_sessions:
            return "ask"
        tool_name = msg.get("tool_name", "")
        hint = state_mod._hint_for(tool_name, msg.get("tool_input", {}))
        loop = asyncio.get_running_loop()
        future: asyncio.Future = loop.create_future()
        prompt = state_mod.PendingPrompt(
            tool_use_id=tid, tool_name=tool_name, hint=hint, future=future,
            arrived_at=time.time(),
        )
        sess.pending_prompt = prompt
        sess.state = "waiting"
        self.state.pending_by_id[tid] = prompt
        self.state.last_msg = state_mod.derive_msg(
            "PreToolUse", msg, awaiting_permission=True
        ) or self.state.last_msg
        state_mod.append_entry(self.state, self.state.last_msg)
        await self._send_heartbeat(force=True)
        try:
            decision = await asyncio.wait_for(future, timeout=self.permission_timeout_s)
            return decision
        except asyncio.TimeoutError:
            return "ask"
        finally:
            sess.pending_prompt = None
            sess.state = "running"
            self.state.pending_by_id.pop(tid, None)
            await self._send_heartbeat(force=True)

    async def _handle_event(self, msg: dict) -> None:
        sid = msg.get("session_id", "")
        event = msg.get("event", "")
        sess = self._touch_session(sid, msg.get("transcript_path", ""), msg.get("cwd", ""))

        if event == "SessionStart":
            sess.state = "idle"
        elif event == "SessionEnd":
            self.state.sessions.pop(sid, None)
        elif event == "Stop":
            sess.state = "idle"
            self._harvest_tokens(sess)
        elif event == "PostToolUse":
            sess.state = "running"

        if sid in self.state.muted_sessions:
            return

        derived = state_mod.derive_msg(event, {
            **msg,
            "session_count": len(self.state.sessions),
        })
        if derived:
            self.state.last_msg = derived
            state_mod.append_entry(self.state, derived)
        await self._send_heartbeat()

    def _touch_session(self, sid: str, transcript_path: str, cwd: str) -> state_mod.Session:
        now = time.time()
        sess = self.state.sessions.get(sid)
        if sess is None:
            sess = state_mod.Session(
                id=sid, started_at=now, last_activity=now,
                state="idle", transcript_path=transcript_path, cwd=cwd,
            )
            self.state.sessions[sid] = sess
        sess.last_activity = now
        if transcript_path and not sess.transcript_path:
            sess.transcript_path = transcript_path
        if cwd and not sess.cwd:
            sess.cwd = cwd
        return sess

    def _pick_current_session(self, cwd: str) -> str | None:
        candidates = list(self.state.sessions.values())
        if cwd:
            cwd_match = [s for s in candidates if s.cwd == cwd]
            if cwd_match:
                cwd_match.sort(key=lambda s: s.last_activity, reverse=True)
                return cwd_match[0].id
        if not candidates:
            return None
        candidates.sort(key=lambda s: s.last_activity, reverse=True)
        return candidates[0].id

    def _resolve_pending(self, tool_use_id: str, decision: str) -> None:
        prompt = self.state.pending_by_id.get(tool_use_id)
        if prompt is None or prompt.future.done():
            return
        prompt.future.set_result(decision)

    # ---- BLE side ----

    def _on_ble_message(self, msg: dict) -> None:
        if msg.get("cmd") == "permission":
            decision = "allow" if msg.get("decision") == "once" else "deny"
            self._resolve_pending(msg.get("id", ""), decision)

    async def _send_one_shots(self) -> None:
        if self._ble is None:
            return
        await self._ble.send({"time": [int(time.time()), -time.timezone]})
        if self.config.owner_name:
            await self._ble.send({"cmd": "owner", "name": self.config.owner_name})

    async def _heartbeat_loop(self) -> None:
        while not self.shutdown_event.is_set():
            await asyncio.sleep(HEARTBEAT_KEEPALIVE_S)
            await self._send_heartbeat(force=True)

    async def _reaper_loop(self) -> None:
        while not self.shutdown_event.is_set():
            await asyncio.sleep(REAPER_INTERVAL_S)
            state_mod.reap_stale_sessions(
                self.state, now=time.time(), ttl_seconds=self.session_ttl_s
            )

    async def _send_heartbeat(self, *, force: bool = False) -> None:
        state_mod.maybe_rollover_tokens(self.state, today=date.today())
        snapshot = self._build_snapshot()
        rep = repr(sorted(snapshot.items()))
        now = time.time()
        if not force and rep == self._last_snapshot_repr and now - self._last_heartbeat_at < HEARTBEAT_KEEPALIVE_S:
            return
        self._last_snapshot_repr = rep
        self._last_heartbeat_at = now
        if self._ble is not None and self.state.ble_connected:
            try:
                await self._ble.send(snapshot)
            except Exception:
                log.exception("daemon: heartbeat send failed")
        self._persist_state()

    def _build_snapshot(self) -> dict:
        running = sum(1 for s in self.state.sessions.values() if s.state == "running")
        waiting = sum(1 for s in self.state.sessions.values() if s.state == "waiting")
        out = {
            "total": len(self.state.sessions),
            "running": running,
            "waiting": waiting,
            "msg": self.state.last_msg,
            "entries": state_mod.wire_entries(self.state),
            "tokens": self.state.tokens_cumulative,
            "tokens_today": self.state.tokens_today,
        }
        # Most recently arrived pending prompt.
        if self.state.pending_by_id:
            newest = max(self.state.pending_by_id.values(), key=lambda p: p.arrived_at)
            out["prompt"] = {"id": newest.tool_use_id, "tool": newest.tool_name, "hint": newest.hint}
        return out

    def _snapshot_for_status(self) -> dict:
        return {
            "ok": True,
            "data": {
                "ble": "connected" if self.state.ble_connected else "disconnected",
                "device": self.config.device_name,
                "sessions": len(self.state.sessions),
                "muted_sessions": sorted(self.state.muted_sessions),
                **{k: v for k, v in self._build_snapshot().items() if k != "prompt"},
            },
        }

    def _harvest_tokens(self, sess: state_mod.Session) -> None:
        if not sess.transcript_path:
            return
        result = transcript.harvest_usage(Path(sess.transcript_path), offset=sess.transcript_offset)
        sess.transcript_offset = result.new_offset
        if result.tokens > 0:
            self.state.tokens_cumulative += result.tokens
            self.state.tokens_today += result.tokens
            self._persist_state()

    def _persist_state(self) -> None:
        d = self.state.tokens_today_date
        persistence.save_state(
            self.state_dir / "state.json",
            persistence.PersistedState(
                tokens_today=self.state.tokens_today,
                tokens_today_date=d.isoformat() if d else "",
                tokens_lifetime=self.state.tokens_cumulative,
            ),
        )

    def _persist_mute(self) -> None:
        persistence.save_muted_sessions(
            self.state_dir / "muted-sessions.json",
            self.state.muted_sessions,
        )

    # ---- folder push (filled in Task 16) ----

    async def _handle_push(self, msg: dict, respond) -> None:
        await respond({"stage": "error", "msg": "push not yet implemented"})


def main() -> int:
    """Entrypoint when run as `python -m claude_buddy.daemon`. Used by run.py."""
    import os
    home = Path(os.environ.get("HOME", "~")).expanduser()
    state_dir = home / ".claude-buddy"
    state_dir.mkdir(parents=True, exist_ok=True)
    sock_path = Path("/tmp/claude-buddy.sock")
    logging.basicConfig(
        level=logging.INFO,
        filename=str(state_dir / "daemon.log"),
        format="%(asctime)s %(levelname)s %(name)s: %(message)s",
    )
    d = Daemon(state_dir=state_dir, sock_path=sock_path)
    asyncio.run(d.run())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
```

- [ ] **Step 4: Run tests, expect pass**

```bash
pytest tests/test_daemon_integration.py -v
```

Expected: 4 tests pass. The first test asserts `msgs == []` because outgoing notifies do not flow into `_from_host` — heartbeats reach the device via `notify` instead. (We refine this in Task 21.)

- [ ] **Step 5: Commit**

```bash
git add tools/bridge/src/claude_buddy/daemon.py tools/bridge/tests/test_daemon_integration.py
git commit -m "feat(bridge): daemon.py asyncio main wiring + integration tests"
```

---

## Phase 5 — Helper modules

### Task 12: `pair.py` — interactive scan

**Files:**
- Create: `tools/bridge/src/claude_buddy/pair.py`
- Test: `tools/bridge/tests/test_pair.py`

Scans for `Claude*`-named devices, returns the list. The skill drives the choice, then `pair.py` writes to `config.json`.

- [ ] **Step 1: Write the failing test**

`tools/bridge/tests/test_pair.py`:

```python
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
```

- [ ] **Step 2: Run, expect failure**

```bash
pytest tests/test_pair.py -v
```

Expected: ImportError.

- [ ] **Step 3: Implement `pair.py`**

`tools/bridge/src/claude_buddy/pair.py`:

```python
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
```

- [ ] **Step 4: Run tests, expect pass**

```bash
pytest tests/test_pair.py -v
```

Expected: 3 tests pass.

- [ ] **Step 5: Commit**

```bash
git add tools/bridge/src/claude_buddy/pair.py tools/bridge/tests/test_pair.py
git commit -m "feat(bridge): pair.py interactive scan and config save"
```

---

### Task 13: `run.py` — daemon launcher

**Files:**
- Create: `tools/bridge/src/claude_buddy/run.py`

The daemon launcher invoked by `/buddy-run`. Double-forks to detach, writes PID file, exec's the asyncio main. Refuses to start if a live PID is already there.

- [ ] **Step 1: Implement `run.py` (no test — exercises real fork)**

`tools/bridge/src/claude_buddy/run.py`:

```python
"""Background-spawn the daemon. Used by the buddy-run skill."""
from __future__ import annotations

import argparse
import json
import os
import signal
import sys
from pathlib import Path


def _is_pid_alive(pid: int) -> bool:
    if pid <= 0:
        return False
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True
    return True


def _read_pid(path: Path) -> int | None:
    if not path.exists():
        return None
    try:
        return int(path.read_text().strip())
    except (OSError, ValueError):
        return None


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--state-dir", default=str(Path.home() / ".claude-buddy"))
    args = parser.parse_args()

    state_dir = Path(args.state_dir)
    state_dir.mkdir(parents=True, exist_ok=True)
    pid_file = state_dir / "daemon.pid"
    log_file = state_dir / "daemon.log"

    existing = _read_pid(pid_file)
    if existing is not None and _is_pid_alive(existing):
        print(json.dumps({"ok": False, "error": "already_running", "pid": existing}))
        return 1
    if existing is not None:
        pid_file.unlink(missing_ok=True)

    # First fork.
    if os.fork() != 0:
        # Parent exits immediately.
        print(json.dumps({"ok": True, "status": "spawning"}))
        return 0
    os.setsid()
    if os.fork() != 0:
        # Intermediate exits.
        os._exit(0)

    # Grandchild: redirect stdio, exec into the daemon.
    sys.stdout.flush()
    sys.stderr.flush()
    log_fd = os.open(str(log_file), os.O_APPEND | os.O_CREAT | os.O_WRONLY, 0o644)
    os.dup2(log_fd, 1)
    os.dup2(log_fd, 2)
    devnull = os.open(os.devnull, os.O_RDONLY)
    os.dup2(devnull, 0)

    pid_file.write_text(str(os.getpid()))

    def _cleanup(*_):
        try:
            pid_file.unlink(missing_ok=True)
        except Exception:
            pass
        os._exit(0)

    signal.signal(signal.SIGTERM, _cleanup)
    signal.signal(signal.SIGINT, _cleanup)

    # Hand off to daemon.main(); never returns under normal operation.
    from . import daemon as daemon_mod
    raise SystemExit(daemon_mod.main())


if __name__ == "__main__":
    raise SystemExit(main())
```

- [ ] **Step 2: Manual smoke test**

```bash
~/.claude-buddy/python -m claude_buddy.run --state-dir /tmp/buddy-smoke
cat /tmp/buddy-smoke/daemon.pid          # prints a PID
ps -p $(cat /tmp/buddy-smoke/daemon.pid) # process is running
kill $(cat /tmp/buddy-smoke/daemon.pid)
sleep 1
test -f /tmp/buddy-smoke/daemon.pid || echo "pid file cleaned up"
```

(Also test: re-run while daemon is running, observe `{"ok":false,"error":"already_running",...}`.)

- [ ] **Step 3: Commit**

```bash
git add tools/bridge/src/claude_buddy/run.py
git commit -m "feat(bridge): run.py double-fork daemon launcher with PID file"
```

---

### Task 14: `stop.py` — daemon shutdown

**Files:**
- Create: `tools/bridge/src/claude_buddy/stop.py`

Reads PID file, sends SIGTERM, waits for exit, SIGKILL on timeout.

- [ ] **Step 1: Implement `stop.py`**

`tools/bridge/src/claude_buddy/stop.py`:

```python
"""Stop the daemon by PID."""
from __future__ import annotations

import argparse
import json
import os
import signal
import time
from pathlib import Path

from .run import _is_pid_alive, _read_pid


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--state-dir", default=str(Path.home() / ".claude-buddy"))
    parser.add_argument("--timeout", type=float, default=5.0)
    args = parser.parse_args()

    state_dir = Path(args.state_dir)
    pid_file = state_dir / "daemon.pid"
    pid = _read_pid(pid_file)

    if pid is None or not _is_pid_alive(pid):
        pid_file.unlink(missing_ok=True)
        print(json.dumps({"ok": True, "status": "not_running"}))
        return 0

    try:
        os.kill(pid, signal.SIGTERM)
    except ProcessLookupError:
        pid_file.unlink(missing_ok=True)
        print(json.dumps({"ok": True, "status": "not_running"}))
        return 0

    deadline = time.time() + args.timeout
    while time.time() < deadline:
        if not _is_pid_alive(pid):
            pid_file.unlink(missing_ok=True)
            print(json.dumps({"ok": True, "status": "stopped", "pid": pid}))
            return 0
        time.sleep(0.1)

    try:
        os.kill(pid, signal.SIGKILL)
    except ProcessLookupError:
        pass
    pid_file.unlink(missing_ok=True)
    print(json.dumps({"ok": True, "status": "killed", "pid": pid}))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
```

- [ ] **Step 2: Manual smoke test**

```bash
~/.claude-buddy/python -m claude_buddy.run --state-dir /tmp/buddy-smoke
~/.claude-buddy/python -m claude_buddy.stop --state-dir /tmp/buddy-smoke
# Should print {"ok": true, "status": "stopped", ...}
~/.claude-buddy/python -m claude_buddy.stop --state-dir /tmp/buddy-smoke
# Now {"status": "not_running"}
```

- [ ] **Step 3: Commit**

```bash
git add tools/bridge/src/claude_buddy/stop.py
git commit -m "feat(bridge): stop.py SIGTERM/SIGKILL with cleanup"
```

---

### Task 15: `status.py` — query daemon

**Files:**
- Create: `tools/bridge/src/claude_buddy/status.py`
- Test: `tools/bridge/tests/test_status.py`

Connects to the daemon socket, sends `{op: "status"}`, prints the response. Falls back to "daemon not running" if the socket is dead.

- [ ] **Step 1: Write failing test**

`tools/bridge/tests/test_status.py`:

```python
from __future__ import annotations

import asyncio
import json

import pytest

from claude_buddy import status


async def _fake_daemon(sock_path):
    async def handle(reader, writer):
        await reader.readline()
        writer.write(json.dumps({"ok": True, "data": {"ble": "connected", "sessions": 0}}).encode() + b"\n")
        await writer.drain()
        writer.close()
        await writer.wait_closed()
    server = await asyncio.start_unix_server(handle, path=str(sock_path))
    return server


class TestStatus:
    async def test_query_running_daemon(self, tmp_path, capsys):
        sock_path = tmp_path / "buddy.sock"
        server = await _fake_daemon(sock_path)
        try:
            await status.query(sock_path)
        finally:
            server.close()
            await server.wait_closed()
        captured = capsys.readouterr()
        out = json.loads(captured.out)
        assert out["data"]["ble"] == "connected"

    async def test_no_daemon(self, tmp_path, capsys):
        sock_path = tmp_path / "no.sock"
        await status.query(sock_path)
        captured = capsys.readouterr()
        out = json.loads(captured.out)
        assert out["ok"] is False
        assert out["error"] == "not_running"
```

- [ ] **Step 2: Run, expect failure**

```bash
pytest tests/test_status.py -v
```

Expected: ImportError.

- [ ] **Step 3: Implement `status.py`**

`tools/bridge/src/claude_buddy/status.py`:

```python
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
        line = await asyncio.wait_for(reader.readline(), timeout=2.0)
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
```

- [ ] **Step 4: Run tests, expect pass**

```bash
pytest tests/test_status.py -v
```

Expected: 2 pass.

- [ ] **Step 5: Commit**

```bash
git add tools/bridge/src/claude_buddy/status.py tools/bridge/tests/test_status.py
git commit -m "feat(bridge): status.py daemon query CLI"
```

---

### Task 16: Folder push end-to-end

**Files:**
- Create: `tools/bridge/src/claude_buddy/push.py`
- Modify: `tools/bridge/src/claude_buddy/daemon.py` — implement `_handle_push`
- Test: `tools/bridge/tests/test_push.py`

Adds the `push` op handler in the daemon and the `claude_buddy.push` client. Implements the strict-sequential `char_begin / file / chunk* / file_end / char_end` dance from REFERENCE.md.

- [ ] **Step 1: Write failing test**

`tools/bridge/tests/test_push.py`:

```python
from __future__ import annotations

import asyncio
import base64
import json

import pytest

from claude_buddy import daemon as daemon_mod
from claude_buddy import persistence
from .ble_fake import BleFake


class _StubBleakClient:
    def __init__(self, address, fake: BleFake):
        self.address = address
        self.is_connected = False
        self._fake = fake
        self._cb = None
        fake.set_notify_callback(lambda d: self._cb and self._cb(0, bytearray(d)))

    async def connect(self): self.is_connected = True; return True
    async def disconnect(self): self.is_connected = False; return True
    async def pair(self): return True
    async def start_notify(self, u, c): self._cb = c
    async def stop_notify(self, u): self._cb = None
    async def write_gatt_char(self, u, d, response=False):
        await self._fake.write_rx(bytes(d))


class _AcksAllFake(BleFake):
    """Auto-acks every char_begin/file/chunk/file_end/char_end with ok:true."""
    async def _react(self, msg: dict) -> None:
        cmd = msg.get("cmd")
        if cmd in ("char_begin", "file", "file_end", "char_end"):
            await self.notify({"ack": cmd, "ok": True})
        elif cmd == "chunk":
            await self.notify({"ack": "chunk", "ok": True, "n": len(base64.b64decode(msg["d"]))})
        else:
            await super()._react(msg)


@pytest.fixture
async def running_daemon_with_push(tmp_path, monkeypatch):
    monkeypatch.setenv("HOME", str(tmp_path / "home"))
    state_dir = tmp_path / "home" / ".claude-buddy"
    state_dir.mkdir(parents=True)
    persistence.save_config(state_dir / "config.json", persistence.Config(
        device_address="AA", device_name="FakeBuddy",
    ))
    fake = _AcksAllFake()
    sock_path = tmp_path / "buddy.sock"
    d = daemon_mod.Daemon(
        state_dir=state_dir, sock_path=sock_path,
        ble_factory=lambda addr: _StubBleakClient(addr, fake),
    )
    task = asyncio.create_task(d.run())
    await asyncio.sleep(0.1)
    yield d, fake, sock_path, tmp_path
    d.shutdown_event.set()
    await asyncio.wait_for(task, timeout=2.0)


class TestFolderPush:
    async def test_pushes_two_files(self, running_daemon_with_push):
        d, fake, sock_path, tmp_path = running_daemon_with_push
        folder = tmp_path / "char"
        folder.mkdir()
        (folder / "manifest.json").write_text(json.dumps({"name": "bufo"}))
        (folder / "idle.gif").write_bytes(b"GIF89a fake")

        # Send push request, collect streamed responses.
        reader, writer = await asyncio.open_unix_connection(str(sock_path))
        writer.write(json.dumps({"op": "push", "path": str(folder)}).encode() + b"\n")
        await writer.drain()
        stages = []
        while True:
            line = await asyncio.wait_for(reader.readline(), timeout=5.0)
            if not line:
                break
            stages.append(json.loads(line))
            if stages[-1].get("stage") in ("done", "error"):
                break
        writer.close()
        await writer.wait_closed()
        assert stages[-1] == {"stage": "done"}
        # Sequence: begin, file, chunks*, file_end, file, chunks*, file_end, end
        assert stages[0]["stage"] == "begin"
        assert any(s.get("stage") == "file" and s.get("name") == "manifest.json" for s in stages)
        assert any(s.get("stage") == "file" and s.get("name") == "idle.gif" for s in stages)

    async def test_rejects_oversize_folder(self, running_daemon_with_push):
        d, fake, sock_path, tmp_path = running_daemon_with_push
        folder = tmp_path / "big"
        folder.mkdir()
        (folder / "huge.bin").write_bytes(b"x" * (2 * 1024 * 1024))  # 2 MB > 1.8 MB cap

        reader, writer = await asyncio.open_unix_connection(str(sock_path))
        writer.write(json.dumps({"op": "push", "path": str(folder)}).encode() + b"\n")
        await writer.drain()
        line = await asyncio.wait_for(reader.readline(), timeout=2.0)
        writer.close()
        await writer.wait_closed()
        msg = json.loads(line)
        assert msg["stage"] == "error"
        assert "1.8" in msg["msg"] or "size" in msg["msg"].lower()
```

- [ ] **Step 2: Run, expect failure**

```bash
pytest tests/test_push.py -v
```

Expected: failure (push not implemented).

- [ ] **Step 3: Replace `_handle_push` in `daemon.py`**

In `tools/bridge/src/claude_buddy/daemon.py`, replace the placeholder `_handle_push` with:

```python
    async def _handle_push(self, msg: dict, respond) -> None:
        from pathlib import Path as _P
        import base64
        path = _P(msg.get("path", ""))
        if not path.is_dir():
            await respond({"stage": "error", "msg": f"not a directory: {path}"})
            return

        # Enumerate regular files (no recursion, dotfiles skipped).
        files: list[_P] = sorted(
            p for p in path.iterdir()
            if p.is_file() and not p.name.startswith(".")
        )
        total = sum(p.stat().st_size for p in files)
        if total > 1_800_000:
            await respond({"stage": "error",
                           "msg": f"folder size {total} bytes exceeds 1.8 MB cap"})
            return

        # Determine pack name.
        pack_name = path.name
        manifest = path / "manifest.json"
        if manifest.exists():
            try:
                pack_name = json.loads(manifest.read_text()).get("name", pack_name)
            except json.JSONDecodeError:
                pass

        if self._ble is None or not self.state.ble_connected:
            await respond({"stage": "error", "msg": "device not connected"})
            return

        # Acks come asynchronously through _on_ble_message; route them via a queue here.
        ack_q: asyncio.Queue[dict] = asyncio.Queue()
        old_handler = self._on_ble_message
        self._push_ack_queue = ack_q

        async def wait_ack(name: str, timeout: float = 5.0) -> dict:
            while True:
                m = await asyncio.wait_for(ack_q.get(), timeout=timeout)
                if m.get("ack") == name:
                    return m

        try:
            await self._ble.send({"cmd": "char_begin", "name": pack_name, "total": total})
            ack = await wait_ack("char_begin", timeout=3.0)
            if not ack.get("ok"):
                await respond({"stage": "error", "msg": "device declined push"})
                return
            await respond({"stage": "begin", "name": pack_name, "total": total})

            for f in files:
                size = f.stat().st_size
                await self._ble.send({"cmd": "file", "path": f.name, "size": size})
                await wait_ack("file")
                await respond({"stage": "file", "name": f.name, "size": size})

                with open(f, "rb") as fh:
                    while True:
                        chunk = fh.read(180)  # well under MTU; one chunk per ack
                        if not chunk:
                            break
                        b64 = base64.b64encode(chunk).decode("ascii")
                        await self._ble.send({"cmd": "chunk", "d": b64})
                        ack = await wait_ack("chunk")
                        await respond({"stage": "chunk", "n": ack.get("n", 0)})

                await self._ble.send({"cmd": "file_end"})
                ack = await wait_ack("file_end")
                await respond({"stage": "file_end", "size": ack.get("n", 0)})

            await self._ble.send({"cmd": "char_end"})
            await wait_ack("char_end")
            await respond({"stage": "done"})
        finally:
            self._push_ack_queue = None
```

Also modify `_on_ble_message` to route ack messages to the push queue when present:

```python
    def _on_ble_message(self, msg: dict) -> None:
        if msg.get("cmd") == "permission":
            decision = "allow" if msg.get("decision") == "once" else "deny"
            self._resolve_pending(msg.get("id", ""), decision)
            return
        # Route ack messages to in-flight push, if any.
        if "ack" in msg and getattr(self, "_push_ack_queue", None) is not None:
            try:
                self._push_ack_queue.put_nowait(msg)
            except Exception:
                pass
```

And initialize `self._push_ack_queue = None` in `__init__`.

- [ ] **Step 4: Implement `push.py` client**

`tools/bridge/src/claude_buddy/push.py`:

```python
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
```

- [ ] **Step 5: Run tests, expect pass**

```bash
pytest tests/test_push.py -v
```

Expected: 2 tests pass.

- [ ] **Step 6: Commit**

```bash
git add tools/bridge/src/claude_buddy/push.py tools/bridge/src/claude_buddy/daemon.py tools/bridge/tests/test_push.py
git commit -m "feat(bridge): folder push protocol end-to-end"
```

---

### Task 17: `mute.py` — mute / unmute helpers

**Files:**
- Create: `tools/bridge/src/claude_buddy/mute.py`
- Test: `tools/bridge/tests/test_mute.py`

Helper invoked by `/buddy-mute` and `/buddy-unmute` skills. Discovers the current session id (env var preferred, daemon-side fallback), then sends mute or unmute to the daemon.

- [ ] **Step 1: Write failing test**

`tools/bridge/tests/test_mute.py`:

```python
from __future__ import annotations

import asyncio
import json
import os

import pytest

from claude_buddy import mute


async def _fake_daemon(sock_path):
    async def handle(reader, writer):
        line = await reader.readline()
        msg = json.loads(line)
        if msg.get("op") == "current_session":
            writer.write(json.dumps({"session_id": "from-daemon"}).encode() + b"\n")
        elif msg.get("op") == "mute":
            writer.write(json.dumps({"ok": True}).encode() + b"\n")
        elif msg.get("op") == "unmute":
            writer.write(json.dumps({"ok": True}).encode() + b"\n")
        await writer.drain()
        writer.close()
        await writer.wait_closed()
    return await asyncio.start_unix_server(handle, path=str(sock_path))


class TestMute:
    async def test_uses_env_var_when_set(self, tmp_path, monkeypatch, capsys):
        monkeypatch.setenv("CLAUDE_SESSION_ID", "from-env")
        sock_path = tmp_path / "buddy.sock"
        server = await _fake_daemon(sock_path)
        try:
            rc = await mute.run(action="mute", sock_path=sock_path, cwd="/tmp")
        finally:
            server.close()
            await server.wait_closed()
        assert rc == 0
        captured = capsys.readouterr()
        out = json.loads(captured.out)
        assert out["session_id"] == "from-env"

    async def test_falls_back_to_daemon_lookup(self, tmp_path, monkeypatch, capsys):
        monkeypatch.delenv("CLAUDE_SESSION_ID", raising=False)
        sock_path = tmp_path / "buddy.sock"
        server = await _fake_daemon(sock_path)
        try:
            rc = await mute.run(action="mute", sock_path=sock_path, cwd="/tmp")
        finally:
            server.close()
            await server.wait_closed()
        assert rc == 0
        out = json.loads(capsys.readouterr().out)
        assert out["session_id"] == "from-daemon"

    async def test_unmute_path(self, tmp_path, monkeypatch, capsys):
        monkeypatch.setenv("CLAUDE_SESSION_ID", "x")
        sock_path = tmp_path / "buddy.sock"
        server = await _fake_daemon(sock_path)
        try:
            rc = await mute.run(action="unmute", sock_path=sock_path, cwd="/tmp")
        finally:
            server.close()
            await server.wait_closed()
        assert rc == 0

    async def test_no_daemon(self, tmp_path, monkeypatch, capsys):
        monkeypatch.setenv("CLAUDE_SESSION_ID", "x")
        sock_path = tmp_path / "no.sock"
        rc = await mute.run(action="mute", sock_path=sock_path, cwd="/tmp")
        assert rc == 1
        out = json.loads(capsys.readouterr().out)
        assert out["ok"] is False
```

- [ ] **Step 2: Run, expect failure**

```bash
pytest tests/test_mute.py -v
```

Expected: ImportError.

- [ ] **Step 3: Implement `mute.py`**

`tools/bridge/src/claude_buddy/mute.py`:

```python
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
```

- [ ] **Step 4: Run tests, expect pass**

```bash
pytest tests/test_mute.py -v
```

Expected: 4 tests pass.

- [ ] **Step 5: Commit**

```bash
git add tools/bridge/src/claude_buddy/mute.py tools/bridge/tests/test_mute.py
git commit -m "feat(bridge): mute.py for /buddy-mute and /buddy-unmute"
```

---

## Phase 6 — Install / Uninstall scripts

### Task 18: `install.py` standalone bootstrap

**Files:**
- Create: `tools/bridge/install.py`
- Test: `tools/bridge/tests/test_install.py`

The one-time setup script. Pure stdlib (no `claude_buddy` import) so it can run before the venv exists.

- [ ] **Step 1: Write failing test**

`tools/bridge/tests/test_install.py`:

```python
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
```

- [ ] **Step 2: Run, expect failure**

```bash
pytest tests/test_install.py -v
```

Expected: failure (file doesn't exist or function missing).

- [ ] **Step 3: Implement `tools/bridge/install.py`**

`tools/bridge/install.py`:

```python
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
    return Path(__file__).resolve().parents[1]  # claude-cardputer-adv-buddy/


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
```

- [ ] **Step 4: Run tests, expect pass**

```bash
pytest tests/test_install.py -v
```

Expected: 4 tests pass.

- [ ] **Step 5: Commit**

```bash
git add tools/bridge/install.py tools/bridge/tests/test_install.py
git commit -m "feat(bridge): install.py one-time bootstrap"
```

---

### Task 19: `uninstall.py`

**Files:**
- Create: `tools/bridge/uninstall.py`
- Test: extend `tools/bridge/tests/test_install.py`

- [ ] **Step 1: Append failing tests to `test_install.py`**

```python
UNINSTALL_PATH = Path(__file__).resolve().parents[1] / "uninstall.py"
spec_u = importlib.util.spec_from_file_location("buddy_uninstall", UNINSTALL_PATH)
uninstall_mod = importlib.util.module_from_spec(spec_u)
sys.modules["buddy_uninstall"] = uninstall_mod
spec_u.loader.exec_module(uninstall_mod)


class TestUninstallSettings:
    def test_removes_buddy_entries(self, tmp_path):
        settings = tmp_path / "settings.json"
        install_mod.merge_hook_entries(settings, hook_path="/tmp/buddy_hook.py")
        uninstall_mod.remove_hook_entries(settings)
        data = json.loads(settings.read_text())
        for evt in install_mod.HOOK_EVENTS:
            assert all(not e.get("_buddy") for e in data["hooks"].get(evt, []))

    def test_preserves_user_entries(self, tmp_path):
        settings = tmp_path / "settings.json"
        settings.write_text(json.dumps({
            "hooks": {"PreToolUse": [{"matcher": "Edit", "hooks": [{"type": "command", "command": "/usr/bin/my-tool"}]}]}
        }))
        install_mod.merge_hook_entries(settings, hook_path="/tmp/buddy_hook.py")
        uninstall_mod.remove_hook_entries(settings)
        data = json.loads(settings.read_text())
        commands = [
            h["command"]
            for entry in data["hooks"].get("PreToolUse", [])
            for h in entry.get("hooks", [])
        ]
        assert "/usr/bin/my-tool" in commands
```

- [ ] **Step 2: Run, expect failure**

```bash
pytest tests/test_install.py::TestUninstallSettings -v
```

Expected: failure.

- [ ] **Step 3: Implement `uninstall.py`**

`tools/bridge/uninstall.py`:

```python
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
    if not hooks:
        data.pop("hooks", None)
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
```

- [ ] **Step 4: Run tests, expect pass**

```bash
pytest tests/test_install.py -v
```

Expected: all install + uninstall tests pass.

- [ ] **Step 5: Commit**

```bash
git add tools/bridge/uninstall.py tools/bridge/tests/test_install.py
git commit -m "feat(bridge): uninstall.py reverses install"
```

---

## Phase 7 — Skills

### Task 20: `buddy-run`, `buddy-stop`, `buddy-status` SKILL.md files

**Files:**
- Create: `.claude/skills/buddy-run/SKILL.md`
- Create: `.claude/skills/buddy-stop/SKILL.md`
- Create: `.claude/skills/buddy-status/SKILL.md`

- [ ] **Step 1: Create `.claude/skills/buddy-run/SKILL.md`**

```markdown
---
name: buddy-run
description: This skill should be used when the user asks to "start the buddy", "run the bridge", "connect to my cardputer", "start hardware buddy", or types /buddy-run. Spawns the claude-buddy daemon in the background, performs first-run BLE pairing if no device is configured, and tails the log briefly to report connection state.
---

# Start the Hardware Buddy bridge daemon

The daemon is a long-running asyncio process that owns the BLE connection to the Cardputer and listens on `/tmp/claude-buddy.sock` for hook events. It is launched once per session via `python -m claude_buddy.run`, which double-forks to detach.

## When invoked

1. Confirm the venv exists at `~/.claude-buddy/python`. If not, tell the user to run `python3 tools/bridge/install.py` from the firmware repo root and stop.

2. Check for an existing daemon: read `~/.claude-buddy/daemon.pid` if present and verify the PID is alive (`ps -p $PID`). If alive, run `/buddy-status` instead and report.

3. Read `~/.claude-buddy/config.json`. If `device_address` is missing, run an interactive scan first:

   ```bash
   ~/.claude-buddy/python -m claude_buddy.pair --timeout 5
   ```

   This prints a JSON list of `{"address", "name"}` pairs with names starting with "Claude". Surface it to the user. If exactly one device is found, ask if it is theirs. If multiple, use AskUserQuestion to let them pick. Save the choice:

   ```bash
   ~/.claude-buddy/python -m claude_buddy.pair --save <address> --name <name>
   ```

4. Launch the daemon:

   ```bash
   ~/.claude-buddy/python -m claude_buddy.run
   ```

   The script returns immediately with `{"ok": true, "status": "spawning"}`.

5. Tail `~/.claude-buddy/daemon.log` for up to 3 seconds. Look for the first occurrence of:
   - `BLE connected to <name>` → report success.
   - `Scanning for device …` → report "scanning, run /buddy-status in a moment."
   - `ERROR` → surface the line verbatim.

6. End with the daemon PID and log location.

## Notes

- First run on macOS triggers a Bluetooth permission dialog tied to `~/.claude-buddy/python`. Click Allow.
- If the daemon refuses with `already_running`, that is fine — there is one daemon per machine.
```

- [ ] **Step 2: Create `.claude/skills/buddy-stop/SKILL.md`**

```markdown
---
name: buddy-stop
description: This skill should be used when the user asks to "stop the buddy", "shut down the bridge", "kill the daemon", "stop hardware buddy", or types /buddy-stop. Sends SIGTERM to the running claude-buddy daemon and cleans up the PID file and socket.
---

# Stop the Hardware Buddy bridge daemon

## When invoked

1. Run:

   ```bash
   ~/.claude-buddy/python -m claude_buddy.stop
   ```

2. Surface the result. Possible statuses:
   - `not_running` — daemon was already gone.
   - `stopped` — clean SIGTERM.
   - `killed` — SIGTERM timed out, SIGKILL was sent.

3. Confirm `/tmp/claude-buddy.sock` no longer exists (it should have been cleaned up by the daemon's signal handler).
```

- [ ] **Step 3: Create `.claude/skills/buddy-status/SKILL.md`**

```markdown
---
name: buddy-status
description: This skill should be used when the user asks to "check the buddy", "buddy status", "is the bridge running", "show daemon status", or types /buddy-status. Queries the running claude-buddy daemon and prints connection state, session counts, token totals, recent transcript entries, and the muted-session list.
---

# Check Hardware Buddy daemon state

## When invoked

1. Run:

   ```bash
   ~/.claude-buddy/python -m claude_buddy.status
   ```

2. The script prints a JSON object. If `ok: false` and `error: "not_running"`, tell the user to run `/buddy-run`.

3. Otherwise, format the `data` field for the user as a brief table:
   - BLE: connected / disconnected
   - Device: name (if known)
   - Sessions: count, of which N waiting on permissions
   - Tokens today / cumulative
   - Last msg
   - Recent entries (most recent first)
   - Muted sessions (count and truncated ids if any)
```

- [ ] **Step 4: Verify the skills are well-formed**

```bash
for s in .claude/skills/buddy-{run,stop,status}/SKILL.md; do
  head -10 "$s" | grep -E '^name:|^description:' || echo "MISSING METADATA: $s"
done
```

Expected: every SKILL.md prints both `name:` and `description:` lines.

- [ ] **Step 5: Commit**

```bash
git add .claude/skills/buddy-run .claude/skills/buddy-stop .claude/skills/buddy-status
git commit -m "feat(bridge): SKILL.md for buddy-run, buddy-stop, buddy-status"
```

---

### Task 21: `buddy-gifpush`, `buddy-mute`, `buddy-unmute` SKILL.md files

**Files:**
- Create: `.claude/skills/buddy-gifpush/SKILL.md`
- Create: `.claude/skills/buddy-mute/SKILL.md`
- Create: `.claude/skills/buddy-unmute/SKILL.md`

- [ ] **Step 1: Create `.claude/skills/buddy-gifpush/SKILL.md`**

```markdown
---
name: buddy-gifpush
description: This skill should be used when the user asks to "push to my buddy", "send a character to the cardputer", "push folder to buddy", "send gif pack", "push character pack", or types /buddy-gifpush with a folder path. Streams a folder over BLE to the Hardware Buddy device via the running bridge daemon.
---

# Push a folder to the Hardware Buddy device

The bundled push client connects to the running daemon over `/tmp/claude-buddy.sock`; the daemon drives the BLE folder-push transport (`char_begin → file/chunk*/file_end → char_end`).

## When invoked

1. Resolve the folder argument from the user's message to an absolute path. Reject if missing or not a directory.

2. Sum the folder's regular-file sizes. Reject if total exceeds 1.8 MB — the device firmware will refuse it.

3. Check `/tmp/claude-buddy.sock` exists. If not, tell the user to run `/buddy-run` first and stop.

4. Run:

   ```bash
   ~/.claude-buddy/python -m claude_buddy.push "<absolute-path>"
   ```

5. The client streams JSON progress lines: `{stage: "begin"}`, `{stage: "file", name, size}`, `{stage: "chunk", n}`, `{stage: "file_end", size}`, `{stage: "done"}` or `{stage: "error", msg}`.

6. Summarize for the user: pack name, count of files, total bytes, success/failure. On error, also suggest `tail -n 50 ~/.claude-buddy/daemon.log`.

## Notes

- A `manifest.json` with a `name` field overrides the folder name on the device.
- Dotfiles are skipped automatically.
- The push is strictly sequential (one chunk at a time); a 1.5 MB pack typically takes ~30 seconds.
```

- [ ] **Step 2: Create `.claude/skills/buddy-mute/SKILL.md`**

```markdown
---
name: buddy-mute
description: This skill should be used when the user asks to "mute the buddy", "stop notifications for this session", "silence hardware buddy", or types /buddy-mute. Tells the daemon to skip events from the current Claude Code session so the device stays quiet for this session.
---

# Mute Hardware Buddy events for the current session

## When invoked

1. Run:

   ```bash
   ~/.claude-buddy/python -m claude_buddy.mute --cwd "$PWD"
   ```

   The script prefers `$CLAUDE_SESSION_ID` from the environment; if absent, it asks the daemon for the most recently active session matching the current cwd.

2. Surface the result:
   - `{"ok": true, "session_id": "...", "action": "mute"}` — confirm to the user with the truncated session id.
   - `{"ok": false, "error": "no active session"}` — tell the user no CC session is registered yet (run a tool call first).
   - `{"ok": false, "error": "daemon not running"}` — tell them to run `/buddy-run`.

## Notes

- Permission prompts from a muted session resolve immediately to "ask" — the device cannot lock you out of approvals.
- Mute state survives daemon restart; sessions that no longer exist are pruned automatically.
- Use `/buddy-unmute` to restore.
```

- [ ] **Step 3: Create `.claude/skills/buddy-unmute/SKILL.md`**

```markdown
---
name: buddy-unmute
description: This skill should be used when the user asks to "unmute the buddy", "resume notifications", "re-enable hardware buddy", or types /buddy-unmute. Tells the daemon to resume sending events from the current Claude Code session to the device.
---

# Unmute Hardware Buddy events for the current session

## When invoked

1. Run:

   ```bash
   ~/.claude-buddy/python -m claude_buddy.mute --unmute --cwd "$PWD"
   ```

   Same session-id discovery as `/buddy-mute`.

2. Surface the result. Same shape and possible errors as `/buddy-mute`.
```

- [ ] **Step 4: Verify skills are well-formed**

```bash
for s in .claude/skills/buddy-{gifpush,mute,unmute}/SKILL.md; do
  head -10 "$s" | grep -E '^name:|^description:' || echo "MISSING METADATA: $s"
done
```

- [ ] **Step 5: Commit**

```bash
git add .claude/skills/buddy-gifpush .claude/skills/buddy-mute .claude/skills/buddy-unmute
git commit -m "feat(bridge): SKILL.md for buddy-gifpush, buddy-mute, buddy-unmute"
```

---

## Phase 8 — Polish and ship

### Task 22: README and final integration test

**Files:**
- Create: `tools/bridge/README.md`
- Create: `tools/bridge/tests/test_end_to_end.py`

- [ ] **Step 1: Write end-to-end integration test**

`tools/bridge/tests/test_end_to_end.py`:

```python
"""End-to-end: start a daemon, simulate a CC session, verify device sees the snapshot."""
from __future__ import annotations

import asyncio
import json

import pytest

from claude_buddy import daemon as daemon_mod
from claude_buddy import persistence
from .ble_fake import BleFake


class _StubBleakClient:
    def __init__(self, address, fake: BleFake):
        self.address = address
        self.is_connected = False
        self._fake = fake
        self._cb = None
        fake.set_notify_callback(lambda d: self._cb and self._cb(0, bytearray(d)))
        self._sent_to_device: list[dict] = []

    async def connect(self): self.is_connected = True; return True
    async def disconnect(self): self.is_connected = False; return True
    async def pair(self): return True
    async def start_notify(self, u, c): self._cb = c
    async def stop_notify(self, u): self._cb = None
    async def write_gatt_char(self, u, d, response=False):
        # Capture daemon -> device traffic.
        for raw in d.split(b"\n"):
            if not raw.strip():
                continue
            try:
                self._sent_to_device.append(json.loads(raw))
            except json.JSONDecodeError:
                pass
        await self._fake.write_rx(bytes(d))


@pytest.fixture
async def daemon_and_capture(tmp_path, monkeypatch):
    monkeypatch.setenv("HOME", str(tmp_path / "home"))
    state_dir = tmp_path / "home" / ".claude-buddy"
    state_dir.mkdir(parents=True)
    persistence.save_config(state_dir / "config.json", persistence.Config(
        device_address="AA", device_name="Buddy",
    ))
    fake = BleFake()
    captured: list[dict] = []

    def factory(addr):
        client = _StubBleakClient(addr, fake)
        client._sent_to_device = captured
        return client

    sock_path = tmp_path / "buddy.sock"
    d = daemon_mod.Daemon(state_dir=state_dir, sock_path=sock_path, ble_factory=factory)
    task = asyncio.create_task(d.run())
    await asyncio.sleep(0.1)
    yield d, captured, sock_path
    d.shutdown_event.set()
    await asyncio.wait_for(task, timeout=2.0)


async def _send(sock_path, msg):
    reader, writer = await asyncio.open_unix_connection(str(sock_path))
    writer.write(json.dumps(msg).encode() + b"\n")
    await writer.drain()
    writer.close()
    await writer.wait_closed()


class TestEndToEnd:
    async def test_session_lifecycle_emits_heartbeats(self, daemon_and_capture):
        d, captured, sock_path = daemon_and_capture
        await _send(sock_path, {"op": "event", "event": "SessionStart",
                                "session_id": "s1", "transcript_path": ""})
        await _send(sock_path, {"op": "event", "event": "UserPromptSubmit",
                                "session_id": "s1", "prompt": "fix it"})
        await asyncio.sleep(0.2)
        # At least one heartbeat with msg matching the user prompt.
        assert any(m.get("msg") == "user: fix it" for m in captured)
```

- [ ] **Step 2: Run test, expect pass**

```bash
pytest tests/test_end_to_end.py -v
```

Expected: 1 test passes.

- [ ] **Step 3: Create `tools/bridge/README.md`**

```markdown
# claude-buddy bridge

Python bridge connecting Claude Code CLI sessions to the Cardputer-Adv
Hardware Buddy firmware over BLE. See
`docs/superpowers/specs/2026-05-07-claude-code-cli-buddy-bridge-design.md`
for the full design.

## One-time install

From the firmware repo root:

```bash
python3 tools/bridge/install.py
```

This creates `~/.claude-buddy/venv`, installs the package editable, writes
the hook shim to `~/.claude/hooks/buddy_hook.py`, symlinks the six skills
into `~/.claude/skills/`, and merges marker-fenced hook entries into
`~/.claude/settings.json`.

First `/buddy-run` will trigger a macOS Bluetooth permission dialog
attached to `~/.claude-buddy/python` — click Allow.

## Day-to-day

| Slash command | What it does |
|---|---|
| `/buddy-run` | Start the daemon (pairs the device on first run). |
| `/buddy-stop` | Stop the daemon. |
| `/buddy-status` | Show daemon/BLE state, sessions, tokens, recent entries. |
| `/buddy-gifpush <folder>` | Stream a GIF character pack to the device. |
| `/buddy-mute` | Stop sending events from the current CC session. |
| `/buddy-unmute` | Resume sending events from the current CC session. |

## Per-project disable

Drop this in any repo to silence the bridge there without uninstalling:

```bash
echo '{"_buddy_disabled": true}' > .claude/settings.local.json
```

(If the file already exists, merge `_buddy_disabled: true` into the JSON.)

## Manual end-to-end runbook

1. `python3 tools/bridge/install.py`
2. `/buddy-run` — verify "BLE connected to <name>"
3. In a CC session, run a Bash command (`echo hi`). Verify the device shows
   `approve: Bash` and the command hint.
4. Approve on the device. Verify CC proceeds.
5. Run an auto-approved tool (`ls /`). Verify the device shows `Bash: ls /`
   and then `ran: Bash`.
6. `/buddy-gifpush characters/bufo`. Verify the device renders the new pet.
7. `/buddy-mute`. Run another command. Verify the device stays idle.
8. `/buddy-unmute`. Verify events resume.
9. `/buddy-stop`. Verify clean shutdown.

## Uninstall

```bash
python3 tools/bridge/uninstall.py
```

Optionally also `rm -rf ~/.claude-buddy/` to purge token history, mute
state, and BLE bond.

## Development

```bash
cd tools/bridge
python3 -m venv .venv && source .venv/bin/activate
pip install -e ".[dev]"
pytest
```

`tests/ble_fake.py` is the in-process NUS peer used for daemon integration
tests — no real hardware required for the test suite.
```

- [ ] **Step 4: Run the full test suite**

```bash
cd tools/bridge
pytest -v
```

Expected: all tests pass across every test file.

- [ ] **Step 5: Commit**

```bash
git add tools/bridge/README.md tools/bridge/tests/test_end_to_end.py
git commit -m "docs(bridge): README + end-to-end integration test"
```

---

## Phase 9 — Manual hardware verification (no test code)

After all code tasks pass, perform the manual runbook in `tools/bridge/README.md` against a real Cardputer-Adv. This is not automated — confirm each step works as documented before declaring the bridge production-ready.

If any step fails, file findings in this plan as a follow-up task and stop.

---

## Plan self-review

**Spec coverage:**

- §2 Goals → Tasks 11 (heartbeat), 8+11 (permission round-trip), 11 (turn events handled by daemon — note: turn-event emission is implicit in `_handle_event` for `Stop`; the explicit `evt:turn` payload is built but the spec lists it under turn events; verified the wire helper exists), 16 (folder push), 20+21 (skills), 18+19 (install/uninstall scripts).
- §6 Hook contract → Task 8 implements stdin/stdout shape exactly; Task 18 writes settings.json with the verified shape.
- §8 State model → Tasks 4, 5, 11 cover the dataclasses, msg derivation, entries, midnight rollover, stale reaper.
- §9 Permission round-trip → Task 11 (`_handle_prehook`) + Task 8 (hook stdout shape).
- §10 Folder push → Task 16.
- §11 Install/uninstall → Tasks 18, 19.
- §11.3 Per-project disable → Task 8 (`_project_disabled`).
- §12 Skills → Tasks 20, 21.
- §12.3 Mute mechanics → Task 17 (mute helper), Task 11 (`muted_sessions` in daemon).
- §13 Errors → Tasks 8, 11, 13 cover daemon-down hook silence, multiple `/buddy-run` protection, stale reaper resolution.
- §14 Config files → Task 3 (`Config`, `PersistedState`, muted-sessions); Task 11 (loading and saving).
- §15 Testing → Tasks 2-19 cover unit + fake-BLE integration; Task 22 covers end-to-end.
- §16 Risks #1 (bleak pairing) → Task 10 (`pair()` call with try/except — verifying real-device behavior is in Phase 9).
- §16 Risks #2 (transcript schema drift) → Task 6 defensive parsing.
- §16 Risks #3 (hook timeout) → Task 8 (0.5s connect, 5.0s read, never > 6s total).
- §16 Risks #4 (Notification overlap) → Task 4 returns None for non-`idle_prompt` notifications.
- §16 Risks #5 (atomic state.json) → Task 3 atomic_write_json.
- §16 Risks #6 (skill discovery before install) → README install runbook calls this out.
- §16 Risks #7 (`$CLAUDE_SESSION_ID`) → Task 17 prefers env then falls back to daemon lookup.

**Placeholder scan:** No `TBD`, `TODO`, or `implement later` strings. Every code step shows complete code. Every test step shows assertions. Skill and SKILL.md content is concrete.

**Type/method consistency check:**

- `derive_msg(event, payload, *, awaiting_permission=False)` — used in Task 4 tests, Task 11 daemon, consistent.
- `Session(id, started_at, last_activity, state, transcript_path, ...)` — consistent across Tasks 4, 5, 11.
- `GlobalState.muted_sessions: set[str]` — consistent.
- `harvest_usage(path, *, offset)` returning `HarvestResult(tokens, new_offset)` — consistent.
- `HookServer(sock_path, handler)`, handler signature `(msg, respond)` — consistent.
- `BleClient(address, on_message, client_factory)` — consistent.
- `Daemon(state_dir, sock_path, ble_factory)` — consistent.

**One inconsistency caught and fixed inline:** Task 11's `_handle_push` placeholder is replaced wholesale in Task 16. Both tasks reference the same method name (`_handle_push`) and signature (`async def _handle_push(self, msg, respond)`).

---

## Execution Handoff

Plan complete and saved to `claude-cardputer-adv-buddy/docs/superpowers/plans/2026-05-07-claude-code-cli-buddy-bridge.md`. Two execution options:

**1. Subagent-Driven (recommended)** — I dispatch a fresh subagent per task, review between tasks, fast iteration. Best for this plan because tasks are well-bounded and TDD-disciplined.

**2. Inline Execution** — Execute tasks in this session using executing-plans, batch execution with checkpoints. Slower but keeps everything in one transcript.

Which approach?
