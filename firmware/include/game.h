#pragma once
#include <Arduino.h>
#include <Preferences.h>

#include "config.h"
#include "lgfx_conf.h"
#include "state.h"

// ---------------------------------------------------------------- pixel art
// Everything in the minigame is drawn from '#'-mask bitmaps blitted at
// GAME_PIX pixels per source pixel — no anti-aliased primitives, so the whole
// scene reads as one consistent low-res sprite set instead of smooth blobs on
// a pixel background.
namespace art {

// Claudie, traced off the real mascot on its own 12x8 grid: an 8-wide blocky
// body with square arm stubs poking out both sides at mid-height, two square
// eyes one cell in from each edge, and four 1-cell legs.
inline const char *const CLAUDIE_BODY[] = {
    "..########..",
    "..########..",
    "############",  // arms
    "############",
    "..########..",
    "..########..",
};
// Airborne: arms thrown up a row and the body drawn out one row taller, so the
// silhouette stretches without leaving the 12-cell width.
inline const char *const CLAUDIE_BODY_JUMP[] = {
    "..########..",
    "############",  // arms, raised
    "############",
    "..########..",
    "..########..",
    "..########..",
    "..########..",
};
// Squashed on landing (and how a dead Claudie lies): a row shorter, a cell
// wider each side, arms folded flat into the body.
inline const char *const CLAUDIE_BODY_SQUASH[] = {
    ".##########.",
    "############",
    "############",
    ".##########.",
    ".##########.",
};

// Two-frame run cycle over the four legs: the outer pair and the inner pair
// take turns being planted, the other pair lifts a cell.
inline const char *const CLAUDIE_LEGS_A[] = {
    "..#.#..#.#..",
    "..#....#....",
};
inline const char *const CLAUDIE_LEGS_B[] = {
    "..#.#..#.#..",
    "....#....#..",
};
// All four tucked up to a single row mid-jump / mid-squash.
inline const char *const CLAUDIE_LEGS_TUCK[] = {
    "..#.#..#.#..",
};

// Dead eyes: a 3x3 cross drawn at one screen pixel per cell, so it fits inside
// the single-cell eye instead of swamping the flattened body.
inline const char *const XEYE[] = {
    "#.#",
    ".#.",
    "#.#",
};

// Obstacles: dino-style cacti — a two-cell trunk with arms branching off it.
inline const char *const CACTUS_SMALL[] = {
    ".##.",
    ".##.",
    "#.##",
    "####",
    ".##.",
    ".##.",
};
inline const char *const CACTUS_TALL[] = {
    "..##..",
    "..##..",
    "#.##..",
    "#.##.#",
    "####.#",
    "..####",
    "..##..",
    "..##..",
    "..##..",
};
inline const char *const CACTUS_PAIR[] = {
    "..##.....",
    "..##.....",
    "#.##.....",
    "####.##..",
    "..##.##.#",
    "..##.####",
    "..##.##..",
};

inline const char *const CLOUD[] = {
    "..####..",
    ".######.",
    "########",
    "..####..",
};

}  // namespace art

// Hidden dyno-style runner: Claudie runs right-to-left past obstacles and the
// only control is jump (the same switch that runs the rest of the device).
//
// The game draws into the UI's full-frame sprite — it never touches the panel
// itself, so entering and leaving is just a flag in AppState. The highscore
// lives in NVS (flash) so it survives the light-sleep restart and power cycles.
class Game {
public:
  void begin() {
    Preferences p;
    if (p.begin(GAME_NVS_NS, /*readOnly=*/true)) {
      _high = p.getUShort("hi", 0);
      p.end();
    }
  }

  // Fresh run, back on the "press to start" screen.
  void reset(uint32_t now) {
    _phase = READY;
    _score = 0;
    _speed = GAME_SPEED_MIN;
    _scroll = 0;
    _y = 0;
    _vy = 0;
    _landedAt = now;
    _diedAt = 0;
    _startedAt = now;
    _nextSpawnX = 0;
    for (auto &o : _obst) o.active = false;
  }

  uint16_t highscore() const { return _high; }

  // The switch click: start, jump, or restart after a crash.
  void press(uint32_t now) {
    switch (_phase) {
      case READY:
        _phase = RUN;
        _startedAt = now;
        _nextSpawnX = 150;  // a little breathing room before the first obstacle
        break;
      case RUN:
        if (_y <= 0.01f) {  // only from the ground — no double jumps
          _vy = -GAME_JUMP_V;
          _jumpedAt = now;
        }
        break;
      case DEAD:
        // Ignore the first moments after a crash so the click that killed you
        // can't instantly restart the run.
        if (now - _diedAt > 600) reset(now);
        break;
    }
  }

