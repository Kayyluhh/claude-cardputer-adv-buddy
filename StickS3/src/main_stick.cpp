// Hardware Buddy firmware — M5Stack StickS3 build.
//
// Ported from upstream `anthropics/claude-desktop-buddy@main:src/main.cpp`
// (the M5StickC Plus reference). Same 2-button UX, same Nordic UART
// protocol, same persona state machine. The differences are mechanical:
//
//   * M5Unified instead of M5StickCPlus (M5.* instead of M5.Lcd/M5.Axp/etc.)
//   * Hardware access funneled through Platform:: / Input:: shims.
//   * No microSD slot → no GIF support → ASCII pets only. character.cpp
//     is excluded from build_src_filter and SPECIES_GIF/gifAvailable/
//     buddyMode logic collapses to "always ASCII".
//   * Side power button is PMIC-managed (single=on/double=off/long=boot),
//     unreadable from firmware. We don't poll it and don't expose a
//     software "turn off" menu entry.
//   * No RGB LED on this board. Settings has a "led" toggle that just
//     stores the preference; nothing visibly happens.
//   * Sound on/off becomes a 5-step volume (0..4). In the settings menu
//     on the volume row, B-tap cycles up wrapping to 0, B-long jumps
//     directly to 0 (quick mute). applyVolume(level) maps to
//     M5.Speaker.setVolume(level * 45) — caps at ~71% per the datasheet's
//     <75% brown-out warning.

#include "gfx_compat.h"      // brings in M5Unified.h via STICKS3_BUILD
#include <stdarg.h>
#include "platform.h"
#include "input.h"
#include "ble_bridge.h"
#include "data.h"
#include "stats.h"
#include "buddy.h"

// ──────────────── globals ────────────────

// Portrait 135×240 — native ST7789P3 resolution on the StickS3.
const int W = 135, H = 240;
const int CX = W / 2;

TFT_eSprite spr(&M5.Display);

// Default palette for the ASCII-only build. The Cardputer-Adv build
// pulls these from character.cpp's parsed manifest; we have no manifest,
// so we hardcode a neutral dark-mode scheme. Species draw inside this.
struct Palette { uint16_t body, bg, text, textDim, ink; };
static const Palette PAL_DEFAULT = {
  /* body    */ 0x07FF,   // cyan — neutral pet body color
  /* bg      */ 0x0000,   // black
  /* text    */ 0xFFFF,   // white
  /* textDim */ 0x8410,   // mid-gray (matches BUDDY_DIM)
  /* ink     */ 0x0000,   // black ink-on-bright accents
};
inline const Palette& palette() { return PAL_DEFAULT; }

const uint16_t HOT      = 0xFA20;   // red-orange: warnings, impatience, deny
const uint16_t PANEL    = 0x2104;   // overlay panel background
// Locally namespaced UI palette — bare `GREEN` / `RED` collide with the
// m5gfx::ili9341_colors class constants that M5GFX makes visible at this
// scope. Using UI_ prefixed names keeps the code unambiguous.
const uint16_t UI_GREEN = 0x07E0;
const uint16_t UI_RED   = 0xF800;

// BLE advertisement name: "Claude-XXXX" with the last two BT-MAC bytes
// so multiple sticks in one room are distinguishable in the picker.
static char btName[16] = "Claude";
static void startBt() {
  uint8_t mac[6] = {0};
  esp_read_mac(mac, ESP_MAC_BT);
  snprintf(btName, sizeof(btName), "Claude-%02X%02X", mac[4], mac[5]);
  bleInit(btName);
}

// Stubs for symbols that ../src/xfer.h references but that live in
// character.cpp on the Cardputer-Adv build. We exclude character.cpp
// from build_src_filter (no SD slot, no GIF support) so we need to
// satisfy the linker here with no-op implementations.
//   gifAvailable: xfer.h's char_begin handler reads/writes this to
//     announce that a GIF has been installed. Always false here — the
//     SD-availability check in xfer.h gates char_begin before this
//     ever changes, so the assignment in xfer.h is unreachable.
//   buddyMode:    xfer.h flips this between ASCII and GIF mode after
//     a folder push. Always true here (ASCII only).
//   characterClose / characterInit: called by xfer.h when a folder
//     push arrives. Both no-op out — char_begin already fails earlier
//     with "no SD card" when Platform::sdAvailable() returns false.
bool gifAvailable = false;
bool buddyMode    = true;
void characterClose() {}
bool characterInit(const char*) { return false; }

enum PersonaState { P_SLEEP, P_IDLE, P_BUSY, P_ATTENTION, P_CELEBRATE, P_DIZZY, P_HEART };
const char* stateNames[] = { "sleep", "idle", "busy", "attention", "celebrate", "dizzy", "heart" };

TamaState    tama;
PersonaState baseState   = P_SLEEP;
PersonaState activeState = P_SLEEP;
uint32_t     oneShotUntil = 0;
uint32_t     lastShakeCheck = 0;
float        accelBaseline = 1.0f;
unsigned long t = 0;

// Menu state
bool    menuOpen    = false;
uint8_t menuSel     = 0;
uint8_t brightLevel = 4;
bool    btnALong    = false;

enum DisplayMode { DISP_NORMAL, DISP_PET, DISP_INFO, DISP_COUNT };
uint8_t displayMode = DISP_NORMAL;
uint8_t infoPage = 0;
uint8_t petPage = 0;
const uint8_t PET_PAGES = 2;
uint8_t msgScroll = 0;
uint16_t lastLineGen = 0;
char     lastPromptId[40] = "";
uint32_t lastInteractMs = 0;
bool     dimmed = false;
bool     screenOff = false;
bool     swallowBtnA = false;
bool     swallowBtnB = false;

uint32_t wakeTransitionUntil = 0;
const uint32_t SCREEN_OFF_MS = 30000;

bool     napping = false;
uint32_t napStartMs = 0;
uint32_t promptArrivedMs = 0;
bool     responseSent = false;

// Brightness mapping: brightLevel 0..4 → setBrightness percent 20..100.
// Platform::setBrightness takes 0..100 and converts to the underlying
// M5GFX 0..255 PWM. 20% is the lowest setting that's still readable.
static void applyBrightness() { Platform::setBrightness(20 + brightLevel * 20); }

// Volume mapping: 0=mute, 1..4 ramp up. M5.Speaker.setVolume range is
// 0..255; we cap at 180 (~71%) per the StickS3 datasheet's <75%
// brown-out warning on battery. level*45 gives 0/45/90/135/180.
static void applyVolume(uint8_t level) {
  if (level > 4) level = 4;
  M5.Speaker.setVolume(level * 45);
}

static void wake() {
  lastInteractMs = millis();
  if (screenOff) {
    Platform::screenOn();
    applyBrightness();
    screenOff = false;
    wakeTransitionUntil = millis() + 12000;
  }
  if (dimmed) { applyBrightness(); dimmed = false; }
}

static void beep(uint16_t freq, uint16_t dur) {
  if (settings().volumeLevel > 0) Platform::tone(freq, dur);
}

// Mario 1-UP — same intervals as upstream's permission-arrival cue.
static const uint16_t SFX_ALERT_F[] = { 659, 784, 1319, 1047, 1175, 1568 };
static const uint16_t SFX_ALERT_D[] = {  35,  35,   35,   35,   35,   90 };
static void sfxAlert() {
  if (settings().volumeLevel > 0)
    Platform::beepSeq(SFX_ALERT_F, SFX_ALERT_D,
                       sizeof(SFX_ALERT_F) / sizeof(SFX_ALERT_F[0]));
}

