#pragma once
/*
 * Minimal EPUB 2/3 parser.
 * Memory budget: parse OPF in one 8 KB stack-allocated buffer.
 * Chapters are decompressed ONE at a time (max 32 KB) into heap.
 * Images extracted to heap (max 16 KB) for TJpgDec rendering.
 */
#include <Arduino.h>
#include "ZipFile.h"

#define MAX_CHAPTERS  64
#define MAX_HREF_LEN  128
#define MAX_PATH_LEN  128

struct EpubInfo {
  char opfBase[MAX_PATH_LEN];          // e.g. "OEBPS/"
  char chapterHrefs[MAX_CHAPTERS][MAX_HREF_LEN];  // full zip paths
  int  chapterCount;
  bool valid;
};

// ── Tiny attribute extractor ───────────────────────────────────────────────
static bool _getAttr(const char* s, const char* attr, char* out, int outLen) {
  char q1[80], q2[80];
  snprintf(q1, sizeof(q1), "%s=\"", attr);
  snprintf(q2, sizeof(q2), "%s='",  attr);
  const char* p = strstr(s, q1);
  char close = '"';
  if (!p) { p = strstr(s, q2); close = '\''; }
  if (!p) return false;
  p += strlen(attr) + 2;
  const char* e = strchr(p, close);
  if (!e) return false;
  int len = min((int)(e - p), outLen - 1);
  strncpy(out, p, len); out[len] = '\0';
  return true;
}

static bool _epubFindOpf(ZipFile& zip, char* opfPath) {
  char buf[1024];
  size_t n = zip.extractSmall("META-INF/container.xml", buf, sizeof(buf) - 1);
  if (!n) return false;
  buf[n] = '\0';
  char* p = buf;
  while ((p = strstr(p, "rootfile")) != nullptr) {
    char* end = strchr(p, '>'); if (!end) break;
    *end = '\0';
    if (_getAttr(p, "full-path", opfPath, MAX_PATH_LEN)) return true;
    *end = '>'; p = end;
  }
  return false;
}

inline bool epubOpen(const char* path, EpubInfo& info) {
  info.valid = false; info.chapterCount = 0;
  ZipFile zip;
  if (!zip.open(path)) return false;

  char opfPath[MAX_PATH_LEN];
  if (!_epubFindOpf(zip, opfPath)) { zip.close(); return false; }

  // Build opfBase
  strncpy(info.opfBase, opfPath, MAX_PATH_LEN);
  char* sl = strrchr(info.opfBase, '/');
  if (sl) *(sl + 1) = '\0'; else info.opfBase[0] = '\0';

  // Read OPF
  static char opf[8192];
  size_t opfLen = zip.extractSmall(opfPath, opf, sizeof(opf) - 1);
  zip.close();
  if (!opfLen) return false;
  opf[opfLen] = '\0';

  // Manifest: id → href (xhtml only)
  struct Item { char id[64]; char href[MAX_HREF_LEN]; };
  static Item manifest[MAX_CHAPTERS];
  int mCount = 0;

  char* p = opf;
  while ((p = strstr(p, "<item ")) && mCount < MAX_CHAPTERS) {
    char* end = strchr(p, '>'); if (!end) break;
    char saved = *end; *end = '\0';
    char media[64] = "";
    _getAttr(p, "media-type", media, sizeof(media));
    if (strstr(media, "html")) {
      _getAttr(p, "id",   manifest[mCount].id,   64);
      _getAttr(p, "href", manifest[mCount].href, MAX_HREF_LEN);
      mCount++;
    }
    *end = saved; p = end + 1;
  }

  // Spine (ordered idrefs)
  char* spineS = strstr(opf, "<spine");
  char* spineE = strstr(opf, "</spine>");
  if (!spineS || !spineE) return false;

  p = spineS;
  while (p < spineE && info.chapterCount < MAX_CHAPTERS) {
    p = strstr(p, "<itemref");
    if (!p || p >= spineE) break;
    char* end = strchr(p, '>'); if (!end) break;
    char saved = *end; *end = '\0';
    char idref[64] = "";
    _getAttr(p, "idref", idref, sizeof(idref));
    for (int i = 0; i < mCount; i++) {
      if (strcmp(manifest[i].id, idref) == 0) {
        // Decode %20 and other URL encoding in href
        char decoded[MAX_HREF_LEN];
        const char* src = manifest[i].href;
        char* dst = decoded;
        while (*src && dst - decoded < MAX_HREF_LEN - 1) {
          if (*src == '%' && isxdigit(src[1]) && isxdigit(src[2])) {
            char hex[3] = { src[1], src[2], 0 };
            *dst++ = (char)strtol(hex, nullptr, 16);
            src += 3;
          } else { *dst++ = *src++; }
        }
        *dst = '\0';
        snprintf(info.chapterHrefs[info.chapterCount],
                 MAX_HREF_LEN, "%s%s", info.opfBase, decoded);
        info.chapterCount++;
        break;
      }
    }
    *end = saved; p = end + 1;
  }

  info.valid = info.chapterCount > 0;
  return info.valid;
}

