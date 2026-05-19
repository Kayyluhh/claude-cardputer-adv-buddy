// Phase 4: full landscape port for Cardputer-Adv. State machine, persona
// derivation, prompt handling, menus/settings/info/pet/approval screens,
// BLE bridge, face-down nap. GIF character pipeline still stubbed —
// Phase 6 swaps character_stub.cpp for the real character.cpp.
#include <M5Cardputer.h>
#include <SD.h>
#include <stdarg.h>
#include "gfx_compat.h"
#include "platform.h"
#include "input.h"
#include "ble_bridge.h"
#include "data.h"
#include "buddy.h"
#include "character.h"
#include "stats.h"

TFT_eSprite spr = TFT_eSprite(&M5Cardputer.Display);

// Advertise as "Claude-XXXX" (last two BT MAC bytes) so multiple
// Cardputers in one room are distinguishable.
static char btName[16] = "Claude";
static void startBt() {
  uint8_t mac[6] = {0};
  esp_read_mac(mac, ESP_MAC_BT);
  snprintf(btName, sizeof(btName), "Claude-%02X%02X", mac[4], mac[5]);
  bleInit(btName);
}

// Landscape geometry. Right panel column starts at PANEL_X (transcript / status).
const int W = 240, H = 135;
const int PANEL_X = 124, PANEL_W = W - PANEL_X;     // right column 116px wide

const uint16_t HOT   = 0xFA20;       // red-orange: warnings, deny, impatience
const uint16_t PANEL = 0x2104;       // overlay panel background
// GREEN (0x07E0) and RED (0xF800) come from M5GFX's `using namespace
// m5gfx::ili9341_colors;` at file scope in M5GFX.h. Defining locals with
// the same names would shadow-conflict (ambiguous lookup) on newer M5GFX.

enum PersonaState { P_SLEEP, P_IDLE, P_BUSY, P_ATTENTION, P_CELEBRATE, P_DIZZY, P_HEART };
const char* stateNames[] = { "sleep", "idle", "busy", "attention", "celebrate", "dizzy", "heart" };

TamaState    tama;
PersonaState baseState   = P_SLEEP;
PersonaState activeState = P_SLEEP;
uint32_t     oneShotUntil = 0;
uint32_t     lastShakeCheck = 0;
float        accelBaseline = 1.0f;

bool    menuOpen    = false;
uint8_t menuSel     = 0;
uint8_t brightLevel = 4;
bool    enterLong   = false;

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
bool     buddyMode = true;          // ASCII vs GIF; flipped by setup() after scan
bool     gifAvailable = false;
const uint8_t SPECIES_GIF = 0xFF;   // species-NVS sentinel: render the GIF

// Cycle pet: GIF (if installed) -> species 0..N-1 -> back to GIF.
static void nextPet() {
  uint8_t n = buddySpeciesCount();
  if (!buddyMode) {                                            // GIF -> species 0
    buddyMode = true;
    buddySetSpeciesIdx(0);
    speciesIdxSave(0);
  } else if (buddySpeciesIdx() + 1 >= n && gifAvailable) {     // last species -> GIF
    buddyMode = false;
    speciesIdxSave(SPECIES_GIF);
  } else {                                                     // species i -> i+1
    buddyNextSpecies();
  }
  characterInvalidate();
  if (buddyMode) buddyInvalidate();
}

uint32_t wakeTransitionUntil = 0;
const uint32_t SCREEN_OFF_MS = 30000;

bool     napping = false;
uint32_t napStartMs = 0;
uint32_t promptArrivedMs = 0;

static bool isFaceDown() {
  float ax, ay, az;
  Platform::imuAccel(ax, ay, az);
  return az < -0.7f && fabsf(ax) < 0.4f && fabsf(ay) < 0.4f;
}

static void applyBrightness() { Platform::setBrightness(20 + brightLevel * 20); }

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

bool responseSent = false;

static void beep(uint16_t freq, uint16_t dur) {
  if (settings().volumeLevel > 0) Platform::tone(freq, dur);
}

// Mario 1-UP — E5 G5 E6 C6 D6 G6, ~35ms/note (90ms tail). Loud enough
// across-the-room "come look at me" cue when a permission prompt lands
// or a pairing passkey appears. (Cherry-picked from y88huang.)
static const uint16_t SFX_ALERT_F[] = { 659, 784, 1319, 1047, 1175, 1568 };
static const uint16_t SFX_ALERT_D[] = {  35,  35,   35,   35,   35,   90 };
static void sfxAlert() {
  if (settings().volumeLevel > 0) Platform::beepSeq(SFX_ALERT_F, SFX_ALERT_D,
                                                     sizeof(SFX_ALERT_F) / sizeof(SFX_ALERT_F[0]));
}

