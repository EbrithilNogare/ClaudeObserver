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
#include "button.h"
#include "game.h"
#include <esp_sleep.h>
#include <driver/gpio.h>

AppState app;
static UI ui;
static Button button;
static Game game;

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

// ---------------------------------------------------------------- button

// Suspend on a button hold: park the display, drop the radio, then light sleep
// with the button armed as the wake source.
//
// Light sleep rather than deep sleep on purpose — the C6 can only wake from
// deep sleep on the LP IO pads (GPIO0-GPIO7) and the button is on D6/GPIO16,
// which light-sleep GPIO wakeup handles on any digital pin.
//
// Waking restarts the chip instead of resuming in place. Resuming would mean
// rebuilding the BLE stack after its deinit, which is fragile; a restart gets
// us a clean stack, a fresh advertisement and a known-good button state for the
// price of re-fetching the stats from the daemon. setup() fades in with the wake
// animation on every boot, so the restart looks like a wake-up either way.
static void enterLightSleep() {
  Serial.println("[btn] hold -> light sleep");
  app.btnHeldMs = 0;      // drop the hold bar for the animation
  ui.playSleepAnim();     // 5 s eyes-closing fade, ends with the panel dark
  // The BLE controller holds a power-management lock that would keep the chip
  // from actually sleeping, so the radio has to go down first.
  NimBLEDevice::stopAdvertising();
  NimBLEDevice::deinit(true);

  // Don't sleep while the switch is still closed, or the LOW-level wake source
  // would fire the instant we go down. The 5 s animation above means this has
  // normally already happened.
  button.waitForRelease();

  // Keep the pad on its active config (input + pull-up) through the sleep,
  // then wake on the button being pulled LOW.
  gpio_sleep_sel_dis((gpio_num_t)PIN_BUTTON);
  gpio_wakeup_enable((gpio_num_t)PIN_BUTTON, GPIO_INTR_LOW_LEVEL);
  esp_sleep_enable_gpio_wakeup();
  Serial.printf("[btn] sleeping, wake on GPIO%d LOW\n", PIN_BUTTON);
  Serial.flush();

  // Sleep until the button pulls the pin LOW. Anything else that returns from
  // light sleep (or a refused sleep) is retried rather than treated as a press,
  // but never forever — falling through to the restart still lands the device
  // back in its normal running state.
  for (int attempt = 0; attempt < 10; attempt++) {
    esp_err_t err = esp_light_sleep_start();
    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    Serial.printf("[btn] light sleep returned %s, cause %d\n",
                  esp_err_to_name(err), (int)cause);
    if (err == ESP_OK && cause == ESP_SLEEP_WAKEUP_GPIO) break;
    delay(100);  // a refused sleep must not spin
  }

  gpio_wakeup_disable((gpio_num_t)PIN_BUTTON);
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_GPIO);
  Serial.println("[btn] woke -> restart");
  Serial.flush();
  esp_restart();  // never returns; setup() plays the wake animation
}

// ---------------------------------------------------------------- minigame

// The hidden runner takes over the screen; the button switches to the game
// thresholds (3 s down quits, no secret window) and the BLE stack keeps running
// untouched in the background, so the stats are fresh again on the way out.
static void enterGame(uint32_t now) {
  Serial.println("[game] secret hold -> dyno minigame");
  app.gameActive = true;
  game.reset(now);
  button.setThresholds(GAME_EXIT_HOLD_MS, GAME_EXIT_HOLD_MS);
}

static void exitGame() {
  Serial.printf("[game] hold -> exit (highscore %u)\n", game.highscore());
  app.gameActive = false;
  button.setThresholds(BTN_HOLD_MS, BTN_SECRET_MS);
  button.waitForRelease();  // the exit hold must not also click the watch face
  app.btnHeldMs = 0;
}

static void updateButton(uint32_t now) {
  Button::Event ev = button.update(now);
  app.btnHeldMs = button.heldMs(now);
  if (app.gameActive) {
    // Jump on the down edge, not the release — waiting for the release costs a
    // whole reaction time and the jump feels late. The release itself (CLICK)
    // is therefore ignored, or every tap would jump twice.
    if (ev == Button::PRESS) game.press(now);
    else if (ev == Button::HOLD) exitGame();
    return;
  }
  if (ev == Button::CLICK) {
    app.showData = !app.showData;
    Serial.printf("[btn] click -> %s\n", app.showData ? "eyes + data" : "eyes only");
  } else if (ev == Button::SECRET) {
    enterGame(now);
  } else if (ev == Button::HOLD) {
    enterLightSleep();
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
  button.begin();
  antennaBegin();
  analogSetPinAttenuation(PIN_BATT_ADC, ADC_11db);  // full-scale ~3.1 V at the pin
  // Every boot fades in the same way, whether it is a cold start or the restart
  // that ends a light sleep.
  ui.begin(/*dark=*/true);   // start dark so the wake animation can fade in
  game.begin();              // load the minigame highscore from flash
  ui.playWakeAnim();         // 2 s eyes-opening fade
  button.waitForRelease();   // a wake press must not also count as a click
  bleBegin();
}

void loop() {
  uint32_t now = millis();
  updateButton(now);
  updateSensors(now);
  if (app.gameActive) {
    static uint32_t lastGame = 0;
    if (!lastGame) lastGame = now;
    game.update(now, now - lastGame);
    lastGame = now;
    game.draw(ui.frame(), now);
    ui.push();
  } else {
    ui.render(now);
  }
  static uint32_t last = 0;
  if (now - last < FRAME_MS) delay(FRAME_MS - (now - last));
  last = millis();
}
