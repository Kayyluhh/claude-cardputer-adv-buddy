# 2026-05-19 01:00 PDT — StickS3 Bringup and Bridge Permission Re-architecture

## Summary

Brought the M5Stack StickS3 port (branch `feat/sticks3-port`) from "compiles on Kayla's Mac" to "running on real hardware, paired over BLE, receiving and responding to permission prompts." Found and bandaided a number of bring-up issues. The session ended with Kayla pointing out — correctly — that the CLI bridge's blocking-hook permission flow is the wrong architecture; it contradicts the non-blocking design in `REFERENCE.md`. The bandaids worked but the underlying mistake (block-on-hook-subprocess as the permission gate) remains in place and is what the next agent should fix from scratch.

## Work Done

Branch: `feat/sticks3-port`. Ten commits added this session:

| Commit  | Subject                                                                         |
|---------|---------------------------------------------------------------------------------|
| `08c6fdc` | docs: add Cardputer-adv and StickS3 datasheets                                  |
| `093fd36` | feat(sticks3): stage 1 — StickS3 build scaffold (compiles, no UI yet)          |
| `5581b38` | feat(sticks3): stage 2 — full main_stick.cpp port + docs                       |
| `1fddf23` | debug(sticks3): trace markers + fallback_board hint for first-boot diagnosis    |
| `fc57b0c` | feat(sticks3): rotation axis fix, landscape home, install.py shim fix          |
| `8e8b43b` | fix: HUD area mismatch + bridge auto-reconnect after reflash                    |
| `2a54354` | fix(sticks3): chronological transcript order + busy threshold                   |
| `02f86ba` | fix(bridge): more transcript entries + longer permission timeout                |
| `2c403b6` | fix(bridge): hook script timeout was the real gate, not daemon timeout         |
| `3c87990` | fix(install): health-check existing venv, recreate if broken                    |

### Firmware (`StickS3/src/`)
- Full port of upstream M5StickC Plus `src/main.cpp` to `main_stick.cpp` with StickS3 hardware adjustments.
- `cfg.output_power = false` + `M5.Power.setExtOutput(false)` after `M5.begin()` to disable the EXT_5V boost rail.
- `cfg.fallback_board = ...` was added as a debug hint, then proven unnecessary and removed by Kayla.
- Removed `M5.Speaker.setVolume(180)` from `Platform::init` (was leaving AW8737 idle-enabled). Volume is set later via `applyVolume()`.
- IMU rotation axis swap: StickS3 has X+ as the LONG axis (toward USB-C), so "sideways" = Y dominant, not X. Fixed `clockUpdateOrient()` accordingly.
- `drawHomeLandscape()` added for true landscape home screen (buddy on left, transcript on right), direct-to-LCD render in rotated coords.
- `drawHUD` AREA bumped 28 → 78 to match `drawApproval` and prevent stale approval pixels above fresh transcript.
- Transcript order reversed: bridge sends newest-first, firmware now iterates in reverse so newest renders at the bottom (matching standard chat layout).
- `derive()` BUSY threshold dropped from `sessionsRunning >= 3` to `>= 1` so solo-session users see the busy animation.
- Settings menu "ascii pet" now shows species name right-aligned instead of `N/22`.
- Info "TO PAIR" page now lists `/buddy-run` first, with the desktop app as the secondary option.

### Shared code (`src/`, build-flag-guarded with `#ifdef STICKS3_BUILD`)
- `buddy.cpp` — `BUDDY_X_CENTER=67, BUDDY_CANVAS_W=135` under `STICKS3_BUILD`; `_tgt` type widened from `M5Canvas*` to `LovyanGFX*`; new `buddyRenderTo()` for direct-to-LCD landscape pet.
- `buddy.h` — pulled in `gfx_compat.h` so `LovyanGFX` resolves from species files.
- `gfx_compat.h` — `<M5Unified.h>` instead of `<M5Cardputer.h>` under `STICKS3_BUILD`.
- `stats.h` — `bool sound` replaced with `uint8_t volumeLevel` (0..4). NVS key `s_snd` → `s_vol`.
- `main_adv.cpp` — four trivial compat substitutions so the Cardputer-Adv build still compiles against the new field.