static void sendCmd(const char* json) {
  Serial.println(json);
  size_t n = strlen(json);
  bleWrite((const uint8_t*)json, n);
  bleWrite((const uint8_t*)"\n", 1);
}

const uint8_t INFO_PAGES = 6;
const uint8_t INFO_PG_BUTTONS = 1;
const uint8_t INFO_PG_CREDITS = 5;

void applyDisplayMode() {
  bool peek = displayMode != DISP_NORMAL;
  characterSetPeek(peek);
  buddySetPeek(peek);
  spr.fillSprite(0x0000);
  characterInvalidate();
}

const char* menuItems[] = { "settings", "help", "about", "demo", "close" };
const uint8_t MENU_N = 5;

bool    settingsOpen = false;
uint8_t settingsSel  = 0;
const char* settingsItems[] = {
  "brightness", "sound", "bluetooth", "wifi", "led", "transcript",
  "ascii pet", "reset", "back"
};
const uint8_t SETTINGS_N = 9;

bool    resetOpen = false;
uint8_t resetSel  = 0;
const char* resetItems[] = { "delete char", "factory reset", "back" };
const uint8_t RESET_N = 3;
static uint32_t resetConfirmUntil = 0;
static uint8_t  resetConfirmIdx = 0xFF;

static void applySetting(uint8_t idx) {
  Settings& s = settings();
  switch (idx) {
    case 0: brightLevel = (brightLevel + 1) % 5; applyBrightness(); return;
    // Cardputer-Adv: binary toggle. 0 = mute, 4 = max. StickS3's main_stick.cpp
    // will replace this with a full 0..4 cycler + hold-B-to-mute in-row.
    case 1: s.volumeLevel = (s.volumeLevel == 0) ? 4 : 0; break;
    case 2: s.bt = !s.bt; break;
    case 3: s.wifi = !s.wifi; break;
    case 4: s.led = !s.led; break;
    case 5: s.hud = !s.hud; break;
    case 6: nextPet(); return;
    case 7: resetOpen = true; resetSel = 0; resetConfirmIdx = 0xFF; return;
    case 8: settingsOpen = false; characterInvalidate(); return;
  }
  settingsSave();
}

static void applyReset(uint8_t idx) {
  uint32_t now = millis();
  bool armed = (resetConfirmIdx == idx) && (int32_t)(now - resetConfirmUntil) < 0;

  if (idx == 2) { resetOpen = false; return; }   // back

  if (!armed) {
    resetConfirmIdx = idx;
    resetConfirmUntil = now + 3000;
    beep(1400, 60);
    return;
  }

  beep(800, 200);
  // Both reset paths walk /characters/ on the SD card. We don't `SD.format()`
  // on factory reset — the card may also hold the user's M5Launcher app cache
  // and other personal files, and wiping it is a destructive surprise. Our
  // domain on the card is /characters/ only; anything outside is the user's.
  if (Platform::sdAvailable()) {
    File d = SD.open("/characters");
    if (d && d.isDirectory()) {
      File e;
      while ((e = d.openNextFile())) {
        char path[80];
        snprintf(path, sizeof(path), "/characters/%s", e.name());
        if (e.isDirectory()) {
          File f;
          while ((f = e.openNextFile())) {
            char fp[128];
            snprintf(fp, sizeof(fp), "%s/%s", path, f.name());
            f.close();
            SD.remove(fp);
          }
          e.close();
          SD.rmdir(path);
        } else {
          e.close();
          SD.remove(path);
        }
      }
      d.close();
    }
  }
  if (idx != 0) {
    // factory reset: also wipe NVS namespace and BLE bonds
    _prefs.begin("buddy", false);
    _prefs.clear();
    _prefs.end();
    bleClearBonds();
  }
  delay(300);
  ESP.restart();
}

