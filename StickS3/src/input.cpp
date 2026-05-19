#include "input.h"
#include <M5Unified.h>

namespace Input {

// M5Unified handles pinMode(G11/G12, INPUT) automatically when it
// recognizes board_M5StickS3 (see M5Unified.cpp:1830-1832). We only need
// to call M5.update() once per poll to refresh BtnA/BtnB state.
//
// Edge tracking for the long-press latches:
//   _longFiredA - true once wasLongPressedA() has fired for the current
//   hold. Cleared when BtnA is released. Without this we'd retrigger the
//   long-press every frame while the button was still held.
//   _longFiredB - same idea for BtnB. Used to gate the in-menu mute
//   shortcut (hold B on the volume row) so a single hold fires once and
//   the matching wasPressedB() on release doesn't ALSO cycle the volume.
static bool _longFiredA = false;
static bool _longFiredB = false;

void init() {
  // Nothing to do; M5.begin() (in Platform::init()) already configured
  // the buttons. This stub exists so call sites symmetrically pair
  // Platform::init() + Input::init().
}

void poll() {
  M5.update();
  if (M5.BtnA.wasReleased()) _longFiredA = false;
  if (M5.BtnB.wasReleased()) _longFiredB = false;
}

bool wasPressedA() {
  // Short-press = release without a long-press having fired in this hold.
  // M5.BtnA.wasReleased() fires once on release regardless of duration;
  // we suppress it when the press has already been claimed by a long-
  // press detector, so a 1-second hold triggers menu without ALSO
  // triggering "advance".
  return M5.BtnA.wasReleased() && !_longFiredA;
}

bool wasPressedB() {
  // Symmetric with wasPressedA: short = press edge that hasn't matured
  // into a long-press. Without this gate, holding B on the volume row
  // (mute shortcut) would fire wasPressedB() at the press edge AND a
  // wasLongPressedB() ~600ms later — the volume would cycle once, then
  // jump to 0. The press edge waits for !_longFiredB.
  return M5.BtnB.wasPressed() && !_longFiredB;
}

bool wasLongPressedA(uint32_t holdMs) {
  if (_longFiredA) return false;
  if (M5.BtnA.pressedFor(holdMs)) {
    _longFiredA = true;
    return true;
  }
  return false;
}

bool wasLongPressedB(uint32_t holdMs) {
  if (_longFiredB) return false;
  if (M5.BtnB.pressedFor(holdMs)) {
    _longFiredB = true;
    return true;
  }
  return false;
}

bool isHeldA() { return M5.BtnA.isPressed(); }
bool isHeldB() { return M5.BtnB.isPressed(); }

}  // namespace Input