### Bridge (`tools/bridge/`)
- `install.py write_hook_shim`: shim now `exec`s `~/.claude-buddy/venv/bin/python` directly, not the `~/.claude-buddy/python` symlink. Symlink loses venv context on macOS 3.12; `sys.prefix` resolves to the system framework and `claude_buddy` isn't on the path.
- `install.py setup_venv`: now health-checks existing venv (launches its python, asserts `sys.prefix != sys.base_prefix`, asserts pip importable) and `shutil.rmtree`s + recreates if any check fails. Prevents the "stale venv silently keeps a broken install" failure mode that bit us during bring-up.
- `ble_client.py`: `send()` now catches `BleakError`, calls a new `_reconnect()` (stop_notify + disconnect + connect), retries the write once. Recovers from "Service Discovery has not been performed yet" after firmware reflashes without manual macOS forget-device.
- `daemon.py _heartbeat_loop`: attempts reconnect if `_ble.is_connected` is False before each heartbeat. On send failure, marks `state.ble_connected = False` so the next iteration tries again.
- `state.py`: `entries` deque maxlen 8 → 32. `wire_entries()` returns 12 (was 4).
- `persistence.py`: default `permission_timeout_ms` 5000 → 30000.
- `hook.py`: `READ_TIMEOUT` 5.0 → 35.0. This was the actual gate, not the daemon's timeout — the hook subprocess gave up reading from the unix socket at 5s and returned "ask" to CC before the daemon's 30s window had a chance to fire.

### Docs (`StickS3/README.md`)
- Button labels corrected (A = front under-screen, B = right side; left side button is PMIC-only).
- Controls table copied from upstream.
- Power button caveat documented.

### Hardware verified working
- Build: `pio run -d StickS3 -e m5stack-sticks3 -t upload` — clean upload via USB-C
- BLE pairing: `Claude-5831` at MAC `3C3414C5-EF22-9F69-46F3-BD96B9C12C74`
- Buttons: A=GPIO 11 (under screen), B=GPIO 12 (right side)
- IMU rotation: correct in all four orientations after the ax↔ay swap
- Transcript display: 12 lines, newest at bottom, scrolls into history with B
- Pet busy state: now fires on `sessionsRunning >= 1`
- Auto-reconnect: confirmed log shows `BLE reconnected to ...` after reflash, no macOS forget-device needed
- Hook chain end-to-end: PreToolUse → `buddy_hook.py` → daemon → BLE → device approval overlay → button press → bridge resolves CC's permission

## Conclusions

`[verified]` claims:

- StickS3 IMU (BMI270, datasheet page 7) has X+ pointing along the long axis toward USB-C; Y+ is the short axis. This is opposite of the M5StickC Plus convention upstream's `clockUpdateOrient` was written for. — `StickS3_Info.pdf` page 7 + Kayla's empirical confirmation across all four orientations
- M5Unified's StickS3 board auto-detection works without `cfg.fallback_board`. Earlier suspicion was wrong. — Kayla removed the fallback line and the device booted fine.
- `M5.Power.setExtOutput(false)` must be called AFTER `M5.begin()` returns to actually gate the EXT_5V_EN rail off. Passing `cfg.output_power = false` to `M5.config()` alone isn't sufficient — the M5PM1 PMIC i²c writes during M5.begin can run before the PMIC case statement initializes. — `M5Unified.cpp` lines 1830-1842 (StickS3 board init) + `Power_Class.cpp` setExtOutput body
- Audible "buzz" on the device persists even after both output_power gates and at volume=0. Treated as the documented "Abnormal Device Noise" — `StickS3_Info.pdf` page 3.
- macOS Python 3.12 follows the `~/.claude-buddy/python` symlink and uses the realpath to detect `sys.prefix`. Result: `sys.prefix` resolves to `/Library/Frameworks/Python.framework/Versions/3.12` instead of the venv. The hook subprocess then can't import `claude_buddy`. Verified by running `~/.claude-buddy/python -c "import sys; print(sys.prefix)"` vs the same via the venv's direct path during diagnosis. Fix: hook shim and any other caller must invoke `~/.claude-buddy/venv/bin/python` directly.
- The `wire_entries()` bridge function was capping transcript at 4 entries — the firmware-side display fix to 9 lines was undone by the bridge bottleneck. Now 12.
- `hook.py READ_TIMEOUT = 5.0` was the actual gate on the device-vs-terminal race, not the daemon's `permission_timeout_ms`. Both needed bumping. Now 35s and 30s respectively, with the hook timeout exceeding the daemon timeout by 5s margin.

`[unverified]` claims that remain (the next agent should not act on these without re-verifying):

