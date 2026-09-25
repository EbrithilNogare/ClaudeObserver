#pragma once
#include <driver/gpio.h>

#include "game.h"  // art:: — the menu's game icon is Claudie herself
#include "lgfx_conf.h"
#include "state.h"

// Full-frame sprite rendering (internal SRAM) — flicker-free animations.
class UI {
  LGFX _lcd;
  LGFX_Sprite _frame{&_lcd};
  uint32_t _nextBlinkAt = 2000;
  uint32_t _blinkStart = 0;
  bool _blinking = false;
  bool _eyesShut = true;       // last drawEyes() frame was the sleeping face
  uint32_t _openStart = 0;     // eye-opening animation start, 0 = not running
  float _openFrom = 0;         // sleeping openness the animation starts from

public:
  // dark=true leaves the backlight off so the caller can fade in with
  // playBootFade() instead of flashing a full-brightness frame first.
  void begin(bool dark = false) {
    // off() latches the backlight pin LOW with a pad hold, and that hold
    // outlives the restart that ends a light sleep — release it before the PWM
    // channel is attached or the backlight never comes back.
    gpio_hold_dis((gpio_num_t)PIN_LCD_BL);
    ledcAttach(PIN_LCD_BL, 5000, 8);      // 5 kHz, 8-bit PWM backlight
    ledcWrite(PIN_LCD_BL, dark ? 0 : LCD_BRIGHTNESS);
    _lcd.init();
    _lcd.setRotation(LCD_ROTATION);
    _lcd.fillScreen(COL_BG);  // visible immediately, even before first frame
    // No PSRAM on the C6 — a 320x170 16-bit sprite is ~106 KB and fits in
    // the 512 KB SRAM; halve the color depth if allocation ever fails.
    if (!_frame.createSprite(_lcd.width(), _lcd.height())) {
      Serial.println("[ui] 16-bit sprite failed, falling back to 8-bit");
      _frame.setColorDepth(8);
      _frame.createSprite(_lcd.width(), _lcd.height());
    }
    _frame.setTextWrap(false);
  }

  // Blank the panel and kill the backlight — called right before sleep so the
  // display doesn't sit there lit with a frozen frame. Detaching the LEDC
  // channel and driving the pin LOW matters: the PWM peripheral is clocked off
  // in sleep, which would otherwise leave the backlight at an arbitrary level.
  void off() {
    ledcWrite(PIN_LCD_BL, 0);
    _lcd.fillScreen(TFT_BLACK);
    _lcd.sleep();
    ledcDetach(PIN_LCD_BL);
    pinMode(PIN_LCD_BL, OUTPUT);
    digitalWrite(PIN_LCD_BL, LOW);
    // Two separate things have to be pinned down, or the backlight comes up at
    // full brightness the moment the chip sleeps:
    //   1. the pad otherwise switches to its *sleep* configuration in light
    //      sleep, which leaves it floating (and the module pulls BL high),
    //   2. the pad hold latches the LOW level so nothing can drive it again
    //      while the CPU is suspended.
    gpio_sleep_sel_dis((gpio_num_t)PIN_LCD_BL);
    gpio_hold_en((gpio_num_t)PIN_LCD_BL);
  }

  // ---------------- sleep animation / boot fade ----------------
  // Both are deliberately blocking: nothing else needs to run while the device
  // is on its way down or coming back up, and the sleep one doubles as the
  // window in which the user lets go of the button.

  // Eyes close, the closed slit narrows to a dot, backlight fades out.
  void playSleepAnim(uint32_t durMs = SLEEP_ANIM_MS) {
    const uint32_t t0 = millis();
    for (;;) {
      uint32_t t = millis() - t0;
      if (t >= durMs) break;
      float p = (float)t / durMs;
      _frame.fillSprite(COL_BG);
      // 0.00-0.40 lids come down, 0.40-0.85 the slit shrinks toward the
      // centre, 0.85-1.00 the dot is gone and only the fade is left.
      float closing = ease(clamp01(p / 0.40f));
      float shrink = ease(clamp01((p - 0.40f) / 0.45f));
      int h = (int)(EYE_H - (EYE_H - 5) * closing);
      int w = (int)(EYE_W * (1.0f - shrink));
      if (w > 1 && h > 0) drawEyePair(_frame.height() / 2, w, h);
      if (p < 0.55f) {
        _frame.setTextSize(1);
        _frame.setTextColor(COL_EYE, COL_BG);
        const char *msg = "going to sleep";
        _frame.setCursor((_frame.width() - _frame.textWidth(msg)) / 2,
                         _frame.height() - 14);
        _frame.print(msg);
      }
      _frame.pushSprite(0, 0);
      // Hold brightness for the first half, then fade the panel out.
      float dim = clamp01((p - 0.5f) / 0.5f);
      ledcWrite(PIN_LCD_BL, (int)(LCD_BRIGHTNESS * (1.0f - dim)));
      delay(FRAME_MS);
    }
    off();
  }

