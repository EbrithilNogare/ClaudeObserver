#pragma once

// ---------------- pins (Seeed XIAO ESP32-S3, GPIO numbers) ----------------
// LCD VCC goes to the 3V3 pin (a GPIO cannot source the module's current).
#define PIN_LCD_SCK   7   // D8
#define PIN_LCD_MOSI  9   // D10
#define PIN_LCD_CS    4   // D3
#define PIN_LCD_DC    8   // D9
#define PIN_LCD_RST   6   // D5
#define PIN_LCD_BL    3   // D2

// ---------------- display ----------------
#define LCD_WIDTH   170
#define LCD_HEIGHT  320
#define LCD_OFFSET_X 35   // ST7789V2 170x320 panels are offset in 240x320 RAM
#define LCD_ROTATION 1    // landscape 320x170
#define LCD_BRIGHTNESS 255  // backlight PWM duty (0-255); 100%

// ---------------- behaviour ----------------
#define DATA_STALE_MS      (5 * 60 * 1000)  // no BLE update for 5 min -> stale marker
#define FRAME_MS           33               // ~30 fps animations

// External antenna: plug a U.FL antenna into the board's LNA_IN connector —
// the XIAO ESP32-S3 uses it automatically, no code/GPIO switch needed.
// BLE has no "auto" TX power, so we run low (the external antenna gives the
// range back, and it saves battery). Raise toward +9 / +18 for more reach.
#define BLE_TX_POWER_DBM   -9

// Claude-ish palette (RGB565)
#define COL_BG        0xDB88   // warm Claude orange (#D97757-ish)
#define COL_EYE       0x2104   // near black
#define COL_PANEL     0x39C7   // dark panel behind text
#define COL_TEXT      0xFFFF
#define COL_DIM       0xC618
#define COL_GOOD      0x2E8B   // green-ish
#define COL_WARN      0xFDA0   // amber
#define COL_BAD       0xF986   // red-ish