const int MENU_HINT_H = 12;
static void drawMenuHints(const Palette& p, int mx, int mw, int hy,
                          const char* downLbl = "Tab", const char* rightLbl = "Ent") {
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
  const Palette& p = characterPalette();
  int mw = 200, mh = 8 + SETTINGS_N * 11 + MENU_HINT_H;
  int mx = (W - mw) / 2, my = (H - mh) / 2;
  spr.fillRoundRect(mx, my, mw, mh, 4, PANEL);
  spr.drawRoundRect(mx, my, mw, mh, 4, p.textDim);
  spr.setTextSize(1);
  Settings& s = settings();
  bool vals[] = { s.volumeLevel > 0, s.bt, s.wifi, s.led, s.hud };
  for (int i = 0; i < SETTINGS_N; i++) {
    bool sel = (i == settingsSel);
    spr.setTextColor(sel ? p.text : p.textDim, PANEL);
    spr.setCursor(mx + 6, my + 4 + i * 11);
    spr.print(sel ? "> " : "  ");
    spr.print(settingsItems[i]);
    spr.setCursor(mx + mw - 50, my + 4 + i * 11);
    spr.setTextColor(p.textDim, PANEL);
    if (i == 0) {
      spr.printf("%u/4", brightLevel);
    } else if (i >= 1 && i <= 5) {
      spr.setTextColor(vals[i-1] ? GREEN : p.textDim, PANEL);
      spr.print(vals[i-1] ? " on" : "off");
    } else if (i == 6) {
      spr.printf("%u/%u", buddySpeciesIdx() + 1, buddySpeciesCount());
    }
  }
  drawMenuHints(p, mx, mw, my + mh - 11);
}

static void drawReset() {
  const Palette& p = characterPalette();
  int mw = 180, mh = 10 + RESET_N * 12 + MENU_HINT_H;
  int mx = (W - mw) / 2, my = (H - mh) / 2;
  spr.fillRoundRect(mx, my, mw, mh, 4, PANEL);
  spr.drawRoundRect(mx, my, mw, mh, 4, HOT);
  spr.setTextSize(1);
  for (int i = 0; i < RESET_N; i++) {
    bool sel = (i == resetSel);
    spr.setTextColor(sel ? p.text : p.textDim, PANEL);
    spr.setCursor(mx + 6, my + 6 + i * 12);
    spr.print(sel ? "> " : "  ");
    bool armed = (i == resetConfirmIdx) &&
                 (int32_t)(millis() - resetConfirmUntil) < 0;
    if (armed) spr.setTextColor(HOT, PANEL);
    spr.print(armed ? "really?" : resetItems[i]);
  }
  drawMenuHints(p, mx, mw, my + mh - 11);
}

void menuConfirm() {
  switch (menuSel) {
    case 0: settingsOpen = true; menuOpen = false; settingsSel = 0; break;
    case 1:
    case 2:
      menuOpen = false;
      displayMode = DISP_INFO;
      infoPage = (menuSel == 1) ? INFO_PG_BUTTONS : INFO_PG_CREDITS;
      applyDisplayMode();
      characterInvalidate();
      break;
    case 3: dataSetDemo(!dataDemo()); break;
    case 4: menuOpen = false; characterInvalidate(); break;
  }
}

void drawMenu() {
  const Palette& p = characterPalette();
  int mw = 160, mh = 10 + MENU_N * 12 + MENU_HINT_H;
  int mx = (W - mw) / 2, my = (H - mh) / 2;
  spr.fillRoundRect(mx, my, mw, mh, 4, PANEL);
  spr.drawRoundRect(mx, my, mw, mh, 4, p.textDim);
  spr.setTextSize(1);
  for (int i = 0; i < MENU_N; i++) {
    bool sel = (i == menuSel);
    spr.setTextColor(sel ? p.text : p.textDim, PANEL);
    spr.setCursor(mx + 6, my + 6 + i * 12);
    spr.print(sel ? "> " : "  ");
    spr.print(menuItems[i]);
    if (i == 3) spr.print(dataDemo() ? "  on" : "  off");
  }
  drawMenuHints(p, mx, mw, my + mh - 11);
}

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
  Platform::imuAccel(ax, ay, az);
  float mag = sqrtf(ax*ax + ay*ay + az*az);
  float delta = fabsf(mag - accelBaseline);
  accelBaseline = accelBaseline * 0.95f + mag * 0.05f;
  return delta > 0.8f;
}

// 1Hz cached RTC read so the IMU reads aren't starved by the I2C bus.
// data.h's time-sync handler writes _clkLastRead = 0 to force re-read.
static struct tm _clkTm;
uint32_t         _clkLastRead = 0;     // referenced as extern from data.h
static bool      _onUsb       = false;
static void clockRefreshRtc() {
  if (millis() - _clkLastRead < 1000) return;
  _clkLastRead = millis();
  _onUsb = Platform::isOnUsb();
  Platform::getRtcLocal(_clkTm);
}

static void _infoHeader(const Palette& p, int& y, const char* section, uint8_t page) {
  spr.setTextColor(p.text, p.bg);
  spr.setCursor(4, y); spr.print("Info");
  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(W - 28, y); spr.printf("%u/%u", page + 1, INFO_PAGES);
  y += 10;
  spr.setTextColor(p.body, p.bg);
  spr.setCursor(4, y); spr.print(section);
  y += 11;
}

