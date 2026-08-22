/* Adafruit_LittleFS.h - host mock backed by a real file on disk, so a test
   can kill the process, restart, and prove settings actually persisted. */
#ifndef MOCK_LFS_H
#define MOCK_LFS_H
#include "Arduino.h"
#include <map>

namespace Adafruit_LittleFS_Namespace {
  enum { FILE_O_READ = 0, FILE_O_WRITE = 1 };
  class MockFS;

  class File {
  public:
    File(MockFS &fs) : _fs(&fs) {}
    bool open(const char *path, int mode);
    int  write(const uint8_t *b, size_t n);
    int  read(uint8_t *b, size_t n);
    void close() { _open = false; }
    operator bool() const { return _open; }
  private:
    MockFS *_fs; std::string _path; int _mode = 0; bool _open = false; size_t _pos = 0;
  };

  class MockFS {
  public:
    std::map<std::string, std::vector<uint8_t>> files;
    bool begin() { return true; }
    bool remove(const char *p) { return files.erase(p) > 0; }
  };
}
extern Adafruit_LittleFS_Namespace::MockFS InternalFS;

namespace Adafruit_LittleFS_Namespace {
  inline bool File::open(const char *path, int mode) {
    _path = path; _mode = mode; _pos = 0;
    if (mode == FILE_O_READ) { _open = _fs->files.count(path) > 0; }
    else { _fs->files[path]; _open = true; }
    return _open;
  }
  inline int File::write(const uint8_t *b, size_t n) {
    if (!_open) return 0;
    auto &v = _fs->files[_path];
    v.insert(v.end(), b, b + n);
    return (int)n;
  }
  inline int File::read(uint8_t *b, size_t n) {
    if (!_open) return 0;
    auto &v = _fs->files[_path];
    size_t got = 0;
    while (got < n && _pos < v.size()) b[got++] = v[_pos++];
    return (int)got;
  }
}
#endif