static void sendCmd(const char* json) {
  Serial.println(json);
  size_t n = strlen(json);
  bleWrite((const uint8_t*)json, n);
  bleWrite((const uint8_t*)"\n", 1);
}

// Pet cycler: 22 ASCII species, no GIF on this build. Wraps via
// buddyNextSpecies(); persisted in NVS by buddy.cpp's species key.
static void nextPet() {
  buddyNextSpecies();
  buddyInvalidate();
}

const uint8_t INFO_PAGES = 6;
const uint8_t INFO_PG_BUTTONS = 1;
const uint8_t INFO_PG_CREDITS = 5;

void applyDisplayMode() {
  bool peek = displayMode != DISP_NORMAL;
  buddySetPeek(peek);
  spr.fillSprite(palette().bg);
  buddyInvalidate();
}

// Menu items — "turn off" removed (StickS3 has no firmware power-off;
// user double-presses the side button per the PMIC button protocol).
const char* menuItems[] = { "settings", "help", "about", "demo", "close" };
const uint8_t MENU_N = 5;

bool    settingsOpen = false;
uint8_t settingsSel  = 0;
// "sound" renamed to "volume" — the row now shows 0..4 and B-long mutes.
const char* settingsItems[] = { "brightness", "volume", "bluetooth", "wifi",
                                 "led", "transcript", "clock rot", "ascii pet",
                                 "reset", "back" };
const uint8_t SETTINGS_N = 10;
const uint8_t SETTINGS_ROW_VOLUME = 1;

// Reset items — "delete char" removed (no SD/GIF on this build).
bool    resetOpen = false;
uint8_t resetSel  = 0;
const char* resetItems[] = { "factory reset", "back" };
const uint8_t RESET_N = 2;
static uint32_t resetConfirmUntil = 0;
static uint8_t  resetConfirmIdx = 0xFF;

static void applySetting(uint8_t idx) {
  Settings& s = settings();
  switch (idx) {
    case 0:                                       // brightness
      brightLevel = (brightLevel + 1) % 5;
      applyBrightness();
      return;
    case 1:                                       // volume cycle 0..4
      s.volumeLevel = (s.volumeLevel + 1) % 5;
      applyVolume(s.volumeLevel);
      break;
    case 2: s.bt = !s.bt; break;                  // bt (stored only — radio stays live)
    case 3: s.wifi = !s.wifi; break;              // wifi (placeholder)
    case 4: s.led = !s.led; break;                // led (no LED on StickS3 — stored only)
    case 5: s.hud = !s.hud; break;                // transcript
    case 6: s.clockRot = (s.clockRot + 1) % 3; break;  // clock rotation lock
    case 7: nextPet(); return;
    case 8: resetOpen = true; resetSel = 0; resetConfirmIdx = 0xFF; return;
    case 9: settingsOpen = false; buddyInvalidate(); return;
  }
  settingsSave();
}

// In-row mute shortcut: hold B on the volume row. Called from the loop
// when settingsOpen && settingsSel == SETTINGS_ROW_VOLUME && long-B fires.
static void applyVolumeMute() {
  Settings& s = settings();
  s.volumeLevel = 0;
  applyVolume(0);
  settingsSave();
}

// Tap-twice confirm — first tap arms (label flips to "really?"),
// second within 3s executes. Scrolling away clears the arm.
static void applyReset(uint8_t idx) {
  uint32_t now = millis();
  bool armed = (resetConfirmIdx == idx) && (int32_t)(now - resetConfirmUntil) < 0;

  if (idx == 1) { resetOpen = false; return; }    // back

  if (!armed) {
    resetConfirmIdx = idx;
    resetConfirmUntil = now + 3000;
    beep(1400, 60);
    return;
  }

  beep(800, 200);
  // Factory reset on StickS3: NVS namespace wipe + BLE bonds clear.
  // No LittleFS to format (no filesystem partition); no /characters/
  // directory to delete (no microSD slot).
  _prefs.begin("buddy", false);
  _prefs.clear();
  _prefs.end();
  bleClearBonds();
  delay(300);
  ESP.restart();
}

// Footer hint row inside a menu panel: "Next ↓  Change →".
const int MENU_HINT_H = 14;
static void drawMenuHints(const Palette& p, int mx, int mw, int hy,
                          const char* downLbl = "A", const char* rightLbl = "B") {
  spr.drawFastHLine(mx + 6, hy - 4, mw - 12, p.textDim);
  spr.setTextColor(p.textDim, PANEL);
  int x = mx + 8;
  spr.setCursor(x, hy); spr.print(downLbl);
  x += strlen(downLbl) * 6 + 4;
  spr.fillTriangle(x, hy + 1, x + 6, hy + 1, x + 3, hy + 6, p.textDim);
  x = mx + mw / 2 + 4;
  spr.setCursor(x, hy); spr.print(rightLbl);
  x += strlen(rightLbl) * 6 + 4;
  spr.fillTriangle(x, hy, x, hy + 6, x + 5, hy + 3, p.textDim);
}

static void drawSettings() {
  const Palette& p = palette();
  int mw = 118, mh = 16 + SETTINGS_N * 14 + MENU_HINT_H;
  int mx = (W - mw) / 2, my = (H - mh) / 2;
  spr.fillRoundRect(mx, my, mw, mh, 4, PANEL);
  spr.drawRoundRect(mx, my, mw, mh, 4, p.textDim);
  spr.setTextSize(1);
  Settings& s = settings();
  // Rows 2..5 display as on/off booleans (bt, wifi, led, transcript).
  bool vals[] = { s.bt, s.wifi, s.led, s.hud };
  for (int i = 0; i < SETTINGS_N; i++) {
    bool sel = (i == settingsSel);
    spr.setTextColor(sel ? p.text : p.textDim, PANEL);
    spr.setCursor(mx + 6, my + 8 + i * 14);
    spr.print(sel ? "> " : "  ");
    spr.print(settingsItems[i]);
    spr.setCursor(mx + mw - 36, my + 8 + i * 14);
    spr.setTextColor(p.textDim, PANEL);
    if (i == 0) {                                 // brightness
      spr.printf("%u/4", brightLevel);
    } else if (i == 1) {                          // volume
      if (s.volumeLevel == 0) {
        spr.setTextColor(p.textDim, PANEL);
        spr.print("mute");
      } else {
        spr.setTextColor(GREEN, PANEL);
        spr.printf("%u/4", s.volumeLevel);
      }
    } else if (i >= 2 && i <= 5) {                // bt / wifi / led / transcript
      bool v = vals[i - 2];
      spr.setTextColor(v ? UI_GREEN : p.textDim, PANEL);
      spr.print(v ? " on" : "off");
    } else if (i == 6) {                          // clock rotation
      static const char* const RN[] = { "auto", "port", "land" };
      spr.print(RN[s.clockRot]);
    } else if (i == 7) {                          // ascii pet — show species name
      // Right-align so the longest names ("capybara", "mushroom" — 8 chars)
      // fit within the panel without overflowing the rounded border.
      const char* nm = buddySpeciesName();
      int textW = (int)strlen(nm) * 6;            // 6px per char at size 1
      spr.setCursor(mx + mw - 6 - textW, my + 8 + i * 14);
      spr.print(nm);
    }
  }
  // Hint row varies on the volume row: B-long mutes there.
  if (settingsSel == SETTINGS_ROW_VOLUME) {
    drawMenuHints(p, mx, mw, my + mh - 12, "Next", "Up/Mute");
  } else {
    drawMenuHints(p, mx, mw, my + mh - 12, "Next", "Change");
  }
}

