/*
 * ESP32-C3 E-Paper eReader
 * WeActStudio GDEY042T81 4.2" 300x400 B/W
 *
 * ── Wiring: GDEY042T81 → XIAO ESP32C3 ──────────────────────────────────
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

#include <Arduino.h>
#include <SPI.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <ArduinoJson.h>

// GxEPD2 — GDEY042T81 uses SSD1683 controller, 400x300 pixels
#include <GxEPD2_BW.h>
// #include <GxEPD2_420_GDEY042T81.h>
// static GxEPD2_BW<GxEPD2_420_GDEY042T81, GxEPD2_420_GDEY042T81::HEIGHT>* display = nullptr;

#include <Fonts/TomThumb.h>   // 5pt font from Adafruit GFX

#include "buttons.h"
#include "bookstate.h"
#include "epub.h"
#include "ZipFile.h"
#include "webserver.h"
#include "ui.h"

// ── Pin Definitions ──────────────────────────────────────────────────────
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

// ── WiFi AP credentials ──────────────────────────────────────────────────
#define WIFI_SSID "eReader"
#define WIFI_PASS "readbooks"

// ── Global display object ────────────────────────────────────────────────
GxEPD2_BW<GxEPD2_420_GDEY042T81, GxEPD2_420_GDEY042T81::HEIGHT>
  display(GxEPD2_420_GDEY042T81(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY));

AppState appState;

void setup() {
  Serial.begin(115200);
  delay(400);

  // Mount filesystem
  if (!LittleFS.begin(true)) {
    Serial.println("[FS] LittleFS mount FAILED");
  } else {
    Serial.println("[FS] Mounted OK");
    // Ensure /books dir exists
    if (!LittleFS.exists("/books")) LittleFS.mkdir("/books");
  }

  // Buttons
  buttonsInit(BTN_NEXT, BTN_PREV, BTN_SELECT, BTN_BACK);

  // Display — custom SPI on C3
  SPI.begin(EPD_SCK, /*MISO*/-1, EPD_MOSI, EPD_CS);
  display.init(115200, true, 2, false);
  display.setRotation(0);  // portrait: width=300, height=400
  display.setFont(&TomThumb);
  display.setTextColor(GxEPD_BLACK);
  display.setFullWindow();

  // Load persisted app state + book list
  loadBookState(appState);

  // Start WiFi AP
  WiFi.softAP(WIFI_SSID, WIFI_PASS);
  Serial.printf("[WiFi] AP started: %s  IP: %s\n",
                WIFI_SSID, WiFi.softAPIP().toString().c_str());

  // Start async web server
  webServerInit();

  // Draw main menu
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

  delay(40);
}