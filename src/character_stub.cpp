// Phase 4 stand-in for src/character.cpp — provides empty implementations
// of the GIF-character API so the rest of the firmware can call them
// unconditionally. Phase 6 swaps this file out (via build_src_filter)
// for the real character.cpp + AnimatedGIF + LittleFS pipeline.
#include "character.h"

bool characterInit(const char*) { return false; }
bool characterLoaded()           { return false; }

void characterSetState(uint8_t) {}
void characterTick()             {}
void characterInvalidate()       {}
void characterClose()            {}
void characterSetPeek(bool)      {}

const Palette& characterPalette() {
  // Default palette used when no GIF pack is installed. RGB565.
  // body=warm gold, bg=black, text=white, dim=grey, ink=dark blue.
  static const Palette p = { 0xFEE0, 0x0000, 0xFFFF, 0x8410, 0x041F };
  return p;
}
