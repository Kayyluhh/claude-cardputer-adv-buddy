#pragma once
#include <stdint.h>

// Edge-triggered keyboard wrapper around M5Cardputer.Keyboard.
//
// Call poll() once per frame after M5Cardputer.update(). The "wasPressed*"
// queries return true exactly once for the frame in which the key crossed
// from up to down; all edges clear at the start of the next poll().
//
// Replaces M5.BtnA / M5.BtnB semantics from the StickC port:
//   M5.BtnA.wasReleased()   -> Input::wasPressedEnter()
//   M5.BtnB.wasReleased()   -> Input::wasPressedDel()    (Esc / Backspace)
//   M5.BtnA.pressedFor(600) -> Input::wasLongPressedEnter()
namespace Input {

void init();
void poll();

bool wasPressedChar(char ch);   // a..z 0..9 . , ; ' / etc.
bool wasPressedEnter();
bool wasPressedDel();           // Backspace / Delete
bool wasPressedEsc();           // alias for wasPressedDel()
bool wasLongPressedEnter(uint32_t holdMs = 600);

bool isHeldChar(char ch);
bool isHeldEnter();
bool isHeldDel();

// Drain queued newly-pressed characters (for text entry like owner name).
// Returns 0 when empty.
char popChar();

}  // namespace Input
