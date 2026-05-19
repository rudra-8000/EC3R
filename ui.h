#pragma once
/*
 * UI rendering — word-wrap text, image display, menu, reading mode.
 *
 * Display: portrait 300 × 400
 * Font: TomThumb — 5×7 pixels, ~6px wide × 8px tall with spacing
 *   → ~48 chars/line, ~48 text lines per page (1 line reserved for status)
 *
 * Page-turn is O(n) text scan, no full document buffering.
 * A ring cache of 8 page-start offsets enables PREV without rescanning.
 */

#include <Arduino.h>
#include <GxEPD2_BW.h>
// #include <GxEPD2_420_GDEY042T81.h>
#include <Fonts/TomThumb.h>
#include <TJpg_Decoder.h>
#include "bookstate.h"
#include "epub.h"
#include "buttons.h"

#define DISP_W   300
#define DISP_H   400
#define FONT_W   6      // TomThumb char advance (px)
#define FONT_H   8      // line height inc. leading
#define MARGIN_X 3
#define MARGIN_Y 7
#define COLS     ((DISP_W - 2*MARGIN_X) / FONT_W)   // 49 chars
#define ROWS     ((DISP_H - MARGIN_Y - 6) / FONT_H)  // ~48 lines
#define TEXT_ROWS (ROWS - 1)  // reserve 1 for status bar
#define MENU_VIS 12           // visible items in menu

// ── Forward declarations ──────────────────────────────────────────────────
typedef GxEPD2_BW<GxEPD2_420_GDEY042T81, GxEPD2_420_GDEY042T81::HEIGHT> Disp;

void uiDrawMenu(Disp& d, const AppState& s);
void uiDrawWifiInfo(Disp& d);
void uiOpenBook(Disp& d, AppState& s);
void handleMenuInput(Disp& d, AppState& s, ButtonEvent ev);
void handleReadingInput(Disp& d, AppState& s, ButtonEvent ev);
void handleWifiInfoInput(Disp& d, AppState& s, ButtonEvent ev);

// ── Text page renderer ────────────────────────────────────────────────────
// Renders text from textBuf[startOff] word-wrapped onto the display.
// Returns byte offset of the first character that did NOT fit (next page start).
static int renderTextPage(Disp& d, const char* buf, int bufLen,
                           int startOff, int pageNum, const char* title) {
  d.setFont(&TomThumb);
  d.setTextColor(GxEPD_BLACK);
  d.fillScreen(GxEPD_WHITE);

  // Status bar (bottom)
  char status[56];
  char t[22]; strncpy(t, title, 21); t[21] = '\0';
  snprintf(status, sizeof(status), "%-21s  p%d", t, pageNum + 1);
  d.setCursor(MARGIN_X, DISP_H - 2);
  d.print(status);

  int row = 0, col = 0, i = startOff;
  while (i < bufLen && row < TEXT_ROWS) {
    char c = buf[i];

    // Newline
    if (c == '\n' || c == '\r') {
      if (c == '\n') { row++; col = 0; }
      i++; continue;
    }
    // Skip leading whitespace on new line
    if ((c == ' ' || c == '\t') && col == 0) { i++; continue; }

    // Find end of word
    int wEnd = i;
    while (wEnd < bufLen && buf[wEnd] != ' ' &&
           buf[wEnd] != '\n' && buf[wEnd] != '\r') wEnd++;
    int wLen = wEnd - i;

    // Wrap
    if (col + wLen > COLS && col > 0) {
      row++; col = 0;
      if (row >= TEXT_ROWS) break;
    }
    // Very long word: force break
    if (wLen > COLS) {
      int r = COLS - col;
      d.setCursor(MARGIN_X + col * FONT_W, MARGIN_Y + row * FONT_H);
      for (int k = 0; k < r && i < bufLen; k++, i++) d.print(buf[i]);
      row++; col = 0; continue;
    }

    // Print word
    d.setCursor(MARGIN_X + col * FONT_W, MARGIN_Y + row * FONT_H);
    for (int k = 0; k < wLen; k++) d.print(buf[i + k]);
    col += wLen; i = wEnd;

    // Trailing space
    if (i < bufLen && buf[i] == ' ' && col < COLS) {
      d.print(' '); col++; i++;
    }
  }
  return i;
}

