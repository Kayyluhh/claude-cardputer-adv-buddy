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
