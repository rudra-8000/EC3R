#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include <LittleFS.h>

#define STATE_FILE  "/state.json"
#define MAX_BOOKS   32

enum AppMode { MODE_MENU = 0, MODE_READING, MODE_WIFI_INFO };

struct BookProgress {
  char filename[64];
  int  chapterIndex;
  int  pageInChapter;
  long byteOffset;
};

struct AppState {
  AppMode      mode;
  int          menuIndex;
  int          menuScroll;
  char         books[MAX_BOOKS][64];
  int          bookCount;
  int          openBookIdx;
  BookProgress progress;
};

inline void saveBookState(const AppState& s) {
  StaticJsonDocument<512> doc;
  doc["openBook"] = (s.openBookIdx >= 0) ? s.books[s.openBookIdx] : "";
  doc["chapter"]  = s.progress.chapterIndex;
  doc["page"]     = s.progress.pageInChapter;
  doc["offset"]   = (long)s.progress.byteOffset;
  File f = LittleFS.open(STATE_FILE, "w");
  if (f) { serializeJson(doc, f); f.close(); }
}

inline void loadBookState(AppState& s) {
  s.mode = MODE_MENU; s.menuIndex = 0; s.menuScroll = 0;
  s.openBookIdx = -1; s.bookCount = 0;
  memset(&s.progress, 0, sizeof(s.progress));

  // Enumerate /books/*.epub
  // On ESP32 LittleFS, entry.name() returns the FULL path e.g. "/books/foo.epub"
  // We store only the bare filename so epubPath = "/books/" + name is correct.
  File root = LittleFS.open("/books");
  if (root && root.isDirectory()) {
    File entry = root.openNextFile();
    while (entry && s.bookCount < MAX_BOOKS) {
      if (!entry.isDirectory()) {
        String fullName = entry.name();  // may be "/books/foo.epub" or "foo.epub"
        // Strip leading directory component if present
        int lastSlash = fullName.lastIndexOf('/');
        String bareName = (lastSlash >= 0) ? fullName.substring(lastSlash + 1) : fullName;
        bareName.toLowerCase();
        if (bareName.endsWith(".epub")) {
          // Store back without toLowerCase so filename is preserved
          String orig = entry.name();
          int ls = orig.lastIndexOf('/');
          String origBare = (ls >= 0) ? orig.substring(ls + 1) : orig;
          strncpy(s.books[s.bookCount], origBare.c_str(), 63);
          s.books[s.bookCount][63] = '\0';
          s.bookCount++;
        }
      }
      entry = root.openNextFile();
    }
  }

  // Load saved progress
  if (!LittleFS.exists(STATE_FILE)) return;
  File f = LittleFS.open(STATE_FILE, "r");
  if (!f) return;
  StaticJsonDocument<512> doc;
  if (deserializeJson(doc, f)) { f.close(); return; }
  f.close();

  const char* ob = doc["openBook"] | "";
  if (strlen(ob) > 0) {
    for (int i = 0; i < s.bookCount; i++) {
      if (strcmp(s.books[i], ob) == 0) {
        s.openBookIdx = i;
        strncpy(s.progress.filename, ob, 63);
        s.progress.chapterIndex  = doc["chapter"] | 0;
        s.progress.pageInChapter = doc["page"]    | 0;
        s.progress.byteOffset    = doc["offset"]  | 0;
        break;
      }
    }
  }
}
