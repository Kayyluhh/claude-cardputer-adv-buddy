# 2026-05-10 20:49 PDT — SD Card Migration and Launcher Compatibility

## Summary

Migrated character pack storage from the on-flash LittleFS partition to the
Cardputer-Adv's microSD slot, so the firmware works identically under both a
full-flash USB install and an app-only install via bmorcelli's M5Launcher
v2.6.8. Two commits now on `main`; firmware builds clean at 1.23 MB.
Hardware test is the unfinished step.

## Work Done

### Investigation phase (no code changes)

- Identified that the user's `M5Launcher[v2.6.8].bin` is **bmorcelli's
  M5Stick-Launcher**, not M5Stack's launcher. Confirmed via in-binary strings
  (`-= Launcher WebUI =-`, default creds `wui_usr:admin / wui_pwd:launcher`,
  config flags `askSpiffs`, `bootToApp`).
- Catalogued the 16 firmwares in `/Users/kayla/VSCode/PlatformIO/Firmwares/`
  and `/Users/kayla/VSCode/PlatformIO/Firmwares/M5Launcher-Apps/`. Renamed
  all 15 hashed files in M5Launcher-Apps to readable names (12 matched parent
  named firmwares by content MD5; 4 were unknowns identified via string
  analysis: TV-B-Mine v1.0, RaisingHell, Cardputer-Doom, MiniAcid).
- Found bmorcelli's launcher accepts **both full-flash and app-only**
  formats. Of 16 files in M5Launcher-Apps, 13 are full-flash and 3 are
  app-only (poseidon-launcher, TV-B-Mine, Cardputer-Doom).
- Determined bmorcelli's launcher partition table from binary dump:
  `nvs 24K @ 0x9000 / app0 1408K @ 0x10000 (test) / app1 5056K @ 0x170000 (ota_0)`
  `/ vfs 512K @ 0x670000 / spiffs 1024K @ 0x6f0000 / coredump 64K @ 0x7f0000`.

### Code changes (committed on `main`)

**Commit `b73690f` — feat: move character storage from LittleFS partition to microSD**

| File | Change |
|------|--------|
| `src/platform.h` | Added `initSdCard()` and `sdAvailable()` declarations |
| `src/platform.cpp` | New SPI + SD init (CS=12, MOSI=14, CLK=40, MISO=39, 25 MHz). Called from `Platform::init()` |
| `src/character.cpp` | `LittleFS.*` → `SD.*` throughout; mount check via `Platform::sdAvailable()` |
| `src/character.h` | Doc update: "Mounts LittleFS" → "Platform::init() mounts SD" |
| `src/xfer.h` | Folder-push writes to SD; 64-bit free-space math (`%llu`); `char_begin` hard-fails with `"no SD card"` when absent; removed stale `#include <M5StickCPlus.h>`; migrated three `M5.Axp.*` calls to `M5.Power.*` + Platform shim |
| `src/main_adv.cpp` | Factory reset and "delete char" walk `/characters/` on SD instead of `LittleFS.format()` — preserves launcher app cache and other user files on the card |
| `partitions_8mb.csv` | Dropped the unused 4.8 MB `littlefs` partition |
| `platformio.ini` | Dropped `board_build.filesystem = littlefs`; updated comment block |
| `src/buddies/goose.cpp` | Renamed `SHIFT[5]` → `SHIFT_F[5]` (collides with `#define SHIFT 0x80` in `M5Cardputer/utility/Keyboard/Keyboard_def.h`) |

Pre-existing build fixes also rolled in: `main_adv.cpp` dropped local
`GREEN`/`RED` consts (collide with `m5gfx::ili9341_colors::GREEN/RED` via
`using namespace` at file scope in `M5GFX.h:289`) and renamed `RX`/`RW`
to `PANEL_X`/`PANEL_W` (collide with `static const uint8_t RX = 44` in
ESP32-S3 `pins_arduino.h:24`).

**Commit `06efc6d` — docs: align user-facing docs and tools with SD-backed character storage**

| File | Change |
|------|--------|
| `README.md` | Rewrote GIF-pets section (SD copy flow, no 1.8 MB cap, drop `flash_character.py` mention); updated project layout block |
| `characters/bufo/README.md` | Install instructions point to SD copy |
| `platformio.ini` | Refreshed `character.cpp` comment (no more LittleFS reference) |
| `tools/flash_character.py` | **Deleted** (only purpose was `data/` → `uploadfs` staging, both now retired) |
| `tools/prep_character.py` | Docstring + end-of-run hint point to SD copy; 1.8 MB warning explicitly scoped to BLE folder-push only |

**Non-repo doc** (workspace-root, outside git):
- `/Users/kayla/VSCode/PlatformIO/CLAUDE.md` was rewritten this session to
  describe SD storage. Not tracked in any repo.

### Git operations