// Quick page-size measurement (no drawing) — mirrors renderTextPage logic
static int measurePage(const char* buf, int bufLen, int startOff) {
  int row = 0, col = 0, i = startOff;
  while (i < bufLen && row < TEXT_ROWS) {
    char c = buf[i];
    if (c == '\n' || c == '\r') { if (c == '\n') { row++; col = 0; } i++; continue; }
    if ((c == ' ' || c == '\t') && col == 0) { i++; continue; }
    int wEnd = i;
    while (wEnd < bufLen && buf[wEnd] != ' ' &&
           buf[wEnd] != '\n' && buf[wEnd] != '\r') wEnd++;
    int wLen = wEnd - i;
    if (col + wLen > COLS && col > 0) { row++; col = 0; if (row >= TEXT_ROWS) break; }
    if (wLen > COLS) { i += COLS - col; row++; col = 0; continue; }
    col += wLen; i = wEnd;
    if (i < bufLen && buf[i] == ' ' && col < COLS) { col++; i++; }
  }
  return i;
}

// ── JPEG image renderer (TJpgDec) ─────────────────────────────────────────
static Disp*   _jpgDisp = nullptr;
static int16_t _jpgX0, _jpgY0;
static uint16_t _jpgMaxW, _jpgMaxH;

static bool _jpgCb(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t* bmp) {
  if (!_jpgDisp) return false;
  for (int by = 0; by < h; by++) {
    for (int bx = 0; bx < w; bx++) {
      uint16_t px = bmp[by * w + bx];
      uint8_t r = (px >> 11) & 0x1F;
      uint8_t g = (px >>  5) & 0x3F;
      uint8_t b = (px      ) & 0x1F;
      uint8_t lum = (uint8_t)((r * 9 + g * 19 + b * 4) >> 5); // ~0.3R+0.59G+0.11B scaled
      int16_t px_ = _jpgX0 + x + bx;
      int16_t py_ = _jpgY0 + y + by;
      if (px_ >= 0 && px_ < DISP_W && py_ >= 0 && py_ < DISP_H - FONT_H)
        _jpgDisp->drawPixel(px_, py_, lum < 128 ? GxEPD_BLACK : GxEPD_WHITE);
    }
  }
  return true;
}

static void renderImagePage(Disp& d, const uint8_t* jpgBuf, size_t jpgLen,
                             const char* title) {
  d.fillScreen(GxEPD_WHITE);
  _jpgDisp = &d;
  uint16_t iw = 0, ih = 0;
  TJpgDec.getJpgSize(&iw, &ih, jpgBuf, jpgLen);
  if (iw == 0 || ih == 0) return;
  // Choose scale factor (TJpgDec supports 1/1 1/2 1/4 1/8)
  uint8_t scale = 1;
  while (iw / scale > (uint16_t)DISP_W || ih / scale > (uint16_t)(DISP_H - FONT_H)) scale *= 2;
  if (scale > 8) scale = 8;
  uint16_t sw = iw / scale, sh = ih / scale;
  _jpgX0 = (DISP_W - sw) / 2;
  _jpgY0 = (DISP_H - FONT_H - sh) / 2;
  TJpgDec.setCallback(_jpgCb);
  TJpgDec.setJpgScale(scale);
  TJpgDec.drawJpg(0, 0, jpgBuf, jpgLen);
  // Caption
  d.setFont(&TomThumb); d.setTextColor(GxEPD_BLACK);
  d.setCursor(MARGIN_X, DISP_H - 2);
  d.print(title);
}

// ── Reader session state ───────────────────────────────────────────────────
#define PAGE_CACHE 10

struct ReaderSession {
  EpubInfo info;
  int      chIdx;         // current chapter index
  int      pageNum;       // display page number (for status bar)
  long     byteOff;       // start byte of current page in textBuf
  long     pageCache[PAGE_CACHE]; // ring of page-start offsets
  int      cHead, cCount;
  char*    textBuf;
  int      textLen;
  bool     hasText;
  bool     isImg;
  uint8_t* imgBuf;
  size_t   imgLen;
  char     title[48];
  char     epubPath[96];
  int      _nextOff;
};

