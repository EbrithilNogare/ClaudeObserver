// Host-side check for the jump climber level (include/jump_level.h).
//
//   c++ -std=c++17 -O2 -I include tools/jk_check.cpp -o /tmp/jk_check && /tmp/jk_check
//
// Flood-fills every spot she can stand on, starting from the ground and
// trying the whole aim sweep from each, using the exact physics the firmware
// runs. It then prints, per floor, the widest aim window (in degrees and in
// milliseconds of the sweep) that reaches the next floor from the best spot on that
// floor — the difficulty curve — and fails if the top is unreachable or a
// ledge can never be stood on.
//
// Then it playtests: a simulated player aims for the middle of the best window
// from wherever they stand, waits for the sweep to come round (with a reaction
// delay), and presses with a Gaussian timing error. Hundreds of runs per skill
// level give the success rate and the typical time, jumps and falls.
#include <algorithm>
#include <cstdio>
#include <map>
#include <random>
#include <vector>
#include <queue>
#include <set>
#include <utility>

#include "jump_level.h"

using namespace jk;

static const float DT = FRAME_MS / 1000.0f;
static const float STEP_DEG = 0.5f;

// A standing spot: which surface (ledge i, or ~wall i) and x to the pixel.
using Spot = std::pair<int, int>;

static int surfaceOf(const Body &b) {
  if (b.ledge >= 0) return b.ledge;
  for (int i = 0; i < N_WALLS; i++)
    if (WALLS[i].y1 == (int16_t)b.y && b.x + BODY_HALF > WALLS[i].x0 &&
        b.x - BODY_HALF < WALLS[i].x1)
      return ~i;
  return ~0;
}

static float surfaceY(int s) { return s >= 0 ? LEDGES[s].y() : WALLS[~s].y1; }

static int floorAt(float y) { return (int)(y / JK_FLOOR_DY + 0.001f); }

// Simulate one jump; steps = frames spent in the air.
static bool jumpFrom(int surface, float x, float deg, Body &out, int *steps = nullptr) {
  out = Body{};
  out.x = x;
  out.y = surfaceY(surface);
  launch(out, deg);
  for (int n = 0; n < 2000; n++)
    if (step(out, DT)) {
      if (steps) *steps = n + 1;
      return true;
    }
  return false;
}
static bool jumpFrom(Spot from, float deg, Body &out) {
  return jumpFrom(from.first, from.second, deg, out);
}

// ---------------------------------------------------------------- playtest

static const int N_ANGLES = (int)((JK_AIM_MAX_DEG - JK_AIM_MIN_DEG) / STEP_DEG) + 1;
static float angleAt(int i) { return JK_AIM_MIN_DEG + i * STEP_DEG; }

// Where each angle of the sweep lands from a spot (memoised).
static const std::vector<Spot> &landings(Spot s) {
  static std::map<Spot, std::vector<Spot>> memo;
  auto it = memo.find(s);
  if (it != memo.end()) return it->second;
  std::vector<Spot> v(N_ANGLES, s);
  for (int i = 0; i < N_ANGLES; i++) {
    Body b;
    if (jumpFrom(s, angleAt(i), b)) v[i] = {surfaceOf(b), (int)lroundf(b.x)};
  }
  return memo[s] = v;
}

// Widest run of consecutive angles where pred(angle index) holds.
template <class F>
static void widestRun(F pred, int &bestLo, int &bestLen) {
  bestLo = -1;
  bestLen = 0;
  for (int i = 0; i < N_ANGLES;) {
    if (!pred(i)) { i++; continue; }
    int j = i;
    while (j < N_ANGLES && pred(j)) j++;
    if (j - i > bestLen) { bestLen = j - i; bestLo = i; }
    i = j;
  }
}

static int upRun(Spot s, int *lo = nullptr) {
  int f = floorAt(surfaceY(s.first)), l, n;
  const auto &land = landings(s);
  widestRun([&](int i) { return floorAt(surfaceY(land[i].first)) > f; }, l, n);
  if (lo) *lo = l;
  return n;
}

// The player's choice: the middle of the widest window that climbs a floor;
// if there is none from here, shuffle to the spot with the best such window.
static float plan(Spot s) {
  int lo, n = upRun(s, &lo);
  if (n > 0) return angleAt(lo) + (n - 1) * STEP_DEG / 2;
  const auto &land = landings(s);
  std::vector<int> score(N_ANGLES);
  int best = 0;
  for (int i = 0; i < N_ANGLES; i++)
    best = std::max(best, score[i] = land[i] == s ? 0 : upRun(land[i]));
  widestRun([&](int i) { return score[i] == best; }, lo, n);
  return angleAt(lo) + (n - 1) * STEP_DEG / 2;
}

struct RunResult {
  bool won;
  float seconds;
  int jumps, falls;
};