  // Power-on: the backlight slowly comes up over the ordinary sleeping face
  // (closed eyes, z Z z). The eyes stay shut until the daemon actually connects
  // and sends data — then drawEyes() plays the slow eye-opening on its own.
  void playBootFade(uint32_t durMs = WAKE_ANIM_MS) {
    const uint32_t t0 = millis();
    for (;;) {
      uint32_t t = millis() - t0;
      if (t >= durMs) break;
      render(millis());
      ledcWrite(PIN_LCD_BL, (int)(LCD_BRIGHTNESS * ease((float)t / durMs)));
      delay(FRAME_MS);
    }
    ledcWrite(PIN_LCD_BL, LCD_BRIGHTNESS);
  }

  // The minigame borrows the same full-frame sprite: it draws into frame() and
  // then calls push(), so nothing else in the UI has to know about it.
  LGFX_Sprite &frame() { return _frame; }
  void push() { _frame.pushSprite(0, 0); }

  void render(uint32_t now) {
    _frame.fillSprite(COL_BG);
    // Eyes-only mode (button click): just the eyes, centred, nothing else.
    if (!app.showData) {
      drawEyes(now, /*sleeping=*/!(app.connected && app.hasData));
    } else if (app.connected && app.hasData) {
      if (app.authError) {
        drawDeadEyes(now);
        drawAuthErrorExtras();
      } else {
        drawStats(now);
        drawEyes(now, /*sleeping=*/false);
      }
    } else {
      drawEyes(now, /*sleeping=*/true);
      drawSleepExtras(now);
    }
    if (app.showData) drawCorner();
    drawHoldProgress(BTN_MENU_HOLD_MS);
    _frame.pushSprite(0, 0);
  }

  // ---------------- menu ----------------
  // Face-orange background with one icon tile per MenuItem in a row. The
  // highlighted tile is filled near-black with the icon knocked out in orange;
  // the others are outlines. Click moves the highlight, the hold bar along the
  // bottom shows how close a hold is to picking it.
  void renderMenu(uint32_t now) {
    _frame.fillSprite(COL_BG);
    const int n = MENU_COUNT;
    const int gap = (_frame.width() - n * MENU_TILE) / (n + 1);
    const int cy = _frame.height() / 2;
    for (int i = 0; i < n; i++) {
      bool sel = i == app.menuSel;
      int cx = gap + i * (MENU_TILE + gap) + MENU_TILE / 2;
      // the highlighted tile floats a little, so it reads as "live"
      int y = cy + (sel ? (int)roundf(2 * sinf(now / 350.0f)) : 0);
      int x0 = cx - MENU_TILE / 2, y0 = y - MENU_TILE / 2;
      uint16_t ink = sel ? COL_BG : COL_EYE;
      if (sel) {
        _frame.fillSmoothRoundRect(x0, y0, MENU_TILE, MENU_TILE, 16, COL_EYE);
      } else {
        for (int t = 0; t < 3; t++)
          _frame.drawRoundRect(x0 + t, y0 + t, MENU_TILE - 2 * t, MENU_TILE - 2 * t,
                               16 - t, COL_EYE);
      }
      switch (i) {
        case MENU_GAME: drawIconGame(cx, y, ink, sel ? COL_EYE : COL_BG); break;
        case MENU_JUMP: drawIconJump(cx, y, ink, sel ? COL_EYE : COL_BG); break;
        case MENU_POWER: drawIconPower(cx, y, ink); break;
        case MENU_BACK: drawIconBack(cx, y, ink); break;
      }
    }
    drawHoldProgress(MENU_SELECT_HOLD_MS);
    _frame.pushSprite(0, 0);
  }

private:
  // ---------------- top-right readout ----------------
  // Battery (e.g. "78%"), ESP temperature ("42C") and BLE signal ("-67db").
  void drawCorner() {
    char buf[28];
    if (app.connected)
      snprintf(buf, sizeof buf, "%u%% %.0fC %ddb", app.battPct, app.espTempC, app.rssi);
    else
      snprintf(buf, sizeof buf, "%u%% %.0fC", app.battPct, app.espTempC);
    _frame.setTextSize(1);
    _frame.setTextColor(COL_EYE, COL_BG);
    _frame.setCursor(_frame.width() - 3 - _frame.textWidth(buf), 2);
    _frame.print(buf);
  }