- Created branch `sd-card-storage` (silent — should have flagged)
- Committed and pushed both commits
- Fast-forward merged `sd-card-storage` → `main`, pushed main
- Deleted `origin/claude/port-cardputer-adv-Mjw6J` (obsolete leftover, fully
  merged at `1cdf981`)
- Deleted `sd-card-storage` locally and on origin after merge

### Build verification

```
pio run -e m5stack-cardputer-adv
RAM:   25.1% (82204 / 327680 bytes)
Flash: 40.9% (1285153 / 3145728 bytes)
firmware.bin: 1,285,520 bytes
Image header: e9 05 02 3f (5 segments, 8 MB / 80 MHz / DIO)
```

## Conclusions

| Claim | Citation |
|-------|----------|
| bmorcelli's launcher v2.6.8 has app0 at 0x10000 (subtype `test`, 1408 KB) and app1 at 0x170000 (subtype `ota_0`, 5056 KB) | `/Users/kayla/VSCode/PlatformIO/Firmwares/M5Launcher[v2.6.8].bin` offset 0x8000, parsed via Python struct unpack of partition table magic `aa 50` |
| The launcher has both SPIFFS and LittleFS libraries linked and a config flag `askSpiffs:1` | `M5Launcher[v2.6.8].bin` strings dump — `"-= Launcher WebUI =-"`, `"LITTLEFS"`, `"SPIFFS Yes"`, `"Spiffs/LittleFs partition"`, config blob with `"askSpiffs":1,"wui_usr":"admin","wui_pwd":"launcher"` |
| Cardputer-Adv microSD pinout: CS=12, MOSI=14, CLK=40, MISO=39 | M5Stack docs at https://docs.m5stack.com/en/core/Cardputer-Adv — quoted verbatim: "Stamp-S3A G12 G14 G40 G39 microSD CS MOSI CLK MISO" |
| M5Cardputer SD example uses the same pinout and `SPI.begin + SD.begin(CS, SPI, 25000000)` pattern | https://raw.githubusercontent.com/m5stack/M5Cardputer/master/examples/Basic/sdcard/sdcard.ino fetched this session |
| bufo character pack is 596 KB on disk, fits on 1 MB launcher spiffs partition AND on SD | `du -sh /Users/kayla/VSCode/PlatformIO/claude-cardputer-adv-buddy/characters/bufo` |
| Arduino-ESP32 ships SD over SPI as a built-in library — no lib_deps entry needed | `~/.platformio/packages/framework-arduinoespressif32/libraries/SD/src/SD.h` |
| Build succeeds with all changes applied | `pio run -e m5stack-cardputer-adv` returned `[SUCCESS] Took 19.24 seconds` (after partition CSV change verified separately) |
| M5GFX 0.2.20 pulls `m5gfx::ili9341_colors::*` into global scope via `using namespace` at file scope | `~/.platformio/.../M5GFX/src/M5GFX.h:289` — `using namespace m5gfx::ili9341_colors;` |
| ESP32-S3 pins_arduino.h defines `RX` as `static const uint8_t RX = 44` | `~/.platformio/packages/framework-arduinoespressif32/variants/esp32s3/pins_arduino.h:24` (cited in compiler error output) |
| All four unknown firmwares in M5Launcher-Apps are correctly identified | Strings found in each: `"TV-B-Mine"`/`"v1.0 by Ray J"` (663 KB bin), `"RaisingHellCardputer/1.0"` (4 MB bin), `"cardputer_doom"`/`"DOOM 2: TNT - Evilution"` (5.38 MB bin), `"MiniAcid"`/`"miniacid_scene"`/`"Select drum pattern 1-8"` (8 MB bin) |
| Cardputer-Doom and TV-B-Mine in M5Launcher-Apps are **app-only** format (no partition table at offset 0x8000) | Format classifier output: `@0x8000` byte values `696e` (Cardputer-Doom) and `fb80` (TV-B-Mine), neither is `aa50` magic |

## Unanswered Questions

- **Hardware runtime is not verified.** Build clean ≠ works on device.
  Specifically untested: (1) SD card mounting on a real Cardputer-Adv,
  (2) GIF rendering from `/characters/bufo/` on SD, (3) BLE folder-push
  writing to SD, (4) install via bmorcelli's launcher WebUI.
- **What format does bmorcelli's launcher actually use** for its 1 MB
  `spiffs` partition by default — SPIFFS or LittleFS? The launcher has both
  libraries linked and `askSpiffs:1` suggests user choice. Not directly
  relevant anymore (we use SD now), but it was an open theoretical concern.
- **Does `prep_character.py`'s output manifest format exactly match what
  `character.cpp` expects?** Both reference `manifest.json` with the same
  schema, but I didn't trace every field round-trip. Should work, never
  verified end-to-end.
