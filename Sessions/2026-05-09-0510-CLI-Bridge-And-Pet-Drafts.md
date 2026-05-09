# CLI Bridge Implementation + ASCII Pet Drafts

**Date:** 2026-05-09 05:10 PDT
**Working dir:** `/Users/kayla/VSCode/PlatformIO/claude-cardputer-adv-buddy/` (branch: `main`)

## 1. Summary

Built a Python bridge under `tools/bridge/` that lets Claude Code CLI sessions
drive the existing Cardputer-Adv firmware via the same Hardware Buddy BLE
protocol the desktop apps use. Spec → plan → 22-task TDD execution → 111
passing tests, then a README pass to surface the bridge in the project's main
docs. Final detour: drafted idle/busy ASCII frames for 15 new pet ideas
(saved to `ideas.md`) and stopped before speccing them. Phase 9 (manual
hardware verification) is the only outstanding item from the bridge plan and
is gated on Kayla having the Cardputer in hand.

## 2. Work Done

### Bridge — design, plan, code (28 commits)

Spec at `docs/superpowers/specs/2026-05-07-claude-code-cli-buddy-bridge-design.md`
(commits `c3ab732`, `a65043e`). Implementation plan at
`docs/superpowers/plans/2026-05-07-claude-code-cli-buddy-bridge.md` (commit
`32032b5`). Then 22 plan tasks across 8 phases:

| Commit | Task |
|---|---|
| `55a9dcf` | T1: scaffolding (pyproject.toml, package skeleton) |
| `d67b588` | T2: `wire.py` (NUS UUIDs, JSON line framing) |
| `b5ac961` | T3: `persistence.py` (atomic JSON, type-coerced loaders) |
| `534e41a` | T4: `state.py` part 1 (dataclasses, msg derivation) |
| `d0b834c` | T5: `state.py` part 2 (entries, midnight rollover, reaper) |
| `73c3f80` | T6: `transcript.py` (JSONL token harvest, truncation) |
| `ac00508` | T7: `hook_server.py` (Unix-socket ND-JSON dispatch) |
| `01fdcf2` | T8: `hook.py` (CC hook entry, timeout/fallback) |
| `ccd0fb8` | docs: align spec text with T8 implementation |
| `5152c0f` | T9: `BleFake` test fixture |
| `fa6792e` | T10: `ble_client.py` (bleak NUS wrapper) |
| `247a4c1` | T11: `daemon.py` asyncio main wiring |
| `078a7e1` | T12: `pair.py` |
| `1fdbe3a` | T13: `run.py` daemon launcher |
| `2d45279` | T14: `stop.py` |
| `a21873c` | T15: `status.py` |
| `11b34fe` | T16: folder push end-to-end (daemon `_handle_push` + `push.py`) |
| `f1d2358` | T17: `mute.py` |
| `cf5d1cc` | T18: `install.py` standalone bootstrap |
| `303ea57` | T19: `uninstall.py` |
| `f10b28d` | fix: `repo_root()` parents[2] not parents[1] (was returning `tools/`) |
| `19fe914` | T20: SKILL.md for buddy-run/stop/status |
| `718a307` | T21: SKILL.md for buddy-gifpush/mute/unmute |
| `be79507` | T22: bridge README + end-to-end integration test |
| `c310c05` | post-review fixes: push timeout error, immediate heartbeat, chunk size 117 (was 120) |

### Project README pass

| Commit | Files |
|---|---|
| `af9b4a6` | `README.md` (added "Claude Code CLI bridge" section, restored menu+window image embed, updated intro and project layout); `docs/device.jpg` deleted (showed an M5StickC Plus, not the Cardputer-Adv this fork targets — confirmed by inspection of the JPEG) |

### Idea draft (post-bridge)

| File | Content |
|---|---|
| `ideas.md` (new) | 15 new ASCII pet sketches with idle+busy frames in 12×5 format. Includes claude (self-portrait), sloth, terminal, real duck, rubberduck, trex, alien, cthulhu, slime, whale, jellyfish, crab, frog, bmo, meeseeks. Open questions on IP-sensitivity for bmo + meeseeks. **Not committed.** |

