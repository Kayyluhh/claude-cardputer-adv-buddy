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
