#pragma once
#include <Arduino.h>

enum ButtonEvent {
  BTN_EVT_NONE = 0,
  BTN_EVT_NEXT,
  BTN_EVT_PREV,
  BTN_EVT_SELECT,
  BTN_EVT_BACK
};

static int _pinNext, _pinPrev, _pinSelect, _pinBack;
static uint32_t _lastDebounce = 0;
#define DEBOUNCE_MS 250

inline void buttonsInit(int next, int prev, int sel, int back) {
  _pinNext = next; _pinPrev = prev; _pinSelect = sel; _pinBack = back;
  pinMode(next, INPUT_PULLUP); pinMode(prev,  INPUT_PULLUP);
  pinMode(sel,  INPUT_PULLUP); pinMode(back,  INPUT_PULLUP);
}

inline ButtonEvent buttonsRead() {
  uint32_t now = millis();
  if (now - _lastDebounce < DEBOUNCE_MS) return BTN_EVT_NONE;
  ButtonEvent ev = BTN_EVT_NONE;
  if      (!digitalRead(_pinNext))   ev = BTN_EVT_NEXT;
  else if (!digitalRead(_pinPrev))   ev = BTN_EVT_PREV;
  else if (!digitalRead(_pinSelect)) ev = BTN_EVT_SELECT;
  else if (!digitalRead(_pinBack))   ev = BTN_EVT_BACK;
  if (ev != BTN_EVT_NONE) _lastDebounce = now;
  return ev;
}