### Test status

All 111 tests pass (`pytest -q` from `tools/bridge/` with venv activated).
No tests on real hardware yet.

## 3. Conclusions

Verified facts from this session:

- Claude Code's PreToolUse hook contract: stdin payload includes `session_id`,
  `transcript_path`, `cwd`, `permission_mode`, `hook_event_name`, `tool_name`,
  `tool_input`, `tool_use_id`. Stdout JSON shape is
  `{"hookSpecificOutput":{"hookEventName":"PreToolUse","permissionDecision":"allow"|"deny"|"ask","permissionDecisionReason":"…"}}`.
  `[verified]` — `https://code.claude.com/docs/en/hooks.md` fetched 2026-05-07
  by `claude-code-guide` agent (see spec §6).
- Stop hook payload does NOT include token usage; tokens must be harvested
  from `transcript_path` JSONL. `[verified]` — same source, spec §6 risk #2,
  defensive parsing implemented in `tools/bridge/src/claude_buddy/transcript.py`.
- macOS BLE write-without-response payload limit ≈ MTU − 3 ≈ 182 bytes when
  CoreBluetooth negotiates the typical 185-byte MTU. The firmware's TX path
  caps at `mtu - 3` (see `src/ble_bridge.cpp:164-168`) and explicitly
  comments "macOS negotiates 185 typically" at `src/ble_bridge.cpp:91-92`.
  `[verified]` — direct read of firmware source.
- Chunk size 120 raw bytes → 160-char base64 → 183-byte JSON envelope (with
  `\n`) exceeds 182. Reduced to 117 raw bytes → 156-char base64 → 179-byte
  envelope, safely under. `[verified]` — math + commit `c310c05`.
- `docs/device.jpg` (now deleted) showed an M5StickC Plus form factor (vertical,
  single side button, no keyboard) — wrong hardware for this fork. The other
  two images (`menu.png`, `hardware-buddy-window.png`) are platform-agnostic
  desktop-app screenshots and are kept. `[verified]` — visual inspection of
  the JPEG in this session.
- The 20 names in `src/buddies/` are ASCII pets (compiled into firmware), not
  GIF character packs. The only GIF character pack in `characters/` is
  `bufo`. `[verified]` — `ls characters/` and `find . -iname '*llama*'`.
- `daemon.py` `_heartbeat_loop` and `_reaper_loop` use
  `asyncio.wait_for(shutdown_event.wait(), timeout=N)` so loops exit
  promptly on shutdown rather than waiting up to 10s/30s for the next
  iteration. `[verified]` — fixture teardown was timing out before this fix
  in commit `247a4c1`.

## 4. Unanswered Questions

- **Real-hardware behavior is unverified.** The bridge has 111 passing tests
  including end-to-end via in-process `BleFake`, but has never been exercised
  against a real Cardputer. Specifically unknown: macOS pairing prompt
  behavior, MTU negotiation under `bleak`'s default `BleakClient.connect()`,
  how the firmware reacts to the 117-byte chunk size in practice, whether
  the 5-second permission timeout is comfortable on real BLE hardware.
- **IP/trademark on `bmo` and `meeseeks` pet drafts.** Cartoon Network
  (Adventure Time) and Adult Swim (Rick and Morty). Upstream firmware ships
  `doge` and `llama` cherry-picked from a community fork, so there's some
  precedent, but BMO and Mr. Meeseeks are more directly trademarked. Open
  question logged in `ideas.md` §"Open questions".
- **`$CLAUDE_SESSION_ID` env var availability** in slash-command bash
  subprocesses. The `mute.py` helper prefers it, falls back to a
  daemon-side cwd-matching lookup. The fallback works (tested), but whether
  the env path triggers in real CC sessions is unverified — depends on what
  CC actually exports to `Bash` tool subprocesses.
