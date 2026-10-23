#include <HalStorage.h>
#include <BoardConfig.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

char g_root[512] = "/tmp/yomu-ts";

void mkdir_p(const char *path) {
  char tmp[512];
  size_t n;
  char *p;
  if (!path || !path[0]) {
    return;
  }
  n = strlen(path);
  if (n >= sizeof tmp) {
    return;
  }
  memcpy(tmp, path, n + 1);
  for (p = tmp + 1; *p; p++) {
    if (*p == '/') {
      *p = '\0';
      mkdir(tmp, 0755);
      *p = '/';
    }
  }
  mkdir(tmp, 0755);
}

void map_path(char *dst, size_t dst_n, const char *path) {
  if (!path) {
    if (dst_n) {
      dst[0] = '\0';
    }
    return;
  }
  if (strncmp(path, "/.yomu", 6) == 0) {
    snprintf(dst, dst_n, "%s%s", g_root, path + 6);
    return;
  }
  snprintf(dst, dst_n, "%s", path);
}

FILE *open_mapped(const char *path, int writable, int trunc) {
  char mapped[512];
  FILE *fp;
  map_path(mapped, sizeof mapped, path);
  if (writable && trunc) {
    return fopen(mapped, "wb+");
  }
  if (writable) {
    fp = fopen(mapped, "rb+");
    if (!fp) {
      fp = fopen(mapped, "wb+");
    }
    return fp;
  }
  return fopen(mapped, "rb");
}

}  // namespace

BoardConfig BoardConfig::ACTIVE = {1236, 1648};

extern "C" void ts_board_set(int page_w, int page_h) {
  BoardConfig::ACTIVE.displayHeight = page_w;
  BoardConfig::ACTIVE.displayWidth = page_h;
}

extern "C" void ts_hal_set_root(const char *home) {
  if (!home || !home[0]) {
    home = "/tmp/yomu";
  }
  snprintf(g_root, sizeof g_root, "%s/ts", home);
  mkdir_p(g_root);
}

HalFile::HalFile(HalFile &&other) noexcept : fp(other.fp) { other.fp = nullptr; }

HalFile &HalFile::operator=(HalFile &&other) noexcept {
  if (this != &other) {
    close();
    fp = other.fp;
    other.fp = nullptr;
  }
  return *this;
}

void HalFile::close() {
  if (fp) {
    fclose(fp);
    fp = nullptr;
  }
}

int HalFile::read(void *buf, size_t count) {
  size_t n;
  if (!fp || !buf) {
    return -1;
  }
  n = fread(buf, 1, count, fp);
  if (n == 0 && ferror(fp)) {
    return -1;
  }
  return (int)n;
}

size_t HalFile::write(const void *buf, size_t count) {
  if (!fp || !buf) {
    return 0;
  }
  return fwrite(buf, 1, count, fp);
}

bool HalFile::seekSet(size_t offset) {
  if (!fp) {
    return false;
  }
  return fseeko(fp, (off_t)offset, SEEK_SET) == 0;
}

bool HalFile::seekCur(int64_t offset) {
  if (!fp) {
    return false;
  }
  return fseeko(fp, (off_t)offset, SEEK_CUR) == 0;
}

size_t HalFile::fileSize() {
  off_t here;
  off_t end;
  if (!fp) {
    return 0;
  }
  here = ftello(fp);
  if (here < 0 || fseeko(fp, 0, SEEK_END) != 0) {
    return 0;
  }
  end = ftello(fp);
  if (here >= 0) {
    fseeko(fp, here, SEEK_SET);
  }
  return end < 0 ? 0 : (size_t)end;
}

bool HalFile::truncate(uint64_t length) {
  if (!fp) {
    return false;
  }
  if (fflush(fp) != 0) {
    return false;
  }
  if (ftruncate(fileno(fp), (off_t)length) != 0) {
    return false;
  }
  return fseeko(fp, (off_t)length, SEEK_SET) == 0;
}

bool HalFile::sync() {
  if (!fp) {
    return false;
  }
  return fflush(fp) == 0;
}

void HalFile::clearWriteError() {
  if (fp) {
    clearerr(fp);
  }
}

HalStorage &HalStorage::getInstance() {
  static HalStorage storage;
  return storage;
}

HalFile HalStorage::open(const char *path, oflag_t oflag) {
  HalFile file;
  int writable = (oflag & (O_RDWR | O_WRONLY)) != 0;
  file.fp = open_mapped(path, writable, 0);
  return file;
}

bool HalStorage::openFileForRead(const char *, const char *path, HalFile &file) {
  file.close();
  file.fp = open_mapped(path, 0, 0);
  return file.fp != nullptr;
}

bool HalStorage::openFileForWrite(const char *, const char *path, HalFile &file) {
  char mapped[512];
  char *slash;
  file.close();
  map_path(mapped, sizeof mapped, path);
  slash = strrchr(mapped, '/');
  if (slash && slash != mapped) {
    *slash = '\0';
    mkdir_p(mapped);
    *slash = '/';
  }
  file.fp = fopen(mapped, "wb+");
  return file.fp != nullptr;
}

bool HalStorage::exists(const char *path) {
  char mapped[512];
  struct stat st;
  map_path(mapped, sizeof mapped, path);
  return stat(mapped, &st) == 0;
}

bool HalStorage::remove(const char *path) {
  char mapped[512];
  map_path(mapped, sizeof mapped, path);
  if (unlink(mapped) == 0 || errno == ENOENT) {
    return true;
  }
  return false;
}

bool HalStorage::removePrefix(const char *dir, const char *prefix) {
  char mapped[512];
  DIR *d;
  struct dirent *ent;
  size_t plen;
  if (!dir || !dir[0] || !prefix || !prefix[0]) {
    return false;
  }
  map_path(mapped, sizeof mapped, dir);
  d = opendir(mapped);
  if (!d) {
    return errno == ENOENT;
  }
  plen = strlen(prefix);
  while ((ent = readdir(d)) != NULL) {
    char child[768];
    int n;
    if (strncmp(ent->d_name, prefix, plen) != 0) {
      continue;
    }
    n = snprintf(child, sizeof child, "%s/%s", dir, ent->d_name);
    if (n > 0 && (size_t)n < sizeof child) {
      this->remove(child);
    }
  }
  closedir(d);
  return true;
}

bool HalStorage::ensureDirectoryExists(const char *path) {
  char mapped[512];
  map_path(mapped, sizeof mapped, path);
  mkdir_p(mapped);
  struct stat st;
  return stat(mapped, &st) == 0 && S_ISDIR(st.st_mode);
}

bool HalStorage::recoverCard(uint8_t *errorBefore) {
  if (errorBefore) {
    *errorBefore = 0;
  }
  return true;
}

bool HalStorage::freeBytes(uint64_t *out) {
  if (out) {
    *out = (uint64_t)512 * 1024 * 1024;
  }
  return true;
}
