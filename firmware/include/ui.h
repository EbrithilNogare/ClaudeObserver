#pragma once
#include "lgfx_conf.h"
#include "state.h"

// Full-frame sprite rendering (internal SRAM) — flicker-free animations.
class UI {
  LGFX _lcd;
  LGFX_Sprite _frame{&_lcd};
  uint32_t _nextBlinkAt = 2000;
  uint32_t _blinkStart = 0;
  bool _blinking = false;

public:
  void begin() {
    ledcAttach(PIN_LCD_BL, 5000, 8);      // 5 kHz, 8-bit PWM backlight
    ledcWrite(PIN_LCD_BL, LCD_BRIGHTNESS);  // ~60% brightness
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

  void render(uint32_t now) {
    _frame.fillSprite(COL_BG);
    if (app.connected && app.hasData) {
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
    drawCorner();
    _frame.pushSprite(0, 0);
  }

private:
  // ---------------- top-right readout ----------------
  // ESP temperature (e.g. "42C") and BLE signal strength (e.g. "-67db").
  void drawCorner() {
    char buf[20];
    if (app.connected)
      snprintf(buf, sizeof buf, "%.0fC %ddb", app.espTempC, app.rssi);
    else
      snprintf(buf, sizeof buf, "%.0fC", app.espTempC);
    _frame.setTextSize(1);
    _frame.setTextColor(COL_EYE, COL_BG);
    _frame.setCursor(_frame.width() - 3 - _frame.textWidth(buf), 2);
    _frame.print(buf);
  }

  // ---------------- eyes ----------------
  // Shared eye geometry, also used by the XX (auth error) variant.
  static constexpr int EYE_GAP = 41;   // centre-to-centre / 2
  static constexpr int EYE_W = 29;
  static constexpr int EYE_H = 73;
  // Stats view: sit midway between the top of the display and the bottom text
  // row, so the eyes stay optically centred in the space that's left.
  static constexpr int EYE_CY_STATS = 76;

  // Two black vertical rounded bars, Claude-style. Blink = height squash.
  void drawEyes(uint32_t now, bool sleeping) {
    const int cx = _frame.width() / 2;
    const int cy = sleeping ? _frame.height() / 2 : EYE_CY_STATS;
    const int gap = EYE_GAP, eyeW = EYE_W, eyeH = EYE_H;

    float openness = 1.0f;
    if (sleeping) {
      // closed, gentle breathing wobble
      openness = 0.08f + 0.03f * sinf(now / 900.0f);
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
    int h = max(4, (int)(eyeH * openness));
    int bob = sleeping ? 0 : (int)(2 * sinf(now / 700.0f));
    for (int side = -1; side <= 1; side += 2) {
      int x = cx + side * gap - eyeW / 2;
      int y = cy - h / 2 + bob;
      _frame.fillSmoothRoundRect(x, y, eyeW, h, min(13, h / 2), COL_EYE);
    }
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