- **Whether the firmware will accept `{cmd:"owner","name":"…"}` packets**
  with the bridge as the sender. `_send_one_shots()` calls it on connect,
  but `config.owner_name` is empty by default after `install.py` (no UI to
  set it). Manual edit to `~/.claude-buddy/config.json` is the workaround;
  needs hardware test to confirm.

## 5. Next Steps

In priority order:

1. **Phase 9 — manual hardware verification** (bridge plan §Phase 9).
   Run the runbook in `tools/bridge/README.md` against a real
   Cardputer-Adv. Specifically watch for: BT permission dialog on first
   `/buddy-run`, MTU-related silent failures (heartbeat snapshots ~300+
   bytes will fail to send if firmware doesn't request MTU expansion via
   `BLEDevice::setMTU(517)` per `src/ble_bridge.cpp:91-92`),
   `daemon.log` errors during `/buddy-gifpush`. The final-review subagent's
   commentary in commit `c310c05` flagged these specifically.

2. **Decide on the 15 ASCII pet drafts in `ideas.md`.** Three implementation
   strategies are documented there (one-at-a-time, batched subagents, or
   spec-only). Recommendation in the doc: `claude` (self-portrait) gets
   careful one-at-a-time treatment; the rest can batch.

3. **Resolve IP/trademark question** for `bmo` and `meeseeks` — keep them,
   rename to generic ("game console", "blue helper"), or drop. See
   `ideas.md` §"Open questions".

