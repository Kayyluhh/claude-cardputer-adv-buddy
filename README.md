# claude-desktop-buddy

Claude for macOS and Windows can connect Claude Cowork and Claude Code to
maker devices over BLE, so developers and makers can build hardware that
displays permission prompts, recent messages, and other interactions. We've
been impressed by the creativity of the maker community around Claude -
providing a lightweight, opt-in API is our way of making it easier to build
fun little hardware devices that integrate with Claude.

> **Building your own device?** You don't need any of the code here. See
> **[REFERENCE.md](REFERENCE.md)** for the wire protocol: Nordic UART
> Service UUIDs, JSON schemas, and the folder push transport.

As an example, we built a desk pet on ESP32 that lives off permission
approvals and interaction with Claude. It sleeps when nothing's happening,
wakes when sessions start, gets visibly impatient when an approval prompt is
waiting, and lets you approve or deny right from the device.

<p align="center">
  <img src="docs/device.jpg" alt="M5Stack Cardputer-Adv running the buddy firmware" width="500">
</p>

## Hardware

This fork targets the **M5Stack Cardputer-Adv** (Stamp-S3A core,
ESP32-S3FN8, 8 MB flash, 240×135 landscape ST7789V2, 56-key keyboard via
TCA8418 I²C expander, BMI270 IMU, ES8311 audio codec + 1 W speaker, RGB
LED, 1750 mAh battery). All hardware-specific knowledge lives in the
M5Cardputer 1.1.1 library; everything above the Platform shim
(`src/platform.{h,cpp}`) and Input layer (`src/input.{h,cpp}`) is
hardware-agnostic.

If you have the original Cardputer or an M5StickC Plus, this branch
won't run on it as-is — see the upstream repo
(`anthropics/claude-desktop-buddy`) for the StickC Plus build.

## Flashing

