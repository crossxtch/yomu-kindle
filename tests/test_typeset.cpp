#include "HtmlIR.h"
#include "Kinsoku.h"
#include "Layout.h"
#include "PicturePage.h"
#include "TextIr.h"

#include "FtFont.h"

#include <HalStorage.h>

#include <cstdio>
#include <cstring>
#include <unistd.h>

namespace {

int g_fails = 0;

void check(bool ok, const char *what) {
  if (!ok) {
    std::fprintf(stderr, "FAIL typeset: %s\n", what);
    g_fails++;
  }
}

int16_t measureLatin(void *, uint32_t cp, int16_t) {
  if (cp == 'i') {
    return 4;
  }
  if (cp == 'm') {
    return 14;
  }
  return 10;
}

void testKinsoku() {
  check(!ts::kinsokuCanStartColumn(0x3015), "〕 does not start a column");
  check(!ts::kinsokuCanEndColumn(0x3014), "〔 does not end a column");
  check(ts::kinsokuHangs(0x3002), "。 hangs");
  check(!ts::kinsokuHangs(0x300C), "「 does not hang");
  check(ts::kinsokuHalfAdvance(0x300C), "「 is half an em");
  check(ts::kinsokuHalfAdvance(0x3015), "〕 is half an em");
  check(ts::kinsokuInlineAdvance(0x300C, 20, false) == 10, "「 advances 10");
  check(ts::kinsokuInlineAdvance(0x300C, 20, true) == 20, "tcy ignores half advance");
  check(ts::kinsokuBracketPairGap(0x300D, 0x300C, 20) == 10, "」「 keeps half an em");
  check(ts::kinsokuBracketPairGap(0x300F, 0x300E, 20) == 10, "』『 keeps half an em");
  check(ts::kinsokuBracketPairGap(0xFF09, 0xFF08, 20) == 10, "）（ keeps half an em");
  check(ts::kinsokuBracketPairGap(0x300C, 0x300D, 20) == 0, "「」 stays packed");
  check(ts::kinsokuBracketPairGap(0x3002, 0x300C, 20) == 0, "。「 stays packed");
  check(ts::kinsokuBracketPairGap(0x300D, 0x3042, 20) == 0, "」あ stays packed");
  check(ts::usesEmAdvance(0x3042), "kana stays on the em");
  check(!ts::usesEmAdvance('A'), "latin uses the face advance");
  check(ts::kinsokuYakumono(0x3001), "、 is 約物");
  check(!ts::kinsokuYakumono(0x3042), "あ is not 約物");
}

void feedCp(ts::PageLayouter &lay, uint32_t cp, uint32_t pos) {
  ts::Atom atom{};
  atom.cp = cp;
  lay.feed(atom, pos);
}

void testBracketPair() {
  ts::PageLayouter lay;
  ts::LayoutOptions opt;
  opt.width = 80;
  opt.height = 200;
  opt.em = 20;
  opt.margin = 0;
  opt.mode = ts::WritingMode::VerticalRl;
  lay.begin(opt);
  const uint32_t seq[] = {0x3042, 0x300D, 0x300C, 0x3044};
  for (int i = 0; i < 4; ++i) {
    feedCp(lay, seq[i], static_cast<uint32_t>(i));
  }
  check(lay.finish(), "」「 page commits");
  if (lay.pageGlyphCount() == 4) {
    const ts::GlyphRun *p = lay.page();
    check(p[1].advance == 10 && p[2].advance == 10, "」「 cells stay half an em");
    check((p[1].flags & ts::kRunHalfCell) != 0 && (p[2].flags & ts::kRunHalfCell) != 0, "」「 stay half cells");
    check(p[2].x == p[1].x && p[2].y == p[1].y + 20, "」「 keeps a half-em gap");
    check(p[3].y == p[2].y + 10, "kana follows the opening bracket");
  } else {
    check(false, "あ」「い is four glyphs");
  }

  lay.begin(opt);
  feedCp(lay, 0x3002, 0);
  feedCp(lay, 0x300C, 1);
  check(lay.finish() && lay.pageGlyphCount() == 2, "。「 page");
  if (lay.pageGlyphCount() == 2) {
    const ts::GlyphRun *p = lay.page();
    check(p[1].y == p[0].y + p[0].advance, "。「 does not open a gap");
  }

  /* One em of column. The gap does not fit, so 「 starts the next column. */
  opt.width = 70;
  opt.height = 20;
  lay.begin(opt);
  feedCp(lay, 0x300D, 0);
  feedCp(lay, 0x300C, 1);
  check(lay.finish() && lay.pageGlyphCount() == 2, "tight 」「 page");
  if (lay.pageGlyphCount() == 2) {
    const ts::GlyphRun *p = lay.page();
    check(p[0].x != p[1].x && p[1].y == p[0].y, "tight column splits 」「");
  }

  opt.width = 400;
  opt.height = 80;
  opt.mode = ts::WritingMode::HorizontalTb;
  lay.begin(opt);
  for (int i = 0; i < 4; ++i) {
    feedCp(lay, seq[i], static_cast<uint32_t>(i));
  }
  check(lay.finish() && lay.pageGlyphCount() == 4, "horizontal 」「 page");
  if (lay.pageGlyphCount() == 4) {
    const ts::GlyphRun *p = lay.page();
    check(p[2].x == p[1].x + 20 && p[3].x == p[2].x + 10, "horizontal 」「 keeps a half-em gap");
    check(p[2].y == p[1].y, "horizontal 」「 stays on the line");
  }

  /* The half-em cells still slide. The gap has to show up in the ink, not
   * only in the advances. Hiragino is the book face; Arial Unicode has vert. */
  const char *paths[] = {
      "/System/Library/Fonts/\xe3\x83\x92\xe3\x83\xa9\xe3\x82\xad\xe3\x82\x99\xe3\x83\x8e\xe6\x98\x8e\xe6\x9c\x9d ProN.ttc",
      "/System/Library/Fonts/Supplemental/Arial Unicode.ttf",
      "/Library/Fonts/Arial Unicode.ttf",
      nullptr,
  };
  FtFont font;
  const char *path = nullptr;
  for (int i = 0; paths[i]; ++i) {
    if (font.load(paths[i], -1)) {
      path = paths[i];
      break;
    }
  }
  if (!path) {
    std::fprintf(stderr, "skip bracket ink: %s\n", font.lastError());
    return;
  }
  const int em = 48;
  opt.width = 96;
  opt.height = em * 6;
  opt.em = static_cast<uint8_t>(em);
  opt.margin = 0;
  opt.mode = ts::WritingMode::VerticalRl;
  lay.begin(opt);
  const uint32_t phrase[] = {0x601D, 0x3046, 0x300D, 0x300C, 0x5BB6, 0x5EAD};
  for (int i = 0; i < 6; ++i) {
    feedCp(lay, phrase[i], static_cast<uint32_t>(i));
  }
  check(lay.finish(), "ink page commits");
  const int pageW = opt.width;
  const int pageH = opt.height;
  uint8_t *page = new uint8_t[static_cast<size_t>(pageW) * static_cast<size_t>(pageH)];
  std::memset(page, 255, static_cast<size_t>(pageW) * static_cast<size_t>(pageH));
  const ts::GlyphRun *drawn = lay.page();
  const uint16_t nDrawn = lay.pageGlyphCount();
  int closeY = -1;
  int openY = -1;
  int colX = 0;
  for (uint16_t i = 0; i < nDrawn; ++i) {
    const int adv = drawn[i].advance ? drawn[i].advance : drawn[i].size;
    const int face = drawn[i].size ? drawn[i].size : em;
    font.drawGray(page, pageW, pageH, drawn[i].x, drawn[i].y, face, adv, drawn[i].cp, face, true, false);
    if (drawn[i].cp == 0x300D) {
      closeY = drawn[i].y;
      colX = drawn[i].x;
    }
    if (drawn[i].cp == 0x300C) {
      openY = drawn[i].y + adv;
    }
  }
  int topInk = -1;
  int botInk = -1;
  int bestWhite = 0;
  int white = 0;
  if (closeY >= 0 && openY > closeY) {
    for (int y = closeY; y < openY && y < pageH; ++y) {
      bool ink = false;
      for (int x = colX; x < colX + em && x < pageW; ++x) {
        if (page[y * pageW + x] < 200) {
          ink = true;
          break;
        }
      }
      if (ink) {
        if (topInk < 0) {
          topInk = y;
        }
        botInk = y;
        white = 0;
      } else if (topInk >= 0) {
        white++;
        if (white > bestWhite) {
          bestWhite = white;
        }
      }
    }
  }
  if (bestWhite < em / 2) {
    std::fprintf(stderr, "FAIL typeset: 」「 ink gap=%d em=%d font=%s\n", bestWhite, em, path);
    g_fails++;
  }
  delete[] page;
}

void testLayout() {
  ts::PageLayouter lay;
  ts::LayoutOptions opt;
  opt.width = 80;
  opt.height = 200;
  opt.em = 20;
  opt.margin = 0;
  opt.mode = ts::WritingMode::VerticalRl;
  lay.begin(opt);

  ts::Atom bracket{};
  bracket.cp = 0x300C;
  lay.feed(bracket, 0);
  ts::Atom kana{};
  kana.cp = 0x3042;
  lay.feed(kana, 1);
  check(lay.finish(), "vertical page commits");
  check(lay.pageGlyphCount() == 2, "bracket then kana");
  if (lay.pageGlyphCount() == 2) {
    const ts::GlyphRun *p = lay.page();
    check(p[0].advance == 10, "「 cell is half an em");
    check((p[0].flags & ts::kRunRotate) == 0, "「 is not a rotated latin mark");
    check((p[0].flags & ts::kRunHalfCell) != 0, "「 is marked half");
    check(p[1].x == p[0].x, "kana stays in the column");
    check(p[1].y == p[0].y + 10, "kana starts after the half cell");
  }

  ts::Atom ruby{};
  ruby.cp = 0x4E00;
  ruby.rubyCount = 6;
  for (uint8_t i = 0; i < 6; ++i) {
    ruby.ruby[i] = 0x3042u + i;
  }
  lay.begin(opt);
  lay.feed(ruby, 0);
  check(lay.finish(), "ruby page commits");
  check(lay.pageGlyphCount() == 1 && lay.page()[0].rubyCount == 6, "six-kana ruby is kept");
  if (lay.pageGlyphCount() == 1) {
    check(lay.page()[0].ruby[5] == 0x3042u + 5, "last ruby char survives");
  }

  ts::Atom group{};
  group.kind = ts::AtomKind::Group;
  group.tcyCount = 5;
  group.cp = 0x4E00;
  for (uint8_t i = 0; i < 5; ++i) {
    group.tcy[i] = 0x4E00u + i;
  }
  group.rubyCount = 6;
  for (uint8_t i = 0; i < 6; ++i) {
    group.ruby[i] = 0x3042u + i;
  }
  lay.begin(opt);
  lay.feed(group, 0);
  check(lay.finish(), "group page commits");
  check(lay.pageGlyphCount() == 5, "five-base 熟語 stays together");
  if (lay.pageGlyphCount() == 5) {
    const ts::GlyphRun *p = lay.page();
    check(p[0].rubyCount == 6 && p[0].ruby[5] == 0x3047u, "group reading is kept");
    check(p[1].cp == 0x4E01u && (p[4].flags & ts::kRunSticky) != 0, "later bases stick");
  }

  opt.width = 400;
  opt.height = 200;
  opt.mode = ts::WritingMode::HorizontalTb;
  opt.measure = measureLatin;
  lay.begin(opt);
  ts::Atom eye{};
  eye.cp = 'i';
  lay.feed(eye, 0);
  ts::Atom em{};
  em.cp = 'm';
  lay.feed(em, 1);
  check(lay.finish(), "horizontal page commits");
  if (lay.pageGlyphCount() == 2) {
    const ts::GlyphRun *p = lay.page();
    check(p[0].advance == 4 && p[1].advance == 14, "latin uses the measured advance");
    check(p[1].x == p[0].x + 4, "m follows i");
    check((p[0].flags & ts::kRunRotate) == 0, "horizontal latin stays upright");
  } else {
    check(false, "two horizontal letters");
  }

  lay.begin(opt);
  ts::Atom tcy{};
  tcy.kind = ts::AtomKind::Tcy;
  tcy.cp = 'i';
  tcy.tcyCount = 2;
  tcy.tcy[0] = 'i';
  tcy.tcy[1] = 'm';
  lay.feed(tcy, 0);
  check(lay.finish() && lay.pageGlyphCount() == 2, "horizontal tcy is two glyphs");
  if (lay.pageGlyphCount() == 2) {
    const ts::GlyphRun *p = lay.page();
    check(p[0].advance == 4 && p[1].advance == 14, "horizontal tcy uses face advances");
    check((p[0].flags & ts::kRunTcy) == 0, "horizontal tcy is not a squeezed box");
    check((p[1].flags & ts::kRunSticky) != 0, "horizontal tcy stays together");
  }

  /* 1648×1236 at 50px is 21 columns of 24. A 256-glyph cap stops near the middle. */
  opt.width = 1648;
  opt.height = 1236;
  opt.em = 50;
  opt.margin = 0;
  opt.mode = ts::WritingMode::VerticalRl;
  opt.measure = nullptr;
  lay.begin(opt);
  ts::Atom wide{};
  wide.cp = 0x3042;
  bool spilled = false;
  for (int i = 0; i < 21 * 24; ++i) {
    if (lay.feed(wide, static_cast<uint32_t>(i))) {
      spilled = true;
      break;
    }
  }
  check(!spilled, "paperwhite page holds the full grid");
  if (!spilled) {
    check(lay.finish(), "paperwhite page commits");
  }
  {
    const ts::GlyphRun *p = lay.page();
    const uint16_t n = lay.pageGlyphCount();
    int16_t minX = 32767;
    int16_t maxX = -1;
    for (uint16_t i = 0; i < n; ++i) {
      if (p[i].x < minX) {
        minX = p[i].x;
      }
      if (p[i].x > maxX) {
        maxX = p[i].x;
      }
    }
    check(n == 21 * 24, "paperwhite page is 21 columns");
    check(maxX > 1500 && minX < 80, "columns span the panel width");
  }
}

int inkIn(const uint8_t *page, int w, int x0, int y0, int x1, int y1) {
  int n = 0;
  for (int y = y0; y < y1; ++y) {
    for (int x = x0; x < x1; ++x) {
      if (page[y * w + x] < 250) {
        n++;
      }
    }
  }
  return n;
}

void testFace() {
  /* Arial Unicode has a vert feature and Japanese glyphs. Hiragino is checked
   * separately so this assertion stays on the vert substitution. */
  const char *paths[] = {
      "/System/Library/Fonts/Supplemental/Arial Unicode.ttf",
      "/Library/Fonts/Arial Unicode.ttf",
      nullptr,
  };
  FtFont font;
  const char *path = nullptr;
  for (int i = 0; paths[i]; ++i) {
    if (font.load(paths[i], -1)) {
      path = paths[i];
      break;
    }
  }
  if (!path) {
    std::fprintf(stderr, "skip typeset face: %s\n", font.lastError());
    return;
  }
  const int em = 48;
  const int16_t ai = font.advancePx(0x3042, em);
  const int16_t ii = font.advancePx('i', em);
  const int16_t im = font.advancePx('m', em);
  check(ai > em - 8 && ii > 0 && ii < im && im < em, "mincho advances: あ on the em, i narrower than m");

  /* Arial Unicode's vert feature substitutes 「, not 、. */
  uint8_t vert[em * em];
  uint8_t flat[em * em];
  std::memset(vert, 255, sizeof vert);
  std::memset(flat, 255, sizeof flat);
  check(font.drawGray(vert, em, em, 0, 0, em, em, 0x300C, em, true, false), "draw vertical 「");
  check(font.drawGray(flat, em, em, 0, 0, em, em, 0x300C, em, false, false), "draw horizontal 「");
  check(std::memcmp(vert, flat, sizeof vert) != 0, "vertical 「 uses the vert glyph");
  /* The vert glyph keeps the bracket low in the full em. A half-em cell slides it up. */
  const int top = inkIn(vert, em, 0, 0, em, em / 2);
  const int bot = inkIn(vert, em, 0, em / 2, em, em);
  if (bot <= top) {
    std::fprintf(stderr, "FAIL typeset: vertical 「 ink top=%d bottom=%d\n", top, bot);
    g_fails++;
  }
  const int halfH = em / 2;
  uint8_t half[em * 48];
  std::memset(half, 255, sizeof half);
  check(font.drawGray(half, em, halfH, 0, 0, em, halfH, 0x300C, em, true, false), "draw 「 in a half-em cell");
  const int slid = inkIn(half, em, 0, 0, em, halfH);
  if (slid < 8) {
    std::fprintf(stderr, "FAIL typeset: half-em 「 ink=%d\n", slid);
    g_fails++;
  }
}

void testCollection() {
  /* NFD: ヒ + combining dakuten. Four CFF faces, one row each. */
  const char *path =
      "/System/Library/Fonts/\xe3\x83\x92\xe3\x83\xa9\xe3\x82\xad\xe3\x82\x99\xe3\x83\x8e\xe6\x98\x8e\xe6\x9c\x9d ProN.ttc";
  int indexes[16];
  char labels[16][48];
  int n;
  int i;
  if (access(path, R_OK) != 0) {
    std::fprintf(stderr, "skip collection face\n");
    return;
  }
  n = ts_font_faces(path, indexes, labels, 16);
  check(n == 4, "collection lists each japanese face");
  check(std::strcmp(labels[0], "ヒラギノ明朝 ProN W3") == 0, "first face is ProN W3");
  check(std::strcmp(labels[1], "ヒラギノ明朝 Pro W3") == 0, "second face is Pro W3");
  check(std::strcmp(labels[2], "ヒラギノ明朝 ProN W6") == 0, "third face is ProN W6");
  check(std::strcmp(labels[3], "ヒラギノ明朝 Pro W6") == 0, "fourth face is Pro W6");
  for (i = 0; i < n; ++i) {
    int j;
    for (j = 0; j < i; ++j) {
      check(indexes[i] != indexes[j], "collection face indexes differ");
    }
  }
  if (n >= 2) {
    FtFont first;
    FtFont last;
    check(first.load(path, indexes[0]), "load the first collection face");
    check(last.load(path, indexes[n - 1]), "load the last collection face");
    check(first.advancePx(0x3042, 48) > 0 && last.advancePx(0x3042, 48) > 0, "each collection face has あ");
  }
}

/* 80x40 and 8x8 gray PNGs, black. An 80x40 GIF of the same rectangle. */
static const unsigned char kPng80x40[] = {
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00,
    0x50, 0x00, 0x00, 0x00, 0x28, 0x08, 0x00, 0x00, 0x00, 0x00, 0x4c, 0x0b, 0x22, 0xdd, 0x00, 0x00, 0x00, 0x1a, 0x49,
    0x44, 0x41, 0x54, 0x78, 0xda, 0xed, 0xc1, 0x31, 0x01, 0x00, 0x00, 0x00, 0xc2, 0xa0, 0xf5, 0x4f, 0x6d, 0x0d, 0x0f,
    0xa0, 0x00, 0x00, 0x00, 0xbe, 0x0c, 0x0c, 0xa8, 0x00, 0x01, 0xef, 0x6c, 0xcf, 0xea, 0x00, 0x00, 0x00, 0x00, 0x49,
    0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82};
static const unsigned char kPng8[] = {
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00,
    0x08, 0x00, 0x00, 0x00, 0x08, 0x08, 0x00, 0x00, 0x00, 0x00, 0xe1, 0x64, 0xe1, 0x57, 0x00, 0x00, 0x00, 0x0c, 0x49,
    0x44, 0x41, 0x54, 0x78, 0xda, 0x63, 0x60, 0xa0, 0x0e, 0x00, 0x00, 0x00, 0x48, 0x00, 0x01, 0x10, 0x45, 0xef, 0xd2,
    0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82};
static const unsigned char kGif80x40[] = {
    0x47, 0x49, 0x46, 0x38, 0x37, 0x61, 0x50, 0x00, 0x28, 0x00, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x21, 0xf9, 0x04, 0x04, 0x00, 0x00, 0x00, 0x00, 0x2c, 0x00, 0x00, 0x00, 0x00, 0x50, 0x00, 0x28, 0x00, 0x00, 0x02,
    0x3b, 0x84, 0x8f, 0xa9, 0xcb, 0xed, 0x0f, 0xa3, 0x9c, 0xb4, 0xda, 0x8b, 0xb3, 0xde, 0xbc, 0xfb, 0x0f, 0x86, 0xe2,
    0x48, 0x96, 0xe6, 0x89, 0xa6, 0xea, 0xca, 0xb6, 0xee, 0x0b, 0xc7, 0xf2, 0x4c, 0xd7, 0xf6, 0x8d, 0xe7, 0xfa, 0xce,
    0xf7, 0xfe, 0x0f, 0x0c, 0x0a, 0x87, 0xc4, 0xa2, 0xf1, 0x88, 0x4c, 0x2a, 0x97, 0xcc, 0xa6, 0xf3, 0x09, 0x8d, 0x4a,
    0xa7, 0xac, 0x02, 0x00, 0x3b};

bool writeBytes(const char *path, const unsigned char *bytes, size_t n) {
  FILE *fp = std::fopen(path, "wb");
  if (!fp) {
    return false;
  }
  const bool ok = std::fwrite(bytes, 1, n, fp) == n;
  std::fclose(fp);
  return ok;
}

struct AtomGrab {
  int cols = 0;
  int chars = 0;
};

bool grabAtom(void *ctx, const ts::Atom &atom, uint32_t) {
  auto *g = static_cast<AtomGrab *>(ctx);
  if (atom.kind == ts::AtomKind::ColumnBreak) {
    g->cols++;
  } else if (atom.kind == ts::AtomKind::Ch) {
    g->chars++;
  }
  return true;
}

int tocBreaks(const char *html) {
  AtomGrab g;
  ts::AtomSink sink;
  sink.ctx = &g;
  sink.emit = grabAtom;
  check(ts::htmlToAtoms(html, std::strlen(html), sink), "toc html");
  return g.cols;
}

// Lay out one letter per contents entry and return how many distinct columns.
int tocColumns(const char *html) {
  ts::PageLayouter lay;
  ts::LayoutOptions opt;
  opt.width = 200;
  opt.height = 40;
  opt.em = 20;
  opt.margin = 0;
  opt.mode = ts::WritingMode::VerticalRl;
  lay.begin(opt);
  ts::AtomSink sink;
  sink.ctx = &lay;
  sink.emit = [](void *ctx, const ts::Atom &atom, uint32_t pos) {
    return static_cast<ts::PageLayouter *>(ctx)->feed(atom, pos) || true;
  };
  check(ts::htmlToAtoms(html, std::strlen(html), sink), "toc layout html");
  if (!lay.finish() || lay.pageGlyphCount() == 0) {
    return 0;
  }
  int cols = 1;
  const ts::GlyphRun *p = lay.page();
  for (uint16_t i = 1; i < lay.pageGlyphCount(); ++i) {
    if (p[i].x != p[i - 1].x) {
      cols++;
    }
  }
  return cols;
}

void testToc() {
  // Harry volume list: section.toc, chapter number and title in separate paragraphs.
  const char *volume =
      "<body><section class=\"toc\" epub:type=\"toc\">"
      "<p class=\"toc_chapter\">A</p><p><span class=\"toc_table\">B</span></p>"
      "<p class=\"toc_chapter\">C</p><p>D</p>"
      "</section></body>";
  check(tocBreaks(volume) == 8, "volume toc breaks each paragraph");
  check(tocColumns(volume) == 4, "volume toc is four columns");

  // Series list: body.cover, p.tochead / p.toc1, no toc section.
  const char *series =
      "<body class=\"cover\"><p class=\"tochead\">T</p><p class=\"toc1\">A</p><p class=\"toc1\">B</p></body>";
  check(tocBreaks(series) == 6, "series toc breaks each entry");
  check(tocColumns(series) == 3, "series toc is three columns");

  const char *densho = "<body class=\"p-toc\"><p>A</p><p>B</p></body>";
  check(tocBreaks(densho) == 4, "body p-toc still breaks");

  const char *prose = "<body><p>AB</p><p>CD</p></body>";
  check(tocBreaks(prose) == 0, "chapter paragraphs stay in the column");

  const char *copy =
      "<body class=\"copyright-page\" epub:type=\"copyright-page\"><p>A</p><p>B</p></body>";
  check(tocBreaks(copy) == 0, "copyright page is not a toc");
}

void testColumnPicture() {
  ts::PageLayouter lay;
  ts::LayoutOptions opt;
  opt.width = 240;
  opt.height = 320;
  opt.em = 16;
  opt.margin = 0;
  opt.mode = ts::WritingMode::VerticalRl;
  lay.begin(opt);

  ts::Atom kana{};
  kana.cp = 0x3042;
  lay.feed(kana, 1);
  ts::Atom pic{};
  pic.kind = ts::AtomKind::Picture;
  pic.cp = 1;
  pic.tcyCount = 2;
  pic.tcy[0] = 80;
  pic.tcy[1] = 40;
  check(!lay.feed(pic, 2), "small picture stays on the open page");
  check(lay.takenPicture() == 0 && lay.deferredPicture() == 0, "small picture is not its own page");
  ts::Atom after{};
  after.cp = 0x3044;
  check(!lay.feed(after, 3), "text continues after the picture");
  check(lay.finish(), "column picture page commits");
  check(lay.pageGlyphCount() == 3, "kana, picture, kana");
  if (lay.pageGlyphCount() == 3) {
    const ts::GlyphRun *p = lay.page();
    check(ts::runPicture(p[1]), "the middle run is the picture");
    check(p[1].cp == 1, "picture id is kept");
    check(p[1].size > 0 && p[1].size <= 16, "picture fits the column");
    check(p[2].y > p[1].y, "text follows the picture down the column");
    check(p[2].x == p[0].x, "text stays in that column");
  }

  lay.begin(opt);
  ts::Atom plate{};
  plate.kind = ts::AtomKind::Picture;
  plate.cp = 4;
  plate.tcyCount = 2;
  plate.tcy[0] = 800;
  plate.tcy[1] = 1200;
  check(lay.feed(plate, 0), "a plate commits a page");
  check(lay.takenPicture() == 4, "a plate is its own page");
  check(lay.pageGlyphCount() == 0, "a plate page has no glyphs");

  lay.begin(opt);
  lay.feed(kana, 1);
  ts::Atom cover{};
  cover.kind = ts::AtomKind::Picture;
  cover.cp = 3;
  cover.startEm = 1;
  cover.tcyCount = 2;
  cover.tcy[0] = 80;
  cover.tcy[1] = 40;
  check(lay.feed(cover, 2), "a cover still breaks the page");
  check(lay.deferredPicture() == 3, "a cover is the next page");
}

void testPictures() {
  char dir[] = "/tmp/yomu-pic-XXXXXX";
  char pngSmall[256];
  char pngBig[256];
  char gifPath[256];
  if (!mkdtemp(dir)) {
    check(false, "picture dir");
    return;
  }
  ts_hal_set_root(dir);
  std::snprintf(pngSmall, sizeof pngSmall, "%s/tiny.png", dir);
  std::snprintf(pngBig, sizeof pngBig, "%s/fig.png", dir);
  std::snprintf(gifPath, sizeof gifPath, "%s/fig.gif", dir);
  check(writeBytes(pngSmall, kPng8, sizeof kPng8), "write tiny png");
  check(writeBytes(pngBig, kPng80x40, sizeof kPng80x40), "write png");
  check(writeBytes(gifPath, kGif80x40, sizeof kGif80x40), "write gif");

  ts::PictureWriter writer;
  check(writer.begin("/.yomu/i_test.bin", 120, 80, false), "picture sidecar");
  HalFile tiny;
  HalFile fig;
  HalFile gif;
  check(Storage.openFileForRead("TS", pngSmall, tiny), "open tiny png");
  check(writer.addPng(tiny) == 0, "icon is not a page");
  check(Storage.openFileForRead("TS", pngBig, fig), "open png");
  check(writer.addPng(fig) == 1, "png is stored");
  check(Storage.openFileForRead("TS", gifPath, gif), "open gif");
  check(writer.addPng(gif) == 2, "gif is stored");
  writer.end();

  uint8_t page[120 * 80];
  check(ts::picturePaint("/.yomu/i_test.bin", 1, page, 120, 80), "paint png");
  check(page[0] == 255, "letterbox top stays white");
  check(page[40 * 120 + 60] == 0, "png center is black");
  check(ts::picturePaint("/.yomu/i_test.bin", 2, page, 120, 80), "paint gif");
  check(page[0] == 255, "gif letterbox top stays white");
  check(page[40 * 120 + 60] == 0, "gif center is black");

  check(writer.begin("/.yomu/i_test.bin", 120, 80, true), "append sidecar");
  check(writer.count() == 2, "append keeps the stored ids");
  check(writer.addPng(fig) == 3, "append stores the next id");
  writer.end();
  check(ts::picturePaint("/.yomu/i_test.bin", 1, page, 120, 80), "first id still paints");
  check(page[40 * 120 + 60] == 0, "first picture stays black");
}

void testBoxPad() {
  ts::PageLayouter lay;
  ts::LayoutOptions opt;
  opt.width = 80;
  opt.height = 200;
  opt.em = 20;
  opt.margin = 0;
  opt.mode = ts::WritingMode::VerticalRl;
  lay.begin(opt);
  ts::Atom a{};
  a.cp = 0x3042;
  a.box = 1;
  a.shade = 220;
  lay.feed(a, 0);
  a.cp = 0x3044;
  a.box = 1;
  a.shade = 0;
  lay.feed(a, 1);
  a.cp = 0x3046;
  a.box = 0;
  a.emphasis = ts::kEmDot;
  lay.feed(a, 2);
  check(lay.finish(), "box page commits");
  check(lay.pageGlyphCount() == 3, "three boxed glyphs");
  if (lay.pageGlyphCount() == 3) {
    const ts::GlyphRun *p = lay.page();
    check(p[0].box == 1 && p[1].box == 1 && p[2].box == 0, "box ids stay on the runs");
    check(p[1].y == p[0].y + 20, "a continued box does not pad");
    check(p[2].y >= p[1].y + 24, "leaving a box pads about 0.22em");
    check(p[2].emphasis == ts::kEmDot, "a dot mark reaches the run");
    check(p[0].shade == 220 && p[1].shade == 0, "background gray stays on its run");
  }
}

struct AtomBag {
  ts::Atom atoms[64]{};
  int n = 0;
  char ids[4][40]{};
  int nId = 0;
};

bool takeAtom(void *ctx, const ts::Atom &atom, uint32_t) {
  auto *bag = static_cast<AtomBag *>(ctx);
  if (bag->n < 64) {
    bag->atoms[bag->n++] = atom;
  }
  return true;
}

void takeAnchor(void *ctx, const char *id) {
  auto *bag = static_cast<AtomBag *>(ctx);
  if (!id || bag->nId >= 4) {
    return;
  }
  std::snprintf(bag->ids[bag->nId], sizeof(bag->ids[0]), "%s", id);
  bag->nId++;
}

const ts::Atom *findCp(const AtomBag &bag, uint32_t cp) {
  for (int i = 0; i < bag.n; ++i) {
    if (bag.atoms[i].kind == ts::AtomKind::Ch && bag.atoms[i].cp == cp) {
      return &bag.atoms[i];
    }
  }
  return nullptr;
}

bool headingAround(const AtomBag &bag, uint32_t cp) {
  for (int i = 0; i < bag.n; ++i) {
    if (bag.atoms[i].kind != ts::AtomKind::ColumnBreak || bag.atoms[i].cp != 1) {
      continue;
    }
    for (int j = i + 1; j < bag.n; ++j) {
      if (bag.atoms[j].kind == ts::AtomKind::ColumnBreak) {
        break;
      }
      if (bag.atoms[j].kind == ts::AtomKind::Ch && bag.atoms[j].cp == cp) {
        return true;
      }
    }
  }
  return false;
}

void testAozora() {
  const char *text =
      "\uFF3B\uFF03\u5927\u898B\u51FA\u3057\uFF3D\u6625\uFF3B\uFF03\u5927\u898B\u51FA\u3057\u7D42\u308F\u308A\uFF3D\n"
      "\u672C\u6587\n"
      "\u6F22\u5B57\uFF3B\uFF03\uFF08\u30EC\uFF09\uFF3D\n"
      "\u984C\uFF3B\uFF03\u300C\u984C\u300D\u306F\u5927\u898B\u51FA\u3057\uFF3D\n"
      "\u70B9\uFF3B\uFF03\u300C\u70B9\u300D\u306B\u508D\u70B9\uFF3D\n"
      "\u7DDA\uFF3B\uFF03\u300C\u7DDA\u300D\u306B\u508D\u7DDA\uFF3D\n"
      "\u767D\uFF3B\uFF03\u300C\u767D\u300D\u306B\u767D\u30B4\u30DE\uFF3D\n";
  const char *path = "/tmp/yomu-aozora.txt";
  check(writeBytes(path, reinterpret_cast<const unsigned char *>(text), std::strlen(text)), "write aozora");
  HalFile in;
  check(Storage.openFileForRead("TS", path, in), "open aozora");
  ts::Utf8AtomReader reader;
  reader.bind(&in);
  AtomBag bag;
  ts::Atom atom{};
  uint32_t pos = 0;
  while (bag.n < 64 && reader.next(atom, pos)) {
    bag.atoms[bag.n++] = atom;
  }
  std::remove(path);
  check(headingAround(bag, 0x6625), "range heading wraps 春");
  check(headingAround(bag, 0x984C), "reference heading wraps 題");
  check(findCp(bag, 0x672C) != nullptr && findCp(bag, 0x6587) != nullptr, "body text survives");
  check(findCp(bag, 0x30EC) != nullptr, "kunten emits the inner kana");
  const ts::Atom *dot = findCp(bag, 0x70B9);
  const ts::Atom *line = findCp(bag, 0x7DDA);
  const ts::Atom *open = findCp(bag, 0x767D);
  check(dot && dot->emphasis == ts::kEmSesame, "傍点 is sesame");
  check(line && line->emphasis == ts::kEmLine, "傍線 is a side line");
  check(open && open->emphasis == ts::kEmSesameOpen, "白ゴマ is an open sesame");
}

void testMarks() {
  const char *html =
      "<html><head><style>"
      "span.board{border:1px solid black}"
      "span.side{border-bottom:1px dotted #531084}"
      "span.mark{text-emphasis-style:open sesame}"
      "em{text-emphasis:none}"
      "</style></head><body>"
      "<p id=\"here\"><span class=\"board\">囲文</span>外<em>強</em>"
      "<span class=\"mark\">点</span><span class=\"side\">下</span></p>"
      "<a name=\"sec\">い</a>"
      "</body></html>";
  AtomBag bag;
  ts::FaceRule rules[8]{};
  uint8_t nRules = 0;
  ts::scanStyleTags(html, std::strlen(html), rules, nRules);
  ts::AtomSink sink{&bag, takeAtom, nullptr, rules, nRules, takeAnchor};
  check(ts::htmlToAtoms(html, std::strlen(html), sink), "marks html");
  const ts::Atom *box = findCp(bag, 0x56F2);
  const ts::Atom *box2 = findCp(bag, 0x6587);
  const ts::Atom *plain = findCp(bag, 0x5916);
  const ts::Atom *em = findCp(bag, 0x5F37);
  const ts::Atom *mark = findCp(bag, 0x70B9);
  const ts::Atom *side = findCp(bag, 0x4E0B);
  check(box && box2 && box->box != 0 && box->box == box2->box, "one border shares a box id");
  check(plain && plain->box == 0, "text outside the border is unboxed");
  check(em && (em->emphasis & ts::kEmphasisMask) == ts::kEmSesame, "bare em stays sesame");
  check(mark && (mark->emphasis & ts::kEmphasisMask) == ts::kEmSesameOpen, "css open sesame");
  check(side && side->box == 0 && (side->emphasis & ts::kEmphasisMask) == ts::kEmLine,
        "one-sided border is a side line");
  check(bag.nId == 2 && std::strcmp(bag.ids[0], "here") == 0 && std::strcmp(bag.ids[1], "sec") == 0,
        "id and a name become anchors");
}

void testSwatch() {
  const char *html =
      "<html><head><style>"
      ".bg-gainsboro { background-color: #dcdcdc; }"
      ".k-solid { border-style: solid solid solid solid; border-width: 1px; border-color: #000000; }"
      "</style></head><body><p>"
      "\u4E8B\u67C4\u306E\u90E8\u5206\u3092<span class=\"bg-gainsboro k-solid\">\u3000</span>\u3001"
      "\u6C17\u6301\u3061\u3084\u614B\u5EA6\u306E\u90E8\u5206\u3092<span class=\"k-solid\">\u3000</span>\u3067"
      "<span class=\"bg-gainsboro\">\u3055</span>"
      "</p></body></html>";
  AtomBag bag;
  ts::FaceRule rules[8]{};
  uint8_t nRules = 0;
  ts::scanStyleTags(html, std::strlen(html), rules, nRules);
  ts::AtomSink sink{&bag, takeAtom, nullptr, rules, nRules, takeAnchor};
  check(ts::htmlToAtoms(html, std::strlen(html), sink), "swatch html");
  const ts::Atom *marks[2]{};
  int nMarks = 0;
  const ts::Atom *sa = nullptr;
  for (int i = 0; i < bag.n; ++i) {
    if (bag.atoms[i].kind != ts::AtomKind::Ch) {
      continue;
    }
    if (bag.atoms[i].cp == 0x3000 && nMarks < 2) {
      marks[nMarks++] = &bag.atoms[i];
    }
    if (bag.atoms[i].cp == 0x3055) {
      sa = &bag.atoms[i];
    }
  }
  check(nMarks == 2 && marks[0]->box != 0 && marks[1]->box != 0 && marks[0]->box != marks[1]->box,
        "each swatch is its own box");
  check(nMarks == 2 && marks[0]->shade == 220 && marks[1]->shade == 0, "only the gainsboro swatch is gray");
  check(sa && sa->shade == 220 && sa->box == 0, "a gray highlight has no box");
}

}  // namespace

extern "C" int test_typeset_rules(void) {
  g_fails = 0;
  testKinsoku();
  testBracketPair();
  testLayout();
  testToc();
  testFace();
  testCollection();
  testColumnPicture();
  testPictures();
  testBoxPad();
  testAozora();
  testMarks();
  testSwatch();
  return g_fails;
}