static void drawReset() {
  const Palette& p = palette();
  int mw = 118, mh = 16 + RESET_N * 14 + MENU_HINT_H;
  int mx = (W - mw) / 2, my = (H - mh) / 2;
  spr.fillRoundRect(mx, my, mw, mh, 4, PANEL);
  spr.drawRoundRect(mx, my, mw, mh, 4, HOT);
  spr.setTextSize(1);
  for (int i = 0; i < RESET_N; i++) {
    bool sel = (i == resetSel);
    spr.setTextColor(sel ? p.text : p.textDim, PANEL);
    spr.setCursor(mx + 6, my + 8 + i * 14);
    spr.print(sel ? "> " : "  ");
    bool armed = (i == resetConfirmIdx) &&
                 (int32_t)(millis() - resetConfirmUntil) < 0;
    if (armed) spr.setTextColor(HOT, PANEL);
    spr.print(armed ? "really?" : resetItems[i]);
  }
  drawMenuHints(p, mx, mw, my + mh - 12);
}

void menuConfirm() {
  switch (menuSel) {
    case 0: settingsOpen = true; menuOpen = false; settingsSel = 0; break;
    case 1:                                       // help
    case 2:                                       // about
      menuOpen = false;
      displayMode = DISP_INFO;
      infoPage = (menuSel == 1) ? INFO_PG_BUTTONS : INFO_PG_CREDITS;
      applyDisplayMode();
      buddyInvalidate();
      break;
    case 3: dataSetDemo(!dataDemo()); break;      // demo toggle
    case 4: menuOpen = false; buddyInvalidate(); break;  // close
  }
}

void drawMenu() {
  const Palette& p = palette();
  int mw = 118, mh = 16 + MENU_N * 14 + MENU_HINT_H;
  int mx = (W - mw) / 2, my = (H - mh) / 2;
  spr.fillRoundRect(mx, my, mw, mh, 4, PANEL);
  spr.drawRoundRect(mx, my, mw, mh, 4, p.textDim);
  spr.setTextSize(1);
  for (int i = 0; i < MENU_N; i++) {
    bool sel = (i == menuSel);
    spr.setTextColor(sel ? p.text : p.textDim, PANEL);
    spr.setCursor(mx + 6, my + 8 + i * 14);
    spr.print(sel ? "> " : "  ");
    spr.print(menuItems[i]);
    if (i == 3) spr.print(dataDemo() ? "  on" : "  off");  // demo state
  }
  drawMenuHints(p, mx, mw, my + mh - 12);
}

// ──────────────── IMU clock rotation ────────────────
// Same hysteresis logic as upstream. Charging clock takes over the home
// screen when on USB + bridge-synced RTC + no urgent state; orientation
// can flip portrait/landscape based on accel, gated by settings.clockRot.

static uint8_t clockOrient   = 0;
static int8_t  orientFrames  = 0;
static uint8_t paintedOrient = 0;
static struct tm _clkTm;
uint32_t        _clkLastRead = 0;
static bool     _onUsb       = false;

static void clockRefreshRtc() {
  if (millis() - _clkLastRead < 1000) return;
  _clkLastRead = millis();
  _onUsb = Platform::isOnUsb();
  Platform::getRtcLocal(_clkTm);
}

static void clockUpdateOrient() {
  float ax, ay, az;
  if (!Platform::imuAccel(ax, ay, az)) return;
  uint8_t lock = settings().clockRot;
  if (lock == 1) { clockOrient = 0; return; }       // portrait lock
  // IMU axis remap for the StickS3 (datasheet page 7): X+ is the LONG axis
  // pointing toward the USB-C end, Y+ is the short axis. So "device on its
  // side" means Y is dominant, not X (which was upstream's StickC Plus
  // assumption). This swap fixes the off-by-90° rotation behavior.
  if (lock == 2) {                                  // landscape lock
    if (clockOrient == 0) clockOrient = (ay >= 0) ? 1 : 3;
    if      (ay >  0.5f && clockOrient != 1) clockOrient = 1;
    else if (ay < -0.5f && clockOrient != 3) clockOrient = 3;
    return;
  }
  // Auto: dual-threshold hysteresis. Strict to enter sideways, loose to stay.
  bool side = (clockOrient == 0)
    ? fabsf(ay) > 0.7f && fabsf(ax) < 0.5f && fabsf(az) < 0.5f
    : fabsf(ay) > 0.4f;
  if (side) { if (orientFrames < 20) orientFrames++; }
  else      { if (orientFrames > -10) orientFrames--; }
  if (clockOrient == 0 && orientFrames >= 15) {
    clockOrient = (ay > 0) ? 1 : 3;
  } else if (clockOrient != 0 && orientFrames <= -8) {
    clockOrient = 0;
  } else if (clockOrient != 0 && side) {
    // Direct 1↔3 swap when the stick is flipped end-to-end while sideways.
    static int8_t swapFrames = 0;
    uint8_t want = (ay > 0) ? 1 : 3;
    if (want != clockOrient) { if (++swapFrames >= 8) { clockOrient = want; swapFrames = 0; } }
    else swapFrames = 0;
  }
}

// Clock face — portrait paints to sprite, landscape paints direct-to-LCD
// in rotated coordinate space (same pattern as upstream).
static const char* const MON[] = {
  "Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"
};
static const char* const DOW[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};

static void drawClock() {
  const Palette& p = palette();
  char hm[6]; snprintf(hm, sizeof(hm), "%02u:%02u", _clkTm.tm_hour, _clkTm.tm_min);
  char ss[4]; snprintf(ss, sizeof(ss), ":%02u", _clkTm.tm_sec);
  uint8_t mi = (_clkTm.tm_mon >= 0 && _clkTm.tm_mon <= 11) ? _clkTm.tm_mon : 0;
  char dl[8]; snprintf(dl, sizeof(dl), "%s %02u", MON[mi], _clkTm.tm_mday);

  if (clockOrient == 0) {
    paintedOrient = 0;
    spr.fillRect(0, 90, W, H - 90, p.bg);
    spr.setTextDatum(MC_DATUM);
    spr.setTextSize(4); spr.setTextColor(p.text, p.bg);    spr.drawString(hm, CX, 140);
    spr.setTextSize(2); spr.setTextColor(p.textDim, p.bg); spr.drawString(ss, CX, 175);
    spr.setTextSize(1);                                     spr.drawString(dl, CX, 200);
    spr.setTextDatum(TL_DATUM);
    return;
  }

  // Landscape — direct-to-LCD with rotation. Full fill on entry; text
  // glyphs self-repaint and the pet box gets a per-tick fillRect.
  M5.Display.setRotation(clockOrient);
  static uint8_t lastSec = 0xFF;
  bool repaint = paintedOrient != clockOrient;
  if (repaint) { M5.Display.fillScreen(p.bg); paintedOrient = clockOrient; lastSec = 0xFF; }

  if (repaint || _clkTm.tm_sec != lastSec) {
    lastSec = _clkTm.tm_sec;
    char wdl[12]; snprintf(wdl, sizeof(wdl), "%s %s %02u",
                          DOW[_clkTm.tm_wday % 7], MON[mi], _clkTm.tm_mday);
    char ssl[3]; snprintf(ssl, sizeof(ssl), "%02u", _clkTm.tm_sec);
    M5.Display.setTextDatum(MC_DATUM);
    M5.Display.setTextSize(3); M5.Display.setTextColor(p.text, p.bg);    M5.Display.drawString(hm,  170, 42);
    M5.Display.setTextSize(2); M5.Display.setTextColor(p.textDim, p.bg); M5.Display.drawString(ssl, 170, 72);
                                                                          M5.Display.drawString(wdl, 170, 102);
    M5.Display.setTextDatum(TL_DATUM);
    M5.Display.setTextSize(1);
  }

  // Pet on left at 5fps. Direct render to the rotated display.
  static uint32_t lastPetTick = 0;
  if (millis() - lastPetTick >= 200) {
    lastPetTick = millis();
    M5.Display.fillRect(0, 0, 115, 90, p.bg);
    buddyRenderTo(&M5.Display, activeState);
  }
  M5.Display.setRotation(0);
}

