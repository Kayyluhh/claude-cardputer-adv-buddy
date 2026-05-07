# Claude Code CLI ↔ Hardware Buddy Bridge

| | |
|---|---|
| **Status** | Approved design, ready for implementation plan |
| **Date** | 2026-05-07 |
| **Owner** | Kayla |
| **Target hardware** | M5Stack Cardputer-Adv running `claude-cardputer-adv-buddy` firmware |
| **Target host** | macOS (developer's machine running Claude Code CLI) |

## 1. Purpose

The `claude-cardputer-adv-buddy` firmware speaks the Hardware Buddy BLE protocol defined in `REFERENCE.md`. That protocol is normally driven by the Claude desktop apps (macOS / Windows). This spec defines a Python bridge that drives the **same wire protocol** from **Claude Code CLI** sessions, so the device displays CC permission prompts, transcript snippets, and token usage exactly as it does for the desktop apps.

The bridge is a long-running local daemon plus six Claude Code skills that drive it (`buddy-run`, `buddy-stop`, `buddy-status`, `buddy-gifpush`, `buddy-mute`, `buddy-unmute`). One-time setup uses two plain Python scripts (`install.py`, `uninstall.py`) — they are run once after cloning, not as skills. Hooks installed in `~/.claude/settings.json` forward CC events to the daemon over a Unix socket; the daemon multiplexes events into the BLE wire protocol and routes the device's permission decisions back to the originating hook.

## 2. Goals

1. Send heartbeat snapshots aggregating across all active CC sessions: `total`, `running`, `waiting`, `msg`, `entries`, `tokens`, `tokens_today`, and `prompt` when a permission decision is needed.
2. Round-trip permission decisions: PreToolUse hook → daemon → device → daemon → hook → CC.
3. Emit one-shot turn events (`{"evt":"turn", ...}`) per completed turn, enforcing the 4KB UTF-8 cap from `REFERENCE.md`.
4. Folder push for GIF character packs via `/buddy-gifpush <folder>`.
5. Per-session mute/unmute via `/buddy-mute` and `/buddy-unmute`.
6. Six Claude Code skills as the user surface — no top-level CLI binary. Install/uninstall are one-time Python scripts run via `python3 tools/bridge/install.py` and `python3 tools/bridge/uninstall.py`.

## 3. Non-goals

- Cross-platform support. macOS only. `bleak` supports Linux/Windows but BLE pairing semantics differ and surfacing those differences is out of scope.
- Background-as-a-service install (launchd, login items). The user explicitly chose manual start/stop.
- A user-facing `claude-buddy` console script. Skills are the only user-facing interface.
- Replacing the desktop apps' Hardware Buddy window. The bridge is a parallel implementation, not a port.
- Multi-device support (multiple Cardputers connected at once). One daemon, one device.

## 4. Architecture overview

A single Python package `claude_buddy` hosts:

- A long-running asyncio daemon (`claude_buddy.daemon`) that owns the BLE connection and a Unix-socket server for hook events.
- A hook-script entry point (`claude_buddy.hook`) that CC invokes from `settings.json`. Reads JSON from stdin, sends it over the socket, and (for PreToolUse only) waits for a reply.
- Per-skill modules (`run`, `stop`, `status`, `push`, `install`, `uninstall`) invoked from each skill's SKILL.md.

The daemon is the **single source of BLE truth.** No skill ever opens its own BLE connection; they all talk to the daemon over `/tmp/claude-buddy.sock`. This avoids fighting the daemon for the device.

Two alternate architectures (HTTP-on-localhost, file-queue) were considered and rejected. Unix sockets give synchronous request/response with the lowest framing overhead, and ND-JSON mirrors the wire format already used over BLE — symmetric and trivial to debug.

## 5. Repository layout

```
claude-cardputer-adv-buddy/
├── .claude/skills/                     # auto-discovered when CC opens this repo
│   ├── buddy-run/SKILL.md
│   ├── buddy-stop/SKILL.md
│   ├── buddy-status/SKILL.md
│   ├── buddy-gifpush/SKILL.md
│   ├── buddy-mute/SKILL.md
│   └── buddy-unmute/SKILL.md
├── tools/bridge/
│   ├── pyproject.toml                  # claude_buddy package; deps: bleak
│   ├── README.md                       # short pointer to skill docs + install runbook
│   ├── install.py                      # standalone bootstrap; run once after clone
│   ├── uninstall.py                    # standalone teardown
│   ├── src/claude_buddy/
│   │   ├── __init__.py
│   │   ├── daemon.py                   # asyncio main: hook server + BLE + state
│   │   ├── hook_server.py              # Unix socket listener
│   │   ├── ble_client.py               # bleak NUS client; reconnect; framing
│   │   ├── state.py                    # SessionTable, msg derivation, tokens, mute set
│   │   ├── persistence.py              # ~/.claude-buddy/state.json + config.json
│   │   ├── transcript.py               # tails session JSONL for usage stats
│   │   ├── wire.py                     # NUS UUIDs, JSON line frame helpers
│   │   ├── hook.py                     # `python -m claude_buddy.hook` entry point
│   │   ├── run.py                      # daemon launcher (PID file, double-fork)
│   │   ├── stop.py
│   │   ├── status.py
│   │   ├── push.py                     # folder push client
│   │   ├── mute.py                     # /buddy-mute and /buddy-unmute helpers
│   │   └── pair.py                     # interactive scan helper used by run.py
│   └── tests/
│       ├── test_state.py
│       ├── test_install.py
│       ├── test_hook_protocol.py
│       ├── test_transcript.py
│       ├── test_mute.py
│       └── test_ble_fake.py            # in-process NUS peer for daemon tests
└── docs/superpowers/specs/
    └── 2026-05-07-claude-code-cli-buddy-bridge-design.md   # this file
```

After `python3 tools/bridge/install.py` runs once, the following user-level state exists:

```
~/.claude-buddy/
├── venv/                               # virtualenv with claude_buddy installed editably
├── python                              # symlink to venv/bin/python
├── config.json                         # {"device_address": "AA:BB:CC:DD:EE:FF", ...}
├── state.json                          # {"tokens_today": 31200, "date": "2026-05-07"}
├── muted-sessions.json                 # {"<session_id>": <epoch_when_muted>}
├── daemon.log
└── daemon.pid

~/.claude/hooks/
└── buddy_hook.py                       # exec ~/.claude-buddy/python -m claude_buddy.hook

~/.claude/skills/buddy-*                # symlinks to <repo>/.claude/skills/buddy-*

~/.claude/settings.json                 # marker-fenced hook entries (see §11)

/tmp/
└── claude-buddy.sock                   # daemon's hook server
```

## 6. Hook contract (verified 2026-05-07)

Source: `https://code.claude.com/docs/en/hooks.md` (verified by `claude-code-guide` agent).

### 6.1 Events the bridge subscribes to

| Event | Use |
|---|---|
| `PreToolUse` | Forward prompt to device; wait up to 5s for `allow`/`deny`; fall through to `ask` on timeout. |
| `PostToolUse` | Fire-and-forget: update session `last_msg = "ran: <tool>"`. |
| `UserPromptSubmit` | Fire-and-forget: append entry `"<HH:MM> user: <first 30 chars>"`. |
| `Stop` | Fire-and-forget: harvest token usage from `transcript_path`. Set `last_msg = "done"`. |
| `SessionStart` | Register session in daemon table with current timestamp. |
| `SessionEnd` | Unregister session. (Not relied on; reaper covers crashes.) |
| `Notification` | Optional: surface `notification_type` events to `msg` (e.g. `idle_prompt`). |

### 6.2 PreToolUse stdin payload (verified)

```json
{
  "session_id": "abc123",
  "transcript_path": "/Users/kayla/.claude/projects/<dir>/abc123.jsonl",
  "cwd": "/path",
  "permission_mode": "default",
  "hook_event_name": "PreToolUse",
  "tool_name": "Bash",
  "tool_input": { "command": "git push" },
  "tool_use_id": "toolu_xyz"
}
```

### 6.3 PreToolUse stdout (verified)

```json
{
  "hookSpecificOutput": {
    "hookEventName": "PreToolUse",
    "permissionDecision": "allow" | "deny" | "ask",
    "permissionDecisionReason": "hardware buddy"
  }
}
```

`"defer"` is documented but only valid in non-interactive mode; the bridge does not use it. Exit code 0 with valid JSON is the only acceptable success path.

### 6.4 Default hook timeout

Command-type hooks default to 600 seconds; the bridge sets `"timeout": 60` in its hook entries to give the 5-second device wait plenty of headroom while still bounding worst-case stalls if the daemon misbehaves.

### 6.5 Settings.json shape

```jsonc
{
  "hooks": {
    "PreToolUse": [
      {
        "_buddy": true,
        "matcher": "*",
        "hooks": [
          { "type": "command", "command": "/Users/<user>/.claude/hooks/buddy_hook.py", "timeout": 60 }
        ]
      }
    ],
    // …same shape for PostToolUse, UserPromptSubmit, Stop, SessionStart, SessionEnd, Notification
  }
}
```

The `"_buddy": true` marker on each top-level matcher object is the install/uninstall fence — only marker-bearing entries are touched. (CC ignores unknown JSON keys; this is a convention used elsewhere by tooling.)

## 7. Wire formats

### 7.1 Hook script ↔ daemon (Unix socket, ND-JSON)

**Hook → daemon (single message per hook invocation):**

```jsonc
// PreToolUse — expects reply
{ "op": "prehook", "session_id": "...", "tool_name": "Bash",
  "tool_input": {...}, "tool_use_id": "...", "cwd": "..." }

// All other events — fire and forget
{ "op": "event", "event": "Stop", "session_id": "...",
  "transcript_path": "...", "stop_reason": "end_turn" }
{ "op": "event", "event": "PostToolUse", "session_id": "...", "tool_name": "...", "tool_use_id": "..." }
{ "op": "event", "event": "UserPromptSubmit", "session_id": "...", "prompt": "..." }
{ "op": "event", "event": "SessionStart", "session_id": "...", "matcher": "startup" }
{ "op": "event", "event": "SessionEnd", "session_id": "...", "matcher": "other" }
{ "op": "event", "event": "Notification", "session_id": "...", "notification_type": "..." }
```

**Daemon → hook (only on PreToolUse, after device responds or timeout):**

```jsonc
{ "decision": "allow" | "deny" | "ask", "reason": "..." }
```

Hook reads exactly one JSON line, closes socket, emits the matching `permissionDecision` JSON to CC. `"ask"` is also returned on socket-write failure or 5s read timeout.

**Skill ↔ daemon (single op, single response):**

```jsonc
{ "op": "status" }
// → { "ok": true, "data": { "ble": "connected", "device": "AA:BB:...", "sessions": 2,
//                            "msg": "approve: Bash", "tokens": 184502, "tokens_today": 31200,
//                            "entries": ["10:42 git push", ...] } }

{ "op": "push", "path": "/abs/path" }
// → streamed: { "stage": "begin", ... }, { "stage": "file", "name": "..." },
//             { "stage": "chunk", "n": 4096 }, { "stage": "done" } | { "stage": "error", "msg": "..." }
```

### 7.2 Daemon ↔ device (BLE NUS, ND-JSON per REFERENCE.md)

Unchanged from `REFERENCE.md`. Service `6e400001-…`, RX `…0002…` (write), TX `…0003…` (notify). Lines terminated with `\n`. The daemon implements:

- Heartbeat snapshot — sent on every state change plus 10s keepalive.
- Turn event — one per Stop, dropped if serialized size > 4KB (UTF-8).
- Time sync + owner one-shots on connect.
- Status command response.
- Folder push sequence (`char_begin` → per-file `file`/`chunk`*/`file_end` → `char_end`).
- Permission ack handler.
- `cmd: name`, `cmd: owner`, `cmd: unpair` ack.

## 8. State model

```python
@dataclass
class Session:
    id: str
    started_at: float
    last_activity: float
    state: Literal["idle", "running", "waiting"]
    pending_prompt: PendingPrompt | None
    last_msg: str
    transcript_path: str
    transcript_offset: int             # last byte read for token harvest

@dataclass
class PendingPrompt:
    tool_use_id: str                   # also used as the protocol id
    tool_name: str
    hint: str                          # cap 30 chars
    future: asyncio.Future             # resolves to "allow" | "deny"
    arrived_at: float

@dataclass
class GlobalState:
    sessions: dict[str, Session]
    pending_by_id: dict[str, PendingPrompt]   # for routing BLE permission acks
    muted_sessions: set[str]                  # persisted; events from these ids skip device
    tokens_cumulative: int                    # since daemon start
    tokens_today: int                         # persisted
    tokens_today_date: date                   # persisted
    entries: deque[str]                       # capped at 8 host-side; 4 sent to device
    last_msg: str
    ble_connected: bool
    device_name: str | None
    owner_name: str | None
```

### 8.1 Heartbeat snapshot derivation

| Field | Derivation |
|---|---|
| `total` | `len(sessions)` |
| `running` | sessions where `state == "running"` |
| `waiting` | sessions where `state == "waiting"` (i.e. `pending_prompt is not None`) |
| `msg` | `GlobalState.last_msg` (latest event wins; see table below) |
| `entries` | last 4 of `GlobalState.entries`, newest first |
| `tokens` | `tokens_cumulative` |
| `tokens_today` | `tokens_today` (after midnight check) |
| `prompt` | `{id, tool, hint}` of the most-recently-arrived `PendingPrompt`, or omitted |

Snapshots are sent on every state mutation **plus** a 10-second keepalive loop. Snapshot equality is checked before send (skip if identical to last sent) to keep BLE traffic bounded under burst loads.

### 8.2 `msg` derivation table

| Source | `msg` |
|---|---|
| PreToolUse with permission required | `approve: <tool_name>` |
| PreToolUse without permission required | `<tool_name>: <hint>` |
| PostToolUse | `ran: <tool_name>` |
| UserPromptSubmit | `user: <first 30 chars of prompt>` |
| Stop | `done` |
| SessionStart (new session) | `<n> sessions` |
| SessionEnd (last session ended) | `idle` |
| Notification (`idle_prompt`) | `idle prompt` |
| Notification (other types) | omit (don't update msg) |

`<hint>` rules per tool:

- `Bash` — `tool_input.command`
- `Edit` / `Write` / `Read` / `NotebookEdit` — `tool_input.file_path` (basename)
- `Grep` / `Glob` — `tool_input.pattern`
- `WebFetch` / `WebSearch` — `tool_input.url` or `tool_input.query`
- everything else — empty string

All hints truncated to 30 UTF-8 chars (not bytes — use char-level slice; the device is fine with multibyte).

### 8.3 Entries

Each event that updates `msg` also appends `"<HH:MM> <msg>"` (local time) to `GlobalState.entries`. `deque(maxlen=8)` host-side; only the newest 4 ride the wire to match the device's display capacity.

### 8.4 Token harvest

CC's Stop hook payload does NOT include token usage (verified). Tokens come from the transcript JSONL.

On every Stop event:

1. Look up `Session.transcript_path` and `Session.transcript_offset`.
2. `seek(offset)` and read to EOF.
3. Parse each line as JSON; for any record with `type == "assistant"` and a `usage` dict, add `usage.output_tokens` (and `usage.cache_creation_input_tokens` if you also want to surface cache cost) to both `tokens_cumulative` and `tokens_today`.
4. Update `transcript_offset` to the new file size.
5. Persist `state.json` if `tokens_today` changed.

If `transcript_path` doesn't exist or isn't readable, skip silently — the bridge tolerates missing telemetry. Token data is best-effort, not a correctness requirement.

### 8.5 `tokens_today` rollover

Checked on every snapshot send: if `local_today() != tokens_today_date`, set `tokens_today = 0`, update `tokens_today_date`, persist.

### 8.6 Stale-session reaper

Every 30 seconds, drop any session with `last_activity < now() - 600s`. SessionEnd is documented as not guaranteed under crashes/SIGHUP/Ctrl-C — the reaper backstops `total`/`running`/`waiting` accuracy.

## 9. Permission round-trip

```
CC ─ stdin ─→ buddy_hook.py
  hook_event_name: PreToolUse
  tool_use_id: toolu_xyz
  tool_name: Bash
  tool_input: { command: "git push" }

buddy_hook.py ─ /tmp/claude-buddy.sock ─→ daemon
  { op: "prehook", session_id, tool_name, tool_input, tool_use_id, cwd }

daemon: registers PendingPrompt[tool_use_id] = (future, ...)
        sets sessions[session_id].state = "waiting"
        sets last_msg = "approve: Bash"

daemon ─ BLE notify ─→ device
  { total: 1, running: 0, waiting: 1, msg: "approve: Bash",
    entries: [...], tokens: ..., tokens_today: ...,
    prompt: { id: "toolu_xyz", tool: "Bash", hint: "git push" } }

device user: presses approve
device ─ BLE write ─→ daemon
  { cmd: "permission", id: "toolu_xyz", decision: "once" }

daemon: PendingPrompt[tool_use_id].future.set_result("allow")
        clears pending_prompt; sessions[session_id].state = "running"
        sends heartbeat without `prompt` field

hook (was awaiting reply): reads { decision: "allow" }
hook ─ stdout ─→ CC
  { hookSpecificOutput: { hookEventName: "PreToolUse",
                          permissionDecision: "allow",
                          permissionDecisionReason: "hardware buddy" } }
```

### 9.1 Timeout / fallback

The hook awaits the daemon reply with a 5-second timeout. On timeout:

1. Hook sends `{op: "prehook_timeout", tool_use_id: "..."}` to the daemon (best-effort, ignore errors).
2. Daemon resolves the future with `"ask"` if it hasn't already, clears `pending_prompt`, removes the prompt from snapshots.
3. Hook emits `{permissionDecision: "ask"}`. CC's native UI runs.

If the socket connect itself fails (daemon down), the hook emits `{permissionDecision: "ask"}` immediately with no socket interaction. **Exit 0 is required** — exit 1 is treated by CC as a non-blocking error notice, and exit 2 blocks the tool call entirely. Both are wrong here; `"ask"` is the right path.

### 9.2 BLE disconnected, daemon up

Daemon accepts hook events normally; heartbeat snapshots have `prompt` omitted because no device is there to display it. PreToolUse reduces to a 5s wait that always times out → `"ask"`. UX-wise this is identical to "daemon down" once the timeout elapses; the failure mode is just slower.

### 9.3 Concurrent prompts across sessions

Each prompt is keyed by `tool_use_id`, which CC guarantees unique per tool call. The daemon's `pending_by_id` map routes BLE acks to the right future. Snapshots only carry **one** `prompt` field at a time (the most recent); the device shows the newest prompt and earlier ones still resolve when their acks arrive (the device tracks ids and only acks the prompt currently displayed — but the daemon doesn't depend on that). If the device acks an unknown id, the daemon logs and ignores it.

## 10. Folder push (`/buddy-gifpush`)

Triggered by `/buddy-gifpush <folder>` or natural-language phrasing. The skill:

1. Resolves folder argument to absolute path; rejects if missing or not a directory.
2. Sums file sizes; rejects if total > 1.8 MB.
3. Verifies `/tmp/claude-buddy.sock` exists.
4. Runs `~/.claude-buddy/python -m claude_buddy.push "<abs-path>"`.
5. Streams stdout progress lines back to the user; surfaces final state.

The push module connects to the socket, sends `{op:"push", path:"<abs-path>"}`, and reads streamed progress objects. The daemon owns the BLE side: enumerates regular files in the folder (no recursion, dotfiles skipped), reads `manifest.json` for the optional `name` override, and runs the strictly-sequential `char_begin → file/chunk*/file_end → char_end` dance with one outstanding chunk at a time. Chunks are base64-encoded inline per `REFERENCE.md`.

If the device fails to ack `char_begin` within ~3s, the daemon aborts and reports `{stage:"error", msg:"device declined push"}`.

## 11. Install / uninstall

Install and uninstall are **standalone Python scripts**, not skills — they run once per machine and a slash-command wrapper would be unnecessary ceremony for a one-shot. They live at `tools/bridge/install.py` and `tools/bridge/uninstall.py` and run on the system `python3` (no `claude_buddy` venv required to start, since the install script's first job is to create that venv).

### 11.1 `python3 tools/bridge/install.py`

Idempotent. Run once after cloning the firmware repo. Accepts `--quiet` to skip the BT permission walk-through.

1. Find the repo root: `Path(__file__).resolve().parents[1]` is `tools/bridge/`; the repo root is one level up. Verify `pyproject.toml` exists in `tools/bridge/`.
2. Create `~/.claude-buddy/` if missing. Create `~/.claude-buddy/venv/` via `python3 -m venv` if missing.
3. `~/.claude-buddy/venv/bin/pip install -e <repo>/tools/bridge` (editable install so firmware-repo edits to the package take effect immediately). Symlink `~/.claude-buddy/python → ~/.claude-buddy/venv/bin/python` (idempotent: `ln -sf`).
4. Write `~/.claude/hooks/buddy_hook.py` (shim that `exec`s the venv python with `-m claude_buddy.hook`). Mode 0755.
5. For each skill in `<repo>/.claude/skills/buddy-*`, create symlink `~/.claude/skills/buddy-<x> → <repo>/.claude/skills/buddy-<x>`. Refuse to overwrite existing non-symlink files at those paths.
6. Read `~/.claude/settings.json` (or initialize `{}`). For each event in `{PreToolUse, PostToolUse, UserPromptSubmit, Stop, SessionStart, SessionEnd, Notification}`:
   - If a top-level matcher object with `_buddy: true` already exists in `hooks[event]`, leave it alone (idempotent).
   - Otherwise append `{ "_buddy": true, "matcher": "*", "hooks": [{ "type": "command", "command": "<HOME>/.claude/hooks/buddy_hook.py", "timeout": 60 }] }`.

   Write via the standard atomic pattern: serialize to `settings.json.tmp` in the same directory, `fsync`, `os.rename` over the original. A backup of the pre-edit file is kept at `~/.claude/settings.json.buddy-bak` (overwritten on each install run).
7. Persist `<repo>` path and version to `~/.claude-buddy/config.json` so the skills and daemon can find canonical paths.
8. Print BT permission warning: "First `/buddy-run` will trigger a macOS Bluetooth permission dialog tied to `~/.claude-buddy/python`. Click Allow."
9. Print install summary: paths created, settings.json entries added, next-step hint to run `/buddy-run` from any CC session inside the firmware repo (or once skills are symlinked, from anywhere).

### 11.2 `python3 tools/bridge/uninstall.py`

1. Remove `_buddy: true` matcher entries from each event in `~/.claude/settings.json`, using the same atomic write pattern as install (tmp + fsync + rename, with `.buddy-bak` backup). Leave hand-edited entries untouched.
2. Delete `~/.claude/hooks/buddy_hook.py`.
3. Remove skill symlinks from `~/.claude/skills/buddy-*` (only if they are symlinks pointing into the firmware repo; refuse to delete real files).
4. Print: "to fully purge, also `rm -rf ~/.claude-buddy/` (this preserves token history, mute state, and BLE bond)."

### 11.3 Per-project disable

`buddy_hook.py`, before any work, reads the merged Claude settings (project-local takes precedence) and checks for `_buddy_disabled: true` at the top level of `<cwd>/.claude/settings.local.json`. If true, the hook exits 0 immediately. This keeps disable state in the same JSON the rest of CC's per-project config lives in — discoverable via `git`, `grep`, and `cat`.

Documented in `tools/bridge/README.md` with a one-liner: `echo '{"_buddy_disabled": true}' > .claude/settings.local.json` (or merge into existing).

## 12. Skills

Each SKILL.md uses imperative form, frontmatter with third-person description and explicit trigger phrases, and is concise (<400 words body). Skills delegate to `~/.claude-buddy/python -m claude_buddy.<module>` for the actual work.

| Skill | Module | Args | Purpose |
|---|---|---|---|
| `buddy-run` | `claude_buddy.run` | none (interactive scan first time) | Background-spawn daemon; tail log 3s; report state. |
| `buddy-stop` | `claude_buddy.stop` | none | SIGTERM daemon, clean up socket and PID file. |
| `buddy-status` | `claude_buddy.status` | none | Print daemon/BLE state, sessions, tokens, recent entries, mute set. |
| `buddy-gifpush` | `claude_buddy.push` | `<folder>` | Push folder to device via daemon. |
| `buddy-mute` | `claude_buddy.mute` | none | Mute the current CC session — daemon stops sending its events to the device. |
| `buddy-unmute` | `claude_buddy.mute --unmute` | none | Restore device events for the current session. |

### 12.1 First-run pairing

`/buddy-run` on a host with no `~/.claude-buddy/config.json`:

1. The skill tells Claude to invoke `~/.claude-buddy/python -m claude_buddy.pair`.
2. The pair module scans for ~5 seconds, lists devices whose name starts with `Claude` plus their MAC.
3. The skill surfaces the list; the user picks one (skill uses `AskUserQuestion` if more than one, auto-selects if exactly one).
4. Skill writes the chosen address to `~/.claude-buddy/config.json` and proceeds to spawn the daemon.

Subsequent `/buddy-run` skips pairing and connects directly.

### 12.2 Run-time daemon launch

`run.py` does a Unix double-fork to detach from the skill's bash subprocess, redirects stdio to `~/.claude-buddy/daemon.log`, writes PID to `~/.claude-buddy/daemon.pid`, then `os.exec`s into the asyncio main loop. Returns the PID to the skill so it can confirm.

The skill then `tail -f`-equivalent watches `~/.claude-buddy/daemon.log` for up to 3 seconds, looking for the first of:

- `BLE connected to <name>` → success.
- `Scanning for device …` → still trying; report "scanning, run /buddy-status in a moment."
- any `ERROR` line → surface verbatim.

Returns within 3 seconds either way.

### 12.3 Mute mechanics

The daemon keeps a set of muted session ids in `GlobalState.muted_sessions`, persisted to `~/.claude-buddy/muted-sessions.json`. When dispatching events, the daemon checks this set and silently skips heartbeat updates and turn events sourced from a muted session id. (Permission round-trip is also skipped: a PreToolUse from a muted session resolves immediately to `decision: "ask"` so CC's native UI runs — that way muting can't lock you out of approvals.)

`/buddy-mute` and `/buddy-unmute` need to know **which session is current**. Two-tier discovery:

1. **Preferred:** read `$CLAUDE_SESSION_ID` from the skill's bash environment. If CC exposes the session id to slash-command subprocesses (to be verified during implementation), this is deterministic.
2. **Fallback:** ask the daemon `{op: "current_session", cwd: "<cwd>"}`. The daemon returns the session id whose `cwd` matches and whose `last_activity` is most recent. If no cwd match, returns the globally most-recent session. If no sessions at all, returns null and the skill reports "no active session to mute."

The skill then sends `{op: "mute", session_id: "..."}` (or `{op: "unmute", session_id}`) to the daemon. Mute state survives daemon restarts via the persisted set; on restart, any muted session ids that no longer exist (because the reaper would have cleaned them) are pruned.

`/buddy-status` includes a `muted_sessions` line listing how many sessions are muted and (if any) the truncated tails of their session ids.

## 13. Errors and edge cases

| Scenario | Behavior |
|---|---|
| Daemon down, hook fires | Hook exits 0 with `permissionDecision:"ask"`. Other events: silent skip. |
| Daemon up, BLE disconnected | Heartbeat omits `prompt`. PreToolUse → 5s timeout → `"ask"`. |
| Daemon BLE drops mid-push | In-flight push aborts; `{stage:"error"}` to skill; daemon retries connect. |
| Multiple `/buddy-run` invocations | Second call sees PID file pointing at live process; refuses with "already running, PID N". Stale PID file (process dead) is cleaned up. |
| `tokens_today` midnight rollover | Detected at next snapshot; resets `tokens_today` and persists. |
| First `/buddy-run` after install | macOS prompts for BT permission tied to `~/.claude-buddy/python`. Daemon waits up to 60s for first scan to succeed before logging permission failure. |
| Settings.json corrupted on uninstall | Refuse to overwrite; report "could not parse, hand-edit required" with the marker pattern to grep for. |
| Hook fires during compaction (SessionStart matcher: "compact") | Treat as normal SessionStart — same `session_id` already exists, just bump `last_activity`. |

## 14. Configuration files

### 14.1 `~/.claude-buddy/config.json`

```json
{
  "device_address": "AA:BB:CC:DD:EE:FF",
  "device_name": "Clawd",
  "owner_name": "Kayla",
  "permission_timeout_ms": 5000,
  "device_idle_timeout_ms": 600000
}
```

Editable by hand. `permission_timeout_ms` and `device_idle_timeout_ms` exposed for tuning.

### 14.2 `~/.claude-buddy/state.json`

```json
{
  "tokens_today": 31200,
  "tokens_today_date": "2026-05-07",
  "tokens_lifetime": 4200000
}
```

Daemon persists on every change to `tokens_today`. `tokens_lifetime` is bonus telemetry, not on the wire.

## 15. Testing

### 15.1 Unit (pytest)

- `test_state.py` — `msg` derivation table coverage, hint truncation, entry rotation, midnight rollover, stale session reaper.
- `test_install.py` — round-trip settings.json edits with markers; survives concurrent hand-edits; uninstall preserves non-marker entries.
- `test_transcript.py` — JSONL parsing with partial last line, missing usage field, file truncation between reads.
- `test_hook_protocol.py` — PreToolUse round-trip with mock socket peer (allow/deny/timeout/no-daemon paths). Asserts stdout JSON shape exactly.

### 15.2 Integration (in-process fake BLE peer)

`test_ble_fake.py` provides `BleFake` implementing the NUS protocol in-process (no real bleak). Daemon talks to it. Asserts:

- Heartbeat shape and cadence
- Permission round-trip routing across two concurrent prompts
- Folder push transcript order (no chunk pipelining)
- 4KB turn-event cap drops oversize events without erroring

### 15.3 Manual e2e runbook

Documented in `tools/bridge/README.md`:

1. `python3 tools/bridge/install.py` (one time, from the repo root)
2. `/buddy-run` — verify "BLE connected to Clawd"
3. Open a CC session; run a Bash command. Verify device shows `approve: Bash` and the command hint.
4. Approve on device. Verify CC proceeds.
5. Run `ls /` (auto-approved tools): verify device shows `Bash: ls /` then `ran: Bash`.
6. Drop `characters/bufo` via `/buddy-gifpush characters/bufo`. Verify device renders the new pet.
7. `/buddy-mute` then run another command — verify device shows nothing for this session. `/buddy-unmute`, retry — events resume.
8. `/buddy-stop`. Verify clean shutdown.

## 16. Risks and open questions

1. **bleak macOS pairing.** `bleak` invokes CoreBluetooth which handles the OS pairing prompt automatically. The firmware advertises encrypted-only NUS characteristics — first read on TX should trigger pairing. **Risk:** if `bleak`'s default `connect()` doesn't request encryption upfront, the first GATT read triggers pairing mid-flight which can manifest as a transient disconnect. Mitigation: explicitly set `pair=True` (via `bleak.BleakClient.pair()`) before subscribing, and tolerate one reconnect on first connect.
2. **Transcript JSONL schema drift.** The token harvest depends on the assistant message shape (`type: "assistant"`, `message.usage.output_tokens`). CC has changed this shape historically. Mitigation: defensive parsing — try multiple known shapes, log unknowns at debug level, never crash on unexpected lines.
3. **Hook timeout interaction with PreToolUse latency.** A 60-second `timeout` setting plus a 5-second daemon wait is comfortable, but if the daemon's socket accept hangs longer than 5s the hook will still time out at 60s, which CC may surface as a tool-call failure. Mitigation: hook uses `socket.settimeout(0.5)` on connect, `5.0` on read, never waits longer than 6 seconds total.
4. **Notification `permission_prompt` overlap with PreToolUse.** Both fire when CC prompts for permission. The bridge owns PreToolUse (canonical); Notification is informational only. Don't double-emit prompts. Mitigation: ignore `Notification` of type `permission_prompt`; only act on the other notification types.
5. **State persistence races.** Daemon writes `state.json` on every snapshot. SIGKILL during write can corrupt. Mitigation: write to `state.json.tmp`, fsync, rename — atomic on POSIX.
6. **Skill discovery before install.** Project-local `.claude/skills/buddy-*` are discovered whenever CC is opened in the firmware repo, but they fail noisily until the venv exists. **The user runs `python3 tools/bridge/install.py` from the repo root once after cloning** (not via a skill — install is one-shot per machine). After that, the skills are also symlinked into `~/.claude/skills/` and work from any directory.

7. **`$CLAUDE_SESSION_ID` availability in skill subprocesses.** §12.3 prefers reading the session id from the environment for /buddy-mute. Whether CC exports `CLAUDE_SESSION_ID` (or some equivalent) to slash-command bash subprocesses needs verification during implementation. Fallback path (daemon picks the cwd-matching most-recent session) is implemented either way.

## 17. Out of scope (future ideas, not this spec)

- Filter sessions by project root so only certain repos drive the device.
- Multi-device support (route different sessions to different devices).
- Linux/Windows ports.
- Web dashboard / TUI for the daemon.
- Daemon auto-restart on crash via launchd.
- Custom `msg` format strings configurable via `~/.claude-buddy/config.json`.

## 18. Glossary

- **Hardware Buddy** — the BLE protocol family defined in `REFERENCE.md`, originally for the desktop apps.
- **NUS** — Nordic UART Service, the de-facto serial-over-BLE standard. UUID `6e400001-…`.
- **Daemon** — the long-running `claude_buddy.daemon` process. One per host.
- **Hook script** — `buddy_hook.py`, the thin bridge between CC's hook subprocess invocation and the daemon socket.
- **Skill** — a Claude Code skill (SKILL.md + bundled scripts/refs). The user-facing surface.
- **Tool use id** — CC-assigned unique id per tool call. Used as the protocol-level prompt id end-to-end.
