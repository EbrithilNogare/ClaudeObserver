#pragma once
#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include "config.h"

// Waveshare 1.9" LCD Module (SKU 23822): ST7789V2, 170x320, SPI
class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ST7789 _panel;
  lgfx::Bus_SPI _bus;

public:
  LGFX() {
    {
      auto cfg = _bus.config();
      cfg.spi_host = SPI2_HOST;
      cfg.spi_mode = 3;           // matches the timing the panel was verified with
      cfg.freq_write = 40000000;  // 40 MHz — normal ST7789 speed
      cfg.freq_read = 16000000;
      cfg.pin_sclk = PIN_LCD_SCK;
      cfg.pin_mosi = PIN_LCD_MOSI;
      cfg.pin_miso = -1;
      cfg.pin_dc = PIN_LCD_DC;
      _bus.config(cfg);
      _panel.setBus(&_bus);
    }
    {
      auto cfg = _panel.config();
      cfg.pin_cs = PIN_LCD_CS;
      cfg.pin_rst = PIN_LCD_RST;
      cfg.panel_width = LCD_WIDTH;
      cfg.panel_height = LCD_HEIGHT;
      cfg.offset_x = LCD_OFFSET_X;
      cfg.offset_y = 0;
      cfg.invert = true;
      cfg.rgb_order = false;
      _panel.config(cfg);
    }
    // Backlight is driven as a plain GPIO in UI::begin() (always full on)
    // rather than via Light_PWM — one less thing that can silently fail.
    setPanel(&_panel);
  }
};
