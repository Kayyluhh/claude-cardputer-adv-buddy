// StickS3 main loop — STAGE-1 SCAFFOLD STUB.
//
// This file is a temporary placeholder so the build links cleanly while
// we verify the hardware-shim layer (platform.cpp, input.cpp) compiles.
// The full port from upstream `anthropics/claude-desktop-buddy@main:
// src/main.cpp` (2-button UX, portrait layout, IMU clock rotation, BLE
// protocol round-trips, settings/menu/info screens, etc.) lands in
// stage 2 per /Users/kayla/.claude/plans/i-would-like-to-fuzzy-pudding.md.
//
// For now this stub:
//   * Defines `TFT_eSprite spr` (the canvas the shared buddy renderer
//     paints into via `extern TFT_eSprite spr`).
//   * Calls Platform::init() (runs M5.begin(cfg) with output_power=false).
//   * Calls Input::init() / Input::poll() so the 2-button wrapper is
//     exercised at link time.
//   * Calls buddyInit() and renders a single sleep-state frame to prove
//     the buddy renderer compiles against the portrait-canvas geometry.
//   * Does NOT touch BLE — bleInit() is left out so the radio stays off
//     during stage-1 bring-up. The link still pulls in ble_bridge.cpp
//     via build_src_filter, which verifies Bluedroid still resolves
//     under espressif32 6.7.0 + M5Unified 0.2.15.

#include "gfx_compat.h"      // pulls in M5Unified.h via STICKS3_BUILD guard
#include "platform.h"
#include "input.h"
#include "buddy.h"

// Portrait canvas dimensions: ST7789P3 native 135×240.
static const int W = 135;
static const int H = 240;

// The shared buddy renderer (../src/buddy.cpp) and the future
// main_stick.cpp draw operations both target this single full-screen
// sprite. Created once in setup(), pushed once per frame in loop().
TFT_eSprite spr(&M5.Display);

void setup() {
  Platform::init();   // M5.begin(cfg) with output_power=false; sets brightness, IMU, audio
  Input::init();

  spr.setColorDepth(16);
  spr.createSprite(W, H);

  buddyInit();

  // One-shot proof-of-life paint so the device shows something other
  // than a blank backlight after first flash.
  spr.fillSprite(0x0000);
  buddyTick(0);       // PersonaState 0 = sleep
  spr.pushSprite(0, 0);
}

void loop() {
  Input::poll();      // refreshes M5.BtnA / M5.BtnB via M5.update()
  Platform::tickAudio();
  delay(20);
}
