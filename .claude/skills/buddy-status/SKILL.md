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
