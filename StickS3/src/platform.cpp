#include "platform.h"
#include <M5Unified.h>
// SD.h isn't used on the StickS3 (no microSD slot) but the shared
// ../src/xfer.h header includes it unconditionally for the Cardputer-Adv
// path. Pulling it in here is how PlatformIO's Library Dependency Finder
// discovers the framework's built-in SD library; without it the
// StickS3 build of xfer.h fails with "SD.h: No such file or directory".
// The library compiles into the binary as dead code (linker drops it).
#include <SD.h>
#include <sys/time.h>

namespace Platform {

static bool _screenOn = true;

bool initSdCard() { return false; }   // StickS3 has no microSD slot.
bool sdAvailable() { return false; }

void init() {
  // The StickS3 ships with EXT_5V_EN enabled by default in M5Unified.
  // That rail powers the Grove port and the IR TX/RX block, neither of
  // which this firmware uses. Leaving it enabled causes the boost
  // converter to whine audibly and drain the 250 mAh battery measurably.
  // Setting cfg.output_power = false disables EXT_5V_EN at init.
  //   - StickS3 datasheet page 6 ("EXT_5V_EN" section)
  //   - M5Unified.hpp:130 (bool output_power = true; default)
  auto cfg = M5.config();
  cfg.output_power = false;
  M5.begin(cfg);

  // Cap speaker volume to ~70% per the StickS3 datasheet page 3 note:
  // "When powered by battery (USB not connected), it is recommended to
  //  keep the speaker volume below 75% to avoid unexpected device reboot
  //  caused by excessive power consumption."
  M5.Speaker.setVolume(180);

  // Default rotation is 0 (native portrait, 135 wide x 240 tall).
  M5.Display.setRotation(0);
}

// ---------------- Display ----------------

void setBrightness(uint8_t pct) {
  if (pct > 100) pct = 100;
  // Linear 0..100 -> 0..255 with rounding, matching the Cardputer-Adv
  // shim. brightLevel * 20 (0,20,40,60,80,100) gives 0/51/102/153/204/255.
  uint8_t b = (uint8_t)((pct * 255 + 50) / 100);
  M5.Display.setBrightness(b);
}

void screenOff() {
  M5.Display.setBrightness(0);
  M5.Display.sleep();
  _screenOn = false;
}

void screenOn() {
  M5.Display.wakeup();
  _screenOn = true;
}

bool isScreenOn() { return _screenOn; }

// ---------------- Power ----------------

bool isOnUsb() {
  // M5Unified's M5PM1 backend returns is_charging when USB is supplying.
  // No need to fall back to battery-current sign here — the PMIC sets the
  // charging bit directly when VBUS is present.
  return M5.Power.isCharging() == m5::Power_Class::is_charging;
}

uint8_t  batteryPct() { return (uint8_t)M5.Power.getBatteryLevel(); }
uint16_t batteryMv()  { return (uint16_t)M5.Power.getBatteryVoltage(); }

void powerOff() {
  // No-op: the StickS3's side button is PMIC-managed (single-press=on,
  // double-press=off, long-press=boot mode). There is no firmware-
  // accessible kill switch; the user double-presses the side button to
  // power down. We leave this stub so call sites that already invoke
  // Platform::powerOff() (e.g. factory reset) compile unchanged.
}

// ---------------- Audio ----------------

void tone(uint16_t freqHz, uint16_t durMs) {
  M5.Speaker.tone(freqHz, durMs);
}

void tickAudio() { /* M5Unified Speaker is non-blocking */ }

void beepSeq(const uint16_t* freqHz, const uint16_t* durMs, uint8_t n) {
  for (uint8_t i = 0; i < n; i++) {
    M5.Speaker.tone(freqHz[i], durMs[i]);
    delay(durMs[i]);
  }
}

// ---------------- LED (no user LED on StickS3) ----------------

void setLed(uint8_t)              { /* no-op */ }
void setLedRgb(uint8_t, uint8_t, uint8_t) { /* no-op */ }

// ---------------- IMU ----------------

bool imuAccel(float& x, float& y, float& z) {
  if (!M5.Imu.isEnabled()) { x = 0; y = 0; z = 1.0f; return false; }
  M5.Imu.update();
  return M5.Imu.getAccel(&x, &y, &z);
}

// ---------------- RTC (ESP32 internal via time.h) ----------------

void setRtcLocal(const struct tm& tmLocal) {
  struct tm copy = tmLocal;
  time_t t = mktime(&copy);
  if (t < 0) return;
  struct timeval tv{ .tv_sec = t, .tv_usec = 0 };
  settimeofday(&tv, nullptr);
}

void getRtcLocal(struct tm& tmLocal) {
  time_t t = time(nullptr);
  struct tm lt;
  gmtime_r(&t, &lt);
  tmLocal = lt;
}

}  // namespace Platform
