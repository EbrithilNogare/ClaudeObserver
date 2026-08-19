#pragma once
#include <Arduino.h>
#include "config.h"

// Single switch wired PIN_BUTTON -> GND, read through the internal pull-up,
// so the raw level is LOW while pressed.
//
// Debounce: the raw level has to stay put for BTN_DEBOUNCE_MS before it counts
// as a real edge, which swallows the contact chatter of a mechanical switch.
// A press that is released before BTN_HOLD_MS is a click; crossing BTN_HOLD_MS
// while still down fires the hold event once (the later release is ignored).
class Button {
  bool _raw = false;        // last sampled level (true = pressed)
  bool _stable = false;     // debounced level
  uint32_t _changedAt = 0;  // when _raw last flipped
  uint32_t _pressedAt = 0;  // when _stable went down
  bool _holdFired = false;

public:
  enum Event { NONE, CLICK, HOLD };

  void begin() { pinMode(PIN_BUTTON, INPUT_PULLUP); }

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
      } else if (!_holdFired) {
        return CLICK;  // released before the hold threshold
      }
    }
    if (_stable && !_holdFired && now - _pressedAt >= BTN_HOLD_MS) {
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