void drawPasskey() {
  const Palette& p = characterPalette();
  spr.fillSprite(p.bg);
  spr.setTextSize(1);
  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(8, 14);  spr.print("BLUETOOTH PAIRING");
  spr.setCursor(8, 110); spr.print("enter on desktop:");
  spr.setTextSize(3);
  spr.setTextColor(p.text, p.bg);
  char b[8]; snprintf(b, sizeof(b), "%06lu", (unsigned long)blePasskey());
  spr.setCursor((W - 18 * 6) / 2, 50);
  spr.print(b);
}

void drawInfo() {
  const Palette& p = characterPalette();
  spr.fillSprite(p.bg);
  spr.setTextSize(1);
  int y = 4;
  auto ln = [&](const char* fmt, ...) {
    char b[40]; va_list a; va_start(a, fmt); vsnprintf(b, sizeof(b), fmt, a); va_end(a);
    spr.setCursor(4, y); spr.print(b); y += 9;
  };

  if (infoPage == 0) {
    _infoHeader(p, y, "ABOUT", infoPage);
    spr.setTextColor(p.textDim, p.bg);
    ln("I watch your Claude");
    ln("desktop sessions:");
    ln("sleep, wake, fret when");
    ln("approvals pile up.");
    y += 4;
    spr.setTextColor(p.text, p.bg);
    ln("Enter approves prompts.");
    ln("Esc denies them.");
    spr.setTextColor(p.textDim, p.bg);
    ln("18 species via settings.");

  } else if (infoPage == 1) {
    _infoHeader(p, y, "KEYS", infoPage);
    spr.setTextColor(p.text, p.bg);    ln("Enter   approve / sel");
    spr.setTextColor(p.textDim, p.bg); ln("Esc     deny / back");
    spr.setTextColor(p.text, p.bg);    ln("Tab     next item / page");
    spr.setTextColor(p.textDim, p.bg); ln("`       cycle screen");
    spr.setTextColor(p.text, p.bg);    ln("hold Ent  open menu");
    spr.setTextColor(p.textDim, p.bg); ln("a/d     approve/deny");
    spr.setTextColor(p.textDim, p.bg); ln(", .     scroll msgs");

  } else if (infoPage == 2) {
    _infoHeader(p, y, "CLAUDE", infoPage);
    spr.setTextColor(p.textDim, p.bg);
    ln("  sessions  %u", tama.sessionsTotal);
    ln("  running   %u", tama.sessionsRunning);
    ln("  waiting   %u", tama.sessionsWaiting);
    y += 3;
    spr.setTextColor(p.text, p.bg); ln("LINK");
    spr.setTextColor(p.textDim, p.bg);
    ln("  via       %s", dataScenarioName());
    ln("  ble       %s", !bleConnected() ? "-" : bleSecure() ? "encrypted" : "OPEN");
    ln("  state     %s", stateNames[activeState]);

  } else if (infoPage == 3) {
    _infoHeader(p, y, "DEVICE", infoPage);
    int pct = Platform::batteryPct();
    int mv  = Platform::batteryMv();
    bool usb = Platform::isOnUsb();
    spr.setTextColor(p.text, p.bg);
    spr.setTextSize(2);
    spr.setCursor(4, y);
    spr.printf("%d%%", pct);
    spr.setTextSize(1);
    spr.setTextColor(usb ? GREEN : p.textDim, p.bg);
    spr.setCursor(60, y + 4);
    spr.print(usb ? "usb" : "battery");
    y += 18;
    spr.setTextColor(p.textDim, p.bg);
    ln("  batt   %d.%02dV", mv/1000, (mv%1000)/10);
    ln("  uptime %luh%02lum", (millis()/1000)/3600, ((millis()/1000)/60)%60);
    ln("  heap   %uKB", ESP.getFreeHeap() / 1024);
    ln("  bright %u/4", brightLevel);
    if (ownerName()[0]) ln("  owner  %s", ownerName());

  } else if (infoPage == 4) {
    _infoHeader(p, y, "BLUETOOTH", infoPage);
    bool linked = settings().bt && dataBtActive();
    spr.setTextColor(linked ? GREEN : (settings().bt ? HOT : p.textDim), p.bg);
    spr.setTextSize(2);
    spr.setCursor(4, y);
    spr.print(linked ? "linked" : (settings().bt ? "discover" : "off"));
    spr.setTextSize(1);
    y += 18;
    spr.setTextColor(p.text, p.bg); ln("  %s", btName);
    spr.setTextColor(p.textDim, p.bg);
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_BT);
    ln("  %02X:%02X:%02X:%02X:%02X:%02X",
       mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
    if (settings().bt && !linked) {
      spr.setTextColor(p.text, p.bg); ln("To pair:");
      spr.setTextColor(p.textDim, p.bg);
      ln(" Claude desktop");
      ln(" > Developer");
      ln(" > Hardware Buddy");
    }

  } else {
    _infoHeader(p, y, "CREDITS", infoPage);
    spr.setTextColor(p.textDim, p.bg); ln("made by");
    spr.setTextColor(p.text, p.bg);    ln("Felix Rieseberg");
    y += 3;
    spr.setTextColor(p.textDim, p.bg); ln("source");
    spr.setTextColor(p.text, p.bg);    ln("github.com/anthropics");
    spr.setTextColor(p.text, p.bg);    ln("/claude-desktop-buddy");
    y += 3;
    spr.setTextColor(p.textDim, p.bg); ln("hardware");
    ln("M5Stack Cardputer-Adv");
  }
}