- **Are there other library-macro collisions waiting** to bite when someone
  adds a new constant or variable in the firmware? `SHIFT`, `RX`, `GREEN`,
  `RED` all collided. M5Cardputer's Keyboard_def.h likely defines other
  short uppercase names (CTRL, ALT, etc.) that could clash with future
  variable names.
- **What's in the workspace `.venv/` at `/Users/kayla/VSCode/PlatformIO/.venv/`?**
  Mentioned in passing as the bridge's dev venv but never inspected this
  session.
- **Cardputer-Adv-User-Demo** was identified as one of the named firmwares
  but its actual purpose was not investigated. It's just an M5Stack reference
  app build. Probably uninteresting.

## Next Steps

1. **Hardware verification of the SD migration.** Format an SD card FAT32,
   copy `characters/bufo/` to root so the device path is `/characters/bufo/`.
   Flash either via USB (`pio run -e m5stack-cardputer-adv -t upload`) or
   via bmorcelli's launcher WebUI (upload `firmware.bin`). Boot and watch
   serial for `[sd] mounted, <N> MB total`. Verify bufo renders. If it
   fails, the most likely failure modes:
   - SD card not detected: check pin order, try a different card
   - Mount fails: check FAT32 vs exFAT formatting
   - No characters scanned: confirm `/characters/bufo/manifest.json` exists
     at the expected path
   - GIF doesn't render: serial will print `[char] open failed: ...` —
     follow the path from there
2. **Decide what to do with `docs/superpowers/specs/` and
   `docs/superpowers/plans/`.** Per user during this session: "treat them as
   historical record, don't rewrite." That's the current state. If the user
   changes their mind, the specs reference LittleFS-based design.
3. **Confirm `tools/test_serial.py` and `tools/test_xfer.py` still work**
   under the new SD-backed xfer.h. These weren't touched this session and
   the BLE folder-push protocol is unchanged at the wire level (still
   `char_begin`/`file`/`chunk`/`file_end`/`char_end`), but I haven't
   verified they still pass.
4. **Run the bridge's pytest suite** to confirm bridge ↔ firmware contract
   wasn't affected: `cd claude-cardputer-adv-buddy/tools/bridge && pytest`.
   Wire format didn't change, so tests should pass — verify it.
5. **Open question — replace `flash_character.py`?** It's deleted. Users
   who previously relied on USB-flashing character packs now have no fast
   path other than "eject SD, copy, reinsert." If iteration speed matters,
   a `dev_push_to_sd.py` that mounts the SD card via USB mass storage and
   copies the pack could be useful — but only if the Cardputer-Adv exposes
   SD as USB MSC, which it may not. Investigate before building.

## Required Sources

| Source | Why |
|--------|-----|
| `/Users/kayla/VSCode/PlatformIO/CLAUDE.md` | Workspace-level CLAUDE.md updated this session; describes the SD storage model and the build/flash workflow. Read first. |
| `/Users/kayla/VSCode/PlatformIO/claude-cardputer-adv-buddy/README.md` | User-facing docs updated this session; reflects SD-based character flow. |
| `/Users/kayla/VSCode/PlatformIO/claude-cardputer-adv-buddy/REFERENCE.md` | BLE wire protocol — unchanged this session but defines the contract `xfer.h` implements |
| `/Users/kayla/VSCode/PlatformIO/claude-cardputer-adv-buddy/src/platform.cpp` | New `initSdCard()` implementation; understand it before modifying SD behavior |
| `/Users/kayla/VSCode/PlatformIO/claude-cardputer-adv-buddy/src/character.cpp` | Reads packs from SD; understand the boot scan + manifest parse flow |
| `/Users/kayla/VSCode/PlatformIO/claude-cardputer-adv-buddy/src/xfer.h` | BLE folder-push writes to SD; understand the file/chunk/file_end protocol implementation |
| `/Users/kayla/VSCode/PlatformIO/claude-cardputer-adv-buddy/partitions_8mb.csv` | Current partition layout (no filesystem) |
| https://github.com/bmorcelli/Launcher/wiki/Explaining-the-project | Launcher behavior — accepts both full-flash and app-only |
| https://github.com/bmorcelli/Launcher/wiki/Functionalities-explained | Launcher WebUI documentation |
| https://docs.m5stack.com/en/core/Cardputer-Adv | SD pin assignments (CS=12, MOSI=14, CLK=40, MISO=39) verbatim |

## Do NOT

- **Do not dismiss something with "looks like" without checking.** This
  session I called the `claude/port-cardputer-adv-Mjw6J` branch "a leftover
  Claude-generated branch from a previous session" without running
  `git log` against it. The user caught it. A 5-second check would have
  given the real answer.
- **Do not silently create git branches.** This session I switched off
  `main` to `sd-card-storage` to commit, per the project-wide
  "branch first" rule — but I didn't tell the user. They expected we
  were working on main. Flag any branch creation in chat at the moment
  you do it.
