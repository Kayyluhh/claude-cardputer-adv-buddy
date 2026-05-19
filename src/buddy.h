#pragma once
#include <stdint.h>
// Pull in the LovyanGFX type used by buddyRenderTo(). gfx_compat.h
// resolves to <M5Unified.h> on STICKS3_BUILD and <M5Cardputer.h>
// otherwise; either way the LovyanGFX base class is visible here.
#include "gfx_compat.h"

// Multi-species ASCII buddy renderer. Each species lives in its own
// src/buddies/<name>.cpp file and exposes 7 state functions matching
// the PersonaState enum order: sleep, idle, busy, attention, celebrate,
// dizzy, heart.
void buddyInit();
void buddyTick(uint8_t personaState);
// Render one frame directly to a non-sprite display surface (used by the
// StickS3 landscape charging-clock branch to paint the pet into the
// rotated M5.Display coordinate space without going through the portrait
// sprite). Forces scale=1; no sprite-clear; advances the same shared
// tickCount as buddyTick.
void buddyRenderTo(LovyanGFX* target, uint8_t personaState);
void buddyInvalidate();
void buddySetSpecies(const char* name);
void buddySetSpeciesIdx(uint8_t idx);
void buddyNextSpecies();
void buddySetPeek(bool peek);
uint8_t buddySpeciesIdx();
uint8_t buddySpeciesCount();
const char* buddySpeciesName();

// Per-species state function: takes the global tickCount and renders
// the buddy + any overlays for the current state into the shared sprite.
typedef void (*StateFn)(uint32_t t);

struct Species {
  const char* name;
  uint16_t bodyColor;
  StateFn states[7];   // index by PersonaState (0=sleep .. 6=heart)
};