  // dtMs = wall time since the previous step, so the physics stay honest even
  // if a frame runs long.
  void update(uint32_t now, uint32_t dtMs) {
    float dt = min(dtMs, (uint32_t)80) / 1000.0f;  // clamp: no tunnelling
    _cloudScroll += dt * 12.0f;
    if (_phase != RUN) return;

    _speed = min(GAME_SPEED_MAX, GAME_SPEED_MIN + _score * GAME_SPEED_RAMP);
    _scroll += _speed * dt;

    // jump arc
    if (_y > 0 || _vy < 0) {
      _vy += GAME_GRAVITY * dt;
      _y -= _vy * dt;  // _y is height above the ground
      if (_y <= 0) {
        _y = 0;
        _vy = 0;
        _landedAt = now;
      }
    }

    // score: one point per 20 px travelled
    _score = (uint16_t)min((float)0xFFFE, _scroll / 20.0f);

    // obstacles march left; spawn once the scroll passes the next mark
    for (auto &o : _obst) {
      if (!o.active) continue;
      o.x -= _speed * dt;
      if (o.x + o.w < -4) o.active = false;
    }
    if (_scroll >= _nextSpawnX) {
      spawn();
      // Grows with speed, so a faster run never squeezes the gap below what one
      // jump arc can clear.
      float gap = GAME_GAP_MIN + (esp_random() % GAME_GAP_RAND) +
                  _speed * GAME_GAP_SPEED;
      _nextSpawnX = _scroll + gap;
    }

    if (hits()) {
      _phase = DEAD;
      _diedAt = now;
      if (_score > _high) {
        _high = _score;
        Preferences p;
        if (p.begin(GAME_NVS_NS, /*readOnly=*/false)) {
          p.putUShort("hi", _high);
          p.end();
        }
      }
    }
  }

  void draw(LGFX_Sprite &f, uint32_t now) {
    const int W = f.width(), H = f.height();
    const int groundY = H - 26;
    f.fillSprite(COL_GAME_BG);
    drawClouds(f, W);
    drawGround(f, W, groundY);
    for (const auto &o : _obst)
      if (o.active) drawObstacle(f, (int)o.x, groundY, o.kind);
    drawClaudie(f, CLAUDIE_X, groundY - (int)_y, now);
    drawHud(f, W);
    if (_phase == READY) center(f, H / 2 - 34, "PRESS TO RUN", 1);
    if (_phase == DEAD) {
      center(f, H / 2 - 40, "GAME OVER", 2);
      if (_score >= _high && _score > 0) center(f, H / 2 - 20, "NEW BEST!", 1);
      if ((now / 400) % 2) center(f, H / 2 - 6, "press to retry", 1);
    }
    drawExitHold(f, W, H);
  }

private:
  enum Phase { READY, RUN, DEAD };
  struct Obstacle {
    float x = 0;
    int w = 0, h = 0;   // pixel size, straight from the sprite
    uint8_t kind = 0;   // index into obstArt()
    bool active = false;
  };

  Phase _phase = READY;
  Obstacle _obst[GAME_MAX_OBST];
  float _y = 0, _vy = 0;      // height above ground / vertical speed
  float _scroll = 0;          // total distance travelled, px
  float _nextSpawnX = 0;
  float _speed = GAME_SPEED_MIN;
  float _cloudScroll = 0;
  uint16_t _score = 0, _high = 0;
  uint32_t _startedAt = 0, _jumpedAt = 0, _landedAt = 0, _diedAt = 0;

  // ---------------- simulation helpers ----------------

  // Every Claudie pose shares the mascot's 12-cell width and stands 8 cells
  // tall, so nothing has to be re-centred between frames.
  static constexpr int CLAUDIE_CELLS_W = 12;
  static constexpr int CLAUDIE_W = CLAUDIE_CELLS_W * GAME_PIX;
  static constexpr int CLAUDIE_H = 8 * GAME_PIX;
  // The arms stick out past the body and the legs are thin: collide on the
  // 8-cell body column range only, or every near-miss clips an arm.
  static constexpr int CLAUDIE_HIT_X = 2 * GAME_PIX;
  static constexpr int CLAUDIE_HIT_W = 8 * GAME_PIX;
  static constexpr int CLAUDIE_X = 34;  // fixed screen X — the world scrolls
  // Landscape: the long panel axis is the horizontal one.
  static constexpr int SCREEN_W = LCD_WIDTH > LCD_HEIGHT ? LCD_WIDTH : LCD_HEIGHT;

