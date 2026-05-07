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
        return 0

    sock_path = os.environ.get("BUDDY_SOCKET", DEFAULT_SOCKET)

    if event == "PreToolUse":
        decision, reason = _request_permission(sock_path, payload)
        _emit_pretooluse(decision, reason or "hardware buddy")
        return 0

    _send_event(sock_path, payload)
    return 0


def _request_permission(sock_path: str, payload: dict) -> tuple[str, str]:
    """Contact the daemon and return (decision, reason).

    decision is one of "allow", "deny", "ask".
    reason is the daemon-supplied string, or "" on any failure (caller applies default).
    On any failure returns ("ask", "").
    """
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
            return ("ask", "")
        reply = json.loads(line)
        decision = reply.get("decision", "ask")
        if decision not in ("allow", "deny", "ask"):
            return ("ask", "")
        return (decision, reply.get("reason", ""))
    except (OSError, json.JSONDecodeError, socket.timeout):
        return ("ask", "")


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
