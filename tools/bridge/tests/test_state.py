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