  void spawn() {
    for (auto &o : _obst) {
      if (o.active) continue;
      // Three flavours: a short cactus, a tall one, a double.
      o.kind = esp_random() % 3;
      ObstArt a = obstArt(o.kind);
      o.w = a.cols * GAME_PIX;
      o.h = a.rows_n * GAME_PIX;
      o.x = SCREEN_W + 2;  // just off-screen right
      o.active = true;
      return;
    }
  }

  // AABB overlap in "height above the ground" space, shrunk by pad on every
  // edge so near-misses feel fair rather than cheap.
  bool hits() const {
    const float pad = 3;
    float cl = CLAUDIE_X + CLAUDIE_HIT_X + pad;
    float cr = CLAUDIE_X + CLAUDIE_HIT_X + CLAUDIE_HIT_W - pad;
    for (const auto &o : _obst) {
      if (!o.active) continue;
      if (cr < o.x + pad || cl > o.x + o.w - pad) continue;  // no x overlap
      if (_y + pad >= o.h) continue;                          // jumped over it
      return true;
    }
    return false;
  }

  // ---------------- drawing ----------------

  // Blit a '#'-mask bitmap, one source pixel to a GAME_PIX square block. Rows
  // above capRows use capCol — that is how the obstacles get their orange top
  // fifth without a second bitmap.
  static void blit(LGFX_Sprite &f, int x, int y, const char *const *rows, int h,
                   uint16_t col, int scale = GAME_PIX, int capRows = 0,
                   uint16_t capCol = 0) {
    for (int r = 0; r < h; r++)
      for (int c = 0; rows[r][c]; c++)
        if (rows[r][c] == '#')
          f.fillRect(x + c * scale, y + r * scale, scale, scale,
                     r < capRows ? capCol : col);
  }

  static void center(LGFX_Sprite &f, int y, const char *s, float size) {
    f.setTextSize(size);
    f.setTextColor(COL_GAME_TEXT, COL_GAME_BG);
    f.setCursor((f.width() - f.textWidth(s)) / 2, y);
    f.print(s);
  }

  // Blocky clouds drifting slower than the ground — cheap parallax.
  void drawClouds(LGFX_Sprite &f, int W) {
    static const int cloud[3][3] = {{0, 16, 2}, {130, 34, 3}, {235, 10, 2}};
    const int span = W + 80;
    for (auto &c : cloud) {
      int x = (int)(c[0] - fmodf(_cloudScroll, (float)span));
      if (x < -80) x += span;
      blit(f, x, c[1], art::CLOUD, 4, COL_GAME_CLOUD, c[2]);
    }
  }

  // Solid line plus scrolling dashes, so speed is readable even mid-jump.
  void drawGround(LGFX_Sprite &f, int W, int groundY) {
    f.fillRect(0, groundY, W, GAME_PIX, COL_GAME_GROUND);
    int off = (int)fmodf(_scroll, 24.0f);
    for (int x = -off; x < W; x += 24) {
      f.fillRect(x, groundY + 3 * GAME_PIX, 5 * GAME_PIX, GAME_PIX, COL_GAME_GROUND);
      f.fillRect(x + 13, groundY + 5 * GAME_PIX, 3 * GAME_PIX, GAME_PIX, COL_GAME_GROUND);
    }
  }

  // Cactus geometry table — index matches Obstacle::kind. The pixel dimensions
  // are what the physics and the collision box use, so the hitbox is exactly
  // the sprite.
  struct ObstArt {
    const char *const *rows;
    int cols, rows_n;
  };
  static ObstArt obstArt(uint8_t kind) {
    switch (kind) {
      case 0: return {art::CACTUS_SMALL, 4, 6};
      case 1: return {art::CACTUS_TALL, 6, 9};
      default: return {art::CACTUS_PAIR, 9, 7};
    }
  }

