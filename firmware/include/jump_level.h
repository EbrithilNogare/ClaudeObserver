#pragma once
// Jump climber physics + the one hand-built level. Plain C++ (no Arduino), so
// tools/jk_check.cpp can compile it on the host and prove every floor is
// reachable after the level is edited.
#include <math.h>
#include <stdint.h>

#include "config.h"

namespace jk {

// World space: y up, ground at 0, x across the landscape panel.
constexpr int SCREEN_W = LCD_WIDTH > LCD_HEIGHT ? LCD_WIDTH : LCD_HEIGHT;
constexpr float WALL_L = 2, WALL_R = SCREEN_W - 2;  // screen edges are walls
constexpr float BODY_HALF = 4 * JK_PIX;  // 8-cell body; the arm stubs don't collide
constexpr float BODY_H = 8 * JK_PIX;

// One-way ledge: she jumps up through it and lands on top.
struct Ledge {
  int16_t x0, x1, floor;  // top surface at floor * JK_FLOOR_DY
  float y() const { return floor * JK_FLOOR_DY; }
  // Wall-to-wall ledges are the checkpoints: nothing falls past them.
  bool checkpoint() const { return x0 <= WALL_L && x1 >= WALL_R && floor > 0 && floor < JK_FLOORS; }
};

// Solid block: bounces her off its sides, bonks her head on its underside,
// and its top is a floor like any other.
struct Wall {
  int16_t x0, y0, x1, y1;
};

// ---------------------------------------------------------------- the level
// 25 floors in four acts, split by full-width checkpoint floors (5, 10, 20)
// that catch every fall from the act above:
//   1-4   wide ledges, two or three per floor — learn the aim
//   6-9   narrower pairs, pillars and backstops to bounce off
//   11-14 a walled chimney: small ledges inside, alternating sides
//   15-19 out of the chimney: a zigzag of small ledges across the tower
//   21-24 lone narrow ledges, long gaps, a low ceiling to keep the arc flat
// clang-format off
inline constexpr Ledge LEDGES[] = {
  {  2, 318,  0},                                  // ground
  // act 1
  { 16, 100,  1}, {130, 190,  1}, {220, 304,  1},
  { 50, 130,  2}, {190, 270,  2},
  { 10,  80,  3}, {125, 195,  3}, {240, 310,  3},
  { 60, 140,  4}, {180, 260,  4},
  {  2, 318,  5},                                  // checkpoint
  // act 2
  { 30,  78,  6}, {240, 288,  6},
  { 84, 132,  7}, {200, 244,  7},
  { 40,  84,  8}, {250, 294,  8},
  { 84, 132,  9}, {214, 254,  9},
  {  2, 318, 10},                                  // checkpoint
  // act 3 — inside the chimney (walls at x 112-122 and 198-208)
  {122, 170, 11},
  {178, 198, 12},
  {122, 142, 13},
  {182, 198, 14},
  // act 3, part two — out of the chimney
  {208, 248, 15},
  {246, 286, 16},
  {208, 248, 17},
  {170, 210, 18},
  {132, 172, 19},
  {  2, 318, 20},                                  // checkpoint
  // act 4
  { 36,  66, 21},
  {112, 138, 22},
  {196, 220, 23},
  {268, 292, 24},
  {150, 250, 25},                                  // top: she waits here
};

inline constexpr Wall WALLS[] = {
  // act 2: a pillar splitting checkpoint 5 and a backstop behind ledge 7-left
  {152, 190, 168, 236},
  {132, 266, 140, 342},
  // act 3: the chimney, open at the bottom so she can hop in from floor 10
  {112, 440, 122, 532},
  {198, 440, 208, 532},
  // act 4: a low ceiling just left of ledge 22 — high, lazy arcs from floor
  // 21 bonk on it, so the approach has to be a flatter, more precise jump
  { 76, 858, 110, 866},
};
// clang-format on
constexpr int N_LEDGES = sizeof(LEDGES) / sizeof(LEDGES[0]);
constexpr int N_WALLS = sizeof(WALLS) / sizeof(WALLS[0]);

// ---------------------------------------------------------------- physics
struct Body {
  float x = SCREEN_W / 2.0f, y = 0, vx = 0, vy = 0;  // y = feet
  bool grounded = true;
  int8_t ledge = 0;  // ledge stood on, -1 when on a wall top
};

// Triangle wave: starts pointing left, sweeps right, and back.
inline float aimDeg(uint32_t now) {
  float ph = (float)(now % JK_AIM_PERIOD_MS) / JK_AIM_PERIOD_MS;
  float tri = fabsf(1.0f - 2.0f * ph);  // 1 -> 0 -> 1
  return JK_AIM_MIN_DEG + (JK_AIM_MAX_DEG - JK_AIM_MIN_DEG) * tri;
}

inline void launch(Body &b, float deg) {
  float a = deg * (float)M_PI / 180.0f;
  b.vx = cosf(a) * JK_JUMP_V;
  b.vy = sinf(a) * JK_JUMP_V;
  b.grounded = false;
}

inline bool overlaps(const Wall &w, float x, float y) {
  return x + BODY_HALF > w.x0 && x - BODY_HALF < w.x1 && y + BODY_H > w.y0 && y < w.y1;
}

inline void settle(Body &b, float y, int8_t ledge) {
  b.y = y;
  b.vx = b.vy = 0;
  b.grounded = true;
  b.ledge = ledge;
}

// One physics step. Horizontal first (side hits bounce), then vertical (a
// falling hit lands on the block, a rising one bonks), then the one-way
// ledges. Returns true on the step she lands.
inline bool step(Body &b, float dt) {
  if (b.grounded) return false;
  b.vy = fmaxf(b.vy - JK_GRAVITY * dt, -JK_MAX_FALL);

  b.x += b.vx * dt;
  if (b.x < WALL_L + BODY_HALF) {
    b.x = WALL_L + BODY_HALF;
    b.vx = fabsf(b.vx) * JK_WALL_BOUNCE;
  } else if (b.x > WALL_R - BODY_HALF) {
    b.x = WALL_R - BODY_HALF;
    b.vx = -fabsf(b.vx) * JK_WALL_BOUNCE;
  }
  for (const Wall &w : WALLS) {
    if (!overlaps(w, b.x, b.y)) continue;
    b.x = b.vx > 0 ? w.x0 - BODY_HALF : w.x1 + BODY_HALF;
    b.vx = -b.vx * JK_WALL_BOUNCE;
  }

  float prevY = b.y;
  b.y += b.vy * dt;
  for (const Wall &w : WALLS) {
    if (!overlaps(w, b.x, b.y)) continue;
    if (b.vy <= 0) {
      settle(b, w.y1, -1);
      return true;
    }
    b.y = w.y0 - BODY_H;  // head against the underside
    b.vy = 0;
  }
  if (b.vy > 0) return false;
  // highest first, so a fast fall past two tops lands on the upper one
  for (int i = N_LEDGES - 1; i >= 0; i--) {
    const Ledge &l = LEDGES[i];
    float ly = l.y();
    if (prevY >= ly && b.y <= ly && b.x + BODY_HALF > l.x0 && b.x - BODY_HALF < l.x1) {
      settle(b, ly, i);
      return true;
    }
  }
  return false;
}

}  // namespace jk
