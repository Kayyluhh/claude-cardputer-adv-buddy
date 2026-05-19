#pragma once
#include <stdint.h>
#include <time.h>

// Thin shim over M5Unified for the StickS3 build. Mirrors the public API
// of the Cardputer-Adv platform.h byte-for-byte so the shared headers in
// ../src/ (data.h, xfer.h, stats.h, buddy.cpp) compile against either
// build without preprocessor guards.
//
// Implementation differences from the Cardputer-Adv shim:
//   * Uses M5Unified (M5.*) instead of M5Cardputer (M5Cardputer.*).
//   * Platform::init() calls M5.begin(cfg) with cfg.output_power = false,
//     the documented workaround for the StickS3's EXT_5V boost-converter
//     whine and battery drain.
//   * SD functions are no-op stubs (the StickS3 has no microSD slot).
//     sdAvailable() always returns false so ../src/xfer.h fails char_begin
//     with "no SD card" via the existing code path.
//   * LED functions are no-op stubs (StickS3 has no user RGB LED; the
//     green status LED is PMIC-routed).
//   * powerOff() is a no-op: the side button is the only hard off and is
//     PMIC-managed (single=on, double=off, long=boot mode) per the
//     StickS3 datasheet, page 6.
namespace Platform {

void init();

// SD card (microSD slot — N/A on StickS3). Kept in the API so shared
// headers compile unchanged; both functions always return false.
bool    initSdCard();
bool    sdAvailable();

// Display
void    setBrightness(uint8_t pct);   // 0..100
void    screenOff();
void    screenOn();
bool    isScreenOn();

// Power
bool    isOnUsb();
uint8_t batteryPct();
uint16_t batteryMv();
void    powerOff();   // no-op on StickS3 (PMIC-managed power button)

// Audio
void    tone(uint16_t freqHz, uint16_t durMs);
void    tickAudio();  // no-op on M5Unified Speaker (non-blocking)
void    beepSeq(const uint16_t* freqHz, const uint16_t* durMs, uint8_t n);

// LED — StickS3 has no user RGB LED; both calls are no-ops, kept for
// call-site compatibility with the Cardputer-Adv build.
void    setLed(uint8_t brightness);
void    setLedRgb(uint8_t r, uint8_t g, uint8_t b);

// IMU (BMI270 on StickS3). Units: g.
bool    imuAccel(float& x, float& y, float& z);

// RTC. StickS3 has no battery-backed RTC chip — these go through the
// ESP32 internal RTC via <time.h>. Time persists only while powered.
void    setRtcLocal(const struct tm& tmLocal);
void    getRtcLocal(struct tm& tmLocal);

}  // namespace Platform
