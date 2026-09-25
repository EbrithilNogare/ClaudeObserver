#pragma once

// ---------------- pins (Seeed XIAO ESP32-C6, GPIO numbers) ----------------
// Same D-pin positions as the old S3 wiring — only the GPIO numbers differ.
// LCD VCC goes to the 3V3 pin (a GPIO cannot source the module's current).
#define PIN_LCD_SCK   22  // D4
#define PIN_LCD_MOSI  18  // D10
#define PIN_LCD_CS    21  // D3
#define PIN_LCD_DC    20  // D9
#define PIN_LCD_RST   23  // D5
#define PIN_LCD_BL    2   // D2
#define PIN_BATT_ADC  1   // A1 — battery through a 2:1 divider (Vbat = 2 * Vadc)
// Push button / switch to GND (internal pull-up, so LOW = pressed).
// D6 = GPIO16. The menu's power item suspends the device with *light* sleep, not
// deep sleep: the C6 can only wake from deep sleep on the low-power IO pads
// GPIO0-GPIO7 (SOC_RTCIO_PIN_COUNT == 8), while light-sleep GPIO wakeup works
// on any digital pin — which is what lets the button live on D6.
#define PIN_BUTTON    16  // D6

// ---------------- display ----------------
#define LCD_WIDTH   170
#define LCD_HEIGHT  320
#define LCD_OFFSET_X 35   // ST7789V2 170x320 panels are offset in 240x320 RAM
#define LCD_ROTATION 3    // landscape 320x170, flipped 180°
#define LCD_BRIGHTNESS 222  // backlight PWM duty (0-255); 100%

// ---------------- behaviour ----------------
#define BTN_DEBOUNCE_MS    25               // mechanical switch settle time
// Watch face: click toggles the stats, holding this long opens the menu.
#define BTN_MENU_HOLD_MS   3000
// Menu: click steps to the next item (wrapping), holding this long picks it.
#define MENU_SELECT_HOLD_MS 2000
// An untouched menu falls back to the watch face after this long.
#define MENU_IDLE_MS       15000
// While the minigame runs, 3 s down quits it and returns to the watch face.
#define GAME_EXIT_HOLD_MS  3000
// Going-to-sleep animation is long on purpose: it also gives the user time to
// let go of the button before the low-level wake source is armed.
#define SLEEP_ANIM_MS      5000
#define WAKE_ANIM_MS       2000             // power-on backlight fade-in
#define EYE_OPEN_MS        1800             // eyes opening once BLE data arrives
#define DATA_STALE_MS      (5 * 60 * 1000)  // no BLE update for 5 min -> stale marker
// A link that carries no writes for this long is dropped from our side. macOS
// can keep a zombie connection up after the daemon has given up on it; while it
// lasts we don't advertise, so the daemon can never find us again. The daemon
// writes every ~30 s, so this only fires on a dead link.
#define LINK_IDLE_KICK_MS  (3 * 60 * 1000)
#define FRAME_MS           33               // ~30 fps animations

// Battery: LiPo read on A1 through a 2:1 resistor divider.
#define BATT_DIVIDER       2.0f
#define BATT_MIN_V         3.3f   // 0 %
#define BATT_MAX_V         4.1f   // 100 %
#define BATT_AVG_SAMPLES   60     // running mean window (1 sample/s)

// Antenna: unlike the S3, the XIAO ESP32-C6 has an RF switch that must be
// driven — GPIO14 LOW enables the switch, GPIO3 selects the antenna
// (LOW = built-in ceramic, HIGH = external U.FL). Set USE_EXTERNAL_ANTENNA
// to 1 if you plugged an antenna into the U.FL connector.
#define PIN_RF_SWITCH_EN     14
#define PIN_ANTENNA_SELECT   3
#define USE_EXTERNAL_ANTENNA 1

// BLE has no "auto" TX power, so we run low (an external antenna gives the
// range back, and it saves battery). Raise toward +9 / +18 for more reach.
#define BLE_TX_POWER_DBM   -9

// Claude-ish palette (RGB565)
#define COL_BG        0xFC89   // Prusa orange (#FF904F) — matches the printed case
#define COL_EYE       0x2104   // near black
#define COL_PANEL     0x39C7   // dark panel behind text
#define COL_TEXT      0xFFFF
#define COL_DIM       0xC618
#define COL_GOOD      0x2E8B   // green-ish
#define COL_WARN      0xFDA0   // amber
#define COL_BAD       0xF986   // red-ish