Install
[PlatformIO Core](https://docs.platformio.org/en/latest/core/installation/),
then:

```bash
pio run -e m5stack-cardputer-adv -t upload
```

If the board doesn't enter download mode automatically, set the side
power switch to OFF, hold the **G0** button on the side, plug in USB
while still holding G0, then release after about a second. Re-run
`pio run -t upload`.

If you're starting from a previously-flashed device, wipe it first:

```bash
pio run -e m5stack-cardputer-adv -t erase && pio run -e m5stack-cardputer-adv -t upload
```

Once running, you can also wipe everything from the device itself:
**hold Enter → settings → reset → factory reset → press Enter twice**.

## Pairing

To pair your device with Claude, first enable developer mode (**Help →
Troubleshooting → Enable Developer Mode**). Then, open the Hardware Buddy
window in **Developer → Open Hardware Buddy…**, click **Connect**, and pick
your device from the list. macOS will prompt for Bluetooth permission on
first connect; grant it.

<p align="center">
  <img src="docs/menu.png" alt="Developer → Open Hardware Buddy… menu item" width="420">
  <img src="docs/hardware-buddy-window.png" alt="Hardware Buddy window with Connect button and folder drop target" width="420">
</p>

Once paired, the bridge auto-reconnects whenever both sides are awake.

If discovery isn't finding the stick:

- Make sure it's awake (any button press)
- Check the stick's settings menu → bluetooth is on

## Controls

The Cardputer-Adv has a 56-key keyboard. The mapping is keyboard-first
(Enter / Esc / Tab) with letter shortcuts where they help.

|                  | Normal                 | Pet         | Info         | Menu / settings | Approval    |
| ---------------- | ---------------------- | ----------- | ------------ | --------------- | ----------- |
| **Enter**        | cycle screen           | next page   | next page    | confirm         | **approve** |
| **Esc / Del**    | —                      | back        | back         | back            | **deny**    |
| **Tab** / **;**  | —                      | next page   | next page    | next item       | —           |
| **Hold Enter**   | open menu              | open menu   | open menu    | close menu      | (n/a)       |
| **`** (backtick) | cycle home → pet → info | "           | "            | —               | —           |
| **a / d**        | —                      | —           | —            | —               | approve / deny |
| **, / .**        | scroll transcript ←/→  | —           | —            | —               | —           |
| **Shake**        | dizzy                  |             |              |                 | —           |
| **Face-down**    | nap (energy refills)   |             |              |                 |             |

Side **power switch** is hardware-only — no programmable power-off on
the Adv. The screen auto-powers-off after 30s of no interaction (stays
on while an approval prompt is up or the device is on USB power). Any
keystroke wakes it.

## ASCII pets

**Twenty** pets, each with seven animations (sleep, idle, busy,
attention, celebrate, dizzy, heart). **Settings → ascii pet** cycles
them with a counter; choice persists to NVS. The default 18 are upstream;
`doge` and `llama` are cherry-picked from
[y88huang/claude-desktop-buddy-cardputer](https://github.com/y88huang/claude-desktop-buddy-cardputer).

## GIF pets

If you want a custom GIF character instead of an ASCII buddy, drag a
character pack folder onto the drop target in the Hardware Buddy window. The
app streams it over BLE and the stick switches to GIF mode live. **Settings
→ delete char** reverts to ASCII mode.

A character pack is a folder with `manifest.json` and 96px-wide GIFs:

```json
{
  "name": "bufo",
  "colors": {
    "body": "#6B8E23",
    "bg": "#000000",
    "text": "#FFFFFF",
    "textDim": "#808080",
    "ink": "#000000"
  },
  "states": {
    "sleep": "sleep.gif",
    "idle": ["idle_0.gif", "idle_1.gif", "idle_2.gif"],
    "busy": "busy.gif",
    "attention": "attention.gif",
    "celebrate": "celebrate.gif",
    "dizzy": "dizzy.gif",
    "heart": "heart.gif"
  }
}
```

State values can be a single filename or an array. Arrays rotate: each
loop-end advances to the next GIF, useful for an idle activity carousel so
the home screen doesn't loop one clip forever.

GIFs are up to 120 px wide and ~120 px tall — the pet renders inside the
left buddy column of the 240×135 landscape canvas, vertically centered.
Crop tight to the character; transparent margins waste pixels.
`tools/prep_character.py` handles the resize: feed it source GIFs at any
size and it produces a 96 px-wide set where the character is the same
scale in every state. (96 px works fine; the column gives ~12 px of
breathing room on either side.)

The whole folder must fit under 1.8MB —
`gifsicle --lossy=80 -O3 --colors 64` typically cuts 40–60%.

See `characters/bufo/` for a working example.

If you're iterating on a character and would rather skip the BLE round-trip,
`tools/flash_character.py characters/bufo` stages it into `data/` and runs
`pio run -t uploadfs` directly over USB.

## The seven states

| State       | Trigger                     | Feel                        |
| ----------- | --------------------------- | --------------------------- |
| `sleep`     | bridge not connected        | eyes closed, slow breathing |
| `idle`      | connected, nothing urgent   | blinking, looking around    |
| `busy`      | sessions actively running   | sweating, working           |
| `attention` | approval pending            | alert, **LED blinks**       |
| `celebrate` | level up (every 50K tokens) | confetti, bouncing          |
| `dizzy`     | you shook the stick         | spiral eyes, wobbling       |
| `heart`     | approved in under 5s        | floating hearts             |

## Project layout

```
src/
  main_adv.cpp     — loop, state machine, UI screens
  platform.{h,cpp} — thin shim over M5Unified (display, power, IMU, RTC,
                     audio, LED) so the rest of the code never touches
                     hardware APIs directly
  input.{h,cpp}    — edge-triggered keyboard wrapper around
                     M5Cardputer.Keyboard with long-press tracking
  gfx_compat.h     — TFT_eSprite -> M5Canvas typedef alias so the buddy
                     renderer keeps the legacy type name
  buddy.cpp        — ASCII species dispatch + render helpers
  buddies/         — one file per species (20), seven anim functions each
  ble_bridge.cpp   — Nordic UART service, line-buffered TX/RX
  character.cpp    — GIF decode + render (LittleFS + AnimatedGIF)
  data.h           — wire protocol, JSON parse
  xfer.h           — folder push receiver
  stats.h          — NVS-backed stats, settings, owner, species choice
characters/        — example GIF character packs
tools/             — generators and converters
partitions_8mb.csv — 3 MB app + 4.8 MB LittleFS, no OTA
```

## Availability

The BLE API is only available when the desktop apps are in developer mode
(**Help → Troubleshooting → Enable Developer Mode**). It's intended for
makers and developers and isn't an officially supported product feature.
