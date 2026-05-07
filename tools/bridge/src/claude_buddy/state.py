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
