#pragma once
// Compatibility shim: the original StickC Plus port used TFT_eSPI's
// TFT_eSprite class. Under M5Cardputer / M5GFX the sprite class is
// M5Canvas. Aliasing here means the 18 species files in src/buddies/
// and src/buddy.cpp keep referring to TFT_eSprite without per-file
// edits beyond their #include line.
#include <M5Cardputer.h>

using TFT_eSprite = M5Canvas;
