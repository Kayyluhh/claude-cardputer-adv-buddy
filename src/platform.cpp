#include "platform.h"
#include <M5Cardputer.h>
#include <sys/time.h>

namespace Platform {

static bool _screenOn = true;

void init() {
  // M5Cardputer.begin() (called by main) already initializes Display, Power,
  // Speaker, Mic, Keyboard, Imu, and the I2C buses. Nothing extra here yet.
}

// ---------------- Display ----------------

void setBrightness(uint8_t pct) {
  if (pct > 100) pct = 100;
  // Linear 0..100 -> 0..255 with rounding. brightLevel * 20 (0,20,40,60,80,100)
  // gives 0/51/102/153/204/255; nap-dim uses pct=4..8 for "barely on" rather
  // than full off. M5GFX's setBrightness is the only dial — there's no
  // separate AXP ScreenBreath nor LDO2 power gate on the Adv.
  uint8_t b = (uint8_t)((pct * 255 + 50) / 100);
  M5Cardputer.Display.setBrightness(b);
}

void screenOff() {
  M5Cardputer.Display.setBrightness(0);
  M5Cardputer.Display.sleep();
  _screenOn = false;
}

void screenOn() {
  M5Cardputer.Display.wakeup();
  _screenOn = true;
}

bool isScreenOn() { return _screenOn; }

// ---------------- Power ----------------

bool isOnUsb() {
  // No AXP on Cardputer-Adv, so getVBUSVoltage() reads zero. Use charging
  // current as a proxy: positive = USB plugged in and charging, zero or
  // negative = on battery.
  auto charging = M5.Power.isCharging();
  if (charging == m5::Power_Class::is_charging) return true;
  return M5.Power.getBatteryCurrent() > 0;
}

uint8_t  batteryPct() { return (uint8_t)M5.Power.getBatteryLevel(); }
uint16_t batteryMv()  { return (uint16_t)M5.Power.getBatteryVoltage(); }

void powerOff() {
  // The Adv only powers off via the side switch; software powerOff() falls
  // back to deep-sleep with no wake source. Caller should warn the user.
  M5.Power.powerOff();
}

// ---------------- Audio ----------------

void tone(uint16_t freqHz, uint16_t durMs) {
  M5Cardputer.Speaker.tone(freqHz, durMs);
}

void tickAudio() { /* M5Unified Speaker is non-blocking */ }

void beepSeq(const uint16_t* freqHz, const uint16_t* durMs, uint8_t n) {
  for (uint8_t i = 0; i < n; i++) {
    M5Cardputer.Speaker.tone(freqHz[i], durMs[i]);
    delay(durMs[i]);
  }
}

// ---------------- LED ----------------

void setLed(uint8_t brightness) {
  // M5.Power.setLed handles whichever LED the board exposes. On Cardputer-Adv
  // this drives the RGB LED at the given brightness in the default color.
  M5.Power.setLed(brightness);
}

void setLedRgb(uint8_t r, uint8_t g, uint8_t b) {
  // Phase-2 placeholder: M5Unified has an LED_Class with color support on
  // some boards. If M5.Led isn't wired up for Cardputer-Adv we'll drop in a
  // direct WS2812 driver after the first hardware bring-up reveals the pin.
  (void)r; (void)g; (void)b;
  // Approximate: any non-black color = on at brightness max-channel.
  uint8_t br = r > g ? r : g;
  if (b > br) br = b;
  M5.Power.setLed(br);
}

// ---------------- IMU ----------------

bool imuAccel(float& x, float& y, float& z) {
  if (!M5.Imu.isEnabled()) { x = 0; y = 0; z = 1.0f; return false; }
  M5.Imu.update();
  return M5.Imu.getAccel(&x, &y, &z);
}

// ---------------- RTC (ESP32 internal via time.h) ----------------

void setRtcLocal(const struct tm& tmLocal) {
  struct tm copy = tmLocal;
  // mktime treats input as local time; we have no TZ set, so this is fine
  // as long as bridge-supplied time was already adjusted to local (it is —
  // see data.h's "time" packet handling: epoch_sec + tz_offset_sec).
  time_t t = mktime(&copy);
  if (t < 0) return;
  struct timeval tv{ .tv_sec = t, .tv_usec = 0 };
  settimeofday(&tv, nullptr);
}

void getRtcLocal(struct tm& tmLocal) {
  time_t t = time(nullptr);
  struct tm lt;
  gmtime_r(&t, &lt);   // we stored "local" via settimeofday above, so gmtime
                       // round-trips back to the same components.
  tmLocal = lt;
}

}  // namespace Platform
