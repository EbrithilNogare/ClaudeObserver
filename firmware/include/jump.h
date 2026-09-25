#pragma once
#include <Arduino.h>
#include <Preferences.h>

#include "config.h"
#include "game.h"  // art:: — same Claudie sprites, drawn smaller
#include "jump_level.h"
#include "lgfx_conf.h"
#include "state.h"

namespace art {

inline const char *const HEART[] = {
    ".##.##.",
    "#######",
    "#######",
    ".#####.",
    "..###..",
    "...#...",
};

// Knotted bow for the Claudie waiting at the top.
inline const char *const BOW[] = {
    "##.##",
    "#####",
    "##.##",
};

}  // namespace art

// Jump King-style climber: small Claudie works her way up a castle tower. The
// only control is when to jump — an aim marker sweeps left and right above
// her, and a press launches her along it at a fixed strength. The level and its
// physics live in jump_level.h (host-checked by tools/jk_check.cpp); this class
// is the clock, the camera, the finish and the drawing.
//
// A bad jump costs height rather than a life: she falls until something catches
// her, and the full-width checkpoint floor of each act always does. Landing on
// the top floor next to the waiting Claudie ends the run, and the time is the
// score (best time kept in NVS).
class JumpGame {
public:
  void begin() {
    Preferences p;
    if (p.begin(JK_NVS_NS, /*readOnly=*/true)) {
      _bestMs = p.getULong("best", 0);
      p.end();
    }
    // She waits at the end of the top ledge away from the last climb, so the
    // climber lands short of her and walks over.
    const jk::Ledge *top = nullptr, *below = nullptr;
    for (const jk::Ledge &l : jk::LEDGES) {
      if (l.floor == JK_FLOORS) top = &l;
      if (l.floor == JK_FLOORS - 1) below = &l;
    }
    float cx = (top->x0 + top->x1) / 2.0f;
    int side = (below->x0 + below->x1) / 2.0f <= cx ? 1 : -1;
    _missX = cx + side * ((top->x1 - top->x0) / 2 - HALF_W);
    _meetX = _missX - side * (2 * HALF_W + 2);
  }

  uint32_t bestMs() const { return _bestMs; }

  // Back on the ground; the clock starts with the first jump.
  void reset(uint32_t now) {
    _phase = CLIMB;
    _b = jk::Body{};
    _startedAt = _finishMs = 0;
    _newBest = false;
    _checkpoint = 0;
    _checkpointAt = 0;
    _landedAt = now;
    _camY = CAM_MIN;
  }

  // The switch's down edge: jump along the aim, or restart after the finish.
  void press(uint32_t now) {
    if (_phase == WON) {
      if (now - _wonAt > 1500) reset(now);  // let the finish play out first
      return;
    }
    if (!_b.grounded) return;
    jk::launch(_b, jk::aimDeg(now));
    if (!_startedAt) _startedAt = now;
  }

  void update(uint32_t now, uint32_t dtMs) {
    float dt = min(dtMs, (uint32_t)40) / 1000.0f;  // clamp: no tunnelling
    if (_phase == WON) {
      // Walk over to her, then the camera settles onto the scene.
      float d = _meetX - _b.x, step = 40.0f * dt;
      _b.x += fabsf(d) <= step ? d : (d > 0 ? step : -step);
      follow(topY() - 40, dt);
      return;
    }
    if (jk::step(_b, dt)) landed(now);
    follow(_b.y - 55, dt);
  }

  void draw(LGFX_Sprite &f, uint32_t now) {
    drawBackdrop(f);
    for (const jk::Wall &w : jk::WALLS) drawWall(f, w);
    for (const jk::Ledge &l : jk::LEDGES) drawLedge(f, l);
    f.fillRect(0, 0, (int)jk::WALL_L, SCREEN_H, COL_JK_WALL);
    f.fillRect((int)jk::WALL_R, 0, SCREEN_W - (int)jk::WALL_R, SCREEN_H, COL_JK_WALL);

    bool won = _phase == WON;
    uint32_t t = won ? now - _wonAt : 0;
    int hopMe = won ? hop(t, 0) : 0, hopHer = won ? hop(t, 200) : 0;
    drawClaudie(f, _missX, sy(topY()) - hopHer, COL_JK_MISS, STAND, true);
    drawClaudie(f, _b.x, sy(_b.y) - hopMe, COL_GAME_CLAUDE, pose(now), false);

    if (!won) {
      if (_b.grounded) drawAim(f, now);
      drawHud(f, now);
    } else {
      drawFinish(f, now);
    }
    drawExitHold(f);
  }

private:
  enum Phase { CLIMB, WON };
  enum Pose { STAND, AIR, SQUASH };

