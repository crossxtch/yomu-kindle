#pragma once

#include <stddef.h>
#include <stdint.h>

#include <fcntl.h>
#include <stdio.h>

/* POSIX stand-in for the Xteink SD file layer. Paths under /.crossjp are
 * rewritten into the yomu home directory. */

typedef int oflag_t;

class HalFile {
 public:
  HalFile() = default;
  ~HalFile() { close(); }
  HalFile(HalFile &&other) noexcept;
  HalFile &operator=(HalFile &&other) noexcept;
  HalFile(const HalFile &) = delete;
  HalFile &operator=(const HalFile &) = delete;

  explicit operator bool() const { return fp != nullptr; }
  bool isOpen() const { return fp != nullptr; }
  void close();

  int read(void *buf, size_t count);
  size_t write(const void *buf, size_t count);
  size_t write(const uint8_t *buf, size_t count) { return write(static_cast<const void *>(buf), count); }
  size_t write(uint8_t b) { return write(&b, 1); }

  bool seekSet(size_t offset);
  bool seekCur(int64_t offset);
  size_t fileSize();
  bool truncate(uint64_t length);
  bool sync();
  bool probeContiguous() { return true; }
  void clearWriteError();

 private:
  friend class HalStorage;
  FILE *fp = nullptr;
};

class HalStorage {
 public:
  static HalStorage &getInstance();

  HalFile open(const char *path, oflag_t oflag = 0);
  bool openFileForRead(const char *module, const char *path, HalFile &file);
  bool openFileForWrite(const char *module, const char *path, HalFile &file);
  bool exists(const char *path);
  bool remove(const char *path);
  bool ensureDirectoryExists(const char *path);
  bool recoverCard(uint8_t *errorBefore);
  bool freeBytes(uint64_t *out);
};

#define Storage HalStorage::getInstance()

extern "C" void ts_hal_set_root(const char *home);
