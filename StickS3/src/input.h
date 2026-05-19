#pragma once
#include <stdint.h>

// Edge-triggered 2-button wrapper around M5Unified's M5.BtnA / M5.BtnB.
//
// Call poll() once per frame after Input::init(). poll() drives M5.update()
// which in turn refreshes the BtnA/B state from KEY1 (G11) and KEY2 (G12).
//
// Mapping (matches upstream M5StickC Plus, anthropics/claude-desktop-buddy):
//   M5.BtnA.wasReleased()  -> Input::wasPressedA()      (short tap on release)
//   M5.BtnB.wasPressed()   -> Input::wasPressedB()      (short tap on down)
//   M5.BtnA.pressedFor(N)  -> Input::wasLongPressedA(N) (open menu, 600 ms)
//
// Only StickS3/src/main_stick.cpp consumes this API; the shared protocol
// and renderer code under ../src/ never calls Input::.
namespace Input {

void init();
void poll();

// Edge events — true exactly once for the frame the edge fires, then clear.
bool wasPressedA();                                 // BtnA short, on release
bool wasPressedB();                                 // BtnB short, on press
bool wasLongPressedA(uint32_t holdMs = 600);        // BtnA held >= holdMs

// Steady-state queries.
bool isHeldA();
bool isHeldB();

}  // namespace Input
