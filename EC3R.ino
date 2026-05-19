/*
 * ESP32-C3 E-Paper eReader
 * WeActStudio GDEY042T81 4.2" 300x400 B/W
 */

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
// runOnBigStack: dispatch a job to a 48 KB FreeRTOS task and block until done.
// Miniz inflate on RISC-V (ESP32-C3) needs ~36 KB of call-frame depth alone.
// ---------------------------------------------------------------------------
struct _BigStackJob { void (*fn)(void*); void* arg; volatile bool done; };

static void _bigStackTrampoline(void* pv) {
  auto* j = (_BigStackJob*)pv;
  j->fn(j->arg);
  j->done = true;
  vTaskDelete(nullptr);
}

void runOnBigStack(void (*fn)(void*), void* arg) {
  volatile _BigStackJob job = { fn, arg, false };
  xTaskCreate(_bigStackTrampoline, "bigStack",
              48 * 1024 / sizeof(StackType_t),  // 48 KB
              (void*)&job,
              tskIDLE_PRIORITY + 1,
              nullptr);
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

  SPI.begin(EPD_SCK, -1, EPD_MOSI, EPD_CS);
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
