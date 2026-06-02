#pragma once
#include <Arduino.h>
#include <esp_heap_caps.h>
#include "ZipFile.h"

#define MAX_CHAPTERS   256
#define MAX_HREF_LEN   192
#define MAX_PATH_LEN   192
// OPF buffer lives on heap now, but cap it sanely
#define OPF_BUF_SIZE   (48 * 1024)

struct EpubInfo {
  char opfBase[MAX_PATH_LEN];
  char chapterHrefs[MAX_CHAPTERS][MAX_HREF_LEN];
  int  chapterCount;
  bool valid;
};

// Three global ZipFile handles - never used concurrently
static ZipFile _epubZipMeta;
static ZipFile _epubZipRead;
static ZipFile _epubZipImg;

// Simple PSRAM-preferring allocator (plain function, not lambda)
static void* _psAlloc(size_t n) {
  void* p = nullptr;
  if (psramFound())
    p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!p) p = malloc(n);
  return p;
}

static bool _getAttr(const char* s, const char* attr, char* out, int outLen) {
  int alen = strlen(attr);
  const char* p = s;
  char closeQ = '"';
  bool found = false;
  while (*p) {
    if (strncmp(p, attr, alen) == 0 && p[alen] == '=') {
      char q = p[alen + 1];
      if (q == '"' || q == '\'') {
        closeQ = q; p += alen + 2; found = true; break;
      }
    }
    p++;
  }
  if (!found) return false;
  const char* e = strchr(p, closeQ);
  if (!e) return false;
  int len = (int)(e - p);
  if (len >= outLen) len = outLen - 1;
  strncpy(out, p, len); out[len] = '\0';
  return true;
}

inline bool epubOpen(const char* path, EpubInfo& info) {
  info.valid = false;
  info.chapterCount = 0;

  // ---- open zip ----
  if (!_epubZipMeta.open(path)) {
    Serial.printf("[epub] zip open failed: %s\n", path);
    return false;
  }

  // ---- read container.xml into small stack buffer ----
  char container[1024];
  size_t cn = _epubZipMeta.extractSmall("META-INF/container.xml",
                                         container, sizeof(container));
  if (!cn) {
    Serial.println("[epub] container.xml missing/too large");
    _epubZipMeta.close(); return false;
  }

  // ---- find OPF path ----
  char opfPath[MAX_PATH_LEN] = {};
  {
    char* p = container;
    bool found = false;
    while ((p = strstr(p, "rootfile")) != nullptr) {
      char* end = strchr(p, '>');
      if (!end) break;
      char sv = *end; *end = '\0';
      if (_getAttr(p, "full-path", opfPath, MAX_PATH_LEN)) { found = true; break; }
      *end = sv; p = end + 1;
    }
    if (!found) {
      Serial.println("[epub] rootfile attr missing");
      _epubZipMeta.close(); return false;
    }
  }
  Serial.printf("[epub] OPF: %s\n", opfPath);

  // ---- derive opfBase ----
  strncpy(info.opfBase, opfPath, MAX_PATH_LEN - 1);
  info.opfBase[MAX_PATH_LEN - 1] = '\0';
  {
    char* sl = strrchr(info.opfBase, '/');
    if (sl) *(sl + 1) = '\0'; else info.opfBase[0] = '\0';
  }

  // ---- load OPF into PSRAM/heap (NOT a static buffer) ----
  char* opf = nullptr;
  size_t opfLen = _epubZipMeta.extractToHeap(opfPath, &opf, OPF_BUF_SIZE, _psAlloc);
  _epubZipMeta.close();
  if (!opfLen || !opf) {
    Serial.println("[epub] OPF load failed");
    if (opf) free(opf); return false;
  }
  Serial.printf("[epub] OPF len=%u\n", (unsigned)opfLen);

  // ---- parse manifest into heap array (NOT static) ----
  // Each Item: 64 + 192 = 256 bytes, 256 items = 64 KB -- must be heap
  struct Item { char id[64]; char href[MAX_HREF_LEN]; };
  Item* manifest = (Item*)_psAlloc(MAX_CHAPTERS * sizeof(Item));
  if (!manifest) {
    Serial.println("[epub] manifest alloc failed");
    free(opf); return false;
  }
  int mCount = 0;

  {
    char* p = opf;
    while ((p = strstr(p, "<item ")) && mCount < MAX_CHAPTERS) {
      char* end = strchr(p, '>');
      if (!end) break;
      char sv = *end; *end = '\0';
      char media[64] = {};
      _getAttr(p, "media-type", media, sizeof(media));
      if (strstr(media, "html") || strstr(media, "xhtml")) {
        _getAttr(p, "id",   manifest[mCount].id,   sizeof(manifest[mCount].id));
        _getAttr(p, "href", manifest[mCount].href,  sizeof(manifest[mCount].href));
        mCount++;
      }
      *end = sv; p = end + 1;
    }
  }
  Serial.printf("[epub] manifest html items=%d\n", mCount);

  // ---- parse spine ----
  char* spineS = strstr(opf, "<spine");
  char* spineE = strstr(opf, "</spine>");
  if (!spineS || !spineE) {
    Serial.println("[epub] spine missing");
    free(opf); free(manifest); return false;
  }

  {
    char* p = spineS;
    while (p < spineE && info.chapterCount < MAX_CHAPTERS) {
      p = strstr(p, "<itemref");
      if (!p || p >= spineE) break;
      char* end = strchr(p, '>');
      if (!end) break;
      char sv = *end; *end = '\0';

      char idref[64] = {};
      _getAttr(p, "idref", idref, sizeof(idref));

      for (int i = 0; i < mCount; i++) {
        if (strcmp(manifest[i].id, idref) == 0) {
          // percent-decode + strip fragment
          char decoded[MAX_HREF_LEN] = {};
          const char* src = manifest[i].href;
          char* dst = decoded;
          while (*src && (dst - decoded) < MAX_HREF_LEN - 1) {
            if (*src == '%' &&
                isxdigit((unsigned char)src[1]) &&
                isxdigit((unsigned char)src[2])) {
              char hex[3] = { src[1], src[2], 0 };
              *dst++ = (char)strtol(hex, nullptr, 16);
              src += 3;
            } else { *dst++ = *src++; }
          }
          *dst = '\0';
          char* frag = strchr(decoded, '#'); if (frag) *frag = '\0';
          char* qry  = strchr(decoded, '?'); if (qry)  *qry  = '\0';

          snprintf(info.chapterHrefs[info.chapterCount], MAX_HREF_LEN,
                   "%s%s", info.opfBase, decoded);
          info.chapterCount++;
          break;
        }
      }
      *end = sv; p = end + 1;
    }
  }

  free(opf);
  free(manifest);

  info.valid = info.chapterCount > 0;
  Serial.printf("[epub] chapters=%d valid=%d\n", info.chapterCount, (int)info.valid);
  return info.valid;
}

