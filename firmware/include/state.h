#pragma once
#include <Arduino.h>

struct ModelShare {
  char name[12] = "";
  uint8_t pct = 0;
};

struct Stats {
  float monthSpent = 0, monthTotal = 0;
  float daySpent = 0, dayTotal = 0;
  uint32_t sessions = 0;
  ModelShare todayModels[3];
  ModelShare monthModels[3];
};

// Watch face (eyes + optional stats), the icon menu, or one of the minigames.
enum class Screen : uint8_t { Face, Menu, Game, Jump };

// Menu entries, left to right on screen.
enum MenuItem : uint8_t { MENU_GAME, MENU_JUMP, MENU_POWER, MENU_BACK, MENU_COUNT };

struct AppState {
  volatile bool connected = false;
  volatile bool hasData = false;
  volatile bool authError = false;         // daemon: creds present but rejected
  uint32_t lastUpdateMs = 0;
  volatile uint16_t connHandle = 0xFFFF;  // BLE_HS_CONN_HANDLE_NONE
  int8_t rssi = 0;                        // BLE signal strength (dBm), <0
  float espTempC = 0;                     // ESP32 internal temperature
  float battV = 0;                        // battery volts (A1 reading * 2)
  uint8_t battPct = 0;                    // 0-100 %, mapped over 3.3-4.1 V
  bool showData = true;                   // false = eyes-only view (button click)
  uint32_t btnHeldMs = 0;                 // >0 while the button is held down
  Screen screen = Screen::Face;           // what owns the display right now
  uint8_t menuSel = 0;                    // highlighted menu item (MenuItem)
  uint32_t menuInputAt = 0;               // last button activity in the menu
  Stats stats;
};

extern AppState app;
