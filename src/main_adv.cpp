// Phase 1 bring-up: M5Cardputer.begin() runs, display shows boot banner,
// keyboard input echoes to USB-CDC serial. No BLE, no buddy, no stats yet
// — those are re-introduced in later phases via build_src_filter.
#include <M5Cardputer.h>

static void drawBanner() {
  auto& d = M5Cardputer.Display;
  d.fillScreen(TFT_BLACK);
  d.setTextColor(TFT_WHITE, TFT_BLACK);
  d.setTextDatum(top_left);
  d.setFont(&fonts::Font0);
  d.setTextSize(2);
  d.setCursor(4, 4);
  d.print("Claude buddy");
  d.setTextSize(1);
  d.setCursor(4, 28);
  d.print("Cardputer-Adv port");
  d.setCursor(4, 42);
  d.printf("heap: %u", (unsigned)ESP.getFreeHeap());
  d.setCursor(4, 56);
  d.print("type to echo to USB");
}

void setup() {
  auto cfg = M5.config();
  M5Cardputer.begin(cfg, true);
  M5Cardputer.Display.setRotation(1);   // 240x135 native landscape
  M5Cardputer.Display.setBrightness(180);
  drawBanner();
  Serial.begin(115200);
  Serial.println("[cardputer-adv] phase1 bring-up");
}

void loop() {
  M5Cardputer.update();
  if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
    auto state = M5Cardputer.Keyboard.keysState();
    for (auto c : state.word) Serial.write(c);
    if (state.enter) Serial.println();
    if (state.del)   Serial.print("[del]");
  }
  delay(10);
}