// Forward declaration of the transcript word-wrapper defined further down
// — drawHomeLandscape calls it before its own definition appears.
static uint8_t wrapInto(const char* in, char out[][24], uint8_t maxRows, uint8_t width);

// Landscape home screen — buddy on the left, transcript on the right.
// Activated when the device is tilted sideways on the home screen and the
// charging-clock conditions aren't met. Paints direct-to-LCD in the rotated
// coordinate space (240×135) using the same buddyRenderTo() helper the
// landscape clock uses for the pet column.
//
// Layout (clockOrient = 1, BtnA-side down):
//   x 0..114   pet column (buddy renders at scale 1)
//   x 120..240 transcript column (5..7 wrapped lines)
//
// Like drawClock(), this function leaves the rotation as 0 on exit so any
// subsequent sprite-based draws aren't pushed into a rotated framebuffer.
static void drawHomeLandscape() {
  const Palette& p = palette();
  M5.Display.setRotation(clockOrient);

  bool repaint = paintedOrient != clockOrient;
  if (repaint) {
    M5.Display.fillScreen(p.bg);
    paintedOrient = clockOrient;
  }

  // Pet on left at 5 fps, same cadence as the landscape clock pet.
  static uint32_t lastPetTick = 0;
  if (millis() - lastPetTick >= 200) {
    lastPetTick = millis();
    M5.Display.fillRect(0, 0, 115, 135, p.bg);
    buddyRenderTo(&M5.Display, activeState);
  }

  // Transcript on right. Repaint only when content changes or scroll
  // shifts — full-rect clears at 60 Hz tear visibly.
  static uint16_t lastDrawnLineGen = 0xFFFF;
  static uint8_t  lastDrawnScroll  = 0xFF;
  if (repaint || tama.lineGen != lastDrawnLineGen || msgScroll != lastDrawnScroll) {
    lastDrawnLineGen = tama.lineGen;
    lastDrawnScroll  = msgScroll;

    M5.Display.fillRect(120, 0, 120, 135, p.bg);
    M5.Display.setTextSize(1);

    if (tama.nLines == 0) {
      M5.Display.setTextColor(p.text, p.bg);
      M5.Display.setCursor(124, 60);
      M5.Display.print(tama.msg);
    } else {
      // Wrap each transcript line at ~19 chars (120 px / 6 px-per-char,
      // minus a couple px of padding) into a flat display buffer. Up to
      // ~13 wrapped lines fit at LH=10.
      const uint8_t WIDTH = 19;
      const int     LH    = 10;
      const uint8_t SHOW  = 12;
      static char    disp[24][24];
      static uint8_t srcOf[24];
      uint8_t nDisp = 0;
      for (uint8_t i = 0; i < tama.nLines && nDisp < 24; i++) {
        uint8_t got = wrapInto(tama.lines[i], &disp[nDisp], 24 - nDisp, WIDTH);
        for (uint8_t j = 0; j < got; j++) srcOf[nDisp + j] = i;
        nDisp += got;
      }

      uint8_t maxBack = (nDisp > SHOW) ? (nDisp - SHOW) : 0;
      uint8_t scroll  = (msgScroll > maxBack) ? maxBack : msgScroll;
      int end   = (int)nDisp - scroll;
      int start = end - SHOW; if (start < 0) start = 0;
      uint8_t newest = tama.nLines - 1;
      for (int i = 0; start + i < end; i++) {
        uint8_t row = start + i;
        bool fresh = (srcOf[row] == newest) && (scroll == 0);
        M5.Display.setTextColor(fresh ? p.text : p.textDim, p.bg);
        M5.Display.setCursor(124, 4 + i * LH);
        M5.Display.print(disp[row]);
      }
      if (scroll > 0) {
        M5.Display.setTextColor(p.body, p.bg);
        M5.Display.setCursor(220, 4);
        M5.Display.printf("-%u", scroll);
      }
    }
  }

  M5.Display.setRotation(0);
}

// ──────────────── state machine ────────────────

PersonaState derive(const TamaState& s) {
  if (!s.connected)            return P_IDLE;
  if (s.sessionsWaiting > 0)   return P_ATTENTION;
  if (s.recentlyCompleted)     return P_CELEBRATE;
  if (s.sessionsRunning >= 3)  return P_BUSY;
  return P_IDLE;
}

void triggerOneShot(PersonaState s, uint32_t durMs) {
  activeState = s;
  oneShotUntil = millis() + durMs;
}

bool checkShake() {
  float ax, ay, az;
  if (!Platform::imuAccel(ax, ay, az)) return false;
  float mag = sqrtf(ax*ax + ay*ay + az*az);
  float delta = fabsf(mag - accelBaseline);
  accelBaseline = accelBaseline * 0.95f + mag * 0.05f;
  return delta > 0.8f;
}

static bool isFaceDown() {
  float ax, ay, az;
  if (!Platform::imuAccel(ax, ay, az)) return false;
  return az < -0.7f && fabsf(ax) < 0.4f && fabsf(ay) < 0.4f;
}

// ──────────────── info pages ────────────────

static void _infoHeader(const Palette& p, int& y, const char* section, uint8_t page) {
  spr.setTextColor(p.text, p.bg);
  spr.setCursor(4, y); spr.print("Info");
  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(W - 28, y); spr.printf("%u/%u", page + 1, INFO_PAGES);
  y += 12;
  spr.setTextColor(p.body, p.bg);
  spr.setCursor(4, y); spr.print(section);
  y += 12;
}

void drawPasskey() {
  const Palette& p = palette();
  spr.fillSprite(p.bg);
  spr.setTextSize(1);
  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(8, 56);  spr.print("BLUETOOTH PAIRING");
  spr.setCursor(8, 184); spr.print("enter on desktop:");
  spr.setTextSize(3);
  spr.setTextColor(p.text, p.bg);
  char b[8]; snprintf(b, sizeof(b), "%06lu", (unsigned long)blePasskey());
  spr.setCursor((W - 18 * 6) / 2, 110);
  spr.print(b);
}

