#pragma once
#include <stdint.h>
#include <time.h>

// Thin shim over M5Unified for the StickC Plus -> Cardputer-Adv port.
// Replaces every M5.Axp.*, M5.Beep.*, M5.Imu.*, M5.Rtc.* call site so the
// rest of the code never sees the underlying M5Unified API directly.
namespace Platform {

void init();

// SD card (microSD slot on Cardputer-Adv). initSdCard() is called from
// init(); it returns true if the card mounted. sdAvailable() reports the
// last mount status so callers (character.cpp, xfer.h) can fail closed
// rather than hitting SD.open() on an empty slot. Pins are wired CS=G12,
// MOSI=G14, CLK=G40, MISO=G39 per the M5Stack Cardputer-Adv pinmap.
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
void    powerOff();   // best-effort: Cardputer-Adv has a hardware power switch
                      // and no programmable kill, so this just deep-sleeps.

// Audio
void    tone(uint16_t freqHz, uint16_t durMs);
void    tickAudio();  // no-op on M5Unified Speaker (non-blocking); kept for
                      // call-site compatibility with M5.Beep.update().
// Play a sequence of (freq, dur) tones back-to-back. Blocks for the
// total duration. Used for the Mario 1-UP permission-arrival jingle.
void    beepSeq(const uint16_t* freqHz, const uint16_t* durMs, uint8_t n);

// LED — Cardputer-Adv has an RGB LED; brightness 0 = off, 1..255 = on.
// Color is left to the caller via setLedRgb if richer indication is needed.
void    setLed(uint8_t brightness);
void    setLedRgb(uint8_t r, uint8_t g, uint8_t b);

// IMU (BMI270 on Adv, MPU6886 on StickC Plus). Units: g.
bool    imuAccel(float& x, float& y, float& z);

// RTC. Cardputer-Adv has no battery-backed RTC chip — these go through the
// ESP32 internal RTC via <time.h>. Time persists only while powered.
void    setRtcLocal(const struct tm& tmLocal);
void    getRtcLocal(struct tm& tmLocal);

}  // namespace Platform
