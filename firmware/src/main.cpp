#include <Arduino.h>
#include <ArduinoJson.h>
#include <NimBLEDevice.h>

#include "config.h"
#if __has_include("secrets.h")
#include "secrets.h"
#else
#include "secrets.example.h"
#endif
#include "state.h"
#include "ui.h"

AppState app;
static UI ui;
static String rxBuffer;

// ---------------------------------------------------------------- payload

static void copyModels(JsonArray src, ModelShare *dst) {
  int i = 0;
  for (JsonArray m : src) {
    if (i >= 3) break;
    strlcpy(dst[i].name, m[0] | "?", sizeof(dst[i].name));
    dst[i].pct = m[1] | 0;
    i++;
  }
  for (; i < 3; i++) dst[i].name[0] = 0;
}

static void applyPayload(const String &json) {
  JsonDocument doc;
  if (deserializeJson(doc, json)) {
    Serial.println("[data] JSON parse error");
    return;
  }
  Stats &s = app.stats;
  s.monthSpent = doc["mb"][0] | 0.0f;
  s.monthTotal = doc["mb"][1] | 0.0f;
  s.daySpent = doc["db"][0] | 0.0f;
  s.dayTotal = doc["db"][1] | 0.0f;
  s.sessions = doc["ses"] | 0;
  copyModels(doc["tm"].as<JsonArray>(), s.todayModels);
  copyModels(doc["mm"].as<JsonArray>(), s.monthModels);
  app.authError = (doc["ae"] | 0) != 0;
  app.hasData = true;
  app.lastUpdateMs = millis();
  Serial.printf("[data] month $%.2f/%.0f\n", s.monthSpent, s.monthTotal);
}

// ---------------------------------------------------------------- BLE

class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer *, NimBLEConnInfo &connInfo) override {
    app.connected = true;
    app.connHandle = connInfo.getConnHandle();
    Serial.println("[ble] mac connected");
  }
  void onDisconnect(NimBLEServer *, NimBLEConnInfo &, int reason) override {
    app.connected = false;
    app.connHandle = 0xFFFF;
    rxBuffer = "";
    Serial.printf("[ble] disconnected (reason %d), advertising again\n", reason);
    NimBLEDevice::startAdvertising();
  }
};

class DataCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic *chr, NimBLEConnInfo &) override {
    std::string chunk = chr->getValue();
    rxBuffer += String(chunk.c_str(), chunk.length());
    int nl;
    while ((nl = rxBuffer.indexOf('\n')) >= 0) {
      applyPayload(rxBuffer.substring(0, nl));
      rxBuffer = rxBuffer.substring(nl + 1);
    }
    if (rxBuffer.length() > 4096) rxBuffer = "";  // runaway guard
  }
};

static void bleBegin() {
  NimBLEDevice::init(BLE_DEVICE_NAME);
  NimBLEDevice::setPower(BLE_TX_POWER_DBM);  // low TX power; external antenna gives range back
  NimBLEDevice::setMTU(517);
  NimBLEServer *server = NimBLEDevice::createServer();
  server->setCallbacks(new ServerCallbacks());
  NimBLEService *svc = server->createService(BLE_SERVICE_UUID);
  NimBLECharacteristic *chr =
      svc->createCharacteristic(BLE_CHAR_UUID, NIMBLE_PROPERTY::WRITE);
  chr->setCallbacks(new DataCallbacks());
  NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
  adv->addServiceUUID(BLE_SERVICE_UUID);
  adv->setName(BLE_DEVICE_NAME);
  adv->enableScanResponse(true);
  adv->start();
  Serial.println("[ble] advertising as " BLE_DEVICE_NAME);
}

// ---------------------------------------------------------------- sensors

// ESP internal temperature + BLE signal strength, polled at 1 Hz (cheap).
static void updateSensors(uint32_t now) {
  static uint32_t last = 0;
  if (now - last < 1000) return;
  last = now;
  app.espTempC = temperatureRead();
  // Battery: one sample per tick, undo the 2:1 divider, then fold it into a
  // running mean of the last ~BATT_AVG_SAMPLES ticks. The mean converges at
  // full weight while the first samples arrive, then decays exponentially.
  float v = analogReadMilliVolts(PIN_BATT_ADC) / 1000.0f * BATT_DIVIDER;
  static float battAvg = 0;
  static uint16_t battN = 0;
  if (battN < BATT_AVG_SAMPLES) battN++;
  battAvg += (v - battAvg) / battN;
  app.battV = battAvg;
  float pct = (app.battV - BATT_MIN_V) / (BATT_MAX_V - BATT_MIN_V) * 100.0f;
  app.battPct = (uint8_t)constrain(pct, 0.0f, 100.0f);
  if (app.connected && app.connHandle != 0xFFFF) {
    int8_t r;
    if (ble_gap_conn_rssi(app.connHandle, &r) == 0) app.rssi = r;
  }
}

// ---------------------------------------------------------------- arduino

// The C6's RF switch must be enabled and pointed at an antenna before the
// radio starts, or BLE range is terrible with either antenna.
static void antennaBegin() {
  pinMode(PIN_RF_SWITCH_EN, OUTPUT);
  digitalWrite(PIN_RF_SWITCH_EN, LOW);  // LOW = RF switch powered
  pinMode(PIN_ANTENNA_SELECT, OUTPUT);
  digitalWrite(PIN_ANTENNA_SELECT, USE_EXTERNAL_ANTENNA ? HIGH : LOW);
}

void setup() {
  Serial.begin(115200);
  Serial.println("[boot] ClaudeObserver starting");
  antennaBegin();
  analogSetPinAttenuation(PIN_BATT_ADC, ADC_11db);  // full-scale ~3.1 V at the pin
  ui.begin();
  bleBegin();
}

void loop() {
  uint32_t now = millis();
  updateSensors(now);
  ui.render(now);
  static uint32_t last = 0;
  if (now - last < FRAME_MS) delay(FRAME_MS - (now - last));
  last = millis();
}