- The bridge's blocking-hook permission flow can be rewritten to be non-blocking AND still let the device's button press resolve CC's permission, with reasonable effort. The session ended at the design-decision point; no implementation path was actually verified to work.

## Unanswered Questions

1. **How does the Claude desktop app actually integrate with CC's permission system?** The desktop app evidently shows its native UI prompt in parallel with sending the prompt to the device — both UIs co-live, either resolves. The CLI bridge's hook model can't do this with public APIs alone. We don't know whether the desktop uses an internal Anthropic API, an MCP server, tty injection, or some other mechanism. **This is the central question for the next session.**

2. **Is there a public CC mechanism for "out-of-band permission decisions"?** Searches in this session were superficial. The hook system as we currently use it requires the hook to return a decision before CC proceeds. We need to know: can a hook return "ask" and then later send an update via some other channel? Is there an MCP-based permission tool pattern? Does CC support stdin/Unix-socket-based permission updates from outside the hook?

3. **Why does the bridge daemon's signal handler not clean up `/tmp/claude-buddy.sock`?** `/buddy-stop` reports `stopped` cleanly but the socket file remains. Doesn't break anything (next start recreates it) but worth filing.

4. **"Done" for assistant turns** — bridge has `transcript_path` from every hook payload but doesn't harvest assistant message text. Currently CC's `Stop` event maps to the literal string `"done"`, so assistant turns all read "done" in the transcript HUD. Bridge needs a JSONL parser for CC's transcript format to extract the actual assistant content snippet.

5. **Permission state-clearing semantics on the bridge.** Currently the bridge clears `tama.promptId` when the prehook handler returns (timeout or device response). If we go non-blocking, when SHOULD the bridge clear `tama.promptId`? Probably: on `PostToolUse` (tool ran, permission was decided some way), or on a separate "permission resolved" signal. Needs design.

6. **`cfg.fallback_board` was added as a debug hint and turned out to be unnecessary.** Why did the device originally hang at the splash, then? The trace markers were added in the same commit; one of those other changes (Serial.begin at start of setup, post-begin setExtOutput, splash delay reduction) actually fixed it. We don't know which.

## Next Steps

**Stop bandaiding the bridge. Re-architect the permission flow per REFERENCE.md.**

Concrete tasks for the next agent, in order:

1. **Read `REFERENCE.md` end-to-end before touching any bridge code.** The protocol is non-blocking. The desktop forwards device responses to "the session manager" asynchronously, not via a hook return value.

2. **Read `tools/bridge/src/claude_buddy/daemon.py` lines 130-165** (the prehook handler with `asyncio.wait_for(future, ...)`). This is where the blocking happens. The future-await-with-timeout pattern is the shortcut to revert.

3. **Read `tools/bridge/src/claude_buddy/hook.py`** to understand the current subprocess entry. The hook script reads payload from stdin, sends it to the daemon, blocks on socket recv. To go non-blocking the hook should return "ask" immediately and NOT wait for a daemon reply.

4. **Investigate how Claude Code can accept a permission decision OUTSIDE the PreToolUse hook return value.** Possibilities to check:
   - MCP server with a permission-tool pattern that CC could be configured to consult
   - Any `permissionDecision` or `PermissionUpdate` event in CC's docs that fires after the initial hook return
   - tty input injection via TIOCSTI (brittle; document the limitation)
   - Whether CC has any "interactive" hook mode where the hook can stay open and stream events
   - Look at how the Anthropic desktop app's source does this; the upstream `claude-desktop-buddy` repo may have hints even though it doesn't include a CLI bridge

5. **Design the new permission flow on paper before coding it.** Two questions to answer:
   - Where does the bridge get authorization to resolve CC's pending prompt when the device button is pressed?
   - When does the bridge clear `tama.promptId` after the prompt is resolved (by either UI)?

6. **Implement the non-blocking flow.** Likely shape: PreToolUse hook returns "ask" immediately. Bridge maintains the pending-prompt state and broadcasts via heartbeat. Device receives it within the heartbeat tick (≤1s). Terminal also shows its prompt. Either resolution path notifies the bridge; bridge clears state; next heartbeat reflects cleared prompt.

7. **If the device-resolves-CC pathway can't be implemented in this session, ship "mirror only" as a clearly-documented interim.** Both UIs visible, terminal owns the resolution, device button only dismisses the local overlay. State this limitation prominently in `StickS3/README.md`. Don't pretend it does more than it does.