  // While the button is held, a bar fills across the bottom edge; when it
  // reaches the full width the hold fires (open the menu / pick the item).
  void drawHoldProgress(uint32_t holdMs) {
    if (app.btnHeldMs < 250) return;  // ignore ordinary clicks
    float r = min(1.0f, (float)app.btnHeldMs / holdMs);
    int h = 3, y = _frame.height() - h;
    _frame.fillRect(0, y, (int)(_frame.width() * r), h, COL_EYE);
  }

  // ---------------- menu icons ----------------
  // Each icon is drawn centred on (cx, cy) and fits a ~48 px square.
  static constexpr int MENU_TILE = 64;

  // Dyno game: Claudie, straight off the minigame's pixel art (4 px cells),
  // with her two square eyes punched through in the tile colour.
  void drawIconGame(int cx, int cy, uint16_t ink, uint16_t eye) {
    const int P = 4, w = 12 * P, h = 8 * P;
    int x = cx - w / 2, y = cy - h / 2;
    art::blit(_frame, x, y, art::CLAUDIE_BODY, 6, ink, P);
    art::blit(_frame, x, y + 6 * P, art::CLAUDIE_LEGS_A, 2, ink, P);
    _frame.fillRect(x + 3 * P, y + P, P, P, eye);
    _frame.fillRect(x + 8 * P, y + P, P, P, eye);
  }

  // Jump climber: small Claudie on a low ledge, aim dots pointing up to a
  // higher one.
  void drawIconJump(int cx, int cy, uint16_t ink, uint16_t eye) {
    const int P = 2;
    _frame.fillRect(cx - 24, cy + 18, 28, 4, ink);  // lower ledge
    _frame.fillRect(cx + 6, cy - 20, 18, 4, ink);   // upper ledge
    int x = cx - 22, y = cy + 18 - 8 * P;
    art::blit(_frame, x, y, art::CLAUDIE_BODY, 6, ink, P);
    art::blit(_frame, x, y + 6 * P, art::CLAUDIE_LEGS_A, 2, ink, P);
    _frame.fillRect(x + 3 * P, y + P, P, P, eye);
    _frame.fillRect(x + 8 * P, y + P, P, P, eye);
    _frame.fillRect(cx - 6, cy - 3, 3, 3, ink);
    _frame.fillRect(cx - 1, cy - 9, 3, 3, ink);
    _frame.fillRect(cx + 4, cy - 16, 4, 4, ink);
  }

  // Power off: the standard power symbol — a ring open at the top with a bar
  // dropping into the gap.
  void drawIconPower(int cx, int cy, uint16_t ink) {
    const int r1 = 22, r0 = 15;
    // arc angles are degrees clockwise from 3 o'clock; top is 270
    _frame.fillArc(cx, cy + 2, r0, r1, 310, 360, ink);
    _frame.fillArc(cx, cy + 2, r0, r1, 0, 230, ink);
    _frame.fillSmoothRoundRect(cx - 4, cy - r1 - 2, 8, r1 + 2, 4, ink);
  }

  // Back: a U-turn arrow — arrowhead pointing left along the top, curving
  // round the right-hand side and back along the bottom.
  void drawIconBack(int cx, int cy, uint16_t ink) {
    const int ax = cx + 6, ay = cy + 4;  // centre of the bend
    const int r0 = 11, r1 = 18, t = r1 - r0;
    _frame.fillArc(ax, ay, r0, r1, 270, 360, ink);
    _frame.fillArc(ax, ay, r0, r1, 0, 90, ink);
    _frame.fillRect(cx - 10, ay - r1, ax - (cx - 10) + 1, t, ink);  // top run
    _frame.fillRect(cx - 18, ay + r0, ax - (cx - 18) + 1, t, ink);  // bottom run
    int my = ay - r1 + t / 2;  // arrowhead sits on the top run's centre line
    _frame.fillTriangle(cx - 24, my, cx - 10, my - 12, cx - 10, my + 12, ink);
  }

  static float clamp01(float v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }
  // smoothstep — no abrupt starts or stops in the animations
  static float ease(float p) { return p * p * (3.0f - 2.0f * p); }

  // ---------------- eyes ----------------
  // Shared eye geometry, also used by the XX (auth error) variant.
  static constexpr int EYE_GAP = 41;   // centre-to-centre / 2
  static constexpr int EYE_W = 29;
  static constexpr int EYE_H = 73;