static ReaderSession _ses;

static void sesFree() {
  if (_ses.textBuf) { free(_ses.textBuf); _ses.textBuf = nullptr; }
  if (_ses.imgBuf)  { free(_ses.imgBuf);  _ses.imgBuf  = nullptr; }
  _ses.textLen = 0; _ses.imgLen = 0;
}

static bool sesLoadChapter(int ci) {
  sesFree();
  _ses.hasText = false; _ses.isImg = false;

  ZipFile zip;
  if (!zip.open(_ses.epubPath)) return false;
  const char* href = _ses.info.chapterHrefs[ci];
  char* html = nullptr;
  size_t hLen = zip.extractToHeap(href, &html, 32768);
  zip.close();
  if (!hLen || !html) { if (html) free(html); return false; }

  _ses.textBuf = (char*)malloc(hLen + 1);
  if (!_ses.textBuf) { free(html); return false; }

  char imgSrc[MAX_HREF_LEN] = "";
  _ses.textLen = stripHtml(html, (int)hLen, _ses.textBuf, (int)hLen + 1,
                            imgSrc, MAX_HREF_LEN);
  free(html);

  // Collapse whitespace
  char* p = _ses.textBuf, *q = _ses.textBuf;
  bool ls = false;
  while (*p) {
    if (*p == ' ' || *p == '\t') { if (!ls) { *q++ = ' '; ls = true; } p++; }
    else if (*p == '\n' || *p == '\r') { if (!ls) { *q++ = '\n'; ls = true; } p++; }
    else { *q++ = *p++; ls = false; }
  }
  *q = '\0'; _ses.textLen = q - _ses.textBuf;
  _ses.hasText = (_ses.textLen > 4);

  // Check for image-only chapter
  if (strlen(imgSrc) > 0 && _ses.textLen < 60) {
    char ipath[MAX_PATH_LEN + MAX_HREF_LEN];
    snprintf(ipath, sizeof(ipath), "%s%s", _ses.info.opfBase, imgSrc);
    ZipFile z2;
    if (z2.open(_ses.epubPath)) {
      char* ib = nullptr;
      size_t il = z2.extractToHeap(ipath, &ib, 16384);
      z2.close();
      if (il && ib) {
        _ses.imgBuf = (uint8_t*)ib; _ses.imgLen = il;
        _ses.isImg = true; _ses.hasText = false;
      }
    }
  }
  return true;
}

static void sesCachePush(long offset) {
  int slot = (_ses.cHead + _ses.cCount) % PAGE_CACHE;
  _ses.pageCache[slot] = offset;
  if (_ses.cCount < PAGE_CACHE) _ses.cCount++;
  else _ses.cHead = (_ses.cHead + 1) % PAGE_CACHE;
}

static long sesCachePop() {
  if (_ses.cCount <= 1) return 0;
  _ses.cCount--;
  return _ses.pageCache[(_ses.cHead + _ses.cCount) % PAGE_CACHE];
}

static void sesRender(Disp& d) {
  if (_ses.isImg) {
    d.setFullWindow(); d.firstPage();
    do { renderImagePage(d, _ses.imgBuf, _ses.imgLen, _ses.title); } while (d.nextPage());
    return;
  }
  if (!_ses.hasText) {
    d.setFullWindow(); d.firstPage();
    do {
      d.fillScreen(GxEPD_WHITE);
      d.setFont(&TomThumb); d.setTextColor(GxEPD_BLACK);
      d.setCursor(MARGIN_X, DISP_H / 2);
      d.print("[empty section - press NEXT]");
    } while (d.nextPage());
    return;
  }
  int nextOff = 0;
  d.setFullWindow(); d.firstPage();
  do {
    nextOff = renderTextPage(d, _ses.textBuf, _ses.textLen,
                             (int)_ses.byteOff, _ses.pageNum, _ses.title);
  } while (d.nextPage());
  // Store next offset for forward navigation
  _ses._nextOff = nextOff;  // see note below
}
// NOTE: sesRender stores the next-page offset in _ses._nextOff.
// Add `int _nextOff;` to ReaderSession struct.
// (Kept separate for clarity; add it to the struct definition above.)