// ---------------- minigame (dyno-style runner, launched from the menu) ----------------
// Very dark orange ground/sky, Claudie in the normal case orange, obstacles
// mostly white with an orange cap.
#define COL_GAME_BG     0x30A0   // #331400 very dark orange
#define COL_GAME_CLOUD  0x5920   // #5A2400 slightly lighter, background props
#define COL_GAME_GROUND 0x6180   // #663000 ground line
#define COL_GAME_CLAUDE COL_BG   // Claudie = the normal case orange
#define COL_GAME_OBST   0xFFFF   // obstacle body (80 %)
#define COL_GAME_OBST_A COL_BG   // obstacle cap (20 %)
#define COL_GAME_TEXT   0xFFFF

#define GAME_GRAVITY     900.0f  // px/s^2
// Jump apex is v^2/2g, so +20 % height means v * sqrt(1.2) — clears ~73 px,
// well over the 27 px tallest cactus.
#define GAME_JUMP_V      362.0f  // px/s upward impulse
// Difficulty curve: keyframes of {score, scroll speed px/s, min gap s, random
// extra gap s}, linearly interpolated and held after the last one. Gaps are in
// seconds, not pixels, so the rhythm stays readable as the speed climbs.
// Tuned against the old linear ramp: 20 % harder at the start, 50 % harder at
// 1000 points (split evenly between speed and density), and still hardening
// up to 2000. For reference, a perfect player can clear two cacti ~0.22 s apart
// at 420 px/s, so the tightest gap here (0.62 s) always leaves a real window.
#define GAME_PACE { \
    {   0, 156, 1.20f, 0.84f}, \
    { 200, 260, 0.93f, 0.49f}, \
    { 431, 387, 0.78f, 0.33f}, \
    {1000, 411, 0.73f, 0.32f}, \
    {2000, 440, 0.62f, 0.28f}, \
}
#define GAME_PIX         3       // screen pixels per pixel-art (mascot grid) cell
#define GAME_MAX_OBST    4
#define GAME_NVS_NS      "dyno"  // flash namespace for the highscore

// ---------------- jump king (climb to the top floor, launched from the menu) ----------------
// Claudie stands on a platform while an aim marker sweeps left <-> right; a
// press launches her along it at a fixed strength. Ledges are one-way (she
// jumps up through them and lands on top), walls and the screen edges are
// solid, and a missed jump just falls to whatever is below. The level itself
// is hand-built in jump_level.h.
#define JK_FLOORS        25      // floors above the ground; reaching the top one wins
#define JK_FLOOR_DY      38.0f   // px between floors
#define JK_GRAVITY       620.0f  // px/s^2
// Fixed jump strength: apex is v^2/2g ~ 63 px, so the next floor (38 px up) is
// in reach for aims between ~55° and ~125°, and two floors never are.
#define JK_JUMP_V        280.0f  // px/s
#define JK_MAX_FALL      420.0f  // px/s terminal velocity
#define JK_WALL_BOUNCE   0.7f    // share of sideways speed kept off a wall
#define JK_AIM_MIN_DEG   30.0f   // aim sweep, degrees above the horizontal (right)
#define JK_AIM_MAX_DEG   150.0f  // ... and its far end (left)
#define JK_AIM_PERIOD_MS 2800    // one full left -> right -> left sweep
#define JK_PIX           2       // small Claudie: 2 screen px per art cell
#define JK_NVS_NS        "jking" // flash namespace for the best time
// High contrast on purpose: a dim castle backdrop, bright white ledges and
// pale stone walls, so everything she can touch pops off the background.
#define COL_JK_BRICK     0x38E1  // #3A1C0C backdrop brick
#define COL_JK_BRICK2    0x4922  // #4A2612 odd backdrop brick, breaks up the tiling
#define COL_JK_MORTAR    0x1860  // #1C0C04 backdrop mortar, windows, HUD backing
#define COL_JK_PLAT      0xFFFF  // ledge brick
#define COL_JK_PLAT_M    0xAD55  // #A8A8A8 ledge mortar
#define COL_JK_WALL      0xD678  // #D0CCC4 wall stone
#define COL_JK_WALL_M    0x7BCE  // #7C7870 wall mortar
#define COL_JK_MISS      0xFCF8  // #FF9EC4 — the Claudie waiting at the top
#define COL_JK_HEART     COL_BAD
