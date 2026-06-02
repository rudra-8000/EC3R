#pragma once
#include <Arduino.h>
#include <LittleFS.h>
#include "miniz.h"

typedef void* (*ZipAllocFn)(size_t);

class ZipFile {
public:
  mz_zip_archive _zip{};
  File            _file;
  bool            _open = false;

  static size_t _mzRead(void* opaque, mz_uint64 ofs, void* buf, size_t n) {
    File* f = (File*)opaque;
    if (!f->seek((uint32_t)ofs, SeekSet)) return 0;
    return f->read((uint8_t*)buf, n);
  }

  bool open(const char* path) {
    close();
    _file = LittleFS.open(path, "r");
    if (!_file) {
      Serial.printf("[Zip] open failed: %s\n", path);
      return false;
    }
    memset(&_zip, 0, sizeof(_zip));
    _zip.m_pRead      = _mzRead;
    _zip.m_pIO_opaque = &_file;
    if (!mz_zip_reader_init(&_zip, (uint64_t)_file.size(), 0)) {
      Serial.printf("[Zip] init failed: %s err=%d\n", path, (int)_zip.m_last_error);
      _file.close();
      return false;
    }
    _open = true;
    return true;
  }

  void close() {
    if (_open) {
      mz_zip_reader_end(&_zip);
      _file.close();
      _open = false;
    }
  }

  // Extract into caller-supplied buffer. Returns 0 if missing or won't fit.
  size_t extractSmall(const char* name, char* buf, size_t bufSize) {
    if (!_open || !buf || bufSize < 2) return 0;
    int idx = mz_zip_reader_locate_file(&_zip, name, nullptr, 0);
    if (idx < 0) {
      Serial.printf("[Zip] not found: %s\n", name);
      return 0;
    }
    mz_zip_archive_file_stat st;
    if (!mz_zip_reader_file_stat(&_zip, idx, &st)) return 0;
    size_t sz = (size_t)st.m_uncomp_size;
    if (sz == 0) return 0;
    if (sz > bufSize - 1) {
      Serial.printf("[Zip] extractSmall too large: %s sz=%u buf=%u\n",
                    name, (unsigned)sz, (unsigned)bufSize);
      return 0;
    }
    if (!mz_zip_reader_extract_to_mem(&_zip, idx, buf, sz, 0)) {
      Serial.printf("[Zip] decomp failed: %s err=%d\n", name, (int)_zip.m_last_error);
      return 0;
    }
    buf[sz] = '\0';
    return sz;
  }

  // Extract into heap buffer using allocFn. Caller must free().
  size_t extractToHeap(const char* name, char** outBuf, size_t maxSize,
                       ZipAllocFn allocFn = malloc) {
    if (!_open || !outBuf) return 0;
    *outBuf = nullptr;
    int idx = mz_zip_reader_locate_file(&_zip, name, nullptr, 0);
    if (idx < 0) {
      Serial.printf("[Zip] not found: %s\n", name);
      return 0;
    }
    mz_zip_archive_file_stat st;
    if (!mz_zip_reader_file_stat(&_zip, idx, &st)) return 0;
    size_t sz = (size_t)st.m_uncomp_size;
    if (sz == 0 || sz > maxSize) {
      Serial.printf("[Zip] extractToHeap skip: %s sz=%u max=%u\n",
                    name, (unsigned)sz, (unsigned)maxSize);
      return 0;
    }
    char* mem = (char*)allocFn(sz + 1);
    if (!mem) {
      Serial.printf("[Zip] alloc failed sz=%u\n", (unsigned)sz);
      return 0;
    }
    if (!mz_zip_reader_extract_to_mem(&_zip, idx, mem, sz, 0)) {
      Serial.printf("[Zip] decomp failed: %s err=%d\n", name, (int)_zip.m_last_error);
      free(mem);
      return 0;
    }
    mem[sz] = '\0';
    *outBuf = mem;
    return sz;
  }
};
