#pragma once
#include <Arduino.h>
#include <LittleFS.h>
#include "miniz.h"

typedef void* (*ZipAllocFn)(size_t);

class ZipFile {
public:
  mz_zip_archive _zip{};
  File _file;
  bool _open = false;

  static size_t _mzRead(void* opaque, mz_uint64 ofs, void* buf, size_t n) {
    File* f = (File*)opaque;
    if (!f->seek((uint32_t)ofs)) return 0;
    return f->read((uint8_t*)buf, n);
  }

  bool open(const char* path) {
    close();
    _file = LittleFS.open(path, "r");
    if (!_file) return false;

    memset(&_zip, 0, sizeof(_zip));
    _zip.m_pRead = _mzRead;
    _zip.m_pIO_opaque = &_file;

    if (!mz_zip_reader_init(&_zip, _file.size(), 0)) {
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

  int locate(const char* name) {
    if (!_open) return -1;
    return mz_zip_reader_locate_file(&_zip, name, nullptr, 0);
  }

  bool statByName(const char* name, mz_zip_archive_file_stat* st) {
    if (!_open) return false;
    int idx = locate(name);
    if (idx < 0) return false;
    return mz_zip_reader_file_stat(&_zip, idx, st);
  }

  size_t extractToHeap(const char* name, char** outBuf, size_t maxSize, ZipAllocFn allocFn = malloc) {
    if (!_open || !outBuf) return 0;
    *outBuf = nullptr;

    int idx = locate(name);
    if (idx < 0) return 0;

    mz_zip_archive_file_stat st;
    if (!mz_zip_reader_file_stat(&_zip, idx, &st)) return 0;

    size_t sz = (size_t)st.m_uncomp_size;
    if (sz == 0 || sz > maxSize) return 0;

    char* mem = (char*)allocFn(sz + 1);
    if (!mem) return 0;

    if (!mz_zip_reader_extract_to_mem(&_zip, idx, mem, sz, 0)) {
      free(mem);
      return 0;
    }

    mem[sz] = '\0';
    *outBuf = mem;
    return sz;
  }

  size_t extractSmall(const char* name, char* buf, size_t bufSize) {
    if (!_open || !buf || bufSize < 2) return 0;

    int idx = locate(name);
    if (idx < 0) return 0;

    mz_zip_archive_file_stat st;
    if (!mz_zip_reader_file_stat(&_zip, idx, &st)) return 0;

    size_t sz = (size_t)st.m_uncomp_size;
    if (sz == 0 || sz >= bufSize) return 0;

    if (!mz_zip_reader_extract_to_mem(&_zip, idx, buf, sz, 0)) return 0;
    buf[sz] = '\0';
    return sz;
  }
};