8. **Once the bridge re-arch is done, harvest assistant message text from `transcript_path` on `Stop` events** to replace the literal "done" string. Will need a JSONL parser for CC's transcript file format. Likely lives in `tools/bridge/src/claude_buddy/transcript.py`.

9. **File the socket-cleanup bug separately.** Not urgent; daemon recreates the socket on next start.

## Required Sources

| Source | Why |
|--------|-----|
| `claude-cardputer-adv-buddy/REFERENCE.md` | The wire protocol spec. **Read end-to-end before touching bridge code.** The protocol is non-blocking; the current bridge implementation deviates from it. |
| `claude-cardputer-adv-buddy/tools/bridge/src/claude_buddy/daemon.py` | Contains the prehook handler with the blocking `asyncio.wait_for(future, ...)` at lines 130-165 — the architectural mistake to undo. Also has `_send_heartbeat` and `_build_snapshot` which need to learn to broadcast `prompt` without waiting. |
| `claude-cardputer-adv-buddy/tools/bridge/src/claude_buddy/hook.py` | The CC hook subprocess entry. `READ_TIMEOUT` (currently 35s) wouldn't matter if the daemon stopped blocking. |
| `claude-cardputer-adv-buddy/tools/bridge/src/claude_buddy/state.py` | Where pending prompts live (`GlobalState.pending_by_id`, `Session.pending_prompt`). Read `wire_entries` and the prompt-resolution paths. |
| `claude-cardputer-adv-buddy/StickS3/src/main_stick.cpp` | The device's prompt-handling code, particularly the loop's prompt-arrival detection (`strcmp(tama.promptId, lastPromptId) != 0`) and the A/B button handlers' `inPrompt` branches. |
| `claude-cardputer-adv-buddy/StickS3/README.md` | Current user-facing controls table and button mapping. Update if the new permission flow changes UX. |
| `claude-cardputer-adv-buddy/StickS3_Info.pdf` | StickS3 hardware datasheet. Pinouts, IMU axes (page 7), abnormal-noise note (page 3). |
| `claude-cardputer-adv-buddy/Sessions/2026-05-10-2049-SD-Card-Migration-And-Launcher-Compat.md` | Previous handoff. Useful context on partition table and launcher compatibility. |
| Conversation thread (this session) | The empirical observations and back-and-forth that led to the realization the architecture was wrong. The "I was confirming the prompt DOES disappear at 30s, not that the fix worked" moment is key context. |

## Do NOT

- **Don't add more timeouts as bandaids.** Adding/tuning timeouts in `daemon.py`, `hook.py`, or anywhere else does not fix the underlying mistake. If you find yourself doing this, stop and re-read REFERENCE.md.
- **Don't trust upstream `claude-desktop-buddy` for the CLI bridge design.** Upstream doesn't have a CLI bridge — `tools/bridge/` was added in this fork. The bridge's design choices are fork decisions, not upstream gospel. REFERENCE.md is the spec; the bridge is just one (currently-flawed) implementation of it.
- **Don't change `ble_bridge.cpp` from Bluedroid to NimBLE** unless explicitly authorized. We deliberately pinned `espressif32@6.7.0` to keep Bluedroid and reuse the ble_bridge verbatim. Bumping the platform means rewriting ~150 lines of BLE code and re-testing pairing on both build targets.
- **Don't touch the Cardputer-Adv build behavior.** All shared-tree changes are `#ifdef STICKS3_BUILD`-guarded. Kayla does not have Cardputer-Adv hardware available to re-test if you break it. Verify with `pio run -e m5stack-cardputer-adv` before any commit that touches `src/`.
- **Don't add `cfg.fallback_board = m5::board_t::board_M5StickS3` back.** Kayla proved by removal that auto-detection works fine without it. The line was a debug-session hypothesis that turned out wrong.
- **Don't reintroduce `M5.Speaker.setVolume(180)` in `Platform::init`.** It leaves the AW8737 idle-enabled with no audio signal. Volume is set later via `applyVolume()` from `setup()` once stats are loaded.
- **Don't assume new tool calls during testing will trigger the hooks immediately.** The hook script is reinvoked on every tool call, but the daemon process holds state from its startup — config and code changes only apply after a daemon restart (`/buddy-stop` + `/buddy-run`).
- **Don't write new code for the assistant-message-content "Done" issue before the permission flow is fixed.** Sequencing matters: get the architecture right first.

## Self-Audit

> Did I ignore or skip anything the user said?