  // The two eye bars at an arbitrary size, centred on cy — shared by the
  // idle rendering and both animations.
  void drawEyePair(int cy, int w, int h) {
    const int cx = _frame.width() / 2;
    int r = min(13, min(w, h) / 2);
    for (int side = -1; side <= 1; side += 2)
      _frame.fillSmoothRoundRect(cx + side * EYE_GAP - w / 2, cy - h / 2, w, h, r,
                                 COL_EYE);
  }

  // Two black vertical rounded bars, Claude-style. Blink = height squash.
  void drawEyes(uint32_t now, bool sleeping) {
    // Always dead centre — the stats rows sit clear of the eyes at the very top
    // and bottom, so toggling them doesn't shift the eyes.
    const int cy = _frame.height() / 2;
    const int eyeW = EYE_W, eyeH = EYE_H;

    float openness = 1.0f;
    if (sleeping) {
      // closed, gentle breathing wobble
      openness = 0.08f + 0.03f * sinf(now / 900.0f);
      _openStart = 0;
    } else if (_eyesShut || _openStart) {
      // Just woke up (BLE data arrived): open slowly from wherever the
      // breathing left the lids, with a small overshoot before settling.
      if (_eyesShut) {
        _openStart = now ? now : 1;
        _openFrom = 0.08f + 0.03f * sinf(now / 900.0f);
      }
      float p = clamp01((float)(now - _openStart) / EYE_OPEN_MS);
      float e = ease(p);
      openness = _openFrom + (1.0f - _openFrom) * e;
      if (p > 0.8f) openness += 0.06f * sinf((p - 0.8f) / 0.2f * 3.14159f);
      if (p >= 1.0f) {
        _openStart = 0;
        _nextBlinkAt = now + 1200;  // a first blink shortly after, like waking
      }
    } else {
      if (!_blinking && now >= _nextBlinkAt) {
        _blinking = true;
        _blinkStart = now;
      }
      if (_blinking) {
        uint32_t t = now - _blinkStart;         // 180 ms blink
        if (t >= 180) {
          _blinking = false;
          _nextBlinkAt = now + 1800 + (esp_random() % 4200);
        } else {
          float p = t / 180.0f;                  // 0..1
          openness = fabsf(1.0f - 2.0f * p);     // 1 -> 0 -> 1
          if (openness < 0.1f) openness = 0.1f;
        }
      }
      // subtle idle bob
    }
    _eyesShut = sleeping;
    int h = max(4, (int)(eyeH * openness));
    int bob = sleeping ? 0 : (int)(2 * sinf(now / 700.0f));
    drawEyePair(cy + bob, eyeW, h);
  }

  // XX eyes — shown when the daemon has credentials but they were rejected.
  // Draws two "×" marks where the normal eyes sit.
  void drawDeadEyes(uint32_t now) {
    const int cx = _frame.width() / 2;
    const int cy = _frame.height() / 2;
    const int gap = EYE_GAP, eyeW = EYE_W, eyeH = EYE_H;
    // subtle pulse so the display doesn't look frozen
    float pulse = 0.85f + 0.15f * sinf(now / 600.0f);
    uint16_t col = COL_EYE;

    for (int side = -1; side <= 1; side += 2) {
      int ex = cx + side * gap;  // eye centre X
      int ey = cy;               // eye centre Y
      int hw = (int)(eyeW * 0.45f * pulse);  // half-width of the X arms
      int hh = (int)(eyeH * 0.45f * pulse);  // half-height
      int th = 4;                             // arm thickness (half)
      // diagonal \ — top-left to bottom-right
      for (int t = -th; t <= th; t++) {
        _frame.drawLine(ex - hw + t, ey - hh, ex + hw + t, ey + hh, col);
        _frame.drawLine(ex - hw, ey - hh + t, ex + hw, ey + hh + t, col);
      }
      // diagonal / — top-right to bottom-left
      for (int t = -th; t <= th; t++) {
        _frame.drawLine(ex + hw + t, ey - hh, ex - hw + t, ey + hh, col);
        _frame.drawLine(ex + hw, ey - hh + t, ex - hw, ey + hh + t, col);
      }
    }
  }

  void drawAuthErrorExtras() {
    _frame.setTextSize(1);
    _frame.setTextColor(COL_EYE, COL_BG);
    const char *msg = "invalid session key";
    _frame.setCursor((_frame.width() - _frame.textWidth(msg)) / 2,
                     _frame.height() - 12);
    _frame.print(msg);
  }