// ── HTML → plain text stripper ────────────────────────────────────────────
// imgSrc (optional): filled with first <img src="..."> encountered.
static int stripHtml(const char* html, int len,
                     char* out, int outLen,
                     char* imgSrc, int imgSrcLen) {
  int oi = 0; bool inTag = false;
  const char* p = html, *end = html + len;
  bool gotImg = false;

  while (p < end && oi < outLen - 1) {
    if (!inTag) {
      if (*p == '<') {
        inTag = true;
        // Capture img src
        if (!gotImg && imgSrc && p + 4 < end &&
            strncasecmp(p + 1, "img ", 4) == 0) {
          const char* te = strchr(p, '>');
          if (te) {
            int tl = min((int)(te - p), 255);
            char tag[256]; strncpy(tag, p, tl); tag[tl] = '\0';
            if (_getAttr(tag, "src", imgSrc, imgSrcLen)) gotImg = true;
          }
        }
        // Inject newline for block elements
        if (p + 2 < end) {
          const char* tags[] = {"p ", "p>", "br", "h1", "h2", "h3",
                                 "h4", "h5", "h6", "li", "/p", "tr"};
          for (auto t : tags) {
            if (strncasecmp(p + 1, t, 2) == 0) {
              if (oi > 0 && out[oi-1] != '\n') out[oi++] = '\n';
              break;
            }
          }
        }
        p++; continue;
      }
      if (*p == '&') {
        struct { const char* e; char c; } ents[] = {
          {"&amp;", '&'}, {"&lt;", '<'}, {"&gt;", '>'},
          {"&nbsp;", ' '}, {"&#160;", ' '}, {"&apos;", '\''},
          {"&quot;", '"'}, {"&mdash;", '-'}, {"&ndash;", '-'},
          {"&ldquo;", '"'}, {"&rdquo;", '"'}, {"&lsquo;", '\''}, {"&rsquo;", '\''}
        };
        bool matched = false;
        for (auto& e : ents) {
          int el = strlen(e.e);
          if (strncasecmp(p, e.e, el) == 0) {
            out[oi++] = e.c; p += el; matched = true; break;
          }
        }
        if (!matched) out[oi++] = *p++;
        continue;
      }
      // Filter out non-printable / high UTF-8 bytes → replace with '?'
      unsigned char c = (unsigned char)*p;
      if (c < 0x20 && c != '\n' && c != '\r' && c != '\t') { p++; continue; }
      if (c > 0x7E) { out[oi++] = '?'; p++; continue; }  // skip multi-byte UTF-8
      out[oi++] = *p++;
    } else {
      if (*p == '>') { inTag = false; }
      p++;
    }
  }
  out[oi] = '\0';
  return oi;
}