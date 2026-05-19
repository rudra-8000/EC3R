#pragma once
/*
 * UI rendering — word-wrap text, image display, menu, reading mode.
 *
 * Display: landscape 400 × 300 (setRotation(1))
 * Font: GxEPD2 built-in (nullptr) — 6×8 px
 *   setCursor(x,y) puts TOP-LEFT of char at (x,y) — NOT a baseline.
 */

#include <Arduino.h>
#include <GxEPD2_BW.h>
#include <GxEPD2_420_GDEY042T81.h>
#include <TJpg_Decoder.h>
#include "bookstate.h"
#include "epub.h"
#include "buttons.h"

#define DISP_W      400
#define DISP_H      300
#define FONT_W      6
#define FONT_H      8
#define MARGIN_X    2
#define LINE_H      (FONT_H + 2)         // 10px row height
#define TITLEBAR_H  (FONT_H + 4)         // 12px title bar
// First text row top-Y (below title bar)
#define TEXT_TOP    (TITLEBAR_H + 1)
// Usable rows for text (leave one LINE_H at bottom for status bar)
#define TEXT_ROWS   ((DISP_H - TEXT_TOP - LINE_H) / LINE_H)
// Chars per line
#define COLS        ((DISP_W - 2*MARGIN_X) / FONT_W)
// Menu visible rows
#define MENU_ROWS   ((DISP_H - TITLEBAR_H - 1) / LINE_H)

#define PAGE_CACHE  10

typedef GxEPD2_BW<GxEPD2_420_GDEY042T81, GxEPD2_420_GDEY042T81::HEIGHT> Disp;

