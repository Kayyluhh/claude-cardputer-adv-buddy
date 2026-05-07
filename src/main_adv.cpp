// Phase 2 bring-up: M5Cardputer init + Platform shim exercised end-to-end.
// Display draws a status panel that updates each second showing every shim
// reading (battery, USB, IMU axis, RTC, free heap). Keyboard input still
// echoes to USB-CDC. No BLE, no buddy, no stats yet.
#include <M5Cardputer.h>
#include "platform.h"
#include <time.h>

static uint32_t lastDraw = 0;
static uint8_t  brightPct = 70;

static void drawStatus() {
  auto& d = M5Cardputer.Display;
  d.fillScreen(TFT_BLACK);
  d.setTextColor(TFT_WHITE, TFT_BLACK);
  d.setTextDatum(top_left);
  d.setFont(&fonts::Font0);
  d.setTextSize(2);
  d.setCursor(4, 2);
  d.print("Claude buddy");
  d.setTextSize(1);
  d.setCursor(4, 22);
  d.printf("Cardputer-Adv  brt:%u%%", brightPct);

  float ax = 0, ay = 0, az = 0;
  Platform::imuAccel(ax, ay, az);
  d.setCursor(4, 36);
  d.printf("imu  x=%+.2f y=%+.2f z=%+.2f", ax, ay, az);

  d.setCursor(4, 50);
  d.printf("batt %u%%  %umV  usb=%d",
           Platform::batteryPct(), Platform::batteryMv(), (int)Platform::isOnUsb());

  struct tm rt{};
  Platform::getRtcLocal(rt);
  d.setCursor(4, 64);
  d.printf("rtc  %04d-%02d-%02d %02d:%02d:%02d",
           rt.tm_year + 1900, rt.tm_mon + 1, rt.tm_mday,
           rt.tm_hour, rt.tm_min, rt.tm_sec);

  d.setCursor(4, 78);
  d.printf("heap %u  scr=%d", (unsigned)ESP.getFreeHeap(), (int)Platform::isScreenOn());

  d.setCursor(4, 96);
  d.print("[ keys: brighter , dimmer .");
  d.setCursor(4, 108);
  d.print("  beep b   led l   off o    ]");
}

void setup() {
  auto cfg = M5.config();
  M5Cardputer.begin(cfg, true);
  M5Cardputer.Display.setRotation(1);   // 240x135 native landscape
  Platform::init();
  Platform::setBrightness(brightPct);

  // Seed RTC to a sentinel so getRtcLocal() returns something readable
  // before the BLE bridge has had a chance to sync time.
  struct tm seed{};
  seed.tm_year = 2026 - 1900;
  seed.tm_mon = 0; seed.tm_mday = 1;
  seed.tm_hour = 0; seed.tm_min = 0; seed.tm_sec = 0;
  Platform::setRtcLocal(seed);

  Serial.begin(115200);
  Serial.println("[cardputer-adv] phase2 platform shim");
  drawStatus();
}

void loop() {
  M5Cardputer.update();

  if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
    auto state = M5Cardputer.Keyboard.keysState();
    for (auto c : state.word) {
      Serial.write(c);
      if (c == ',') { brightPct = brightPct >= 10 ? brightPct - 10 : 0; Platform::setBrightness(brightPct); }
      else if (c == '.') { brightPct = brightPct <= 90 ? brightPct + 10 : 100; Platform::setBrightness(brightPct); }
      else if (c == 'b') { Platform::tone(880, 120); }
      else if (c == 'l') { static bool led = false; led = !led; Platform::setLed(led ? 200 : 0); }
      else if (c == 'o') { if (Platform::isScreenOn()) Platform::screenOff(); else Platform::screenOn(); }
    }
    if (state.enter) Serial.println();
  }

  uint32_t now = millis();
  if (now - lastDraw > 1000) { drawStatus(); lastDraw = now; }
  Platform::tickAudio();
  delay(10);
}
