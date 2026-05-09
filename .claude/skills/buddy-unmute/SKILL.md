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
