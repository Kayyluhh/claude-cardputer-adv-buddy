# Hardware Buddy — M5Stack StickS3 build

A portrait, 2-button variant of the Hardware Buddy firmware in this
repo's root. Same wire protocol (Nordic UART, see [`../REFERENCE.md`](../REFERENCE.md)),
same Python bridge under [`../tools/bridge/`](../tools/bridge/),
different hardware shim under [`src/`](src/).

## Hardware

M5Stack **StickS3** ([SKU K150](https://docs.m5stack.com/en/products/sku/k150)).

- SoC: ESP32-S3-PICO-1-N8R8 (Xtensa LX7 dual-core, 8 MB flash, 8 MB Octal PSRAM)
- Display: ST7789P3, 135×240 native portrait, IPS LCD
- IMU: BMI270 (6-axis, accel + gyro)
- Audio: ES8311 codec + AW8737 1 W speaker amplifier
- Buttons: 2 firmware-readable (KEY1=G11, KEY2=G12)
- Side button: PMIC-managed (single-press=on, double-press=off, long-press=download/boot mode) — **not firmware-readable**
- Battery: 250 mAh LiPo
- No microSD slot, no IR control used by this firmware

## Build & flash

PlatformIO Core required ([installation docs](https://docs.platformio.org/en/latest/core/installation/)):

```bash
# build only
pio run -d StickS3 -e m5stack-sticks3

# build + flash over USB-C
pio run -d StickS3 -e m5stack-sticks3 -t upload

# full chip erase before re-flashing a previously-flashed unit
pio run -d StickS3 -e m5stack-sticks3 -t erase
```

If the board doesn't auto-enter download mode, **hold the side button while
plugging USB**, release after ~1 second; the internal LED flashes green to
confirm download mode. Then re-run upload.

## Controls

The StickS3 has two firmware-readable buttons:
- **Button A** — the button on the front face, below the screen (GPIO 11)
- **Button B** — the small button on the right side of the device (GPIO 12)

The left-side button is the power button. It's wired to the PMIC and is
not exposed to firmware; single-press wakes the device, double-press
hard-offs, long-hold enters download/boot mode.

|              | Normal               | Pet         | Info        | Menu / Settings / Reset | Approval    |
| ------------ | -------------------- | ----------- | ----------- | ----------------------- | ----------- |
| **A** short  | cycle screen mode    | next page   | next page   | advance row             | **approve** |
| **B** short  | scroll transcript    | next page   | next page   | confirm / apply         | **deny**    |
| **A** long (≥600 ms) | open menu     | open menu   | open menu   | close / back out        | (n/a)       |
| **B** long (≥600 ms) | —             | —           | —           | mute (only on volume row) | —         |
| Shake        | dizzy animation      | —           | —           | —                       | —           |
| Face-down    | nap (energy refills) | —           | —           | —                       | —           |

**Side power button** (left side, PMIC-managed):
- Single-press: power on / wake
- Double-press: hard power off
- Long-hold: enter download/boot mode

The 30 s idle-screen-off timer is the only software screen-off; press any
firmware button to wake.

## Settings

Hold A on the home screen to open the menu, then choose **settings**:

| Row | Behavior | Display |
| --- | --- | --- |
| `brightness` | cycles 0..4 (20..100 % PWM) | `0/4` .. `4/4` |
| `volume`     | **B-tap** cycles 0..4, wraps to 0. **B-long** = quick mute. | `mute` or `1/4` .. `4/4` |
| `bluetooth`  | toggle (stored only — radio stays live) | `on` / `off` |
| `wifi`       | placeholder, no WiFi stack | `on` / `off` |
| `led`        | no LED on this board — stored only | `on` / `off` |
| `transcript` | hide/show the HUD transcript | `on` / `off` |
| `clock rot`  | charging-clock orientation lock | `auto`/`port`/`land` |
| `ascii pet`  | cycle through 22 species | `n/22` |
| `reset`      | enter reset submenu | — |
| `back`       | exit settings | — |

Volume is capped at 180/255 (~71 %) per the StickS3 datasheet's <75 %
brown-out warning on battery — peak speaker draw can otherwise reset the
device on a low cell.

## Pairing

Pair via the Claude desktop app (Developer → Open Hardware Buddy) or the
Python CLI bridge in [`../tools/bridge/`](../tools/bridge/) by running
`/buddy-run` in a Claude Code session. The Nordic UART contract is
identical to the Cardputer-Adv build — same `Claude-XXXX` advertisement
naming, same JSON snapshots.

Six-digit pairing passkey appears on screen during initial bond; type it
into the desktop prompt to complete pairing.

## What's different from the Cardputer-Adv build

| Feature | Cardputer-Adv | StickS3 |
| --- | --- | --- |
| Display | 240×135 landscape | 135×240 portrait |
| Input | 56-key QWERTY keyboard | 2 buttons (A + B) |
| GIF pets | yes, microSD-backed | **no** (no SD slot) |
| ASCII pets | 22 species | 22 species (same) |
| Sound | on/off toggle | 5-step volume |
| RGB LED | yes | **no LED** |
| Charging clock | n/a | IMU rotation (portrait ↔ landscape) |
| Power button | hardware slide switch | PMIC button (firmware unreadable) |
| Battery | 1750 mAh | 250 mAh |

The wire protocol (`REFERENCE.md`) and the Python bridge daemon are
unchanged. A single `/buddy-run` session can pair with either device.

## Power-button caveat

The StickS3's side button is wired to the M5PM1 PMIC, not to a GPIO the
ESP32 can read. The PMIC handles three behaviors directly:

- **Single-press** → power on / wake the SoC
- **Double-press** → hard power off
- **Long-hold** → enter download/boot mode (held while connecting USB)

There is no firmware-controlled screen-off via this button; the firmware
also has no "turn off" menu entry. To shut the device down cleanly,
**double-press the side button**.

## Build internals

The StickS3 build pins:

- `platform = espressif32@6.7.0` — arduino-esp32 v2.0.x + **Bluedroid BLE**, matching the Cardputer-Adv build so [`../src/ble_bridge.cpp`](../src/ble_bridge.cpp) is reused verbatim. M5Unified 0.2.15+ supports both arduino-esp32 v2.0.x and v3.x; we pin to the older platform to keep BLE code unified across both builds.
- `m5stack/M5Unified @ ^0.2.15` — supports the StickS3 board internally (sets up KEY1/KEY2 pins, drives the M5PM1 PMIC, ES8311 audio, BMI270 IMU). No separate `M5PM1` library required.

The `cfg.output_power = false` workaround in [`src/platform.cpp`](src/platform.cpp) disables the EXT_5V boost converter rail (Grove / Hat / IR power), which otherwise emits an audible whine and drains the battery — see datasheet page 6. This firmware doesn't use any of those rails.

Source layout:

```
StickS3/
├── platformio.ini       — m5stack-sticks3 env, build_src_filter pointing at ../../src/ shared code
├── partitions_8mb.csv   — 3 MB single app slot, no OTA, no filesystem
├── README.md            — this file
└── src/
    ├── main_stick.cpp   — loop, state machine, UI screens, button handling
    ├── platform.{h,cpp} — M5Unified shim (display, power, IMU, audio, RTC); SD stubs
    └── input.{h,cpp}    — 2-button wrapper around M5.BtnA/M5.BtnB
```

Shared with the Cardputer-Adv build (referenced from `../src/`):

```
src/
├── ble_bridge.{cpp,h}   — Nordic UART (Bluedroid)
├── buddy.{cpp,h}        — multi-species ASCII renderer
├── buddies/*.cpp        — 22 species
├── buddy_common.h       — layout constants (portrait variants via STICKS3_BUILD)
├── data.h               — JSON parse + TamaState
├── xfer.h               — folder-push receiver (rejects on this build — no SD)
├── stats.h              — NVS-backed stats + settings
└── gfx_compat.h         — TFT_eSprite alias; selects M5Unified.h on STICKS3_BUILD
```