static int stripHtml(const char* html, int len,
                     char* out, int outLen,
                     char* imgSrc, int imgSrcLen) {
  int oi = 0;
  bool inTag = false;
  const char* p = html;
  const char* end = html + len;
  bool gotImg = false;
  char tag[320];

  static const char* const es[] = {
    "&amp;","&lt;","&gt;","&nbsp;","&#160;",
    "&apos;","&quot;","&mdash;","&ndash;",
    "&ldquo;","&rdquo;","&lsquo;","&rsquo;"
  };
  static const char ec[] = {
    '&','<','>',' ',' ',
    '\'','"','-','-',
    '"','"','\'','\''
  };

  while (p < end && oi < outLen - 1) {
    if (!inTag) {
      if (*p == '<') {
        inTag = true;
        if (!gotImg && imgSrc && p + 4 < end && strncasecmp(p+1,"img ",4)==0) {
          const char* te = strchr(p,'>');
          if (te) {
            int tl = (int)(te - p);
            if (tl > (int)sizeof(tag)-1) tl = sizeof(tag)-1;
            strncpy(tag, p, tl); tag[tl] = '\0';
            if (_getAttr(tag,"src",imgSrc,imgSrcLen)) gotImg = true;
          }
        }
        if (p + 2 < end) {
          const char* blk[] = {"p ","p>","br","h1","h2","h3",
                               "h4","h5","h6","li","/p","tr"};
          for (int bi = 0; bi < 12; bi++)
            if (strncasecmp(p+1, blk[bi], 2) == 0) {
              if (oi > 0 && out[oi-1] != '\n') out[oi++] = '\n';
              break;
            }
        }
        p++; continue;
      }
      if (*p == '&') {
        bool matched = false;
        for (int ei = 0; ei < 13; ei++) {
          int el = strlen(es[ei]);
          if (strncasecmp(p, es[ei], el) == 0) {
            out[oi++] = ec[ei]; p += el; matched = true; break;
          }
        }
        if (!matched) out[oi++] = *p++;
        continue;
      }
      unsigned char c = (unsigned char)*p;
      if (c < 0x20 && c != '\n' && c != '\r' && c != '\t') { p++; continue; }
      if (c > 0x7E) { p++; continue; }
      out[oi++] = *p++;
    } else {
      if (*p == '>') inTag = false;
      p++;
    }
  }
  out[oi] = '\0';
  return oi;
}