void drawInfo() {
  const Palette& p = palette();
  const int TOP = 70;
  spr.fillRect(0, TOP, W, H - TOP, p.bg);
  spr.setTextSize(1);
  int y = TOP + 2;
  auto ln = [&](const char* fmt, ...) {
    char b[32]; va_list a; va_start(a, fmt); vsnprintf(b, sizeof(b), fmt, a); va_end(a);
    spr.setCursor(4, y); spr.print(b); y += 8;
  };

  if (infoPage == 0) {
    _infoHeader(p, y, "ABOUT", infoPage);
    spr.setTextColor(p.textDim, p.bg);
    ln("I watch your Claude");
    ln("Code sessions.");
    y += 6;
    ln("I sleep when nothing's");
    ln("happening, wake when");
    ln("you start working,");
    ln("get impatient when");
    ln("approvals pile up.");
    y += 6;
    spr.setTextColor(p.text, p.bg);
    ln("Press A on a prompt");
    ln("to approve from here.");
    y += 6;
    spr.setTextColor(p.textDim, p.bg);
    ln("22 species. Settings");
    ln("> ascii pet to cycle.");

  } else if (infoPage == 1) {
    _infoHeader(p, y, "BUTTONS", infoPage);
    spr.setTextColor(p.text, p.bg);    ln("A   front");
    spr.setTextColor(p.textDim, p.bg); ln("    next screen");
    ln("    approve prompt"); y += 4;
    spr.setTextColor(p.text, p.bg);    ln("B   right side");
    spr.setTextColor(p.textDim, p.bg); ln("    next page");
    ln("    deny prompt"); y += 4;
    spr.setTextColor(p.text, p.bg);    ln("hold A");
    spr.setTextColor(p.textDim, p.bg); ln("    menu"); y += 4;
    spr.setTextColor(p.text, p.bg);    ln("Power  left side");
    spr.setTextColor(p.textDim, p.bg); ln("    tap = wake");
    ln("    double = off");
    ln("    hold = boot mode");

  } else if (infoPage == 2) {
    _infoHeader(p, y, "CLAUDE", infoPage);
    spr.setTextColor(p.textDim, p.bg);
    ln("  sessions  %u", tama.sessionsTotal);
    ln("  running   %u", tama.sessionsRunning);
    ln("  waiting   %u", tama.sessionsWaiting);
    y += 8;
    spr.setTextColor(p.text, p.bg);
    ln("LINK");
    spr.setTextColor(p.textDim, p.bg);
    ln("  via       %s", dataScenarioName());
    ln("  ble       %s", !bleConnected() ? "-" : bleSecure() ? "encrypted" : "OPEN");
    uint32_t age = (millis() - tama.lastUpdated) / 1000;
    ln("  last msg  %lus", (unsigned long)age);
    ln("  state     %s", stateNames[activeState]);

  } else if (infoPage == 3) {
    _infoHeader(p, y, "DEVICE", infoPage);
    int vBat_mV = Platform::batteryMv();
    int pct     = Platform::batteryPct();
    bool usb    = Platform::isOnUsb();

    spr.setTextColor(p.text, p.bg);
    spr.setTextSize(2);
    spr.setCursor(4, y);
    spr.printf("%d%%", pct);
    spr.setTextSize(1);
    spr.setTextColor(usb ? HOT : p.textDim, p.bg);
    spr.setCursor(60, y + 4);
    spr.print(usb ? "charging" : "battery");
    y += 20;

    spr.setTextColor(p.textDim, p.bg);
    ln("  battery  %d.%02dV", vBat_mV/1000, (vBat_mV%1000)/10);
    y += 8;

    spr.setTextColor(p.text, p.bg);
    ln("SYSTEM");
    spr.setTextColor(p.textDim, p.bg);
    if (ownerName()[0]) ln("  owner    %s", ownerName());
    uint32_t up = millis() / 1000;
    ln("  uptime   %luh %02lum", up / 3600, (up / 60) % 60);
    ln("  heap     %uKB", ESP.getFreeHeap() / 1024);
    ln("  bright   %u/4", brightLevel);
    ln("  volume   %u/4", settings().volumeLevel);
    ln("  bt       %s", settings().bt ? (dataBtActive() ? "linked" : "on") : "off");

  } else if (infoPage == 4) {
    _infoHeader(p, y, "BLUETOOTH", infoPage);
    bool linked = settings().bt && dataBtActive();
    spr.setTextColor(linked ? UI_GREEN : (settings().bt ? HOT : p.textDim), p.bg);
    spr.setTextSize(2);
    spr.setCursor(4, y);
    spr.print(linked ? "linked" : (settings().bt ? "discover" : "off"));
    spr.setTextSize(1);
    y += 20;

    spr.setTextColor(p.text, p.bg);
    ln("  %s", btName);
    spr.setTextColor(p.textDim, p.bg);
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_BT);
    ln("  %02X:%02X:%02X:%02X:%02X:%02X",
       mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
    y += 8;

    if (linked) {
      uint32_t age = (millis() - tama.lastUpdated) / 1000;
      ln("  last msg  %lus", (unsigned long)age);
    } else if (settings().bt) {
      spr.setTextColor(p.text, p.bg);
      ln("TO PAIR");
      spr.setTextColor(p.textDim, p.bg);
      ln(" From Claude Code:");
      ln("   /buddy-run");
      y += 2;
      ln(" Or Claude desktop:");
      ln("   Developer >");
      ln("   Hardware Buddy");
    }

  } else {
    _infoHeader(p, y, "CREDITS", infoPage);
    spr.setTextColor(p.textDim, p.bg);
    ln("based on");
    y += 4;
    spr.setTextColor(p.text, p.bg);
    ln("Felix Rieseberg");
    ln(" desktop-buddy");
    y += 8;
    spr.setTextColor(p.textDim, p.bg);
    ln("port");
    y += 4;
    spr.setTextColor(p.text, p.bg);
    ln("Kayyluhh");
    y += 8;
    spr.setTextColor(p.textDim, p.bg);
    ln("hardware");
    y += 4;
    ln("M5Stack StickS3");
    ln("ESP32-S3 + M5PM1");
  }
}

// Greedy word-wrap for transcript display.
static uint8_t wrapInto(const char* in, char out[][24], uint8_t maxRows, uint8_t width) {
  uint8_t row = 0, col = 0;
  const char* p = in;
  while (*p && row < maxRows) {
    while (*p == ' ') p++;
    const char* w = p;
    while (*p && *p != ' ') p++;
    uint8_t wlen = p - w;
    if (wlen == 0) break;
    uint8_t need = (col > 0 ? 1 : 0) + wlen;
    if (col + need > width) {
      out[row][col] = 0;
      if (++row >= maxRows) return row;
      out[row][0] = ' '; col = 1;
    }
    if (col > 1 || (col == 1 && out[row][0] != ' ')) out[row][col++] = ' ';
    else if (col == 1 && row > 0) {}
    while (wlen > width - col) {
      uint8_t take = width - col;
      memcpy(&out[row][col], w, take); col += take; w += take; wlen -= take;
      out[row][col] = 0;
      if (++row >= maxRows) return row;
      out[row][0] = ' '; col = 1;
    }
    memcpy(&out[row][col], w, wlen); col += wlen;
  }
  if (col > 0 && row < maxRows) { out[row][col] = 0; row++; }
  return row;
}