4. **Optional follow-ups flagged by final review subagent** (commit
   `c310c05` already applied items #1, #2, and chunk-size; remaining):
   - Delete or implement the dead `prehook_timeout` op handler
     (`daemon.py` line ~121). Currently the daemon registers it but
     `hook.py` never sends it (the daemon's own `wait_for` self-resolves).
   - Update the chunk size comment to be more explicit about the MTU
     dependency (already partially addressed in `c310c05`).

## 6. Required Sources

| Source | Why |
|--------|-----|
| `tools/bridge/README.md` | Manual end-to-end runbook for Phase 9 hardware verification. |
| `docs/superpowers/specs/2026-05-07-claude-code-cli-buddy-bridge-design.md` | Authoritative design; understand before changing bridge architecture. |
| `docs/superpowers/plans/2026-05-07-claude-code-cli-buddy-bridge.md` | 22-task implementation plan with full code blocks; needed for context on any of the 22 tasks. |
| `src/ble_bridge.cpp` (lines 91-92, 164-168) | Firmware MTU negotiation and TX chunking — directly relevant to whether the bridge's 117-byte chunk size is safe and to debugging silent BLE write failures. |
| `REFERENCE.md` | Wire protocol authoritative reference. The bridge implements this; any protocol-level issue should be diagnosed against it. |
| `ideas.md` | If continuing the ASCII pet work, this is the source of truth for what's been drafted and what remains. |
| `src/buddies/cat.cpp` | Reference structure for any new ASCII pet — sequence arrays, color hex codes, state cadence pattern. |

## 7. Do NOT

- Do not edit `src/buddies/cat.cpp` or other existing pet files unless the
  user explicitly asks. Their behavior is dialed in.
- Do not modify the hook_server's `respond` callback to re-add an
  idempotency gate. It was deliberately removed in commit `cd26ad5` (now
  rolled into `11b34fe`) because the folder push protocol relies on
  multiple sequential `respond()` calls per connection. Sequential calls
  within one handler are not racy.
- Do not assume `repo_root()` in `install.py` uses `parents[1]` — that was
  the original plan-spec bug, fixed to `parents[2]` in commit `f10b28d`.
- Do not delete `~/.claude-buddy/` casually — it preserves token history,
  mute state, and the BLE bond. The bridge README's uninstall section
  documents this explicitly.
- Do not write a session handoff manually. Use `/handoff` (this file was
  created via that skill).
- Do not assume the test suite covers real hardware. `BleFake` is an
  in-process stub. Silent BLE write failures (e.g. MTU exceeded) will not
  show up in pytest.
- Do not commit `ideas.md` without checking with Kayla on the IP/trademark
  question for `bmo` and `meeseeks` — the file is intentionally left
  uncommitted for now.

## 8. Self-Audit

- **Did I ignore or skip anything the user said?** No. Every direction was
  followed — including the explicit "leave README.md alone" instruction
  during all 22 bridge tasks (subagents were told this verbatim every
  time), and the corresponding "now update README" instruction at the end
  was honored with verification that the existing dirty changes were
  unrelated and that the pre-existing image was wrong-hardware.

- **Did I fail to acknowledge a correction or interruption?** Yes —
  twice. (1) When Kayla said the auto-paste of `source .venv/bin/activate`
  wasn't intentional, I had already paused and asked clarifying questions,
  which was correct, but I could have detected the auto-paste pattern
  earlier. (2) When the model classifier was unavailable for several
  minutes, I asked a clarifying question that Kayla pushed back on with
  "I can't sit here and babysit notifications" — I should have continued
  retrying silently rather than escalating.

- **Did any passive rule fail to prevent a mistake?** Two:
  1. The plan I wrote had a bug in `repo_root()` (`parents[1]` should
     have been `parents[2]`). The reviewer caught it. The structural fix
     is to add a path-derivation regression test to the plan template
     (the test added in commit `f10b28d` is the right shape).
  2. The plan's Task 8 had an internal contradiction (test asserts
     silent-on-disable, code emits "ask" on disable). The implementer
     surfaced it and we resolved by silencing — correct outcome, but the
     plan-self-review step in the writing-plans skill should have caught
     it before dispatch.

## 9. Advice for Next Agent

- **MTU is the most likely day-one failure on hardware.** If
  `/buddy-status` shows BLE connected but the device displays nothing,
  check `~/.claude-buddy/daemon.log` for `heartbeat send failed` lines.
  The fix is in firmware: `BLEDevice::setMTU(517)` must execute before
  advertising. If the firmware on Kayla's Cardputer is older than
  `src/ble_bridge.cpp:91-92`'s setMTU call, reflash before debugging the
  bridge.

- **The bridge has zero hardware test coverage.** All 111 tests use
  `BleFake` in-process. Treat any divergence between expected and actual
  hardware behavior as new information, not a regression in the bridge.
  Specifically: bleak's pairing semantics on macOS, `_send_one_shots()`
  packet ordering, and the firmware's response to streamed JSON heartbeats
  larger than 182 bytes are all unverified.

- **The ASCII pet work is creative-by-default.** Kayla picked 15 specific
  pets including a self-portrait of "claude" — the wispy thought-cloud
  design is intentional, not a placeholder. If iterating on the
  silhouettes, treat the existing draft in `ideas.md` as Kayla's approved
  design intent, not a starting suggestion.

- **When dispatching subagents for code, use sonnet for orchestration
  tasks (daemon, push) and haiku for mechanical TDD tasks.** Haiku
  consistently produced correct code for the Phase 1-7 tasks but missed
  subtle issues in the daemon (the heartbeat loop using bare
  `asyncio.sleep` instead of the shutdown-aware variant). Sonnet caught
  multiple plan bugs immediately on Task 11 and Task 16.

- **The reviewer subagents are catching real bugs.** Don't skip the
  spec-then-quality two-stage review even on tasks that look mechanical.
  Examples this session: Task 6 missed the file-truncation test required
  by spec §15.1; Task 7 had a subtle race in `respond` idempotency; Task 8
  dropped the daemon-supplied permission reason; Task 16 had the wrong
  chunk size for macOS MTU. All caught by reviewers, not the implementer.

- **Kayla has memory:** "I can't sit here and babysit notifications, that's
  why I put it on auto." When auto-mode is implied, retry classifier
  outages silently rather than asking the user to choose between waiting
  or proceeding. The user wants forward progress.

- **The firmware repo's main `README.md` had pre-existing unrelated dirty
  changes when this session started.** Do NOT silently commit them. They
  were — correctly — image-embed removals from a previous session that I
  partially reverted (kept the menu+window image, kept the device.jpg
  removal because the underlying file was wrong-hardware). If you find
  similar dirty state in a future session, ask before staging.
