#include "input.h"
#include <M5Cardputer.h>
#include <Arduino.h>
#include <string.h>

namespace Input {

static constexpr uint8_t MAX_HELD = 8;
static constexpr uint8_t QUEUE_CAP = 16;
static constexpr uint8_t EDGE_CAP  = 16;

static char     _heldNow[MAX_HELD];
static uint8_t  _heldNowN = 0;
static char     _heldPrev[MAX_HELD];
static uint8_t  _heldPrevN = 0;

static bool     _enterNow = false, _enterPrev = false;
static bool     _delNow   = false, _delPrev   = false;

static uint32_t _enterDownMs = 0;
static bool     _enterLongFired = false;

static char     _firedChars[EDGE_CAP];
static uint8_t  _firedCharsN = 0;
static bool     _enterEdge = false;
static bool     _delEdge   = false;
static bool     _enterLongEdge = false;

static char     _queue[QUEUE_CAP];
static uint8_t  _qHead = 0, _qTail = 0;

static bool _contains(const char* arr, uint8_t n, char c) {
  for (uint8_t i = 0; i < n; i++) if (arr[i] == c) return true;
  return false;
}

void init() {
  _heldNowN = _heldPrevN = 0;
  _enterNow = _enterPrev = false;
  _delNow = _delPrev = false;
  _enterDownMs = 0;
  _enterLongFired = false;
  _firedCharsN = 0;
  _enterEdge = _delEdge = _enterLongEdge = false;
  _qHead = _qTail = 0;
}

void poll() {
  // Clear last frame's edge events.
  _firedCharsN = 0;
  _enterEdge = false;
  _delEdge = false;
  _enterLongEdge = false;

  // Snapshot current state. M5Cardputer.update() must have been called this
  // frame so the keyboard scan is fresh.
  bool changed = M5Cardputer.Keyboard.isChange();
  if (changed) {
    auto state = M5Cardputer.Keyboard.keysState();
    _enterNow = state.enter;
    _delNow   = state.del;
    _heldNowN = 0;
    for (auto c : state.word) {
      if (_heldNowN < MAX_HELD) _heldNow[_heldNowN++] = c;
    }
  }
  // If no change this frame, _heldNow / _enterNow / _delNow stay as last
  // observed. M5Cardputer.Keyboard.isChange() only fires on transitions.

  // Compute edges: in _heldNow but not in _heldPrev.
  for (uint8_t i = 0; i < _heldNowN; i++) {
    if (!_contains(_heldPrev, _heldPrevN, _heldNow[i])) {
      if (_firedCharsN < EDGE_CAP) _firedChars[_firedCharsN++] = _heldNow[i];
      uint8_t next = (uint8_t)((_qTail + 1) % QUEUE_CAP);
      if (next != _qHead) { _queue[_qTail] = _heldNow[i]; _qTail = next; }
    }
  }
  _enterEdge = _enterNow && !_enterPrev;
  _delEdge   = _delNow   && !_delPrev;

  uint32_t now = millis();
  if (_enterEdge) { _enterDownMs = now; _enterLongFired = false; }
  if (!_enterNow) { _enterDownMs = 0; }
  if (_enterNow && _enterDownMs && !_enterLongFired && (now - _enterDownMs) >= 600) {
    _enterLongFired = true;
    _enterLongEdge  = true;
  }

  // Roll over for next frame.
  memcpy(_heldPrev, _heldNow, _heldNowN);
  _heldPrevN = _heldNowN;
  _enterPrev = _enterNow;
  _delPrev   = _delNow;
}

bool wasPressedChar(char ch) {
  for (uint8_t i = 0; i < _firedCharsN; i++) if (_firedChars[i] == ch) return true;
  return false;
}
bool wasPressedEnter() { return _enterEdge; }
bool wasPressedDel()   { return _delEdge; }
bool wasPressedEsc()   { return _delEdge; }
bool wasLongPressedEnter(uint32_t holdMs) {
  // holdMs override: only relevant if caller wants a different threshold;
  // for the default 600ms case we use the precomputed _enterLongEdge.
  if (holdMs == 600) return _enterLongEdge;
  return _enterNow && _enterDownMs && (millis() - _enterDownMs) >= holdMs && !_enterLongFired;
}

bool isHeldChar(char ch) { return _contains(_heldNow, _heldNowN, ch); }
bool isHeldEnter()       { return _enterNow; }
bool isHeldDel()         { return _delNow; }

char popChar() {
  if (_qHead == _qTail) return 0;
  char c = _queue[_qHead];
  _qHead = (uint8_t)((_qHead + 1) % QUEUE_CAP);
  return c;
}

}  // namespace Input