Yes — twice. First, when Kayla said the buzz was still happening, I focused on `cfg.output_power` and the EXT_5V rail, when the datasheet's note on page 3 ("Abnormal Device Noise" — early batches produce slight noise normally) was the actual answer all along. Kayla called this out directly: "you didn't read the section on whistling in the StickS3 document." I had read it but hadn't accepted what it said. Second, I misread Kayla's "confirmed the prompt will not appear in terminal until after it times out on the device. I had to wait the full 30 seconds for it to appear in terminal" as confirmation that the fix worked, when she was actually confirming that the prompt STILL races in (just at 30s instead of 5s) and the device prompt STILL gets cancelled.

> Did I fail to acknowledge a correction or interruption?

Three significant ones. Kayla pointed out the rotation was 90° off and I'd misnamed the buttons ("M5 logo button" — there's no such thing on the StickS3); I corrected the docs. She pointed out the fallback_board line was unnecessary; I'd kept it as a debug aid past its usefulness. She pointed out the bridge architecture was wrong and the CLI bridge code introduced the blocking-hook shortcut; I had been treating this as a fundamental CC constraint when REFERENCE.md proves otherwise.

> Did any passive rule fail to prevent a mistake — and if so, what structural change would have caught it?

Yes. The `/handoff` workflow and the verification-against-primary-sources rule in CLAUDE.md both failed to fire when they should have. Specifically: when Kayla pointed at REFERENCE.md, the right move was to read it fully and rethink the architecture before writing more code. Instead I had already shipped two commits' worth of timeout bandaids. Structural fix: when the user references a doc file in the repo, treat that reference as a hard signal to STOP coding and READ before any more edits. Add a check in the agent's workflow: "If the user has referenced a doc, has it been read end-to-end since the reference?"

## Advice for Next Agent

- **Kayla is correct more often than you'll initially assume.** Of the corrections she made this session — rotation axis, button labels, fallback_board unnecessary, "confirmed" misread, bridge architecture wrong, "this is bandaid" — every single one was right. When she pushes back, stop and audit the underlying assumption, don't argue.

- **REFERENCE.md is the contract.** The CLI bridge is an implementation of that contract, not the spec itself. If a behavior in the bridge doesn't match REFERENCE.md, the bridge is wrong, not the protocol. Use REFERENCE.md to check any design decision you're considering.

- **Real hardware will surprise you.** Several issues this session weren't visible in `pio run` (no compile errors) but only showed up after flashing. Examples: the cfg.output_power workaround order, the IMU axis difference, the macOS Python symlink behavior, the daemon's blocking-hook vs REFERENCE.md disagreement. Plan for flash-test cycles, don't assume "compiles clean = works on device."

- **The 5s `READ_TIMEOUT` in `hook.py` and the 30s `permission_timeout_ms` in the daemon are both bandaids.** Once you go non-blocking, neither matters. Don't tune them further; obsolete them.

- **The bridge daemon caches a lot at startup.** Config is loaded once. BLE address is loaded once. After ANY code or config change, the daemon must be restarted to pick it up. The hook subprocess does re-load on every fire, so hook-side changes are immediate; daemon-side changes are not.

- **`/buddy-stop` + `/buddy-run` is the universal "I changed bridge code, apply it" sequence.** Memorize it. If a bridge behavior didn't change after you expected it to, suspect a stale daemon first.

- **`pio` on system PATH is broken on Kayla's machine** (it points at an unrelated `pio` Python package, not PlatformIO). Use `~/.platformio/penv/bin/pio` directly. There's an alias in `~/.zshrc` but it doesn't apply to existing shell sessions.

- **The hook shim at `~/.claude/hooks/buddy_hook.py` calls `~/.claude-buddy/venv/bin/python` directly, NOT via the `~/.claude-buddy/python` symlink.** That symlink loses venv context on macOS. The installer is now correct (commit `2c403b6`) but if you ever regenerate the shim by hand, mirror the direct-venv-python pattern.

- **Test against `Claude-5831` at MAC `3C3414C5-EF22-9F69-46F3-BD96B9C12C74`.** Kayla's device is paired and ready. If the pair is dropped on macOS (after factory reset, or after some BLE quirks), she'll need to "forget device" in System Settings → Bluetooth before the next pair. The bridge's auto-reconnect handles normal post-reflash reconnects automatically.

- **Don't promise device-can-resolve-terminal-prompts until you've verified the integration path exists.** It might be straightforward via an MCP-based permission tool, or it might require tty injection (which is brittle). Find out before designing the UX around it.
