#include "input.h"
#include <M5Unified.h>

namespace Input {

// M5Unified handles pinMode(G11/G12, INPUT) automatically when it
// recognizes board_M5StickS3 (see M5Unified.cpp:1830-1832). We only need
// to call M5.update() once per poll to refresh BtnA/BtnB state.
//
// Edge tracking for the long-press latch:
//   _longFired - true once wasLongPressedA() has fired for the current
//   hold. Cleared when BtnA is released. Without this we'd retrigger the
//   long-press every frame while the button was still held.
static bool _longFired = false;

void init() {
  // Nothing to do; M5.begin() (in Platform::init()) already configured
  // the buttons. This stub exists so call sites symmetrically pair
  // Platform::init() + Input::init().
}

void poll() {
  M5.update();
  if (M5.BtnA.wasReleased()) {
    _longFired = false;
  }
}

bool wasPressedA() {
  // Short-press = release without a long-press having fired in this hold.
  // M5.BtnA.wasReleased() fires once on release regardless of duration;
  // we suppress it when the press has already been claimed by a long-
  // press detector, so a 1-second hold triggers menu without ALSO
  // triggering "advance".
  return M5.BtnA.wasReleased() && !_longFired;
}

bool wasPressedB() {
  return M5.BtnB.wasPressed();
}

bool wasLongPressedA(uint32_t holdMs) {
  if (_longFired) return false;
  if (M5.BtnA.pressedFor(holdMs)) {
    _longFired = true;
    return true;
  }
  return false;
}

bool isHeldA() { return M5.BtnA.isPressed(); }
bool isHeldB() { return M5.BtnB.isPressed(); }

}  // namespace Input
