#include "PicturePage.h"

#include <Logging.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_ONLY_GIF
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

namespace ts {
namespace {

constexpr uint32_t kMaxImageBytes = 8u * 1024u * 1024u;
constexpr int kMinSide = 64;
constexpr int kMaxPixels = 12 * 1000 * 1000;

void putU32(uint8_t* d, const uint32_t v) {
  d[0] = static_cast<uint8_t>(v);
  d[1] = static_cast<uint8_t>(v >> 8);
  d[2] = static_cast<uint8_t>(v >> 16);
  d[3] = static_cast<uint8_t>(v >> 24);
}

uint32_t getU32(const uint8_t* d) {
  return static_cast<uint32_t>(d[0]) | (static_cast<uint32_t>(d[1]) << 8) | (static_cast<uint32_t>(d[2]) << 16) |
         (static_cast<uint32_t>(d[3]) << 24);
}

bool readU32(HalFile& f, uint32_t& v) {
  uint8_t b[4];
  if (f.read(b, 4) != 4) {
    return false;
  }
  v = getU32(b);
  return true;
}

bool writeHeader(HalFile& f, const uint16_t count) {
  uint8_t hdr[kPictureHeader]{};
  putU32(hdr, kPictureMagic);
  hdr[4] = static_cast<uint8_t>(count);
  hdr[5] = static_cast<uint8_t>(count >> 8);
  return f.seekSet(0) && f.write(hdr, sizeof hdr) == sizeof hdr;
}

/* Scale src into the center of a white page. Downscales average the source block. */
void fitGray(uint8_t* dst, const int dw, const int dh, const uint8_t* src, const int sw, const int sh) {
  memset(dst, 255, static_cast<size_t>(dw) * static_cast<size_t>(dh));
  if (sw < 1 || sh < 1 || dw < 1 || dh < 1) {
    return;
  }
  int fw;
  int fh;
  if (static_cast<long long>(sw) * dh <= static_cast<long long>(sh) * dw) {
    fh = dh;
    fw = static_cast<int>(static_cast<long long>(sw) * dh / sh);
  } else {
    fw = dw;
    fh = static_cast<int>(static_cast<long long>(sh) * dw / sw);
  }
  if (fw < 1) {
    fw = 1;
  }
  if (fh < 1) {
    fh = 1;
  }
  if (fw > dw) {
    fw = dw;
  }
  if (fh > dh) {
    fh = dh;
  }
  const int ox = (dw - fw) / 2;
  const int oy = (dh - fh) / 2;
  for (int y = 0; y < fh; ++y) {
    int sy0 = static_cast<int>(static_cast<long long>(y) * sh / fh);
    int sy1 = static_cast<int>(static_cast<long long>(y + 1) * sh / fh);
    if (sy1 <= sy0) {
      sy1 = sy0 + 1;
    }
    if (sy0 < 0) {
      sy0 = 0;
    }
    if (sy1 > sh) {
      sy1 = sh;
    }
    uint8_t* row = dst + static_cast<size_t>(oy + y) * static_cast<size_t>(dw) + ox;
    for (int x = 0; x < fw; ++x) {
      int sx0 = static_cast<int>(static_cast<long long>(x) * sw / fw);
      int sx1 = static_cast<int>(static_cast<long long>(x + 1) * sw / fw);
      if (sx1 <= sx0) {
        sx1 = sx0 + 1;
      }
      if (sx0 < 0) {
        sx0 = 0;
      }
      if (sx1 > sw) {
        sx1 = sw;
      }
      unsigned sum = 0;
      unsigned n = 0;
      for (int sy = sy0; sy < sy1; ++sy) {
        const uint8_t* srow = src + static_cast<size_t>(sy) * static_cast<size_t>(sw);
        for (int sx = sx0; sx < sx1; ++sx) {
          sum += srow[sx];
          ++n;
        }
      }
      row[x] = n > 0 ? static_cast<uint8_t>(sum / n) : 255;
    }
  }
}

}  // namespace

bool PictureWriter::begin(const char* path, const uint16_t pageW, const uint16_t pageH, const bool append) {
  end();
  n = 0;
  filePath[0] = '\0';
  if (!path || !path[0] || pageW == 0 || pageH == 0) {
    return false;
  }
  snprintf(filePath, sizeof filePath, "%s", path);
  if (append) {
    HalFile in;
    uint8_t hdr[kPictureHeader];
    if (!Storage.openFileForRead("TS", filePath, in) || in.read(hdr, sizeof hdr) != static_cast<int>(sizeof hdr) ||
        getU32(hdr) != kPictureMagic) {
      return false;
    }
    const uint16_t count = static_cast<uint16_t>(hdr[4] | (hdr[5] << 8));
    size_t endAt = kPictureHeader;
    for (uint16_t i = 0; i < count; ++i) {
      uint32_t sz = 0;
      if (!readU32(in, sz) || sz == 0 || sz > kMaxImageBytes || !in.seekCur(static_cast<int64_t>(sz))) {
        return false;
      }
      endAt += 4u + sz;
    }
    in.close();
    out = Storage.open(filePath, O_RDWR);
    if (!out || !out.seekSet(endAt)) {
      out.close();
      return false;
    }
    n = count;
    return true;
  }
  if (!Storage.openFileForWrite("TS", filePath, out) || !writeHeader(out, 0)) {
    out.close();
    return false;
  }
  return true;
}

void PictureWriter::end() {
  if (!out.isOpen()) {
    return;
  }
  writeHeader(out, n);
  out.close();
}

uint16_t PictureWriter::store(HalFile& image) {
  if (!out.isOpen() || n >= kMaxPictures) {
    return 0;
  }
  const size_t got = image.fileSize();
  if (got == 0 || got > kMaxImageBytes || !image.seekSet(0)) {
    return 0;
  }
  uint8_t* bytes = static_cast<uint8_t*>(malloc(got));
  if (!bytes || image.read(bytes, got) != static_cast<int>(got)) {
    free(bytes);
    return 0;
  }
  int w = 0;
  int h = 0;
  int comp = 0;
  if (!stbi_info_from_memory(bytes, static_cast<int>(got), &w, &h, &comp) || w < 1 || h < 1 ||
      (w < kMinSide && h < kMinSide) || static_cast<long long>(w) * h > kMaxPixels) {
    free(bytes);
    return 0;
  }
  const size_t start = out.fileSize();
  uint8_t len[4];
  putU32(len, static_cast<uint32_t>(got));
  if (out.write(len, 4) != 4 || out.write(bytes, got) != got) {
    out.truncate(start);
    free(bytes);
    return 0;
  }
  free(bytes);
  ++n;
  return n;
}

uint16_t PictureWriter::addJpeg(HalFile& image) { return store(image); }

uint16_t PictureWriter::addPng(HalFile& image) { return store(image); }

bool picturePaint(const char* path, const uint16_t id, uint8_t* gray, const int pageW, const int pageH) {
  if (!path || !path[0] || !gray || id == 0 || pageW < 1 || pageH < 1) {
    return false;
  }
  HalFile in;
  uint8_t hdr[kPictureHeader];
  if (!Storage.openFileForRead("TS", path, in) || in.read(hdr, sizeof hdr) != static_cast<int>(sizeof hdr) ||
      getU32(hdr) != kPictureMagic) {
    return false;
  }
  const uint16_t count = static_cast<uint16_t>(hdr[4] | (hdr[5] << 8));
  if (id > count) {
    return false;
  }
  uint32_t sz = 0;
  for (uint16_t i = 1; i <= id; ++i) {
    if (!readU32(in, sz) || sz == 0 || sz > kMaxImageBytes) {
      return false;
    }
    if (i < id && !in.seekCur(static_cast<int64_t>(sz))) {
      return false;
    }
  }
  uint8_t* bytes = static_cast<uint8_t*>(malloc(sz));
  if (!bytes || in.read(bytes, sz) != static_cast<int>(sz)) {
    free(bytes);
    return false;
  }
  int w = 0;
  int h = 0;
  int comp = 0;
  uint8_t* pix = stbi_load_from_memory(bytes, static_cast<int>(sz), &w, &h, &comp, 1);
  free(bytes);
  if (!pix || w < 1 || h < 1) {
    stbi_image_free(pix);
    return false;
  }
  fitGray(gray, pageW, pageH, pix, w, h);
  stbi_image_free(pix);
  return true;
}

}  // namespace ts