static void drawApproval() {
  const Palette& p = palette();
  const int AREA = 78;
  spr.fillRect(0, H - AREA, W, AREA, p.bg);
  spr.drawFastHLine(0, H - AREA, W, p.textDim);

  spr.setTextSize(1);
  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(4, H - AREA + 4);
  uint32_t waited = (millis() - promptArrivedMs) / 1000;
  if (waited >= 10) spr.setTextColor(HOT, p.bg);
  spr.printf("approve? %lus", (unsigned long)waited);

  int toolLen = strlen(tama.promptTool);
  spr.setTextColor(p.text, p.bg);
  spr.setTextSize(toolLen <= 10 ? 2 : 1);
  spr.setCursor(4, H - AREA + (toolLen <= 10 ? 14 : 18));
  spr.print(tama.promptTool);
  spr.setTextSize(1);

  spr.setTextColor(p.textDim, p.bg);
  int hlen = strlen(tama.promptHint);
  spr.setCursor(4, H - AREA + 34);
  spr.printf("%.21s", tama.promptHint);
  if (hlen > 21) {
    spr.setCursor(4, H - AREA + 42);
    spr.printf("%.21s", tama.promptHint + 21);
  }

  if (responseSent) {
    spr.setTextColor(p.textDim, p.bg);
    spr.setCursor(4, H - 12);
    spr.print("sent...");
  } else {
    spr.setTextColor(UI_GREEN, p.bg);
    spr.setCursor(4, H - 12);
    spr.print("A: approve");
    spr.setTextColor(HOT, p.bg);
    spr.setCursor(W - 48, H - 12);
    spr.print("B: deny");
  }
}

static void tinyHeart(int x, int y, bool filled, uint16_t col) {
  if (filled) {
    spr.fillCircle(x - 2, y, 2, col);
    spr.fillCircle(x + 2, y, 2, col);
    spr.fillTriangle(x - 4, y + 1, x + 4, y + 1, x, y + 5, col);
  } else {
    spr.drawCircle(x - 2, y, 2, col);
    spr.drawCircle(x + 2, y, 2, col);
    spr.drawLine(x - 4, y + 1, x, y + 5, col);
    spr.drawLine(x + 4, y + 1, x, y + 5, col);
  }
}

static void drawPetStats(const Palette& p) {
  const int TOP = 70;
  spr.fillRect(0, TOP, W, H - TOP, p.bg);
  spr.setTextSize(1);
  int y = TOP + 16;

  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(6, y - 2); spr.print("mood");
  uint8_t mood = statsMoodTier();
  uint16_t moodCol = (mood >= 3) ? UI_RED : (mood >= 2) ? HOT : p.textDim;
  for (int i = 0; i < 4; i++) tinyHeart(54 + i * 16, y + 2, i < mood, moodCol);

  y += 20;
  spr.setCursor(6, y - 2); spr.print("fed");
  uint8_t fed = statsFedProgress();
  for (int i = 0; i < 10; i++) {
    int px = 38 + i * 9;
    if (i < fed) spr.fillCircle(px, y + 1, 2, p.body);
    else spr.drawCircle(px, y + 1, 2, p.textDim);
  }

  y += 20;
  spr.setCursor(6, y - 2); spr.print("energy");
  uint8_t en = statsEnergyTier();
  uint16_t enCol = (en >= 4) ? 0x07FF : (en >= 2) ? 0xFFE0 : HOT;
  for (int i = 0; i < 5; i++) {
    int px = 54 + i * 13;
    if (i < en) spr.fillRect(px, y - 2, 9, 6, enCol);
    else spr.drawRect(px, y - 2, 9, 6, p.textDim);
  }

  y += 24;
  spr.fillRoundRect(6, y - 2, 42, 14, 3, p.body);
  spr.setTextColor(p.bg, p.body);
  spr.setCursor(11, y + 1); spr.printf("Lv %u", stats().level);

  y += 20;
  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(6, y);     spr.printf("approved %u", stats().approvals);
  spr.setCursor(6, y + 10); spr.printf("denied   %u", stats().denials);
  uint32_t nap = stats().napSeconds;
  spr.setCursor(6, y + 20); spr.printf("napped   %luh%02lum", nap/3600, (nap/60)%60);
  auto tokFmt = [&](const char* label, uint32_t v, int yPx) {
    spr.setCursor(6, yPx);
    if (v >= 1000000)   spr.printf("%s%lu.%luM", label, v/1000000, (v/100000)%10);
    else if (v >= 1000) spr.printf("%s%lu.%luK", label, v/1000, (v/100)%10);
    else                spr.printf("%s%lu", label, v);
  };
  tokFmt("tokens   ", stats().tokens, y + 30);
  tokFmt("today    ", tama.tokensToday, y + 40);
}

static void drawPetHowTo(const Palette& p) {
  const int TOP = 70;
  spr.fillRect(0, TOP, W, H - TOP, p.bg);
  spr.setTextSize(1);
  int y = TOP + 2;
  auto ln = [&](uint16_t c, const char* s) {
    spr.setTextColor(c, p.bg); spr.setCursor(6, y); spr.print(s); y += 9;
  };
  auto gap = [&]() { y += 4; };
  y += 12;

  ln(p.body,    "MOOD");
  ln(p.textDim, " approve fast = up");
  ln(p.textDim, " deny lots = down"); gap();

  ln(p.body,    "FED");
  ln(p.textDim, " 50K tokens =");
  ln(p.textDim, " level up + confetti"); gap();

  ln(p.body,    "ENERGY");
  ln(p.textDim, " face-down to nap");
  ln(p.textDim, " refills to full"); gap();

  ln(p.textDim, "idle 30s = off");
  ln(p.textDim, "any button = wake"); gap();

  ln(p.textDim, "A: screens  B: page");
  ln(p.textDim, "hold A: menu");
}

void drawPet() {
  const Palette& p = palette();
  int y = 70;

  if (petPage == 0) drawPetStats(p);
  else drawPetHowTo(p);

  spr.setTextSize(1);
  spr.setTextColor(p.text, p.bg);
  spr.setCursor(4, y + 2);
  if (ownerName()[0]) spr.printf("%s's %s", ownerName(), petName());
  else                spr.print(petName());
  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(W - 28, y + 2);
  spr.printf("%u/%u", petPage + 1, PET_PAGES);
}