- **Do not treat pre-existing build errors as "not your problem."** When
  asked to verify a change with a build, fix whatever is needed to make
  that build run. The user explicitly clarified this mid-session.
- **Do not call cleanup of dead code "harmless" or "risky" to defer it.**
  Unreferenced partitions, deleted-tool references in docs, and dead
  `board_build.filesystem` configs are debt, not zero cost.
- **Do not propose options without research first.** The user pushed back
  on this pattern explicitly. If the answer requires reading library code,
  fetching docs, or examining binaries — do that first, then propose with
  evidence.
- **Do not add new top-level constants or variables in `main_adv.cpp`
  with short uppercase names** (single letters, common abbreviations like
  RX/TX/CS/MOSI, color names like RED/GREEN/BLUE, modifier keys like
  SHIFT/CTRL/ALT) without grepping the M5Cardputer / arduino-esp32
  headers first for macro collisions. Multiple such collisions tripped
  the build this session.
- **Do not rewrite `docs/superpowers/specs/` or `Sessions/`** without an
  explicit "yes, update those too" — they're historical record per user
  decision this session.

## Self-Audit

- **Did I ignore or skip anything the user said?** No outright ignoring,
  but several initial responses were proposals rather than research, which
  the user explicitly redirected (twice). Once redirected, I did the work.
- **Did I fail to acknowledge a correction or interruption?** Each time
  the user pushed back (M5 Launcher = the file they have not generic
  M5Stack launcher; pre-existing bugs are mine to fix; "harmless" cleanup
  framing was wrong; "looks like" was unverified), I acknowledged the
  correction and acted on it. But I shouldn't have triggered the
  corrections in the first place.
- **Did any passive rule fail to prevent a mistake — and if so, what
  structural change would have caught it?**
  - The "If on the default branch, branch first" rule from CLAUDE.md
    fired correctly but silently. **Structural fix**: when this rule
    fires, the agent must emit a one-line announcement in the response
    ("Creating branch `<name>` because we're on `main`") before
    proceeding. I'll propose this addition.
  - The "Verify claims of completion against actual file state" rule
    did not prevent the "looks like" hand-wave about the other branch.
    Strengthening the rule isn't the fix; the fix is treating
    qualifiers like "looks like," "seems to be," "probably," etc., as
    flags that I should run the verification command instead of writing
    the qualifier.

## Advice for Next Agent

- **The user values evidence over speed.** Quoted from her global
  CLAUDE.md: "Kayla values accuracy and honesty in responses over speed
  and placating." This session bore that out repeatedly. If you're about
  to write "probably" or "looks like" or "it should be," stop and run
  the check that would let you write the verified version instead.
- **The git operations were silent and surprising.** When you create,
  switch, delete, or push a branch, announce it in chat at the moment
  you do it. The user thought we were on main the whole time because I
  branched without telling her. Even when following a project rule
  ("branch first when on main"), surface the action.
- **Pre-existing bugs found during verification work are yours to fix.**
  The user was explicit: "Pre existing bugs are still your
  responsibility. The final deliverable is a working firmware." Apply
  the same principle to whatever you're verifying next.
- **Library macro pollution is a real recurring issue in this
  codebase.** This session found 4 distinct collisions (SHIFT, GREEN,
  RED, RX) between project identifiers and library macros. If you add
  any new top-level constant or buddy species variable, grep
  `~/.platformio/.../M5Cardputer/`, `~/.platformio/.../M5GFX/`, and
  `~/.platformio/.../framework-arduinoespressif32/variants/esp32s3/`
  for collisions first. The buddies pattern uses lots of short
  ALL_CAPS names for animation frames — high risk.
- **The hardware test is the gap.** Don't claim the SD migration "works"
  until you've watched serial logs from a real device. Build clean is
  necessary but not sufficient.
- **The user has the Firmwares folder organized for SD-card loading.**
  The renamed files in `/Users/kayla/VSCode/PlatformIO/Firmwares/M5Launcher-Apps/`
  are the launcher's SD-card app cache. If she copies firmware.bin to
  that folder, she's installing it as a launcher-loadable app. That's
  the deployment target for the SD migration work.
- **Don't propose Option A/B/C if you can just go research and
  recommend the answer.** This session the "three options" pattern
  was a deferral mechanism. The user explicitly called it out and asked
  for the research to happen first. When you face a fork like
  "SD-only vs SD+LittleFS-fallback," do the analysis (look at code,
  measure sizes, read docs) before laying out choices — sometimes the
  research collapses the fork.
- **The workspace CLAUDE.md at `/Users/kayla/VSCode/PlatformIO/CLAUDE.md`
  is outside the firmware git repo.** Updates there don't get committed.
  If you change it, mention that it's an out-of-band change.
