#pragma once
/*
 * Async web file manager — fully self-contained, no LittleFS HTML file needed.
 * Routes:
 *   GET  /          → inline file manager SPA
 *   GET  /list      → JSON array of books
 *   POST /upload    → multipart upload → /books/<filename>
 *   GET  /delete?f= → delete a book
 *   GET  /space     → disk usage JSON
 *   GET  /reboot    → ESP.restart()
 */
#include <Arduino.h>
#include <LittleFS.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>

static AsyncWebServer _srv(80);

static const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>eReader File Manager</title>
<style>
  *{box-sizing:border-box;margin:0;padding:0}
  body{font-family:system-ui,sans-serif;background:#f0f0f0;color:#222;padding:16px}
  h1{font-size:1.3rem;margin-bottom:12px}
  #space{font-size:.85rem;color:#555;margin-bottom:6px}
  .bar-bg{height:10px;background:#ddd;border-radius:5px;margin-bottom:14px}
  .bar-fg{height:10px;background:#4a90e2;border-radius:5px;transition:width .4s}
  table{width:100%;border-collapse:collapse;background:#fff;border-radius:8px;overflow:hidden;box-shadow:0 1px 4px #0002;margin-bottom:16px}
  th,td{padding:10px 12px;text-align:left;font-size:.9rem}
  th{background:#4a90e2;color:#fff;font-weight:600}
  tr:nth-child(even){background:#f7f7f7}
  .del{background:#e74c3c;color:#fff;border:none;padding:4px 10px;border-radius:4px;cursor:pointer;font-size:.8rem}
  .del:hover{background:#c0392b}
  #upload-area{background:#fff;border:2px dashed #4a90e2;border-radius:8px;padding:24px;text-align:center;cursor:pointer;transition:background .2s;margin-bottom:12px}
  #upload-area.drag{background:#e8f0fe}
  #file-input{display:none}
  #progress{display:none;margin-bottom:8px}
  progress{width:100%;height:16px;border-radius:4px}
  #status{font-size:.85rem;color:#555;margin-bottom:12px;min-height:1.2em}
  button.reboot{background:#e67e22;color:#fff;border:none;padding:8px 18px;border-radius:6px;cursor:pointer;font-size:.9rem}
  button.reboot:hover{background:#ca6f1e}
  .empty{color:#999;font-style:italic;padding:12px}
</style>
</head>
<body>
<h1>&#x1F4DA; eReader File Manager</h1>
<div id="space">Loading storage info...</div>
<div class="bar-bg"><div class="bar-fg" id="bar" style="width:0%"></div></div>
<table>
  <thead><tr><th>Book</th><th>Size</th><th></th></tr></thead>
  <tbody id="book-list"><tr><td colspan="3" class="empty">Loading...</td></tr></tbody>
</table>
<div id="upload-area" onclick="document.getElementById('file-input').click()">
  <p>&#x1F4C2; Click or drag &amp; drop .epub files here</p>
  <input type="file" id="file-input" accept=".epub" multiple>
</div>
<div id="progress"><progress id="prog" value="0" max="100"></progress></div>
<div id="status"></div>
<button class="reboot" onclick="reboot()">&#x1F504; Reboot Device</button>

<script>
function fmt(b){
  if(b<1024) return b+'B';
  if(b<1048576) return (b/1024).toFixed(1)+'KB';
  return (b/1048576).toFixed(2)+'MB';
}

function loadSpace(){
  fetch('/space').then(function(r){return r.json();}).then(function(d){
    document.getElementById('space').textContent =
      'Storage: '+fmt(d.used)+' used of '+fmt(d.total)+' ('+fmt(d.free)+' free)';
    document.getElementById('bar').style.width = (d.used/d.total*100).toFixed(1)+'%';
  }).catch(function(){
    document.getElementById('space').textContent = 'Could not load storage info';
  });
}

function loadBooks(){
  fetch('/list').then(function(r){return r.json();}).then(function(books){
    var tb = document.getElementById('book-list');
    if(!books.length){
      tb.innerHTML = '<tr><td colspan="3" class="empty">No books uploaded yet.</td></tr>';
      return;
    }
    tb.innerHTML = books.map(function(b){
      return '<tr><td>'+b.name+'</td><td>'+fmt(b.size)+'</td><td>'+
             '<button class="del" onclick="del(\'' + b.name + '\')">Delete</button></td></tr>';
    }).join('');
  });
}

function del(name){
  if(!confirm('Delete '+name+'?')) return;
  fetch('/delete?f='+encodeURIComponent(name)).then(function(){
    loadBooks(); loadSpace();
  });
}

function reboot(){
  if(!confirm('Reboot device?')) return;
  fetch('/reboot').then(function(){
    document.getElementById('status').textContent = 'Rebooting...';
  });
}

function uploadFiles(files){
  if(!files.length) return;
  var i = 0;
  var prog = document.getElementById('prog');
  var status = document.getElementById('status');
  var progressDiv = document.getElementById('progress');
  progressDiv.style.display = 'block';
  function next(){
    if(i >= files.length){
      loadBooks(); loadSpace();
      status.textContent = 'Upload complete!';
      progressDiv.style.display = 'none';
      return;
    }
    var f = files[i++];
    status.textContent = 'Uploading '+f.name+'...';
    var fd = new FormData();
    fd.append('file', f, f.name);
    var xhr = new XMLHttpRequest();
    xhr.open('POST', '/upload');
    xhr.upload.onprogress = function(e){
      if(e.lengthComputable) prog.value = e.loaded/e.total*100;
    };
    xhr.onload = function(){ prog.value=0; next(); };
    xhr.onerror = function(){
      status.textContent = 'Error uploading '+f.name;
      prog.value = 0; next();
    };
    xhr.send(fd);
  }
  next();
}

document.getElementById('file-input').addEventListener('change', function(e){
  uploadFiles(e.target.files);
});
var ua = document.getElementById('upload-area');
ua.addEventListener('dragover', function(e){ e.preventDefault(); ua.classList.add('drag'); });
ua.addEventListener('dragleave', function(){ ua.classList.remove('drag'); });
ua.addEventListener('drop', function(e){
  e.preventDefault(); ua.classList.remove('drag');
  uploadFiles(e.dataTransfer.files);
});

loadBooks();
loadSpace();
</script>
</body>
</html>
)rawliteral";

inline void webServerInit() {
  _srv.on("/", HTTP_GET, [](AsyncWebServerRequest* req) {
    req->send_P(200, "text/html", INDEX_HTML);
  });

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

  _srv.on("/upload", HTTP_POST,
    [](AsyncWebServerRequest* req) {
      req->send(200, "text/plain", "OK");
    },
    [](AsyncWebServerRequest* req, String filename,
       size_t index, uint8_t* data, size_t len, bool final) {
      static File _upFile;
      if (index == 0) {
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

  _srv.on("/delete", HTTP_GET, [](AsyncWebServerRequest* req) {
    if (req->hasParam("f")) {
      String fn = req->getParam("f")->value();
      fn.replace("/", "_");
      LittleFS.remove("/books/" + fn);
      req->send(200, "text/plain", "Deleted");
    } else {
      req->send(400, "text/plain", "Missing f param");
    }
  });

  _srv.on("/space", HTTP_GET, [](AsyncWebServerRequest* req) {
    size_t total = LittleFS.totalBytes();
    size_t used  = LittleFS.usedBytes();
    size_t fr    = total - used;
    char buf[80];
    snprintf(buf, sizeof(buf),
             "{\"total\":%lu,\"used\":%lu,\"free\":%lu}",
             (unsigned long)total, (unsigned long)used, (unsigned long)fr);
    req->send(200, "application/json", buf);
  });

  _srv.on("/reboot", HTTP_GET, [](AsyncWebServerRequest* req) {
    req->send(200, "text/plain", "Rebooting...");
    delay(500); ESP.restart();
  });

  _srv.begin();
  Serial.println("[Web] Server started on port 80");
}