void drawHUD() {
  // Stop deferring to drawApproval the instant we've sent a decision —
  // don't wait for the bridge to clear tama.promptId. Otherwise the
  // overlay sticks around showing "sent..." until the next heartbeat,
  // and during the wait the transcript stays hidden.
  if (tama.promptId[0] && !responseSent) { drawApproval(); return; }
  const Palette& p = palette();
  // AREA matches drawApproval's 78 px so that when we transition
  // approval → transcript, the fillRect below wipes every pixel
  // drawApproval painted. With the old AREA=28 (3 lines), the top
  // 50 px of stale approval text persisted indefinitely beneath the
  // freshly-drawn 3 transcript lines. Bumping SHOW to 9 also gives the
  // user a much more useful slice of recent context.
  const int SHOW = 9, LH = 8, WIDTH = 21;
  const int AREA = 78;
  spr.fillRect(0, H - AREA, W, AREA, p.bg);
  spr.setTextSize(1);

  if (tama.lineGen != lastLineGen) { msgScroll = 0; lastLineGen = tama.lineGen; wake(); }

  if (tama.nLines == 0) {
    spr.setTextColor(p.text, p.bg);
    spr.setCursor(4, H - LH - 2);
    spr.print(tama.msg);
    return;
  }

  static char disp[32][24];
  static uint8_t srcOf[32];
  uint8_t nDisp = 0;
  for (uint8_t i = 0; i < tama.nLines && nDisp < 32; i++) {
    uint8_t got = wrapInto(tama.lines[i], &disp[nDisp], 32 - nDisp, WIDTH);
    for (uint8_t j = 0; j < got; j++) srcOf[nDisp + j] = i;
    nDisp += got;
  }

  uint8_t maxBack = (nDisp > SHOW) ? (nDisp - SHOW) : 0;
  if (msgScroll > maxBack) msgScroll = maxBack;

  int end = (int)nDisp - msgScroll;
  int start = end - SHOW; if (start < 0) start = 0;
  uint8_t newest = tama.nLines - 1;
  for (int i = 0; start + i < end; i++) {
    uint8_t row = start + i;
    bool fresh = (srcOf[row] == newest) && (msgScroll == 0);
    spr.setTextColor(fresh ? p.text : p.textDim, p.bg);
    spr.setCursor(4, H - AREA + 2 + i * LH);
    spr.print(disp[row]);
  }
  if (msgScroll > 0) {
    spr.setTextColor(p.body, p.bg);
    spr.setCursor(W - 18, H - LH - 2);
    spr.printf("-%u", msgScroll);
  }
}

// ──────────────── setup / loop ────────────────

// Trace helper for setup() — prints with flush so a hang shows the last
// successful checkpoint instead of being lost in stdio buffering.
static void trace(const char* msg) {
  Serial.print("[stick] ");
  Serial.println(msg);
  Serial.flush();
}

void setup() {
  Serial.begin(115200);
  delay(50);
  trace("setup: begin");

  Platform::init();           // M5.begin(cfg) with output_power=false + fallback_board
  trace("setup: Platform::init done");

  Input::init();
  trace("setup: Input::init done");

  startBt();
  trace("setup: BLE up");

  applyBrightness();
  applyVolume(settings().volumeLevel);
  trace("setup: brightness/volume set");

  lastInteractMs = millis();
  statsLoad();
  trace("setup: stats loaded");
  settingsLoad();
  trace("setup: settings loaded");
  petNameLoad();
  trace("setup: petname loaded");
  applyVolume(settings().volumeLevel);
  buddyInit();
  trace("setup: buddy initialized");

  spr.setColorDepth(16);
  spr.createSprite(W, H);
  trace("setup: sprite created");
  applyDisplayMode();
  trace("setup: applyDisplayMode done");

  // Splash: "<owner>'s <pet>" if owner is set, else a generic greeting.
  {
    const Palette& p = palette();
    spr.fillSprite(p.bg);
    spr.setTextDatum(MC_DATUM);
    spr.setTextSize(2);
    if (ownerName()[0]) {
      char line[40];
      snprintf(line, sizeof(line), "%s's", ownerName());
      spr.setTextColor(p.text, p.bg); spr.drawString(line, W/2, H/2 - 12);
      spr.setTextColor(p.body, p.bg); spr.drawString(petName(), W/2, H/2 + 12);
    } else {
      spr.setTextColor(p.body, p.bg); spr.drawString("Hello!", W/2, H/2 - 12);
      spr.setTextSize(1);
      spr.setTextColor(p.textDim, p.bg);
      spr.drawString("a buddy appears", W/2, H/2 + 12);
    }
    spr.setTextDatum(TL_DATUM); spr.setTextSize(1);
    spr.pushSprite(0, 0);
    trace("setup: splash pushed");
    delay(500);                            // shortened for faster bring-up iteration
    trace("setup: splash delay done");
  }

  trace("setup: buddy ready, entering loop");
}

