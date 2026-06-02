#include <Arduino.h>
SET_LOOP_TASK_STACK_SIZE(24 * 1024);

#include <SPI.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <ArduinoJson.h>
#include <GxEPD2_BW.h>
#include <esp_heap_caps.h>

#include "buttons.h"
#include "bookstate.h"
#include "epub.h"
#include "ZipFile.h"
#include "webserver.h"
#include "ui.h"

// ── Pin Definitions ───────────────────────────────────────────────────────
#define EPD_MOSI  11
#define EPD_SCK   12
#define EPD_MISO  13   // not used by display but needed for SPI init
#define EPD_CS    10
#define EPD_DC     9
#define EPD_RST    3
#define EPD_BUSY  46

#define BTN_NEXT    4
#define BTN_PREV    5
#define BTN_SELECT  6
#define BTN_BACK    7

#define WIFI_SSID "eReader"
#define WIFI_PASS "readbooks"

GxEPD2_BW<GxEPD2_420_GDEY042T81, GxEPD2_420_GDEY042T81::HEIGHT>
  display(GxEPD2_420_GDEY042T81(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY));

AppState appState;

// -------------------------------------------------------------------------
// Run heavy work on the other core with a fat stack.
// Arduino loop runs on one core by default; this worker handles EPUB parsing.
// -------------------------------------------------------------------------
struct PinnedJob {
  void (*fn)(void*);
  void* arg;
  SemaphoreHandle_t done;
};

static void _pinnedJobTask(void* pv) {
  PinnedJob* job = (PinnedJob*)pv;
  job->fn(job->arg);
  xSemaphoreGive(job->done);
  vTaskDelete(nullptr);
}

bool runPinnedJob(void (*fn)(void*), void* arg, BaseType_t core, uint32_t stackBytes) {
  SemaphoreHandle_t done = xSemaphoreCreateBinary();
  if (!done) return false;

  PinnedJob job { fn, arg, done };

  BaseType_t ok = xTaskCreatePinnedToCore(
    _pinnedJobTask,
    "epubWorker",
    stackBytes / sizeof(StackType_t),
    &job,
    2,
    nullptr,
    core
  );

  if (ok != pdPASS) {
    vSemaphoreDelete(done);
    return false;
  }

  xSemaphoreTake(done, portMAX_DELAY);
  vSemaphoreDelete(done);
  return true;
}

void setup() {
  Serial.begin(115200);
  delay(300);

  Serial.printf("[SYS] chip=%s cores=%d cpu=%dMHz\n",
                ESP.getChipModel(),
                ESP.getChipCores(),
                ESP.getCpuFreqMHz());

  if (psramFound()) {
    Serial.printf("[SYS] PSRAM OK total=%u free=%u\n",
                  ESP.getPsramSize(),
                  ESP.getFreePsram());
  } else {
    Serial.println("[SYS] PSRAM NOT FOUND");
  }

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
    case MODE_MENU:
      handleMenuInput(display, appState, ev);
      break;
    case MODE_READING:
      handleReadingInput(display, appState, ev);
      break;
    case MODE_WIFI_INFO:
      handleWifiInfoInput(display, appState, ev);
      break;
  }

  delay(30);
}