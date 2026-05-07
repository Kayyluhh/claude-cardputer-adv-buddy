from __future__ import annotations

import asyncio
from datetime import date, datetime
from unittest.mock import patch

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
            future=asyncio.new_event_loop().create_future(),
            arrived_at=0,
        )
        sess.pending_prompt = prompt
        gs.pending_by_id["t1"] = prompt
        state.reap_stale_sessions(gs, now=2000, ttl_seconds=600)
        assert "t1" not in gs.pending_by_id
        # The pending future is resolved with "ask" so any waiting hook unblocks.
        assert prompt.future.done()
        assert prompt.future.result() == "ask"