struct ReaderSession {
  EpubInfo info;
  int      chIdx;
  int      pageNum;
  long     byteOff;
  long     pageCache[PAGE_CACHE];
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

// ---- Text page renderer --------------------------------------------------
static int renderTextPage(Disp& d, const char* buf, int bufLen,
                           int startOff, int pageNum, const char* title) {
  d.setFont(nullptr); d.setTextSize(1); d.setTextColor(GxEPD_BLACK);
  d.fillScreen(GxEPD_WHITE);

  // Status bar at bottom
  static char status[80];
  snprintf(status, sizeof(status), "%.36s  p%d", title, pageNum + 1);
  // setCursor puts TOP of char; status bar at bottom: DISP_H - LINE_H + 1
  d.setCursor(MARGIN_X, DISP_H - LINE_H + 1);
  d.print(status);

  int row = 0, col = 0, i = startOff;
  while (i < bufLen && row < TEXT_ROWS) {
    char c = buf[i];
    if (c == '\n' || c == '\r') {
      if (c == '\n') { row++; col = 0; }
      i++; continue;
    }
    if ((c == ' ' || c == '\t') && col == 0) { i++; continue; }
    int wEnd = i;
    while (wEnd < bufLen && buf[wEnd] != ' ' &&
           buf[wEnd] != '\n' && buf[wEnd] != '\r') wEnd++;
    int wLen = wEnd - i;
    if (col + wLen > COLS && col > 0) { row++; col = 0; if (row >= TEXT_ROWS) break; }
    if (wLen > COLS) {
      int r = COLS - col;
      d.setCursor(MARGIN_X + col * FONT_W, TEXT_TOP + row * LINE_H);
      for (int k = 0; k < r && i < bufLen; k++, i++) d.print(buf[i]);
      row++; col = 0; continue;
    }
    d.setCursor(MARGIN_X + col * FONT_W, TEXT_TOP + row * LINE_H);
    for (int k = 0; k < wLen; k++) d.print(buf[i + k]);
    col += wLen; i = wEnd;
    if (i < bufLen && buf[i] == ' ' && col < COLS) { d.print(' '); col++; i++; }
  }
  return i;
}

static int measurePage(const char* buf, int bufLen, int startOff) {
  int row = 0, col = 0, i = startOff;
  while (i < bufLen && row < TEXT_ROWS) {
    char c = buf[i];
    if (c == '\n' || c == '\r') { if (c=='\n'){row++;col=0;} i++; continue; }
    if ((c==' '||c=='\t') && col==0) { i++; continue; }
    int wEnd = i;
    while (wEnd < bufLen && buf[wEnd]!=' ' && buf[wEnd]!='\n' && buf[wEnd]!='\r') wEnd++;
    int wLen = wEnd - i;
    if (col+wLen > COLS && col>0) { row++; col=0; if(row>=TEXT_ROWS) break; }
    if (wLen > COLS) { i += COLS-col; row++; col=0; continue; }
    col += wLen; i = wEnd;
    if (i < bufLen && buf[i]==' ' && col<COLS) { col++; i++; }
  }
  return i;
}

// ---- JPEG renderer -------------------------------------------------------
static Disp*   _jpgDisp = nullptr;
static int16_t _jpgX0, _jpgY0;

static bool _jpgCb(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t* bmp) {
  if (!_jpgDisp) return false;
  for (int by=0;by<h;by++) for (int bx=0;bx<w;bx++) {
    uint16_t px=bmp[by*w+bx];
    uint8_t r=(px>>11)&0x1F, g=(px>>5)&0x3F, b=(px)&0x1F;
    uint8_t lum=(uint8_t)((r*9+g*19+b*4)>>5);
    int16_t px_=_jpgX0+x+bx, py_=_jpgY0+y+by;
    if(px_>=0&&px_<DISP_W&&py_>=0&&py_<DISP_H-LINE_H)
      _jpgDisp->drawPixel(px_,py_,lum<128?GxEPD_BLACK:GxEPD_WHITE);
  }
  return true;
}

static void renderImagePage(Disp& d,const uint8_t* jpgBuf,size_t jpgLen,const char* title) {
  d.fillScreen(GxEPD_WHITE); _jpgDisp=&d;
  uint16_t iw=0,ih=0;
  TJpgDec.getJpgSize(&iw,&ih,jpgBuf,jpgLen);
  if(!iw||!ih) return;
  uint8_t scale=1;
  while(iw/scale>(uint16_t)DISP_W||ih/scale>(uint16_t)(DISP_H-LINE_H)) scale*=2;
  if(scale>8) scale=8;
  _jpgX0=(DISP_W-iw/scale)/2; _jpgY0=(DISP_H-LINE_H-ih/scale)/2;
  TJpgDec.setCallback(_jpgCb); TJpgDec.setJpgScale(scale);
  TJpgDec.drawJpg(0,0,jpgBuf,jpgLen);
  d.setFont(nullptr);d.setTextSize(1);d.setTextColor(GxEPD_BLACK);
  d.setCursor(MARGIN_X,DISP_H-LINE_H+1); d.print(title);
}

// ---- Chapter loader ------------------------------------------------------
static bool sesLoadChapter(int ci) {
  sesFree(); _ses.hasText=false; _ses.isImg=false;
  ZipFile zip;
  if(!zip.open(_ses.epubPath)) return false;
  const char* href=_ses.info.chapterHrefs[ci];
  char* html=nullptr;
  size_t hLen=zip.extractToHeap(href,&html,32768);
  zip.close();
  if(!hLen||!html){if(html)free(html);return false;}
  _ses.textBuf=(char*)malloc(hLen+1);
  if(!_ses.textBuf){free(html);return false;}
  static char imgSrc[MAX_HREF_LEN];
  imgSrc[0]='\0';
  _ses.textLen=stripHtml(html,(int)hLen,_ses.textBuf,(int)hLen+1,imgSrc,MAX_HREF_LEN);
  free(html);
  char*p=_ses.textBuf,*q=_ses.textBuf; bool ls=false;
  while(*p){
    if(*p==' '||*p=='\t'){if(!ls){*q++=' ';ls=true;}p++;}
    else if(*p=='\n'||*p=='\r'){if(!ls){*q++='\n';ls=true;}p++;}
    else{*q++=*p++;ls=false;}
  }
  *q='\0';_ses.textLen=q-_ses.textBuf;
  _ses.hasText=(_ses.textLen>4);
  if(strlen(imgSrc)>0&&_ses.textLen<60){
    static char ipath[MAX_PATH_LEN+MAX_HREF_LEN];
    snprintf(ipath,sizeof(ipath),"%s%s",_ses.info.opfBase,imgSrc);
    ZipFile z2;
    if(z2.open(_ses.epubPath)){
      char*ib=nullptr;size_t il=z2.extractToHeap(ipath,&ib,16384);z2.close();
      if(il&&ib){_ses.imgBuf=(uint8_t*)ib;_ses.imgLen=il;_ses.isImg=true;_ses.hasText=false;}
    }
  }
  return true;
}

static void sesCachePush(long o){int s=(_ses.cHead+_ses.cCount)%PAGE_CACHE;_ses.pageCache[s]=o;if(_ses.cCount<PAGE_CACHE)_ses.cCount++;else _ses.cHead=(_ses.cHead+1)%PAGE_CACHE;}
static long sesCachePop(){if(_ses.cCount<=1)return 0;_ses.cCount--;return _ses.pageCache[(_ses.cHead+_ses.cCount)%PAGE_CACHE];}

static void sesRender(Disp& d) {
  if(_ses.isImg){
    d.setFullWindow();d.firstPage();
    do{renderImagePage(d,_ses.imgBuf,_ses.imgLen,_ses.title);}while(d.nextPage());
    return;
  }
  if(!_ses.hasText){
    d.setFullWindow();d.firstPage();
    do{
      d.fillScreen(GxEPD_WHITE);
      d.setFont(nullptr);d.setTextSize(1);d.setTextColor(GxEPD_BLACK);
      d.setCursor(MARGIN_X,DISP_H/2);        d.print("[empty section]");
      d.setCursor(MARGIN_X,DISP_H/2+LINE_H); d.print("Press NEXT to continue");
    }while(d.nextPage());
    return;
  }
  int nextOff=0;
  d.setFullWindow();d.firstPage();
  do{nextOff=renderTextPage(d,_ses.textBuf,_ses.textLen,(int)_ses.byteOff,_ses.pageNum,_ses.title);}while(d.nextPage());
  _ses._nextOff=nextOff;
}

static bool sesAdvanceChapter(Disp& d) {
  _ses.chIdx++;
  if(_ses.chIdx>=_ses.info.chapterCount){
    d.setFullWindow();d.firstPage();
    do{
      d.fillScreen(GxEPD_WHITE);
      d.setFont(nullptr);d.setTextSize(1);d.setTextColor(GxEPD_BLACK);
      d.setCursor(MARGIN_X,DISP_H/2-LINE_H); d.print("--- End of book ---");
      d.setCursor(MARGIN_X,DISP_H/2+2);      d.print("[BACK] to menu");
    }while(d.nextPage());
    return false;
  }
  sesLoadChapter(_ses.chIdx);
  _ses.byteOff=0;_ses.pageNum=0;_ses.cHead=0;_ses.cCount=1;_ses.pageCache[0]=0;
  return true;
}

// ---- uiDrawMenu ----------------------------------------------------------
void uiDrawMenu(Disp& d, const AppState& s) {
  d.setFullWindow(); d.firstPage();
  do {
    d.fillScreen(GxEPD_WHITE);
    d.setFont(nullptr); d.setTextSize(1);

    // Title bar
    d.fillRect(0, 0, DISP_W, TITLEBAR_H, GxEPD_BLACK);
    d.setTextColor(GxEPD_WHITE);
    // setCursor y=1: char top is 1px from display top, fits in TITLEBAR_H=12
    d.setCursor(MARGIN_X, 1);
    d.print("eReader   NEXT/PREV=scroll  SEL=open  BCK=wifi");
    d.setTextColor(GxEPD_BLACK);

    if (s.bookCount == 0) {
      d.setCursor(MARGIN_X, TEXT_TOP + LINE_H);     d.print("No books found.");
      d.setCursor(MARGIN_X, TEXT_TOP + LINE_H * 2); d.print("Connect WiFi: eReader / readbooks");
      d.setCursor(MARGIN_X, TEXT_TOP + LINE_H * 3); d.print("http://192.168.4.1");
    } else {
      for (int i = 0; i < MENU_ROWS && (s.menuScroll + i) < s.bookCount; i++) {
        int bi  = s.menuScroll + i;
        int rowY = TITLEBAR_H + 1 + i * LINE_H;  // top of this row in pixels

        if (bi == s.menuIndex) {
          d.fillRect(0, rowY, DISP_W, LINE_H, GxEPD_BLACK);
          d.setTextColor(GxEPD_WHITE);
        } else {
          d.setTextColor(GxEPD_BLACK);
        }

        // setCursor y = rowY + 1: 1px padding, char top-left at that pixel
        d.setCursor(MARGIN_X, rowY + 1);

        const char* fname = s.books[bi];
        const char* slash = strrchr(fname, '/');
        if (slash) fname = slash + 1;
        static char dname[64];
        strncpy(dname, fname, 63); dname[63]='\0';
        char* dot = strrchr(dname, '.'); if(dot)*dot='\0';

        if (s.openBookIdx == bi) d.print('>');
        d.print(dname);
        d.setTextColor(GxEPD_BLACK);
      }
      if (s.menuScroll > 0) {
        d.setCursor(DISP_W - FONT_W*4, TITLEBAR_H + 1);
        d.print(" [^]" );
      }
      if (s.menuScroll + MENU_ROWS < s.bookCount) {
        d.setCursor(DISP_W - FONT_W*4, DISP_H - LINE_H + 1);
        d.print(" [v]");
      }
    }
  } while (d.nextPage());
}

// ---- uiDrawWifiInfo ------------------------------------------------------
void uiDrawWifiInfo(Disp& d) {
  d.setFullWindow(); d.firstPage();
  do {
    d.fillScreen(GxEPD_WHITE);
    d.setFont(nullptr); d.setTextSize(1);
    d.fillRect(0, 0, DISP_W, TITLEBAR_H, GxEPD_BLACK);
    d.setTextColor(GxEPD_WHITE);
    d.setCursor(MARGIN_X, 1); d.print("WiFi File Manager");
    d.setTextColor(GxEPD_BLACK);
    int y = TEXT_TOP + LINE_H;
    d.setCursor(MARGIN_X, y); y+=LINE_H; d.print("SSID : eReader");
    d.setCursor(MARGIN_X, y); y+=LINE_H; d.print("Pass : readbooks");
    d.setCursor(MARGIN_X, y); y+=LINE_H; d.print("URL  : http://192.168.4.1");
    d.setCursor(MARGIN_X, y); y+=LINE_H; d.print("Upload .epub then tap Reboot");
    d.setCursor(MARGIN_X, y);             d.print("BCK or SEL = return to menu");
  } while (d.nextPage());
}

// ---- uiOpenBook ----------------------------------------------------------
void uiOpenBook(Disp& d, AppState& s) {
  sesFree(); memset(&_ses,0,sizeof(_ses)); _ses.cHead=0; _ses.cCount=0;

  const char* fname = s.books[s.openBookIdx];
  const char* slash = strrchr(fname, '/');
  if(slash) fname=slash+1;
  strncpy(_ses.title,fname,47); _ses.title[47]='\0';
  char* dot=strrchr(_ses.title,'.'); if(dot)*dot='\0';

  snprintf(_ses.epubPath,sizeof(_ses.epubPath),"/books/%s",fname);
  Serial.printf("[Book] Opening: %s\n",_ses.epubPath);

  d.setFullWindow(); d.firstPage();
  do{
    d.fillScreen(GxEPD_WHITE);
    d.setFont(nullptr);d.setTextSize(1);d.setTextColor(GxEPD_BLACK);
    d.setCursor(MARGIN_X,DISP_H/2-LINE_H); d.print("Opening...");
    d.setCursor(MARGIN_X,DISP_H/2);        d.print(_ses.title);
  }while(d.nextPage());

  if(!epubOpen(_ses.epubPath,_ses.info)){
    Serial.println("[Book] epubOpen failed");
    d.setFullWindow(); d.firstPage();
    do{
      d.fillScreen(GxEPD_WHITE);
      d.setFont(nullptr);d.setTextSize(1);d.setTextColor(GxEPD_BLACK);
      d.setCursor(MARGIN_X,DISP_H/2);        d.print("Failed to open EPUB.");
      d.setCursor(MARGIN_X,DISP_H/2+LINE_H); d.print("[BACK] to menu");
    }while(d.nextPage());
    s.mode=MODE_MENU; return;
  }

  _ses.chIdx   = s.progress.chapterIndex;
  _ses.byteOff = s.progress.byteOffset;
  _ses.pageNum = s.progress.pageInChapter;
  if(_ses.chIdx>=_ses.info.chapterCount){_ses.chIdx=0;_ses.byteOff=0;_ses.pageNum=0;}
  sesLoadChapter(_ses.chIdx);
  _ses.pageCache[0]=_ses.byteOff; _ses.cCount=1; _ses.cHead=0;
  sesRender(d);
}

// ---- handleMenuInput -----------------------------------------------------
void handleMenuInput(Disp& d, AppState& s, ButtonEvent ev) {
  if(ev==BTN_EVT_NONE) return;
  bool redraw=false;
  if(ev==BTN_EVT_NEXT){
    if(s.menuIndex<s.bookCount-1){
      s.menuIndex++;
      if(s.menuIndex>=s.menuScroll+MENU_ROWS) s.menuScroll++;
      redraw=true;
    }
  } else if(ev==BTN_EVT_PREV){
    if(s.menuIndex>0){
      s.menuIndex--;
      if(s.menuIndex<s.menuScroll) s.menuScroll--;
      redraw=true;
    }
  } else if(ev==BTN_EVT_SELECT&&s.bookCount>0){
    s.openBookIdx=s.menuIndex;
    if(strcmp(s.progress.filename,s.books[s.menuIndex])!=0){
      strncpy(s.progress.filename,s.books[s.menuIndex],63);
      s.progress.chapterIndex=0;s.progress.pageInChapter=0;s.progress.byteOffset=0;
    }
    s.mode=MODE_READING; uiOpenBook(d,s); return;
  } else if(ev==BTN_EVT_BACK){
    s.mode=MODE_WIFI_INFO; uiDrawWifiInfo(d); return;
  }
  if(redraw) uiDrawMenu(d,s);
}

void handleWifiInfoInput(Disp& d, AppState& s, ButtonEvent ev) {
  if(ev==BTN_EVT_BACK||ev==BTN_EVT_SELECT){s.mode=MODE_MENU;uiDrawMenu(d,s);}
}

void handleReadingInput(Disp& d, AppState& s, ButtonEvent ev) {
  if(ev==BTN_EVT_NONE) return;
  if(ev==BTN_EVT_NEXT){
    long newOff=_ses._nextOff;
    bool needNext=_ses.isImg||!_ses.hasText||(newOff>=_ses.textLen);
    if(needNext){if(!sesAdvanceChapter(d))return;}
    else{sesCachePush(newOff);_ses.byteOff=newOff;_ses.pageNum++;}
    s.progress.chapterIndex=_ses.chIdx;s.progress.pageInChapter=_ses.pageNum;
    s.progress.byteOffset=_ses.byteOff;
    saveBookState(s);sesRender(d);
  } else if(ev==BTN_EVT_PREV){
    if(_ses.pageNum>0&&_ses.cCount>1){_ses.byteOff=sesCachePop();_ses.pageNum--;}
    else if(_ses.chIdx>0){
      _ses.chIdx--;sesLoadChapter(_ses.chIdx);
      _ses.byteOff=0;_ses.pageNum=0;_ses.cHead=0;_ses.cCount=1;_ses.pageCache[0]=0;
      if(_ses.hasText&&_ses.textBuf){
        long off=0;
        while(true){long n=measurePage(_ses.textBuf,_ses.textLen,(int)off);if(n>=_ses.textLen)break;sesCachePush(n);_ses.pageNum++;off=n;}
        _ses.byteOff=off;
      }
    }
    s.progress.chapterIndex=_ses.chIdx;s.progress.pageInChapter=_ses.pageNum;
    s.progress.byteOffset=_ses.byteOff;
    saveBookState(s);sesRender(d);
  } else if(ev==BTN_EVT_BACK){
    sesFree();s.mode=MODE_MENU;uiDrawMenu(d,s);
  }
}
