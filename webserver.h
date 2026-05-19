#pragma once
/*
 * Async web file manager.
 * Routes:
 *   GET  /          → file manager SPA (served from LittleFS /data/index.html)
 *   GET  /list      → JSON array of books
 *   POST /upload    → multipart upload → /books/<filename>
 *   GET  /delete?f= → delete a book
 *   GET  /reboot    → ESP.restart()
 */
#include <Arduino.h>
#include <LittleFS.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>

static AsyncWebServer _srv(80);

inline void webServerInit() {
  // Serve SPA
  _srv.on("/", HTTP_GET, [](AsyncWebServerRequest* req) {
    req->send(LittleFS, "/data/index.html", "text/html");
  });

  // List books
  _srv.on("/list", HTTP_GET, [](AsyncWebServerRequest* req) {
    String json = "[";
    File root = LittleFS.open("/books");
    bool first = true;
    if (root && root.isDirectory()) {
      File f = root.openNextFile();
      while (f) {
        if (!f.isDirectory()) {
          if (!first) json += ",";
          json += "{\"name\":\"";
          json += String(f.name());
          json += "\",\"size\":";
          json += f.size();
          json += "}";
          first = false;
        }
        f = root.openNextFile();
      }
    }
    json += "]";
    req->send(200, "application/json", json);
  });

  // Upload
  _srv.on("/upload", HTTP_POST,
    [](AsyncWebServerRequest* req) {
      req->send(200, "text/plain", "OK");
    },
    [](AsyncWebServerRequest* req, String filename,
       size_t index, uint8_t* data, size_t len, bool final) {
      static File _upFile;
      if (index == 0) {
        // Sanitise filename
        String fn = filename;
        fn.replace("/", "_");
        String path = "/books/" + fn;
        Serial.printf("[Upload] %s\n", path.c_str());
        _upFile = LittleFS.open(path, "w");
      }
      if (_upFile) _upFile.write(data, len);
      if (final && _upFile) { _upFile.close(); Serial.println("[Upload] Done"); }
    }
  );

  // Delete
  _srv.on("/delete", HTTP_GET, [](AsyncWebServerRequest* req) {
    if (req->hasParam("f")) {
      String fn = req->getParam("f")->value();
      fn.replace("/", "_");
      String path = "/books/" + fn;
      LittleFS.remove(path);
      req->send(200, "text/plain", "Deleted");
    } else {
      req->send(400, "text/plain", "Missing f param");
    }
  });

  // Disk space
  _srv.on("/space", HTTP_GET, [](AsyncWebServerRequest* req) {
    size_t total = LittleFS.totalBytes();
    size_t used  = LittleFS.usedBytes();
    char buf[80];
    snprintf(buf, sizeof(buf),
             "{\"total\":%u,\"used\":%u,\"free\":%u}", total, used, total - used);
    req->send(200, "application/json", buf);
  });

  _srv.on("/reboot", HTTP_GET, [](AsyncWebServerRequest* req) {
    req->send(200, "text/plain", "Rebooting...");
    delay(500); ESP.restart();
  });

  _srv.begin();
  Serial.println("[Web] Server started on port 80");
}