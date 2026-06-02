#pragma once
#include <Arduino.h>
#include "ZipFile.h"

#define MAX_CHAPTERS   256
#define MAX_HREF_LEN   192
#define MAX_PATH_LEN   192
#define OPF_BUF_SIZE   24576

struct EpubInfo {
  char opfBase[MAX_PATH_LEN];
  char chapterHrefs[MAX_CHAPTERS][MAX_HREF_LEN];
  int  chapterCount;
  bool valid;
};

static ZipFile _epubZipMeta;
static ZipFile _epubZipRead;
static ZipFile _epubZipImg;

static bool _getAttr(const char* s, const char* attr, char* out, int outLen) {
  int alen = strlen(attr);
  const char* p = s;
  char close = '"';
  bool found = false;

  while (*p) {
    if (strncmp(p, attr, alen) == 0 && p[alen] == '=') {
      char q = p[alen + 1];
      if (q == '"' || q == '\'') {
        close = q;
        p += alen + 2;
        found = true;
        break;
      }
    }
    p++;
  }

  if (!found) return false;

  const char* e = strchr(p, close);
  if (!e) return false;

  int len = (int)(e - p);
  if (len >= outLen) len = outLen - 1;
  strncpy(out, p, len);
  out[len] = '\0';
  return true;
}

static bool _epubFindOpf(char* opfPath) {
  static char buf[768];
  size_t n = _epubZipMeta.extractSmall("META-INF/container.xml", buf, sizeof(buf) - 1);
  if (!n) return false;

  char* p = buf;
  while ((p = strstr(p, "rootfile")) != nullptr) {
    char* end = strchr(p, '>');
    if (!end) break;
    *end = '\0';
    if (_getAttr(p, "full-path", opfPath, MAX_PATH_LEN)) return true;
    *end = '>';
    p = end + 1;
  }
  return false;
}

inline bool epubOpen(const char* path, EpubInfo& info) {
  info.valid = false;
  info.chapterCount = 0;

  if (!_epubZipMeta.open(path)) {
    Serial.printf("[epub] zip.open failed: %s\n", path);
    return false;
  }

  static char opfPath[MAX_PATH_LEN];
  if (!_epubFindOpf(opfPath)) {
    Serial.println("[epub] container.xml missing");
    _epubZipMeta.close();
    return false;
  }

  strncpy(info.opfBase, opfPath, MAX_PATH_LEN - 1);
  info.opfBase[MAX_PATH_LEN - 1] = '\0';
  char* sl = strrchr(info.opfBase, '/');
  if (sl) *(sl + 1) = '\0';
  else info.opfBase[0] = '\0';

  static char opf[OPF_BUF_SIZE];
  size_t opfLen = _epubZipMeta.extractSmall(opfPath, opf, sizeof(opf) - 1);
  _epubZipMeta.close();
  if (!opfLen) {
    Serial.println("[epub] OPF too large or unreadable");
    return false;
  }

  struct Item { char id[64]; char href[MAX_HREF_LEN]; };
  static Item manifest[MAX_CHAPTERS];
  int mCount = 0;

  char* p = opf;
  static char media[64];

  while ((p = strstr(p, "<item ")) && mCount < MAX_CHAPTERS) {
    char* end = strchr(p, '>');
    if (!end) break;
    char saved = *end;
    *end = '\0';

    media[0] = '\0';
    _getAttr(p, "media-type", media, sizeof(media));
    if (strstr(media, "html") || strstr(media, "xhtml")) {
      _getAttr(p, "id", manifest[mCount].id, sizeof(manifest[mCount].id));
      _getAttr(p, "href", manifest[mCount].href, sizeof(manifest[mCount].href));
      mCount++;
    }

    *end = saved;
    p = end + 1;
  }

  char* spineS = strstr(opf, "<spine");
  char* spineE = strstr(opf, "</spine>");
  if (!spineS || !spineE) {
    Serial.println("[epub] spine missing");
    return false;
  }

  static char idref[64];
  static char decoded[MAX_HREF_LEN];
  p = spineS;

  while (p < spineE && info.chapterCount < MAX_CHAPTERS) {
    p = strstr(p, "<itemref");
    if (!p || p >= spineE) break;

    char* end = strchr(p, '>');
    if (!end) break;
    char saved = *end;
    *end = '\0';

    idref[0] = '\0';
    _getAttr(p, "idref", idref, sizeof(idref));

    for (int i = 0; i < mCount; i++) {
      if (strcmp(manifest[i].id, idref) == 0) {
        const char* src = manifest[i].href;
        char* dst = decoded;

        while (*src && (dst - decoded) < (MAX_HREF_LEN - 1)) {
          if (*src == '%' &&
              isxdigit((unsigned char)src[1]) &&
              isxdigit((unsigned char)src[2])) {
            char hex[3] = { src[1], src[2], 0 };
            *dst++ = (char)strtol(hex, nullptr, 16);
            src += 3;
          } else {
            *dst++ = *src++;
          }
        }
        *dst = '\0';

        snprintf(info.chapterHrefs[info.chapterCount],
                 MAX_HREF_LEN,
                 "%s%s",
                 info.opfBase,
                 decoded);
        info.chapterCount++;
        break;
      }
    }

    *end = saved;
    p = end + 1;
  }

  info.valid = info.chapterCount > 0;
  Serial.printf("[epub] chapters=%d\n", info.chapterCount);
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
  static char tag[320];

  static const char* const es[] = {
    "&amp;", "&lt;", "&gt;", "&nbsp;", "&#160;",
    "&apos;", "&quot;", "&mdash;", "&ndash;",
    "&ldquo;", "&rdquo;", "&lsquo;", "&rsquo;"
  };
  static const char ec[] = {
    '&', '<', '>', ' ', ' ',
    '\'', '"', '-', '-',
    '"', '"', '\'', '\''
  };

  while (p < end && oi < outLen - 1) {
    if (!inTag) {
      if (*p == '<') {
        inTag = true;

        if (!gotImg && imgSrc && p + 4 < end && strncasecmp(p + 1, "img ", 4) == 0) {
          const char* te = strchr(p, '>');
          if (te) {
            int tl = (int)(te - p);
            if (tl > (int)sizeof(tag) - 1) tl = sizeof(tag) - 1;
            strncpy(tag, p, tl);
            tag[tl] = '\0';
            if (_getAttr(tag, "src", imgSrc, imgSrcLen)) gotImg = true;
          }
        }

        if (p + 2 < end) {
          const char* blk[] = { "p ", "p>", "br", "h1", "h2", "h3", "h4", "h5", "h6", "li", "/p", "tr" };
          for (int bi = 0; bi < 12; bi++) {
            if (strncasecmp(p + 1, blk[bi], 2) == 0) {
              if (oi > 0 && out[oi - 1] != '\n') out[oi++] = '\n';
              break;
            }
          }
        }

        p++;
        continue;
      }

      if (*p == '&') {
        bool matched = false;
        for (int ei = 0; ei < 13; ei++) {
          int el = strlen(es[ei]);
          if (strncasecmp(p, es[ei], el) == 0) {
            out[oi++] = ec[ei];
            p += el;
            matched = true;
            break;
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