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
