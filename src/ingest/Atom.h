#pragma once

#include <cstdint>

namespace ts {

enum class AtomKind : uint8_t {
  Ch,
  Tcy,
  Gap,
  Space,
  ColumnBreak,
  PageBreak,
  Group,    // unbreakable 熟語ルビ: tcy[] = bases, ruby[] = reading
  Mode,     // cp 0 = vertical-rl, 1 = horizontal-tb
  Picture,  // cp = 1-based page id in the picture sidecar. 0 is not a picture.
};

// Picture atom cp is the sidecar id in the low 15 bits. The high bit forces a
// page of its own (a cover, 扉, or a spine image). Pixel size is not stored.
constexpr uint16_t kPictureForcePage = 0x8000u;
constexpr uint16_t kPictureIdMask = 0x7fffu;

// Readings, 縦中横, and 熟語 bases kept on one atom or glyph.
constexpr uint8_t kMaxCluster = 8;

struct Atom {
  AtomKind kind = AtomKind::Ch;
  uint32_t cp = 0;  // Ch: base. Tcy/Group: first base. Mode: 0 vert, 1 horiz.
                    // ColumnBreak, in memory only: 1 starts a heading. Not stored.
  uint8_t rubyCount = 0;
  uint32_t ruby[kMaxCluster]{};  // Ch/Group reading. Tcy glyphs reuse this only on the run.
  uint8_t tcyCount = 0;
  uint32_t tcy[kMaxCluster]{};  // Tcy chars, or Group bases (base 0 duplicated in cp)
  uint8_t startEm = 0;  // ColumnBreak indent. Ch/Tcy/Group: percent of the body em, 0 means 100.
  uint8_t emphasis = 0;  // low bits: a kEm* mark. kEmphasisBold is a heavier face.
  uint8_t box = 0;       // 囲み罫 id. 0 is none. One id is one rule per column or line.
  uint8_t shade = 0;     // 0 is no fill. 2–255 is the background gray; black is stored as 2.
  uint8_t rulePx = 0;    // 0 keeps the em thickness. Otherwise the CSS border width in px.
  uint8_t ruleEm = 0;    // Percent of the em when the width is em or rem. 0 means absent.
  uint8_t ruleRoot = 0;  // 1 uses the body em (rem). 0 uses this glyph's em.
  uint8_t ruleSides = 0; // Partial 囲み罫. 0 draws every side. See kBorder*.
  uint8_t family = 0;    // 0 is the body face. kFaceSans draws gothic.
  uint8_t gapBefore = 0; // ColumnBreak: percent of the em before this column.
  uint8_t gapAfter = 0;  // ColumnBreak: percent of the em after this column.
};

constexpr uint8_t kEmphasisMask = 0x7f;
constexpr uint8_t kEmphasisBold = 0x80;
// Low 7 bits. 1 and 2 stay the old 圏点 and 傍線 codes.
constexpr uint8_t kEmSesame = 1;
constexpr uint8_t kEmLine = 2;
constexpr uint8_t kEmSesameOpen = 3;
constexpr uint8_t kEmDot = 4;
constexpr uint8_t kEmDotOpen = 5;
constexpr uint8_t kEmCircle = 6;
constexpr uint8_t kEmCircleOpen = 7;
constexpr uint8_t kEmDoubleCircle = 8;
constexpr uint8_t kEmDoubleCircleOpen = 9;
constexpr uint8_t kEmTriangle = 10;

// Bits of a partial 囲み罫. Top, right, bottom, left, in that CSS order.
constexpr uint8_t kBorderTop = 1;
constexpr uint8_t kBorderRight = 2;
constexpr uint8_t kBorderBottom = 4;
constexpr uint8_t kBorderLeft = 8;

enum class WritingMode : uint8_t { VerticalRl, HorizontalTb };

}  // namespace ts
