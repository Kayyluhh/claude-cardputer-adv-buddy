#pragma once
// Compatibility shim: the original StickC Plus port used TFT_eSPI's
// TFT_eSprite class. Under M5Cardputer / M5Unified / M5GFX the sprite
// class is M5Canvas. Aliasing here means the 22 species files in
// src/buddies/ and src/buddy.cpp keep referring to TFT_eSprite without
// per-file edits beyond their #include line.
//
// The Cardputer-Adv build pulls in M5Canvas via M5Cardputer; the StickS3
// build pulls it in via M5Unified. Both ultimately come from M5GFX.
#ifdef STICKS3_BUILD
  #include <M5Unified.h>
#else
  #include <M5Cardputer.h>
#endif

using TFT_eSprite = M5Canvas;
