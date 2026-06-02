// SET_LOOP_TASK_STACK_SIZE must be the very first statement, before any #include
SET_LOOP_TASK_STACK_SIZE(24 * 1024);

#include <Arduino.h>
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
#define EPD_MISO  13
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
// PinnedJob: allocated on the HEAP so the pointer is valid for the lifetime
// of the worker task. The calling task blocks on the semaphore; the worker
// frees the job struct and signals done before deleting itself.
// -------------------------------------------------------------------------
struct PinnedJob {
  void (*fn)(void*);
  void*             arg;
  SemaphoreHandle_t done;
};

static void _pinnedJobTask(void* pv) {
  // pv points to a heap-allocated PinnedJob — safe to dereference any time.
  PinnedJob* job = (PinnedJob*)pv;
  job->fn(job->arg);
  xSemaphoreGive(job->done);   // unblock caller
  // Do NOT free(job) here — caller frees after taking the semaphore.
  vTaskDelete(nullptr);
}

// Runs fn(arg) on `core` with `stackBytes` bytes of task stack.
// Blocks until fn returns. Thread-safe via binary semaphore.
bool runPinnedJob(void (*fn)(void*), void* arg, BaseType_t core, uint32_t stackBytes) {
  // Allocate job on heap so the pointer is valid while the worker runs.
  PinnedJob* job = (PinnedJob*)malloc(sizeof(PinnedJob));
  if (!job) { Serial.println("[runPinnedJob] malloc failed"); return false; }

  job->fn   = fn;
  job->arg  = arg;
  job->done = xSemaphoreCreateBinary();
  if (!job->done) { free(job); return false; }

  // xTaskCreatePinnedToCore takes stack depth in WORDS (4 bytes each on S3)
  const uint32_t stackWords = stackBytes / 4;

  BaseType_t ok = xTaskCreatePinnedToCore(
    _pinnedJobTask,
    "epubWorker",
    stackWords,
    job,           // pointer to heap struct — stays valid
    2,             // priority above idle, below Arduino loop (1)
    nullptr,
    core
  );

  if (ok != pdPASS) {
    Serial.printf("[runPinnedJob] xTaskCreate failed (stack=%u words, free heap=%u)\n",
                  stackWords, ESP.getFreeHeap());
    vSemaphoreDelete(job->done);
    free(job);
    return false;
  }

  // Block until worker signals done
  xSemaphoreTake(job->done, portMAX_DELAY);
  vSemaphoreDelete(job->done);
  free(job);
  return true;
}

void setup() {
  Serial.begin(115200);
  delay(300);

  Serial.printf("[SYS] chip=%s cores=%d cpu=%dMHz\n",
                ESP.getChipModel(), ESP.getChipCores(), ESP.getCpuFreqMHz());
  Serial.printf("[SYS] free heap=%u largest block=%u\n",
                ESP.getFreeHeap(), ESP.getMaxAllocHeap());

  if (psramFound()) {
    Serial.printf("[SYS] PSRAM OK total=%u free=%u\n",
                  ESP.getPsramSize(), ESP.getFreePsram());
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
    case MODE_MENU:      handleMenuInput(display, appState, ev);     break;
    case MODE_READING:   handleReadingInput(display, appState, ev);  break;
    case MODE_WIFI_INFO: handleWifiInfoInput(display, appState, ev); break;
  }
  delay(30);
}
