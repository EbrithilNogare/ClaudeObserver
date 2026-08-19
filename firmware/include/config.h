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
// D6 = GPIO16. Holding the button suspends the device with *light* sleep, not
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
#define BTN_HOLD_MS        5000             // hold this long -> deep sleep
// Going-to-sleep animation is long on purpose: it also gives the user time to
// let go of the button before the low-level wake source is armed.
#define SLEEP_ANIM_MS      5000
#define WAKE_ANIM_MS       2000
#define DATA_STALE_MS      (5 * 60 * 1000)  // no BLE update for 5 min -> stale marker
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