  static constexpr int SCREEN_W = jk::SCREEN_W;
  static constexpr int SCREEN_H = LCD_WIDTH > LCD_HEIGHT ? LCD_HEIGHT : LCD_WIDTH;
  static constexpr int P = JK_PIX;
  static constexpr int HALF_W = 6 * P;   // 12-cell sprite, arms included
  static constexpr float CAM_MIN = -12;  // ground sits a little above the bottom
  static constexpr int LEDGE_H = 7;      // drawn thickness; only the top collides

  Phase _phase = CLIMB;
  jk::Body _b;
  float _camY = CAM_MIN;  // world height at the bottom screen edge
  float _missX = 0, _meetX = 0;
  bool _newBest = false;
  int _checkpoint = 0;  // highest checkpoint floor reached this run
  uint32_t _startedAt = 0, _landedAt = 0, _wonAt = 0, _finishMs = 0, _bestMs = 0;
  uint32_t _checkpointAt = 0;

  static float topY() { return JK_FLOORS * JK_FLOOR_DY; }
  int sy(float worldY) const { return SCREEN_H - (int)lroundf(worldY - _camY); }
  int floorNow() const { return (int)(_b.y / JK_FLOOR_DY + 0.001f); }

  void landed(uint32_t now) {
    _landedAt = now;
    if (_b.ledge < 0) return;  // a wall top
    const jk::Ledge &l = jk::LEDGES[_b.ledge];
    if (l.checkpoint() && l.floor > _checkpoint) {
      _checkpoint = l.floor;
      _checkpointAt = now;
    }
    if (l.floor < JK_FLOORS) return;
    _phase = WON;
    _wonAt = now;
    _finishMs = now - _startedAt;
    if (!_bestMs || _finishMs < _bestMs) {
      _bestMs = _finishMs;
      _newBest = true;
      Preferences p;
      if (p.begin(JK_NVS_NS, /*readOnly=*/false)) {
        p.putULong("best", _bestMs);
        p.end();
      }
    }
  }

  // Ease the camera toward a target height; it never looks below the ground.
  void follow(float target, float dt) {
    if (_phase == CLIMB) target = constrain(target, CAM_MIN, topY() - (SCREEN_H - 60));
    _camY += (target - _camY) * min(1.0f, dt * 5.0f);
  }

  Pose pose(uint32_t now) const {
    if (!_b.grounded) return AIR;
    return now - _landedAt < 110 ? SQUASH : STAND;
  }

  // Finish celebration: both Claudies hop, a little out of step.
  static int hop(uint32_t t, uint32_t offset) {
    return (int)(5 * fabsf(sinf((t + offset) / 400.0f * PI)));
  }

  static uint32_t hash(int a, int b) {
    uint32_t h = (uint32_t)a * 73856093u ^ (uint32_t)b * 19349663u;
    return h ^ (h >> 13);
  }

  // ---------------- drawing ----------------

  static void fmtTime(char *out, size_t n, uint32_t ms) {
    snprintf(out, n, "%lu.%lus", (unsigned long)(ms / 1000),
             (unsigned long)(ms / 100 % 10));
  }

  static void center(LGFX_Sprite &f, int y, const char *s, float size) {
    f.setTextSize(size);
    f.setTextColor(COL_GAME_TEXT, COL_JK_MORTAR);
    f.setCursor((f.width() - f.textWidth(s)) / 2, y);
    f.print(s);
  }

