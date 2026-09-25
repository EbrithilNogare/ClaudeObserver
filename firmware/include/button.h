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
// fires the hold event once (the later release is ignored). The threshold is
// settable because every screen uses its own (open menu / pick item / quit game).
class Button {
  bool _raw = false;        // last sampled level (true = pressed)
  bool _stable = false;     // debounced level
  uint32_t _changedAt = 0;  // when _raw last flipped
  uint32_t _pressedAt = 0;  // when _stable went down
  bool _holdFired = false;
  uint32_t _holdMs = BTN_MENU_HOLD_MS;

public:
  enum Event { NONE, PRESS, CLICK, HOLD };

  void begin() { pinMode(PIN_BUTTON, INPUT_PULLUP); }

  // How long a hold has to last to fire HOLD. Changing it mid-press is safe: a
  // press that already fired its HOLD stays spent until it is released.
  void setHoldMs(uint32_t holdMs) { _holdMs = holdMs; }

  uint32_t holdMs() const { return _holdMs; }

  bool isDown() const { return _stable; }

  // True while the current press has already fired its HOLD — the rest of that
  // press (and its release) does nothing, so a hold that switched screens can't
  // also act on the screen it landed on.
  bool holdSpent() const { return _stable && _holdFired; }

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
        return PRESS;  // down edge, before we know click vs hold
      } else if (!_holdFired) {
        return CLICK;  // released before the hold threshold
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