// Greedy word-wrap with continuation-row indent.
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
  const Palette& p = characterPalette();
  spr.fillSprite(p.bg);
  spr.drawFastHLine(0, 14, W, p.textDim);

  spr.setTextSize(1);
  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(4, 4);
  uint32_t waited = (millis() - promptArrivedMs) / 1000;
  if (waited >= 10) spr.setTextColor(HOT, p.bg);
  spr.printf("APPROVE? %lus", (unsigned long)waited);

  int toolLen = strlen(tama.promptTool);
  spr.setTextColor(p.text, p.bg);
  spr.setTextSize(toolLen <= 18 ? 2 : 1);
  spr.setCursor(4, 22);
  spr.print(tama.promptTool);
  spr.setTextSize(1);

  spr.setTextColor(p.textDim, p.bg);
  int hintY = (toolLen <= 18) ? 46 : 38;
  spr.setCursor(4, hintY);
  spr.printf("%.38s", tama.promptHint);
  if (strlen(tama.promptHint) > 38) {
    spr.setCursor(4, hintY + 10);
    spr.printf("%.38s", tama.promptHint + 38);
  }

  if (responseSent) {
    spr.setTextColor(p.textDim, p.bg);
    spr.setCursor(4, H - 12);
    spr.print("sent...");
  } else {
    spr.setTextColor(GREEN, p.bg);
    spr.setCursor(4, H - 12);
    spr.print("Enter: approve");
    spr.setTextColor(HOT, p.bg);
    spr.setCursor(W - 64, H - 12);
    spr.print("Esc: deny");
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
  spr.fillSprite(p.bg);
  spr.setTextSize(1);
  int y = 18;

  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(6, y - 2); spr.print("mood");
  uint8_t mood = statsMoodTier();
  uint16_t moodCol = (mood >= 3) ? RED : (mood >= 2) ? HOT : p.textDim;
  for (int i = 0; i < 4; i++) tinyHeart(54 + i * 16, y + 2, i < mood, moodCol);

  y += 16;
  spr.setCursor(6, y - 2); spr.print("fed");
  uint8_t fed = statsFedProgress();
  for (int i = 0; i < 10; i++) {
    int px = 38 + i * 9;
    if (i < fed) spr.fillCircle(px, y + 1, 2, p.body);
    else spr.drawCircle(px, y + 1, 2, p.textDim);
  }

  y += 16;
  spr.setCursor(6, y - 2); spr.print("energy");
  uint8_t en = statsEnergyTier();
  uint16_t enCol = (en >= 4) ? 0x07FF : (en >= 2) ? 0xFFE0 : HOT;
  for (int i = 0; i < 5; i++) {
    int px = 54 + i * 13;
    if (i < en) spr.fillRect(px, y - 2, 9, 6, enCol);
    else spr.drawRect(px, y - 2, 9, 6, p.textDim);
  }

  y += 18;
  spr.fillRoundRect(6, y - 2, 42, 12, 3, p.body);
  spr.setTextColor(p.bg, p.body);
  spr.setCursor(11, y); spr.printf("Lv %u", stats().level);

  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(60, y);
  spr.printf("appr %u", stats().approvals);
  spr.setCursor(60, y + 9);
  spr.printf("deny %u", stats().denials);
}