  // Running-bond brickwork over a screen rect: rows of rowH (the last pixel is
  // mortar), every other row shifted half a brick. Joints sit on absolute x so
  // they don't swim as the rect moves.
  static void bricks(LGFX_Sprite &f, int x, int y, int w, int h, int brickW,
                     int rowH, uint16_t face, uint16_t mortar) {
    f.fillRect(x, y, w, h, face);
    for (int r = 0, ry = y; ry < y + h; r++, ry += rowH) {
      int bottom = min(ry + rowH - 1, y + h);
      if (bottom < y + h) f.drawFastHLine(x, bottom, w, mortar);
      int off = (r & 1) ? brickW / 2 : 0;
      for (int jx = (x / brickW) * brickW + off; jx < x + w; jx += brickW)
        if (jx > x) f.drawFastVLine(jx, ry, bottom - ry, mortar);
    }
  }

  // Castle wall behind everything, scrolling at half the camera speed: dim
  // bricks with the odd lighter one, and a narrow arched window every few rows.
  void drawBackdrop(LGFX_Sprite &f) {
    const int BW = 24, RH = 10;
    float by0 = _camY * 0.5f;  // backdrop height at the bottom screen edge
    f.fillSprite(COL_JK_BRICK);
    int k0 = (int)floorf(by0 / RH) - 1;
    for (int k = k0; k <= k0 + SCREEN_H / RH + 2; k++) {
      int bottom = SCREEN_H - (int)lroundf(k * RH - by0);  // screen y of row k's base
      int top = bottom - RH;
      int off = (k & 1) ? BW / 2 : 0;
      for (int n = -1; n <= SCREEN_W / BW; n++) {
        int x = n * BW + off;
        if (hash(k, n) % 6 == 0) f.fillRect(x + 1, top + 1, BW - 1, RH - 1, COL_JK_BRICK2);
        f.drawFastVLine(x, top, RH, COL_JK_MORTAR);
      }
      f.drawFastHLine(0, top, SCREEN_W, COL_JK_MORTAR);
      // arrow-slit window, anchored to a row so it scrolls with the bricks
      if (((k % 5) + 5) % 5 == 2) {
        int wx = 20 + hash(k, 7) % (SCREEN_W - 40);
        f.fillRect(wx, top - 14, 8, 22, COL_JK_MORTAR);
        f.fillCircle(wx + 3, top - 14, 4, COL_JK_MORTAR);
        f.drawFastHLine(wx - 2, top + 8, 12, COL_JK_BRICK2);  // sill
      }
    }
  }

  // Ledges: white brick, two courses deep. The ground and the checkpoints run
  // wall to wall; the ground is also filled down to the bottom edge, and each
  // checkpoint carries a flag that turns orange once reached.
  void drawLedge(LGFX_Sprite &f, const jk::Ledge &l) {
    int y = sy(l.y());
    if (y < -24 || y > SCREEN_H) return;
    int h = l.floor == 0 ? SCREEN_H - y : LEDGE_H;
    bricks(f, l.x0, y, l.x1 - l.x0, h, 12, 4, COL_JK_PLAT, COL_JK_PLAT_M);
    if (!l.checkpoint()) return;
    const int px = 12;
    uint16_t flag = _checkpoint >= l.floor ? COL_BG : COL_JK_WALL_M;
    f.fillRect(px, y - 20, 2, 20, COL_JK_WALL);
    f.fillTriangle(px + 2, y - 20, px + 2, y - 12, px + 12, y - 16, flag);
  }

  // Walls: pale stone blocks, clearly solid next to the white ledges.
  void drawWall(LGFX_Sprite &f, const jk::Wall &w) {
    int top = sy(w.y1), bottom = sy(w.y0);
    if (bottom < 0 || top > SCREEN_H) return;
    bricks(f, w.x0, top, w.x1 - w.x0, bottom - top, 8, 6, COL_JK_WALL, COL_JK_WALL_M);
  }

