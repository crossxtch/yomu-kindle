#pragma once

#include <HalStorage.h>

#include <cstdint>

namespace ts {

/* IMG1. The original JPEG, PNG, or GIF bytes, not a rendered page.
 * Header is 16 bytes, then count records of u32 length + bytes.
 * Id 1 is the first record. A page is decoded when it is shown. */
constexpr uint32_t kPictureMagic = 0x31474D49u;
constexpr uint16_t kMaxPictures = 512;
constexpr uint32_t kPictureHeader = 16;
/* Picture atom cp is the sidecar id in the low 15 bits. The high bit forces a
 * page of its own (a cover, 扉, or a spine image). */
constexpr uint16_t kPictureForcePage = 0x8000u;
constexpr uint16_t kPictureIdMask = 0x7fffu;

class PictureWriter {
 public:
  PictureWriter() = default;
  ~PictureWriter() { end(); }
  PictureWriter(const PictureWriter&) = delete;
  PictureWriter& operator=(const PictureWriter&) = delete;

  /* `append` keeps ids already stored. pageW and pageH only gate the caller. */
  bool begin(const char* path, uint16_t pageW, uint16_t pageH, bool append = false);
  void end();
  bool ready() const { return out.isOpen(); }
  uint16_t count() const { return n; }

  /* Copy one image. Returns a 1-based id, or 0 when it is too small or unusable. */
  uint16_t addJpeg(HalFile& image);
  uint16_t addPng(HalFile& image);

 private:
  HalFile out;
  char filePath[64]{};
  uint16_t n = 0;

  uint16_t store(HalFile& image);
};

/* Pixel size of image `id`. False when the record is missing. */
bool pictureSize(const char* path, uint16_t id, int* w, int* h);

/* Fit image `id` inside the page. 0 is black and 255 is white. Margins stay white. */
bool picturePaint(const char* path, uint16_t id, uint8_t* gray, int pageW, int pageH);
/* Same fit, inside a band that leaves `marginY` white at the top and bottom. */
bool picturePaintInset(const char* path, uint16_t id, uint8_t* gray, int pageW, int pageH, int marginY);

/* Draw image `id` into the box at (x, y). The rest of the page is left alone. */
bool picturePaintBox(const char* path, uint16_t id, uint8_t* gray, int pageW, int pageH, int x, int y, int boxW,
                     int boxH);

}  // namespace ts