  // 80 % white with the top fifth in orange, so the obstacles read as the same
  // family as Claudie without being mistaken for her.
  void drawObstacle(LGFX_Sprite &f, int x, int groundY, uint8_t kind) {
    ObstArt a = obstArt(kind);
    int cap = max(1, a.rows_n / 5);  // the orange 20 %
    blit(f, x, groundY - a.rows_n * GAME_PIX, a.rows, a.rows_n, COL_GAME_OBST,
         GAME_PIX, cap, COL_GAME_OBST_A);
  }

  // Claudie: the mascot sprite, in one of four poses — run (two-frame leg
  // cycle plus a one-cell bob), jump (arms up, body stretched, legs tucked),
  // landing squash, dead (flattened, X eyes) — plus a blink while running.
  void drawClaudie(LGFX_Sprite &f, int x, int baseY, uint32_t now) {
    const int P = GAME_PIX;
    bool airborne = _y > 0.01f;
    bool dead = _phase == DEAD;
    bool squash = !dead && !airborne && _phase == RUN && now - _landedAt < 110;
    bool stepB = (now / 110) % 2;
    int bob = (_phase == RUN && !airborne && !squash && stepB) ? -P : 0;

    const char *const *body = art::CLAUDIE_BODY;
    int bodyRows = 6;
    if (airborne) {
      body = art::CLAUDIE_BODY_JUMP;
      bodyRows = 7;
    } else if (dead || squash) {
      body = art::CLAUDIE_BODY_SQUASH;
      bodyRows = 5;
    }

    // Legs hang below the body; a dead Claudie has none left under her.
    const char *const *legs = nullptr;
    int legRows = 0;
    if (!dead) {
      if (airborne || squash) {
        legs = art::CLAUDIE_LEGS_TUCK;
        legRows = 1;
      } else {
        legs = stepB ? art::CLAUDIE_LEGS_B : art::CLAUDIE_LEGS_A;
        legRows = 2;
      }
    }

    // Every pose is 12 cells wide, so the only placement is standing the feet
    // on the ground.
    int by = baseY - (bodyRows + legRows) * P + bob;
    if (legs) blit(f, x, by + bodyRows * P, legs, legRows, COL_GAME_CLAUDE);
    blit(f, x, by, body, bodyRows, COL_GAME_CLAUDE);

    // Square eyes, one cell in from each side of the body, on the second row.
    const int eyeRow = 1;
    if (dead) {
      // Drawn at 1:1 pixels, centred on where each square eye sits.
      blit(f, x + 3 * P, by + eyeRow * P, art::XEYE, 3, COL_EYE, 1);
      blit(f, x + 8 * P, by + eyeRow * P, art::XEYE, 3, COL_EYE, 1);
      return;
    }
    int eyeH = airborne ? 2 : 1;  // wide eyed in the air
    if (_phase == RUN && !airborne && (now % 2600) < 110) {
      // blink: a flat slit across the bottom of the eye cell
      f.fillRect(x + 3 * P, by + (eyeRow + 1) * P - P / 3, P, max(1, P / 3), COL_EYE);
      f.fillRect(x + 8 * P, by + (eyeRow + 1) * P - P / 3, P, max(1, P / 3), COL_EYE);
    } else {
      f.fillRect(x + 3 * P, by + eyeRow * P, P, eyeH * P, COL_EYE);
      f.fillRect(x + 8 * P, by + eyeRow * P, P, eyeH * P, COL_EYE);
    }
  }

  // Highscore top-left, live score top-right (the top-right corner is where
  // the watch face keeps its readout too).
  void drawHud(LGFX_Sprite &f, int W) {
    char buf[16];
    f.setTextSize(1);
    f.setTextColor(COL_GAME_TEXT, COL_GAME_BG);
    snprintf(buf, sizeof buf, "HI %05u", _high);
    f.setCursor(4, 3);
    f.print(buf);
    snprintf(buf, sizeof buf, "%05u", _score);
    f.setCursor(W - 4 - f.textWidth(buf), 3);
    f.print(buf);
  }

  // Same idiom as the watch face: a bar filling along the bottom edge while the
  // button is held, full width = the game quits. Held presses jump first, so the
  // bar only appears after 1 s (already a third full) — short enough to explain
  // the quit, long enough that ordinary jump taps never flash it.
  void drawExitHold(LGFX_Sprite &f, int W, int H) {
    if (app.btnHeldMs < 1000) return;
    float r = min(1.0f, (float)app.btnHeldMs / GAME_EXIT_HOLD_MS);
    f.fillRect(0, H - 3, (int)(W * r), 3, COL_GAME_TEXT);
  }
};
