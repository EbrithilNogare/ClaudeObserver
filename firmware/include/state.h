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

struct AppState {
  volatile bool connected = false;
  volatile bool hasData = false;
  volatile bool authError = false;         // daemon: creds present but rejected
  uint32_t lastUpdateMs = 0;
  volatile uint16_t connHandle = 0xFFFF;  // BLE_HS_CONN_HANDLE_NONE
  int8_t rssi = 0;                        // BLE signal strength (dBm), <0
  float espTempC = 0;                     // ESP32 internal temperature
  Stats stats;
};

extern AppState app;
