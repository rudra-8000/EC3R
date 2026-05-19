/*
 * ESP32-C3 E-Paper eReader
 * WeActStudio GDEY042T81 4.2" 300x400 B/W
 *
 * ── Wiring ───────────────────────────────────────────────────────────────
 *   VCC→3V3, GND→GND
 *   DIN/MOSI → GPIO10 (D10)
 *   CLK/SCK  → GPIO8  (D8)
 *   CS       → GPIO5  (D3)
 *   DC       → GPIO3  (D1)
 *   RST      → GPIO2  (D0)
 *   BUSY     → GPIO4  (D2)
 *
 * ── Buttons (active-LOW, INPUT_PULLUP) ──────────────────────────────────
 *   NEXT   → GPIO6  (D4)
 *   PREV   → GPIO7  (D5)
 *   SELECT → GPIO21 (D6)
 *   BACK   → GPIO20 (D7)
 */

// Give the Arduino loop() task 16 KB of stack instead of the default 8 KB.
// Must be defined BEFORE any #include that pulls in the Arduino framework.
SET_LOOP_TASK_STACK_SIZE(16 * 1024);

#include <Arduino.h>
#include <SPI.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <ArduinoJson.h>
#include <GxEPD2_BW.h>

#include "buttons.h"
#include "bookstate.h"
#include "epub.h"
#include "ZipFile.h"
#include "webserver.h"
#include "ui.h"

// ── Pin Definitions ───────────────────────────────────────────────────────
#define EPD_CS    5
#define EPD_DC    3
#define EPD_RST   2
#define EPD_BUSY  4
#define EPD_MOSI  10
#define EPD_SCK   8

#define BTN_NEXT   6
#define BTN_PREV   7
#define BTN_SELECT 21
#define BTN_BACK   20

#define WIFI_SSID "eReader"
#define WIFI_PASS "readbooks"

GxEPD2_BW<GxEPD2_420_GDEY042T81, GxEPD2_420_GDEY042T81::HEIGHT>
  display(GxEPD2_420_GDEY042T81(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY));

AppState appState;

// ---------------------------------------------------------------------------
// runOnBigStack: run a void(*)(void*) on a temporary 24 KB FreeRTOS task and
// block until it finishes.  Use this for any call that would overflow loopTask.
// ---------------------------------------------------------------------------
struct _BigStackJob { void (*fn)(void*); void* arg; volatile bool done; };

static void _bigStackTrampoline(void* pv) {
  auto* j = (struct _BigStackJob*)pv;
  j->fn(j->arg);
  j->done = true;
  vTaskDelete(nullptr);
}

void runOnBigStack(void (*fn)(void*), void* arg) {
  volatile struct _BigStackJob job = { fn, arg, false };
  xTaskCreate(_bigStackTrampoline, "bigStack",
              24 * 1024,          // 24 KB stack
              (void*)&job,
              tskIDLE_PRIORITY + 1,
              nullptr);
  // Spin-wait; job runs concurrently but we don't touch display/state until done
  while (!job.done) delay(5);
}

void setup() {
  Serial.begin(115200);
  delay(400);

  if (!LittleFS.begin(true)) {
    Serial.println("[FS] LittleFS mount FAILED");
  } else {
    Serial.println("[FS] Mounted OK");
    if (!LittleFS.exists("/books")) LittleFS.mkdir("/books");
  }

  buttonsInit(BTN_NEXT, BTN_PREV, BTN_SELECT, BTN_BACK);

  SPI.begin(EPD_SCK, /*MISO*/-1, EPD_MOSI, EPD_CS);
  display.init(115200, true, 2, false);
  display.setRotation(1);
  display.setFont(nullptr);
  display.setTextSize(1);
  display.setTextColor(GxEPD_BLACK);
  display.setFullWindow();

  loadBookState(appState);

  WiFi.softAP(WIFI_SSID, WIFI_PASS);
  Serial.printf("[WiFi] AP started: %s  IP: %s\n",
                WIFI_SSID, WiFi.softAPIP().toString().c_str());

  webServerInit();

  appState.mode = MODE_MENU;
  uiDrawMenu(display, appState);
}

void loop() {
  ButtonEvent ev = buttonsRead();
  switch (appState.mode) {
    case MODE_MENU:      handleMenuInput(display, appState, ev);     break;
    case MODE_READING:   handleReadingInput(display, appState, ev);  break;
    case MODE_WIFI_INFO: handleWifiInfoInput(display, appState, ev); break;
  }
  delay(40);
}
