#pragma once
#include <Arduino.h>
#include "config.h"

// Single switch wired PIN_BUTTON -> GND, read through the internal pull-up,
// so the raw level is LOW while pressed.
//
// Debounce: the raw level has to stay put for BTN_DEBOUNCE_MS before it counts
// as a real edge, which swallows the contact chatter of a mechanical switch.
// The debounced down edge reports PRESS immediately — for anything that has to
// feel responsive rather than wait for the release. A short press then also
// reports a click on release; crossing the hold threshold while still down
// fires the hold event once (the later release is ignored). A press released inside
// [_secretMs, _holdMs) reports SECRET instead of CLICK — the hidden gesture.
// Both thresholds are settable so the minigame can use its own (shorter hold to
// quit, no secret window).
class Button {
  bool _raw = false;        // last sampled level (true = pressed)
  bool _stable = false;     // debounced level
  uint32_t _changedAt = 0;  // when _raw last flipped
  uint32_t _pressedAt = 0;  // when _stable went down
  bool _holdFired = false;
  uint32_t _holdMs = BTN_HOLD_MS;
  uint32_t _secretMs = BTN_SECRET_MS;

public:
  enum Event { NONE, PRESS, CLICK, HOLD, SECRET };

  void begin() { pinMode(PIN_BUTTON, INPUT_PULLUP); }

  // holdMs = how long a hold has to last to fire HOLD; secretMs = start of the
  // release window that reports SECRET (pass holdMs to disable it).
  void setThresholds(uint32_t holdMs, uint32_t secretMs) {
    _holdMs = holdMs;
    _secretMs = secretMs;
  }

  uint32_t holdMs() const { return _holdMs; }

  bool isDown() const { return _stable; }

  // Millis the button has been held down for, 0 when up. Lets the UI show
  // hold progress.
  uint32_t heldMs(uint32_t now) const { return _stable ? now - _pressedAt : 0; }

  Event update(uint32_t now) {
    bool raw = digitalRead(PIN_BUTTON) == LOW;
    if (raw != _raw) {
      _raw = raw;
      _changedAt = now;
    }
    if (_raw != _stable && now - _changedAt >= BTN_DEBOUNCE_MS) {
      _stable = _raw;
      if (_stable) {
        _pressedAt = now;
        _holdFired = false;
        return PRESS;  // down edge, before we know click vs hold vs secret
      } else if (!_holdFired) {
        // released before the hold threshold: late release = secret gesture
        return (now - _pressedAt >= _secretMs) ? SECRET : CLICK;
      }
    }
    if (_stable && !_holdFired && now - _pressedAt >= _holdMs) {
      _holdFired = true;
      return HOLD;
    }
    return NONE;
  }

  // Block until the switch has been released and stayed released, so a still
  // pressed button can't immediately re-trigger a wakeup.
  void waitForRelease() {
    uint32_t releasedAt = millis();
    while (millis() - releasedAt < BTN_DEBOUNCE_MS * 4) {
      if (digitalRead(PIN_BUTTON) == LOW) releasedAt = millis();
      delay(5);
    }
    _raw = _stable = false;
    _holdFired = false;
  }
};
