// Phase 3 bring-up: full sprite + landscape buddy renderer + keyboard
// input layer. Home screen is split into a buddy column (left) and a
// status panel (right). Press . / , to cycle species, Enter to cycle
// persona state, b to beep, l to toggle LED. Long-press Enter (600ms)
// dumps a "menu open" debug line — phase 4 will wire it to a real menu.
#include <M5Cardputer.h>
#include "gfx_compat.h"
#include "platform.h"
#include "input.h"
#include "buddy.h"
#include "buddy_common.h"
#include <time.h>

TFT_eSprite spr = TFT_eSprite(&M5Cardputer.Display);

static const char* STATE_NAMES[] = { "sleep", "idle", "busy", "attn", "celeb", "dizzy", "heart" };
static uint8_t personaIdx = 1;   // start in IDLE so the buddy is visible

static void drawPanel() {
  // Right column: status / debug. Width 120px, x=120..239.
  const int panelX = 120;
  spr.fillRect(panelX, 0, 120, 135, TFT_BLACK);
  spr.setTextDatum(top_left);
  spr.setFont(&fonts::Font0);
  spr.setTextSize(1);
  spr.setTextColor(TFT_WHITE, TFT_BLACK);

  spr.setCursor(panelX + 2, 4);
  spr.print("Cardputer-Adv");
  spr.setCursor(panelX + 2, 16);
  spr.printf("species  %s", buddySpeciesName());
  spr.setCursor(panelX + 2, 28);
  spr.printf("state    %s", STATE_NAMES[personaIdx]);

  spr.setCursor(panelX + 2, 46);
  spr.printf("batt %u%% %dmV", Platform::batteryPct(), (int)Platform::batteryMv());
  spr.setCursor(panelX + 2, 58);
  spr.printf("usb  %s", Platform::isOnUsb() ? "yes" : "no");

  float ax = 0, ay = 0, az = 0;
  Platform::imuAccel(ax, ay, az);
  spr.setCursor(panelX + 2, 76);
  spr.printf("ax %+.1f", ax);
  spr.setCursor(panelX + 2, 88);
  spr.printf("ay %+.1f", ay);
  spr.setCursor(panelX + 2, 100);
  spr.printf("az %+.1f", az);

  spr.setTextColor(0x8410, TFT_BLACK);   // dim grey
  spr.setCursor(panelX + 2, 118);
  spr.print(", . ent b l");
}

void setup() {
  auto cfg = M5.config();
  M5Cardputer.begin(cfg, true);
  M5Cardputer.Display.setRotation(1);
  Platform::init();
  Platform::setBrightness(80);

  spr.setColorDepth(16);
  spr.createSprite(240, 135);
  spr.setFont(&fonts::Font0);
  spr.setTextSize(1);

  buddyInit();
  Input::init();

  // Seed RTC so getRtcLocal() doesn't return uninitialized junk.
  struct tm seed{};
  seed.tm_year = 2026 - 1900;
  Platform::setRtcLocal(seed);

  Serial.begin(115200);
  Serial.println("[cardputer-adv] phase3 buddy + input");
}

void loop() {
  M5Cardputer.update();
  Input::poll();

  if (Input::wasPressedChar('.')) { buddyNextSpecies(); buddyInvalidate(); }
  if (Input::wasPressedChar(',')) {
    // No "prev species" API; advance N-1 times.
    uint8_t n = buddySpeciesCount();
    for (uint8_t i = 0; i < n - 1; i++) buddyNextSpecies();
    buddyInvalidate();
  }
  if (Input::wasPressedEnter()) {
    personaIdx = (personaIdx + 1) % 7;
    buddyInvalidate();
  }
  if (Input::wasPressedChar('b')) Platform::tone(880, 120);
  if (Input::wasPressedChar('l')) {
    static bool led = false; led = !led;
    Platform::setLed(led ? 200 : 0);
  }
  if (Input::wasLongPressedEnter()) Serial.println("[long-enter] menu open (phase 4)");

  buddyTick(personaIdx);
  drawPanel();
  spr.pushSprite(0, 0);

  Platform::tickAudio();
  delay(10);
}