void loop() {
  Input::poll();              // wraps M5.update()
  Platform::tickAudio();      // no-op (Speaker is non-blocking)
  t++;
  uint32_t now = millis();

  // Heartbeat once per second so we can tell if loop is running at all.
  // If this print stops, we know what frame it died on.
  static uint32_t lastHeartbeat = 0;
  if (now - lastHeartbeat >= 1000) {
    lastHeartbeat = now;
    Serial.printf("[stick] loop tick=%lu state=%d btA=%d btB=%d\n",
                  (unsigned long)t, activeState,
                  Input::isHeldA() ? 1 : 0, Input::isHeldB() ? 1 : 0);
    Serial.flush();
  }

  dataPoll(&tama);
  if (statsPollLevelUp()) triggerOneShot(P_CELEBRATE, 3000);
  baseState = derive(tama);

  // After waking, hold P_SLEEP for 12s so users see the wake-up animation.
  if (baseState == P_IDLE && (int32_t)(now - wakeTransitionUntil) < 0) baseState = P_SLEEP;
  if ((int32_t)(now - oneShotUntil) >= 0) activeState = baseState;

  // Shake → dizzy.
  if (now - lastShakeCheck > 50) {
    lastShakeCheck = now;
    if (!menuOpen && !screenOff && checkShake() && (int32_t)(now - oneShotUntil) >= 0) {
      wake();
      triggerOneShot(P_DIZZY, 2000);
      Serial.println("shake: dizzy");
    }
  }

  // Prompt arrival: alert chirp, force home, clear menus.
  if (strcmp(tama.promptId, lastPromptId) != 0) {
    strncpy(lastPromptId, tama.promptId, sizeof(lastPromptId)-1);
    lastPromptId[sizeof(lastPromptId)-1] = 0;
    responseSent = false;
    if (tama.promptId[0]) {
      promptArrivedMs = millis();
      wake();
      sfxAlert();
      displayMode = DISP_NORMAL;
      menuOpen = settingsOpen = resetOpen = false;
      applyDisplayMode();
      buddyInvalidate();
    }
  }

  bool inPrompt = tama.promptId[0] && !responseSent;

  // Wake-on-press swallow: when the screen was off and a button woke it,
  // suppress that same button's edge events so the wake itself doesn't
  // also cycle screens or open the menu.
  if (Input::isHeldA() || Input::isHeldB()) {
    if (screenOff) {
      if (Input::isHeldA()) swallowBtnA = true;
      if (Input::isHeldB()) swallowBtnB = true;
    }
    wake();
  }

  // ─── Button A: long-press toggles menu / closes overlays ───
  if (Input::wasLongPressedA(600) && !swallowBtnA) {
    beep(800, 60);
    if (resetOpen) { resetOpen = false; }
    else if (settingsOpen) { settingsOpen = false; buddyInvalidate(); }
    else {
      menuOpen = !menuOpen;
      menuSel = 0;
      if (!menuOpen) buddyInvalidate();
    }
    Serial.println(menuOpen ? "menu open" : "menu close");
  }

  // ─── Button A: short-press = advance / approve / cycle screen ───
  if (Input::wasPressedA()) {
    if (swallowBtnA) {
      swallowBtnA = false;
    } else if (inPrompt) {
      char cmd[96];
      snprintf(cmd, sizeof(cmd),
               "{\"cmd\":\"permission\",\"id\":\"%s\",\"decision\":\"once\"}",
               tama.promptId);
      sendCmd(cmd);
      responseSent = true;
      uint32_t tookS = (millis() - promptArrivedMs) / 1000;
      statsOnApproval(tookS);
      beep(2400, 60);
      if (tookS < 5) triggerOneShot(P_HEART, 2000);
    } else if (resetOpen) {
      beep(1800, 30);
      resetSel = (resetSel + 1) % RESET_N;
      resetConfirmIdx = 0xFF;
    } else if (settingsOpen) {
      beep(1800, 30);
      settingsSel = (settingsSel + 1) % SETTINGS_N;
    } else if (menuOpen) {
      beep(1800, 30);
      menuSel = (menuSel + 1) % MENU_N;
    } else {
      beep(1800, 30);
      displayMode = (displayMode + 1) % DISP_COUNT;
      applyDisplayMode();
    }
  }

  // ─── Button B: long-press = volume row mute shortcut (only) ───
  if (Input::wasLongPressedB(600) && !swallowBtnB) {
    if (settingsOpen && settingsSel == SETTINGS_ROW_VOLUME) {
      applyVolumeMute();
      beep(400, 80);              // distinct low chirp = "muted"
    }
  }

  // ─── Button B: short-press = confirm / deny / scroll / page ───
  if (Input::wasPressedB()) {
    if (swallowBtnB) {
      swallowBtnB = false;
    } else if (inPrompt) {
      char cmd[96];
      snprintf(cmd, sizeof(cmd),
               "{\"cmd\":\"permission\",\"id\":\"%s\",\"decision\":\"deny\"}",
               tama.promptId);
      sendCmd(cmd);
      responseSent = true;
      statsOnDenial();
      beep(600, 60);
    } else if (resetOpen) {
      beep(2400, 30);
      applyReset(resetSel);
    } else if (settingsOpen) {
      beep(2400, 30);
      applySetting(settingsSel);
    } else if (menuOpen) {
      beep(2400, 30);
      menuConfirm();
    } else if (displayMode == DISP_INFO) {
      beep(2400, 30);
      infoPage = (infoPage + 1) % INFO_PAGES;
    } else if (displayMode == DISP_PET) {
      beep(2400, 30);
      petPage = (petPage + 1) % PET_PAGES;
      applyDisplayMode();
    } else {
      beep(2400, 30);
      msgScroll = (msgScroll >= 30) ? 0 : msgScroll + 1;
    }
  }

  // ─── Home screen + IMU rotation ───
  // Orientation tracking now runs whenever the home screen is visible
  // (not just when the charging clock is showing). That gives us landscape
  // mode on the regular home screen too — buddy on the left, transcript
  // on the right (see drawHomeLandscape). Menus, settings, info pages and
  // approval prompts force homeVisible=false so they stay portrait.
  clockRefreshRtc();
  bool homeVisible = displayMode == DISP_NORMAL
                  && !menuOpen && !settingsOpen && !resetOpen && !inPrompt;
  bool clocking = homeVisible
               && tama.sessionsRunning == 0 && tama.sessionsWaiting == 0
               && dataRtcValid() && _onUsb;
  if (homeVisible) clockUpdateOrient();
  else { clockOrient = 0; orientFrames = 0; paintedOrient = 0; }
  bool landscape      = homeVisible && clockOrient != 0;
  bool landscapeClock = clocking    && clockOrient != 0;
  bool landscapeHome  = landscape   && !clocking;

  static bool wasClocking  = false;
  static bool wasLandscape = false;
  if (clocking != wasClocking || landscape != wasLandscape) {
    if (landscape) {
      // Switching INTO landscape — clear the rotated screen once and
      // force the per-mode draw functions to repaint from scratch.
      M5.Display.setRotation(clockOrient);
      M5.Display.fillScreen(palette().bg);
      M5.Display.setRotation(0);
      paintedOrient = 0;
    } else if (clocking) {
      // Portrait clock: shrink the buddy and let the clock face take
      // the lower half of the sprite.
      buddySetPeek(true);
    } else {
      // Back to plain portrait home — restore full-size buddy + HUD.
      applyDisplayMode();
    }
    buddyInvalidate();
    wasClocking  = clocking;
    wasLandscape = landscape;
  }
  if (clocking) {
    uint8_t dow = _clkTm.tm_wday % 7;
    bool weekend = (dow == 0 || dow == 6);
    bool friday  = (dow == 5);
    uint8_t h = _clkTm.tm_hour;
    if (h >= 1 && h < 7)             activeState = P_SLEEP;
    else if (weekend)                activeState = (now/8000 % 6 == 0) ? P_HEART : P_SLEEP;
    else if (h < 9)                  activeState = (now/6000 % 4 == 0) ? P_IDLE  : P_SLEEP;
    else if (h == 12)                activeState = (now/5000 % 3 == 0) ? P_HEART : P_IDLE;
    else if (friday && h >= 15)      activeState = (now/4000 % 3 == 0) ? P_CELEBRATE : P_IDLE;
    else if (h >= 22 || h == 0)      activeState = (now/7000 % 3 == 0) ? P_DIZZY : P_SLEEP;
    else                             activeState = (now/10000 % 5 == 0) ? P_SLEEP : P_IDLE;
  }

  // Passkey display arrival chirp.
  static uint32_t lastPasskey = 0;
  uint32_t pk = blePasskey();
  if (pk && !lastPasskey) { wake(); beep(1800, 60); }
  lastPasskey = pk;

  // ─── Render ───
  // Landscape mode (clock OR plain home) paints direct-to-LCD in the
  // rotated coordinate space; skip the portrait sprite tick to avoid
  // wasted work and torn frames.
  if (napping || screenOff || landscape) {
    // skip sprite render
  } else {
    buddyTick(activeState);
  }
  if (landscapeClock) {
    drawClock();
  } else if (landscapeHome) {
    drawHomeLandscape();
  } else if (!napping && !screenOff) {
    if (blePasskey()) drawPasskey();
    else if (clocking) drawClock();
    else if (displayMode == DISP_INFO) drawInfo();
    else if (displayMode == DISP_PET) drawPet();
    else if (settings().hud) drawHUD();
    if (resetOpen) drawReset();
    else if (settingsOpen) drawSettings();
    else if (menuOpen) drawMenu();
    spr.pushSprite(0, 0);
  }

  // Face-down nap: dim immediately, pause animations, accumulate sleep.
  static int8_t faceDownFrames = 0;
  if (!inPrompt) {
    bool down = isFaceDown();
    if (down) { if (faceDownFrames < 20) faceDownFrames++; }
    else      { if (faceDownFrames > -10) faceDownFrames--; }
  }
  if (!napping && faceDownFrames >= 15) {
    napping = true;
    napStartMs = now;
    Platform::setBrightness(4);
    dimmed = true;
  } else if (napping && faceDownFrames <= -8) {
    napping = false;
    statsOnNapEnd((now - napStartMs) / 1000);
    statsOnWake();
    wake();
  }

  // Auto screen-off after 30s idle (only on battery — keep on while charging).
  if (!screenOff && !inPrompt && !_onUsb
      && millis() - lastInteractMs > SCREEN_OFF_MS) {
    Platform::screenOff();
    screenOff = true;
  }

  delay(screenOff ? 100 : 16);
}