  // Small Claudie centred on cx with her feet on footY (screen space).
  void drawClaudie(LGFX_Sprite &f, float cx, int footY, uint16_t col, Pose pose,
                   bool bow) {
    if (footY < -4 || footY > SCREEN_H + 24) return;
    const char *const *body = art::CLAUDIE_BODY, *const *legs = art::CLAUDIE_LEGS_A;
    int bodyRows = 6, legRows = 2;
    if (pose == AIR) {
      body = art::CLAUDIE_BODY_JUMP;
      bodyRows = 7;
      legs = art::CLAUDIE_LEGS_TUCK;
      legRows = 1;
    } else if (pose == SQUASH) {
      body = art::CLAUDIE_BODY_SQUASH;
      bodyRows = 5;
      legs = art::CLAUDIE_LEGS_TUCK;
      legRows = 1;
    }
    int x = (int)cx - HALF_W;
    int by = footY - (bodyRows + legRows) * P;
    art::blit(f, x, by + bodyRows * P, legs, legRows, col, P);
    art::blit(f, x, by, body, bodyRows, col, P);
    int eyeH = pose == AIR ? 2 * P : P;
    f.fillRect(x + 3 * P, by + P, P, eyeH, COL_EYE);
    f.fillRect(x + 8 * P, by + P, P, eyeH, COL_EYE);
    if (bow) art::blit(f, x + 7 * P, by - 2 * P, art::BOW, 3, COL_JK_HEART, P);
  }

  // The aim helper: a dotted ray out of her head, ending in a bigger block.
  // White with a dark rim, so it reads over the backdrop and the white ledges.
  void drawAim(LGFX_Sprite &f, uint32_t now) {
    float a = jk::aimDeg(now) * DEG_TO_RAD;
    float ox = _b.x, oy = sy(_b.y) - 8 * P;  // top of her head
    for (int k = 1; k <= 4; k++) {
      int d = 6 + k * 6, s = k == 4 ? 4 : 2;
      int px = (int)(ox + cosf(a) * d), py = (int)(oy - sinf(a) * d);
      f.fillRect(px - s / 2 - 1, py - s / 2 - 1, s + 2, s + 2, COL_EYE);
      f.fillRect(px - s / 2, py - s / 2, s, s, COL_GAME_TEXT);
    }
  }

  // Best time top-left, floor in the middle, running clock top-right, and a
  // short banner whenever a new checkpoint is reached.
  void drawHud(LGFX_Sprite &f, uint32_t now) {
    char buf[24], t[12];
    f.setTextSize(1);
    f.setTextColor(COL_GAME_TEXT, COL_JK_MORTAR);
    if (_bestMs) {
      fmtTime(t, sizeof t, _bestMs);
      snprintf(buf, sizeof buf, "BEST %s", t);
    } else {
      snprintf(buf, sizeof buf, "BEST --");
    }
    f.setCursor(5, 3);
    f.print(buf);
    snprintf(buf, sizeof buf, "%d/%d", floorNow(), JK_FLOORS);
    center(f, 3, buf, 1);
    fmtTime(buf, sizeof buf, _startedAt ? now - _startedAt : 0);
    f.setCursor(SCREEN_W - 5 - f.textWidth(buf), 3);
    f.print(buf);
    if (!_startedAt) center(f, 20, "PRESS TO JUMP", 1);
    else if (_checkpointAt && now - _checkpointAt < 1500) center(f, 20, "CHECKPOINT", 1);
  }

  // Hearts float up between the two of them, over the result.
  void drawFinish(LGFX_Sprite &f, uint32_t now) {
    uint32_t t = now - _wonAt;
    int midX = (int)((_b.x + _missX) / 2);
    int headY = sy(topY()) - 8 * P;
    for (int k = 0; k < 3; k++) {
      if (t < k * 500u) continue;
      float ph = fmodf((t - k * 500) / 1500.0f, 1.0f);
      art::blit(f, midX - 7 + (int)(4 * sinf(ph * 6.28f)), headY - 10 - (int)(ph * 40),
                art::HEART, 6, COL_JK_HEART, 2);
    }
    char buf[24], tm[12];
    center(f, 12, "SAVED!", 2);
    fmtTime(tm, sizeof tm, _finishMs);
    snprintf(buf, sizeof buf, "TIME %s", tm);
    center(f, 34, buf, 1);
    if (_newBest) center(f, 46, "NEW BEST!", 1);
    if (t > 1500 && (now / 400) % 2) center(f, SCREEN_H - 14, "press to play again", 1);
  }

  // Same exit bar as the dyno game: 3 s down quits back to the watch face.
  void drawExitHold(LGFX_Sprite &f) {
    if (app.btnHeldMs < 1000) return;
    float r = min(1.0f, (float)app.btnHeldMs / GAME_EXIT_HOLD_MS);
    f.fillRect(0, SCREEN_H - 3, (int)(SCREEN_W * r), 3, COL_GAME_TEXT);
  }
};