static RunResult playOnce(std::mt19937 &rng, float sigmaMs, float reactMs) {
  std::normal_distribution<float> err(0, sigmaMs);
  std::uniform_real_distribution<float> startT(0, JK_AIM_PERIOD_MS);
  Body b;
  int surface = 0, jumps = 0, falls = 0;
  double t = startT(rng);  // the sweep is wherever it is when the game starts
  const double start = t;
  while (jumps < 1500 && t - start < 30 * 60 * 1000) {
    Spot s{surface, (int)lroundf(b.x)};
    float target = plan(s);
    // wait for the sweep to reach the target, then press a little off
    double tp = t + reactMs;
    while (fabsf(aimDeg((uint32_t)tp) - target) > 0.6f) tp += 1;
    tp = std::max(t, tp + err(rng));
    float deg = aimDeg((uint32_t)tp);
    int fromFloor = floorAt(b.y), steps = 0;
    Body nb;
    if (!jumpFrom(surface, b.x, deg, nb, &steps)) break;
    jumps++;
    t = tp + steps * FRAME_MS;
    b = nb;
    surface = surfaceOf(b);
    if (floorAt(b.y) < fromFloor) falls++;
    if (surface >= 0 && LEDGES[surface].floor == JK_FLOORS)
      return {true, (float)((t - start) / 1000), jumps, falls};
  }
  return {false, (float)((t - start) / 1000), jumps, falls};
}

static void playtest(const char *label, float sigmaMs) {
  const int RUNS = 400;
  std::mt19937 rng(1234);
  std::vector<float> secs;
  std::vector<int> jumps, falls;
  int wins = 0;
  for (int r = 0; r < RUNS; r++) {
    RunResult res = playOnce(rng, sigmaMs, 250);
    if (!res.won) continue;
    wins++;
    secs.push_back(res.seconds);
    jumps.push_back(res.jumps);
    falls.push_back(res.falls);
  }
  auto med = [](auto v) { std::sort(v.begin(), v.end()); return v.empty() ? 0 : v[v.size() / 2]; };
  auto p90 = [](auto v) { std::sort(v.begin(), v.end()); return v.empty() ? 0 : v[v.size() * 9 / 10]; };
  printf("  %-8s (timing error %3.0f ms): %3d%% finish  time %4.0fs median / %4.0fs p90"
         "  jumps %3d  falls %2d\n",
         label, sigmaMs, wins * 100 / RUNS, med(secs), p90(secs), med(jumps), med(falls));
}

int main() {
  std::set<Spot> seen;
  std::queue<Spot> todo;
  Spot start{0, SCREEN_W / 2};
  seen.insert(start);
  todo.push(start);

  std::map<int, float> bestWindow;  // floor -> widest upward aim window, deg
  std::set<int> stoodOn;
  int topFloorReached = 0;

  while (!todo.empty()) {
    Spot s = todo.front();
    todo.pop();
    stoodOn.insert(s.first);
    float y = surfaceY(s.first);
    int floor = (int)(y / JK_FLOOR_DY + 0.001f);
    if (floor > topFloorReached) topFloorReached = floor;
    if (s.first >= 0 && LEDGES[s.first].floor == JK_FLOORS) continue;  // won

    int up = 0;
    for (float d = JK_AIM_MIN_DEG; d <= JK_AIM_MAX_DEG + 1e-3f; d += STEP_DEG) {
      Body b;
      if (!jumpFrom(s, d, b)) continue;
      if (b.y >= (floor + 1) * JK_FLOOR_DY - 0.5f) up++;  // made a floor
      Spot n{surfaceOf(b), (int)lroundf(b.x)};
      if (seen.insert(n).second) todo.push(n);
    }
    float w = up * STEP_DEG;
    if (w > bestWindow[floor]) bestWindow[floor] = w;
  }

  const float msPerDeg = (JK_AIM_PERIOD_MS / 2.0f) / (JK_AIM_MAX_DEG - JK_AIM_MIN_DEG);
  printf("spots explored: %zu\n", seen.size());
  printf("floor  best upward window\n");
  for (auto &[f, w] : bestWindow)
    if (f < JK_FLOORS) printf("  %2d   %5.1f deg  %4.0f ms\n", f, w, w * msPerDeg);

  bool ok = topFloorReached == JK_FLOORS;
  for (int i = 0; i < N_LEDGES; i++)
    if (!stoodOn.count(i)) {
      printf("ledge %d (floor %d, x %d-%d) can never be stood on\n", i,
             LEDGES[i].floor, LEDGES[i].x0, LEDGES[i].x1);
      ok = false;
    }
  printf(ok ? "OK: top floor reachable\n" : "FAIL: top reached floor %d of %d\n",
         topFloorReached, JK_FLOORS);
  if (!ok) return 1;

  printf("playtest (400 runs each, 250 ms reaction, 30 min cap):\n");
  playtest("skilled", 30);
  playtest("medium", 60);
  playtest("casual", 100);
  return 0;
}
