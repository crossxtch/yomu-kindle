#pragma once

#include <cstdint>

/* FreeType face. Drawing goes to an 8-bit page. */

class FtFont {
 public:
  FtFont() = default;
  ~FtFont() { close(); }
  FtFont(const FtFont &) = delete;
  FtFont &operator=(const FtFont &) = delete;

  /* faceIndex < 0 uses the first face that has あ and 一. */
  bool load(const char *path, int faceIndex);
  void close();
  bool loaded() const { return face != nullptr; }
  const char *lastError() const { return error; }
  uint8_t emPx() const { return em; }
  uint8_t rubyEmPx() const { return ruby; }

  void releaseFile() {}
  bool reopenFile() { return face != nullptr; }
  void releaseMaps() {}
  bool restoreMaps() { return face != nullptr; }

  /* 1 when the face has cp, else 0xFFFF. The run still carries the codepoint. */
  uint16_t glyphId(uint32_t cp) const;
  void prewarm(const uint16_t *, uint16_t, bool) {}

  /* Draw `cp` into the cell (x, y, cellW, cellH). `em` is the face size.
   * `vertical` uses the vert/vrt2 glyph on the em, or a centered 90° rotate
   * when the face has no vertical form. Body ink sits on the em. 約物 is
   * centered in the cell. 0 is paper, 255 is ink, and darker ink wins. */
  bool drawGray(uint8_t *page, int pageW, int pageH, int x, int y, int cellW, int cellH, uint32_t cp, int em,
                bool vertical, bool bold = false);
  /* Horizontal advance in pixels at `em`. 0 when the face has no glyph. */
  int16_t advancePx(uint32_t cp, int em);

 private:
  struct Slot {
    uint32_t gid = 0;
    uint16_t em = 0;
    uint8_t rot = 0;
    uint8_t taken = 0;
    int16_t left = 0;
    int16_t top = 0;
    int16_t advance = 0;
    int16_t bw = 0;
    int16_t bh = 0;
    int16_t ascent = 0;
    int16_t descent = 0;
    uint8_t *bits = nullptr;
  };

  void *library = nullptr;
  void *face = nullptr;
  void *vert = nullptr;
  const char *error = "closed";
  uint8_t em = 50;
  uint8_t ruby = 25;
  Slot slots[256]{};
  int clock = 0;

  void loadVert();
  void clearVert();
  uint32_t vertOf(uint32_t gid) const;
  const Slot *slotFor(uint32_t gid, int px, bool rotate, bool bold);
};

extern "C" void ts_font_set_em(int em);

/* Faces that contain あ and 一. labels[i] is that face's name. */
extern "C" int ts_font_faces(const char *path, int *indexes, char labels[][48], int cap);