  void drawSleepExtras(uint32_t now) {
    // floating z Z z
    const int cx = _frame.width() / 2 + 62;  // clear of the wider eyes
    const int cy = _frame.height() / 2 - 16;
    static const char *zs[3] = {"z", "Z", "z"};
    for (int i = 0; i < 3; i++) {
      float phase = fmodf(now / 1000.0f + i * 0.8f, 2.4f) / 2.4f;  // 0..1 loop
      int y = cy - (int)(phase * 34);
      int x = cx + i * 13 + (int)(4 * sinf(phase * 6.28f));
      if (phase < 0.85f) {
        _frame.setTextColor(COL_EYE, COL_BG);
        _frame.setTextSize(i == 1 ? 2 : 1);
        _frame.setCursor(x, y);
        _frame.print(zs[i]);
      }
    }
    _frame.setTextSize(1);
    _frame.setTextColor(COL_EYE, COL_BG);
    _frame.setCursor(6, _frame.height() - 12);
    _frame.print(app.hasData ? "sleeping (BLE lost)" : "waiting for BLE...");
  }

  // ---------------- stats ----------------
  static void fmtMoney(char *out, size_t n, float v) {
    if (v >= 1000) snprintf(out, n, "$%.0f", v);
    else if (v >= 100) snprintf(out, n, "$%.0f", v);
    else snprintf(out, n, "$%.1f", v);
  }

  uint16_t budgetColor(float spent, float total) {
    if (total <= 0) return COL_DIM;
    float r = spent / total;
    return r >= 1.0f ? COL_BAD : (r >= 0.9f ? COL_WARN : COL_GOOD);
  }

  // One half-width budget cell + progress bar. labelLeft=true renders
  // "MO X/Y" left-aligned; false renders "X/Y DAY" right-aligned — so a
  // left+right pair reads "MO X/Y   <wide gap>   X/Y DAY" across the screen.
  void drawBudgetCell(int x, int w, int y, const char *label, float spent,
                      float total, bool labelLeft) {
    char a[16], b[16], line[32];
    fmtMoney(a, sizeof a, spent);
    fmtMoney(b, sizeof b, total);
    if (labelLeft) snprintf(line, sizeof line, "%s %s/%s", label, a, b);
    else           snprintf(line, sizeof line, "%s/%s %s", a, b, label);
    _frame.setTextSize(1.5f);  // between the size-2 headline and the size-1 body
    _frame.setTextColor(COL_EYE, COL_BG);
    _frame.setCursor(labelLeft ? x : (x + w - _frame.textWidth(line)), y);
    _frame.print(line);
    // progress bar (half-height)
    int by = y + 14, bh = 3;
    _frame.fillSmoothRoundRect(x, by, w, bh, 1, COL_PANEL);
    float r = total > 0 ? min(1.0f, spent / total) : 0;
    if (r > 0.01f)
      _frame.fillSmoothRoundRect(x, by, (int)(w * r), bh, 1, budgetColor(spent, total));
  }

  // Month model shares on one line: "54% fable | 10% opus | 5% sonnet".
  // Shares the top row with the temp/RSSI readout, which is right-aligned.
  void drawModelLine(const ModelShare *models, int x, int y) {
    char buf[64];
    int n = 0;
    for (int i = 0; i < 3; i++) {
      if (!models[i].name[0]) continue;
      n += snprintf(buf + n, sizeof buf - n, "%s%d%% %s", n ? " | " : "",
                    models[i].pct, models[i].name);
      if (n >= (int)sizeof buf) break;
    }
    if (!n) return;
    _frame.setCursor(x, y);
    _frame.print(buf);
  }

  void drawStats(uint32_t now) {
    const Stats &s = app.stats;
    _frame.setTextSize(1);
    _frame.setTextColor(COL_EYE, COL_BG);

    // Top row, left side: month model shares — or the stale marker in their
    // place, since the right side of the row belongs to the temp/RSSI readout.
    if (millis() - app.lastUpdateMs > DATA_STALE_MS) {
      _frame.setCursor(6, 2);
      _frame.print("data stale...");
    } else {
      drawModelLine(s.monthModels, 6, 2);
    }

    // bottom: usage graphs side by side, below the eyes — month left, day right
    const int gap = 10;
    int cw = (_frame.width() - 12 - gap) / 2;
    int by = _frame.height() - 18;
    drawBudgetCell(6, cw, by, "MO", s.monthSpent, s.monthTotal, /*labelLeft=*/true);
    drawBudgetCell(6 + cw + gap, cw, by, "DAY", s.daySpent, s.dayTotal, /*labelLeft=*/false);
  }
};
