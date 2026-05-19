#pragma once
/*
 * ZipFile — LittleFS-backed ZIP reader using miniz.
 * Add miniz.h + miniz.c from https://github.com/richgel999/miniz
 * to your sketch folder. They compile fine on ESP32-C3.
 */
#include <Arduino.h>
#include <LittleFS.h>
#include "miniz.h"

class ZipFile {
public:
  mz_zip_archive _zip;
  File           _file;
  bool           _open = false;

  // miniz I/O callback
  static size_t _mzRead(void* opaque, mz_uint64 ofs, void* buf, size_t n) {
    File* f = (File*)opaque;
    if (!f->seek((uint32_t)ofs)) return 0;
    return f->read((uint8_t*)buf, n);
  }

  bool open(const char* path) {
    _file = LittleFS.open(path, "r");
    if (!_file) return false;
    memset(&_zip, 0, sizeof(_zip));
    _zip.m_pRead      = _mzRead;
    _zip.m_pIO_opaque = &_file;
    if (!mz_zip_reader_init(&_zip, _file.size(), 0)) {
      _file.close(); return false;
    }
    _open = true;
    return true;
  }

  void close() {
    if (_open) { mz_zip_reader_end(&_zip); _file.close(); _open = false; }
  }

  // Decompress a named entry into a malloc'd buffer (caller must free).
  // Returns 0 on failure. maxSize caps allocation to protect heap.
  size_t extractToHeap(const char* name, char** outBuf, size_t maxSize = 32768) {
    if (!_open) return 0;
    int idx = mz_zip_reader_locate_file(&_zip, name, nullptr, 0);
    if (idx < 0) return 0;
    mz_zip_archive_file_stat st;
    if (!mz_zip_reader_file_stat(&_zip, idx, &st)) return 0;
    size_t sz = (size_t)st.m_uncomp_size;
    if (sz == 0) return 0;
    if (sz > maxSize) sz = maxSize;
    *outBuf = (char*)malloc(sz + 1);
    if (!*outBuf) return 0;
    if (!mz_zip_reader_extract_to_mem(&_zip, idx, *outBuf, sz, 0)) {
      free(*outBuf); *outBuf = nullptr; return 0;
    }
    (*outBuf)[sz] = '\0';
    return sz;
  }

  // Simple line-by-line read of a small entry into a fixed stack buffer.
  // Returns bytes read.
  size_t extractSmall(const char* name, char* buf, size_t bufSize) {
    char* tmp = nullptr;
    size_t n = extractToHeap(name, &tmp, bufSize);
    if (n && tmp) { memcpy(buf, tmp, n); buf[n] = '\0'; free(tmp); return n; }
    return 0;
  }
};