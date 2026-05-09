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