static void drawPetHowTo(const Palette& p) {
  spr.fillSprite(p.bg);
  spr.setTextSize(1);
  int y = 18;
  auto ln = [&](uint16_t c, const char* s) {
    spr.setTextColor(c, p.bg); spr.setCursor(6, y); spr.print(s); y += 9;
  };

  ln(p.body,    "MOOD");
  ln(p.textDim, " approve fast = up");
  ln(p.textDim, " deny lots = down");
  y += 3;
  ln(p.body,    "FED");
  ln(p.textDim, " 50K tokens = level");
  y += 3;
  ln(p.body,    "ENERGY");
  ln(p.textDim, " face-down to nap");
}

void drawPet() {
  const Palette& p = characterPalette();
  if (petPage == 0) drawPetStats(p);
  else              drawPetHowTo(p);

  spr.setTextSize(1);
  spr.setTextColor(p.text, p.bg);
  spr.setCursor(4, 4);
  if (ownerName()[0]) spr.printf("%s's %s", ownerName(), petName());
  else                spr.print(petName());
  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(W - 28, 4);
  spr.printf("%u/%u", petPage + 1, PET_PAGES);
}

// Right-column transcript: x=124..240, y=2..132. Approval steals full screen.
void drawHUD() {
  if (tama.promptId[0]) { drawApproval(); return; }
  const Palette& p = characterPalette();
  const int LH = 9, WIDTH = 18;
  const int TY = 16;
  const int VIS = (H - TY - 4) / LH;

  spr.fillRect(PANEL_X, 0, PANEL_W, H, p.bg);

  spr.setTextColor(p.text, p.bg);
  spr.setCursor(PANEL_X + 2, 2);
  if (tama.connected) spr.printf("%u running", tama.sessionsRunning);
  else                spr.print("no Claude");
  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(PANEL_X + PANEL_W - 30, 2);
  spr.printf("%d%%", Platform::batteryPct());
  spr.drawFastHLine(PANEL_X, 12, PANEL_W, p.textDim);

  if (tama.lineGen != lastLineGen) { msgScroll = 0; lastLineGen = tama.lineGen; wake(); }

  if (tama.nLines == 0) {
    spr.setTextColor(p.text, p.bg);
    spr.setCursor(PANEL_X + 2, TY);
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

  uint8_t maxBack = (nDisp > VIS) ? (nDisp - VIS) : 0;
  if (msgScroll > maxBack) msgScroll = maxBack;

  int end = (int)nDisp - msgScroll;
  int start = end - VIS; if (start < 0) start = 0;
  uint8_t newest = tama.nLines - 1;
  for (int i = 0; start + i < end; i++) {
    uint8_t row = start + i;
    bool fresh = (srcOf[row] == newest) && (msgScroll == 0);
    spr.setTextColor(fresh ? p.text : p.textDim, p.bg);
    spr.setCursor(PANEL_X + 2, TY + i * LH);
    spr.print(disp[row]);
  }
  if (msgScroll > 0) {
    spr.setTextColor(p.body, p.bg);
    spr.setCursor(PANEL_X + PANEL_W - 18, H - 10);
    spr.printf("-%u", msgScroll);
  }
}

void setup() {
  auto cfg = M5.config();
  M5Cardputer.begin(cfg, true);
  M5Cardputer.Display.setRotation(1);
  Platform::init();
  Input::init();
  applyBrightness();
  startBt();
  lastInteractMs = millis();
  statsLoad();
  settingsLoad();
  petNameLoad();
  buddyInit();

  spr.setColorDepth(16);
  spr.createSprite(W, H);
  spr.setFont(&fonts::Font0);
  characterInit(nullptr);
  gifAvailable = characterLoaded();
  buddyMode = !(gifAvailable && speciesIdxLoad() == SPECIES_GIF);
  applyDisplayMode();

  {
    const Palette& p = characterPalette();
    spr.fillSprite(p.bg);
    spr.setTextDatum(MC_DATUM);
    spr.setTextSize(2);
    if (ownerName()[0]) {
      char line[40];
      snprintf(line, sizeof(line), "%s's", ownerName());
      spr.setTextColor(p.text, p.bg);   spr.drawString(line, W/2, H/2 - 12);
      spr.setTextColor(p.body, p.bg);   spr.drawString(petName(), W/2, H/2 + 12);
    } else {
      spr.setTextColor(p.body, p.bg);   spr.drawString("Hello!", W/2, H/2 - 12);
      spr.setTextSize(1);
      spr.setTextColor(p.textDim, p.bg);
      spr.drawString("a buddy appears", W/2, H/2 + 12);
    }
    spr.setTextDatum(TL_DATUM); spr.setTextSize(1);
    spr.pushSprite(0, 0);
    delay(1500);
  }

  Serial.begin(115200);
  Serial.printf("buddy: %s\n", buddyMode ? "ASCII mode" : "GIF character loaded");
}

void loop() {
  M5Cardputer.update();
  Input::poll();
  Platform::tickAudio();
  uint32_t now = millis();

  dataPoll(&tama);
  if (statsPollLevelUp()) triggerOneShot(P_CELEBRATE, 3000);
  baseState = derive(tama);

  if (baseState == P_IDLE && (int32_t)(now - wakeTransitionUntil) < 0) baseState = P_SLEEP;
  if ((int32_t)(now - oneShotUntil) >= 0) activeState = baseState;

  if (activeState == P_ATTENTION && settings().led) {
    Platform::setLed((now / 400) % 2 ? 200 : 0);
  } else {
    Platform::setLed(0);
  }

  if (now - lastShakeCheck > 50) {
    lastShakeCheck = now;
    if (!menuOpen && !screenOff && checkShake() && (int32_t)(now - oneShotUntil) >= 0) {
      wake();
      triggerOneShot(P_DIZZY, 2000);
    }
  }

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
      characterInvalidate();
      if (buddyMode) buddyInvalidate();
    }
  }

  bool inPrompt = tama.promptId[0] && !responseSent;

  // Wake on any keystroke
  if (Input::isHeldEnter() || Input::isHeldDel()) wake();
  // popChar() is destructive — only drain it when text entry is wanted;
  // for the wake heuristic just check pressed-edge below.

  // Long-press Enter → menu (toggle)
  if (Input::wasLongPressedEnter() && !screenOff) {
    enterLong = true;
    beep(800, 60);
    if (resetOpen) resetOpen = false;
    else if (settingsOpen) { settingsOpen = false; characterInvalidate(); }
    else { menuOpen = !menuOpen; menuSel = 0; if (!menuOpen) characterInvalidate(); }
  }

  // Enter (short press): primary action — but only if a long-press hasn't
  // already fired this hold.
  if (Input::wasPressedEnter() && !enterLong) {
    if (inPrompt) {
      char cmd[96];
      snprintf(cmd, sizeof(cmd), "{\"cmd\":\"permission\",\"id\":\"%s\",\"decision\":\"once\"}", tama.promptId);
      sendCmd(cmd);
      responseSent = true;
      uint32_t tookS = (millis() - promptArrivedMs) / 1000;
      statsOnApproval(tookS);
      beep(2400, 60);
      if (tookS < 5) triggerOneShot(P_HEART, 2000);
    } else if (resetOpen)         { beep(2400, 30); applyReset(resetSel); }
    else if (settingsOpen)         { beep(2400, 30); applySetting(settingsSel); }
    else if (menuOpen)             { beep(2400, 30); menuConfirm(); }
    else if (displayMode == DISP_INFO) { beep(2400, 30); infoPage = (infoPage + 1) % INFO_PAGES; }
    else if (displayMode == DISP_PET)  { beep(2400, 30); petPage = (petPage + 1) % PET_PAGES; applyDisplayMode(); }
  }
  // Reset the long-press latch when Enter is fully released
  if (!Input::isHeldEnter()) enterLong = false;

  // Esc / Backspace: deny prompt / close overlay
  if (Input::wasPressedDel()) {
    if (inPrompt) {
      char cmd[96];
      snprintf(cmd, sizeof(cmd), "{\"cmd\":\"permission\",\"id\":\"%s\",\"decision\":\"deny\"}", tama.promptId);
      sendCmd(cmd);
      responseSent = true;
      statsOnDenial();
      beep(600, 60);
    } else if (resetOpen)     { resetOpen = false; }
    else if (settingsOpen)    { settingsOpen = false; characterInvalidate(); }
    else if (menuOpen)        { menuOpen = false; characterInvalidate(); }
    else if (displayMode != DISP_NORMAL) { displayMode = DISP_NORMAL; applyDisplayMode(); }
  }

  // Tab — next item / next page
  if (Input::wasPressedChar('\t') || Input::wasPressedChar(';')) {
    if (resetOpen)         { beep(1800, 30); resetSel = (resetSel + 1) % RESET_N; resetConfirmIdx = 0xFF; }
    else if (settingsOpen) { beep(1800, 30); settingsSel = (settingsSel + 1) % SETTINGS_N; }
    else if (menuOpen)     { beep(1800, 30); menuSel = (menuSel + 1) % MENU_N; }
    else if (displayMode == DISP_INFO) { beep(2400, 30); infoPage = (infoPage + 1) % INFO_PAGES; }
    else if (displayMode == DISP_PET)  { beep(2400, 30); petPage = (petPage + 1) % PET_PAGES; applyDisplayMode(); }
  }

  // Backtick — cycle home/pet/info modes
  if (Input::wasPressedChar('`')) {
    beep(1800, 30);
    displayMode = (displayMode + 1) % DISP_COUNT;
    applyDisplayMode();
  }

  // Letter shortcuts on the approval screen: a = approve, d = deny.
  if (inPrompt) {
    if (Input::wasPressedChar('a')) {
      char cmd[96];
      snprintf(cmd, sizeof(cmd), "{\"cmd\":\"permission\",\"id\":\"%s\",\"decision\":\"once\"}", tama.promptId);
      sendCmd(cmd); responseSent = true;
      uint32_t tookS = (millis() - promptArrivedMs) / 1000;
      statsOnApproval(tookS); beep(2400, 60);
      if (tookS < 5) triggerOneShot(P_HEART, 2000);
    } else if (Input::wasPressedChar('d')) {
      char cmd[96];
      snprintf(cmd, sizeof(cmd), "{\"cmd\":\"permission\",\"id\":\"%s\",\"decision\":\"deny\"}", tama.promptId);
      sendCmd(cmd); responseSent = true;
      statsOnDenial(); beep(600, 60);
    }
  }

  // , and . scroll the transcript on the home screen
  if (!menuOpen && !settingsOpen && !resetOpen && !inPrompt && displayMode == DISP_NORMAL) {
    if (Input::wasPressedChar('.')) msgScroll = (msgScroll >= 30) ? 30 : msgScroll + 1;
    if (Input::wasPressedChar(',')) msgScroll = msgScroll > 0 ? msgScroll - 1 : 0;
  }

  // Pairing-passkey beep on first appearance
  static uint32_t lastPasskey = 0;
  uint32_t pk = blePasskey();
  if (pk && !lastPasskey) { wake(); sfxAlert(); }
  lastPasskey = pk;

  clockRefreshRtc();

  // Render: pet in left column first, then full-screen overlay or
  // right-column HUD on top.
  if (!napping && !screenOff) {
    if (buddyMode) {
      buddyTick(activeState);
    } else if (characterLoaded()) {
      characterSetState(activeState);
      characterTick();
    } else {
      // No buddy and no GIF — show install progress if a char pack is
      // streaming in over BLE, otherwise a quiet "no character loaded".
      const Palette& p = characterPalette();
      spr.fillRect(0, 0, 120, H, p.bg);
      spr.setTextColor(p.textDim, p.bg);
      spr.setTextSize(1);
      if (xferActive()) {
        uint32_t done = xferProgress(), total = xferTotal();
        spr.setCursor(8, H/2 - 20); spr.print("installing");
        spr.setCursor(8, H/2 - 8);  spr.printf("%luK/%luK", done/1024, total/1024);
        int barW = 120 - 16;
        spr.drawRect(8, H/2 + 6, barW, 8, p.textDim);
        if (total > 0) {
          int fill = (int)((uint64_t)barW * done / total);
          if (fill > 1) spr.fillRect(9, H/2 + 7, fill - 1, 6, p.body);
        }
      } else {
        spr.setCursor(8, H/2 - 4);
        spr.print("no character");
      }
    }
  }

  if (!napping && !screenOff) {
    if (blePasskey())                  drawPasskey();
    else if (displayMode == DISP_INFO) drawInfo();
    else if (displayMode == DISP_PET)  drawPet();
    else if (settings().hud)            drawHUD();
    if (resetOpen)         drawReset();
    else if (settingsOpen) drawSettings();
    else if (menuOpen)     drawMenu();
    spr.pushSprite(0, 0);
  }

  // Face-down nap (BMI270 z-axis dominant negative for ~0.75s)
  static int8_t faceDownFrames = 0;
  if (!inPrompt) {
    bool down = isFaceDown();
    if (down) { if (faceDownFrames < 20) faceDownFrames++; }
    else      { if (faceDownFrames > -10) faceDownFrames--; }
  }
  if (!napping && faceDownFrames >= 15) {
    napping = true; napStartMs = now;
    Platform::setBrightness(8);
    dimmed = true;
  } else if (napping && faceDownFrames <= -8) {
    napping = false;
    statsOnNapEnd((now - napStartMs) / 1000);
    statsOnWake();
    wake();
  }

  // Auto screen-off (battery only — clock face / desk presence wants to
  // stay visible while charging on USB).
  if (!screenOff && !inPrompt && !_onUsb
      && millis() - lastInteractMs > SCREEN_OFF_MS) {
    Platform::screenOff();
    screenOff = true;
  }

  delay(screenOff ? 100 : 16);
}
