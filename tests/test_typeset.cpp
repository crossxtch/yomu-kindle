#include "AtomFile.h"
#include "Epub.h"
#include "HtmlIR.h"
#include "Kinsoku.h"
#include "Layout.h"
#include "Mobi.h"
#include "PicturePage.h"
#include "TextIr.h"
#include "TypesetBook.h"
#include "Zip.h"

#include "FtFont.h"

#include <HalStorage.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

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

// 電書協 小見出し: div.m-top-1em.m-right-*.m-left-* is its own column, indented one em.
void testKoMidashi() {
  const char *html =
      "<html class=\"vrtl\"><head><style>"
      ".bold { font-weight: bold; }"
      ".hltr .gfont, .vrtl .gfont { font-family: sans-serif-ja, sans-serif; }"
      "</style></head><body>"
      "<p>\u524D\u6587\u3002</p>"
      "<p><br/></p>"
      "<div class=\"m-top-1em m-right-0em50 m-left-0em50\">"
      "<p><span class=\"bold gfont\">\u8A00\u8A9E\u6F2C\u3051</span></p>"
      "</div>"
      "<p>\u3000\u672C\u6587</p>"
      "<div class=\"m-top-1em m-left-0em50 m-right-0em50\">"
      "<p>\u7532</p><p>\u4E59</p>"
      "</div>"
      "<p>\u4E19</p>"
      "</body></html>";
  ts::PageLayouter lay;
  ts::LayoutOptions opt;
  opt.width = 360;
  opt.height = 100;
  opt.em = 20;
  opt.margin = 0;
  opt.mode = ts::WritingMode::VerticalRl;
  lay.begin(opt);
  ts::FaceRule rules[8]{};
  uint16_t nRules = 0;
  ts::scanStyleTags(html, std::strlen(html), rules, nRules);
  ts::AtomSink sink;
  sink.ctx = &lay;
  sink.rules = rules;
  sink.nRules = nRules;
  sink.emit = [](void *ctx, const ts::Atom &atom, uint32_t pos) {
    return static_cast<ts::PageLayouter *>(ctx)->feed(atom, pos) || true;
  };
  check(ts::htmlToAtoms(html, std::strlen(html), sink), "ko-midashi html");
  check(lay.finish() && lay.pageGlyphCount() > 0, "ko-midashi page");
  const ts::GlyphRun *p = lay.page();
  const uint16_t n = lay.pageGlyphCount();
  const ts::GlyphRun *zen = nullptr;
  const ts::GlyphRun *bun = nullptr;
  const ts::GlyphRun *title = nullptr;
  const ts::GlyphRun *body = nullptr;
  const ts::GlyphRun *kou = nullptr;
  const ts::GlyphRun *otsu = nullptr;
  const ts::GlyphRun *hei = nullptr;
  for (uint16_t i = 0; i < n; ++i) {
    if (!zen && p[i].cp == 0x524D) {
      zen = &p[i];
    } else if (zen && !bun && p[i].cp == 0x6587) {
      bun = &p[i];
    } else if (!title && p[i].cp == 0x8A00) {
      title = &p[i];
    } else if (!body && p[i].cp == 0x672C) {
      body = &p[i];
    } else if (!kou && p[i].cp == 0x7532) {
      kou = &p[i];
    } else if (!otsu && p[i].cp == 0x4E59) {
      otsu = &p[i];
    } else if (!hei && p[i].cp == 0x4E19) {
      hei = &p[i];
    }
  }
  check(zen && bun && zen->x == bun->x, "the paragraph before the title stays one column");
  check(title && zen && title->x != zen->x, "言語漬け starts its own column");
  check(title && zen && title->y == zen->y + opt.em, "m-top-1em indents the title one em");
  check(body && title && body->x != title->x, "the paragraph after the title starts a new column");
  check(kou && otsu && kou->x != otsu->x && kou->y == otsu->y, "a two-line title is two columns");
  check(hei && zen && otsu && hei->x != otsu->x && hei->y == zen->y, "body after the title returns to the column top");
  const int pitch = opt.em + (opt.em + 1) / 2;
  const int half = opt.em / 2;
  check(zen && title && zen->x - title->x == pitch + half, "half an em before the title column");
  check(title && body && title->x - body->x == pitch + half, "half an em after the title column");
  check(kou && otsu && kou->x - otsu->x == pitch, "title lines keep the normal pitch");
  check(otsu && hei && otsu->x - hei->x == pitch + half, "half an em after the last title line");
  check(title && (title->flags & ts::kRunBold) != 0 && (title->flags & ts::kRunGothic) != 0,
        "the title is bold gothic");
  check(zen && (zen->flags & ts::kRunBold) == 0 && (zen->flags & ts::kRunGothic) == 0,
        "the paragraph before stays the body face");

  const char *bare =
      "<html class=\"vrtl\"><body><p>AB</p><div class=\"m-top-1em\"><p>C</p></div><p>D</p></body></html>";
  ts::PageLayouter plain;
  plain.begin(opt);
  ts::AtomSink bareSink;
  bareSink.ctx = &plain;
  bareSink.emit = [](void *ctx, const ts::Atom &atom, uint32_t pos) {
    return static_cast<ts::PageLayouter *>(ctx)->feed(atom, pos) || true;
  };
  check(ts::htmlToAtoms(bare, std::strlen(bare), bareSink) && plain.finish(), "bare m-top html");
  bool oneCol = plain.pageGlyphCount() > 0;
  for (uint16_t i = 1; oneCol && i < plain.pageGlyphCount(); ++i) {
    oneCol = plain.page()[i].x == plain.page()[0].x;
  }
  check(oneCol, "m-top without a side margin stays in the column");
}

void testGfontCss() {
  char css[] =
      "/* @import \"hidden.css\"; */\n"
      "@import \"style-standard.css\";\n"
      "@import url(\"style-advance.css\");\n"
      "@import \"style-standard.css\";\n"
      ".hltr .gfont, .vrtl .gfont { font-family: sans-serif-ja, sans-serif; }\n"
      ".bold { font-weight: bold; }\n"
      "html { font-family: serif-ja, serif; }\n"
      ".vrtl .mfont { font-family: serif; }\n";
  char hrefs[8][96]{};
  const int nHref = ts::appendCssImports(css, std::strlen(css), hrefs, 0, 8);
  check(nHref == 2, "a comment hides an import and a repeat is skipped");
  check(nHref == 2 && std::strcmp(hrefs[0], "style-standard.css") == 0, "the first import is style-standard.css");
  check(nHref == 2 && std::strcmp(hrefs[1], "style-advance.css") == 0, "url() import is kept");

  ts::FaceRule rules[8]{};
  uint16_t n = 0;
  ts::scanFaceCss(css, std::strlen(css), rules, n);
  bool vertSans = false;
  bool horizSans = false;
  bool bold = false;
  bool htmlSerif = false;
  bool mfont = false;
  for (uint16_t i = 0; i < n; ++i) {
    if (std::strcmp(rules[i].cls, "gfont") == 0 && rules[i].family == ts::kFaceSans && rules[i].mode == 2) {
      vertSans = true;
    }
    if (std::strcmp(rules[i].cls, "gfont") == 0 && rules[i].family == ts::kFaceSans && rules[i].mode == 1) {
      horizSans = true;
    }
    if (std::strcmp(rules[i].cls, "bold") == 0 && rules[i].weight == 2) {
      bold = true;
    }
    if (std::strcmp(rules[i].tag, "html") == 0 && rules[i].family == ts::kFaceSerif) {
      htmlSerif = true;
    }
    if (std::strcmp(rules[i].cls, "mfont") == 0 && rules[i].family == ts::kFaceSerif && rules[i].mode == 2) {
      mfont = true;
    }
  }
  check(vertSans && horizSans, "gfont is sans in each writing mode");
  check(bold, ".bold is a heavier face");
  check(htmlSerif && mfont, "serif-ja stays the body face");
}

void testAtomFace() {
  char dir[] = "/tmp/yomu-atom-XXXXXX";
  if (!mkdtemp(dir)) {
    check(false, "atom face dir");
    return;
  }
  ts_hal_set_root(dir);
  ts::AtomWriter writer;
  check(writer.open("/.yomu/face.atom"), "open face atom");
  ts::Atom ch{};
  ch.kind = ts::AtomKind::Ch;
  ch.cp = 0x8A00;
  ch.emphasis = ts::kEmphasisBold;
  ch.family = ts::kFaceSans;
  ts::Atom col{};
  col.kind = ts::AtomKind::ColumnBreak;
  col.startEm = 1;
  col.gapBefore = 50;
  col.gapAfter = 50;
  check(writer.write(ch) && writer.write(col), "write face atoms");
  writer.close();
  ts::AtomReader reader;
  check(reader.open("/.yomu/face.atom"), "read face atom");
  ts::Atom a{};
  check(reader.next(a) && a.kind == ts::AtomKind::Ch && a.cp == 0x8A00 && a.family == ts::kFaceSans &&
            (a.emphasis & ts::kEmphasisBold) != 0,
        "a character keeps its gothic face");
  check(reader.next(a) && a.kind == ts::AtomKind::ColumnBreak && a.startEm == 1 && a.gapBefore == 50 &&
            a.gapAfter == 50,
        "a column break keeps the side gaps");
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
  uint16_t nRules = 0;
  ts::scanStyleTags(html, std::strlen(html), rules, nRules);
  ts::AtomSink sink{&bag, takeAtom, nullptr, rules, nRules, takeAnchor};
  check(ts::htmlToAtoms(html, std::strlen(html), sink), "marks html");
  const ts::Atom *box = findCp(bag, 0x56F2);
  const ts::Atom *box2 = findCp(bag, 0x6587);
  const ts::Atom *plain = findCp(bag, 0x5916);
  const ts::Atom *em = findCp(bag, 0x5F37);
  const ts::Atom *mark = findCp(bag, 0x70B9);
  const ts::Atom *side = findCp(bag, 0x4E0B);
  check(box && box2 && box->box != 0 && box->box == box2->box && box->rulePx == 1, "one border shares a box id");
  check(plain && plain->box == 0, "text outside the border is unboxed");
  check(em && (em->emphasis & ts::kEmphasisMask) == ts::kEmSesame, "bare em stays sesame");
  check(mark && (mark->emphasis & ts::kEmphasisMask) == ts::kEmSesameOpen, "css open sesame");
  check(side && side->box == 0 && side->rulePx == 1 && (side->emphasis & ts::kEmphasisMask) == ts::kEmLine,
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
  uint16_t nRules = 0;
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
  check(nMarks == 2 && marks[0]->rulePx == 1 && marks[1]->rulePx == 1, "a 1px box keeps that width");
  check(sa && sa->shade == 220 && sa->box == 0 && sa->rulePx == 0, "a gray highlight has no box");
}

bool markOf(const AtomBag &bag, uint32_t cp, uint8_t &emph, uint8_t &box, uint8_t &px) {
  const ts::Atom *a = findCp(bag, cp);
  if (!a) {
    return false;
  }
  emph = static_cast<uint8_t>(a->emphasis & ts::kEmphasisMask);
  box = a->box;
  px = a->rulePx;
  return true;
}

void testOneSide() {
  const char *html =
      "<html><head><style>"
      ".top{border-style:solid none none none;border-width:2px}"
      ".right{border-style:none solid none none;border-width:1px}"
      ".pair{border-style:solid none}"
      ".tri{border-style:solid solid none}"
      ".box{border-style:solid solid solid solid;border-width:1px}"
      ".short{border:1px solid black}"
      ".line{border-bottom:1px dotted}"
      ".gone{border-style:none}"
      ".emw{border-bottom:0.1em dotted}"
      ".rem{border-left:0.25rem solid}"
      ".wide,.pairw{border-width:2px}"
      ".wide{border-style:solid none none none}"
      ".pairw{border-style:solid none solid none}"
      "</style></head><body>"
      "<span class=\"top\">\u4E0A</span><span class=\"right\">\u53F3</span>"
      "<span class=\"pair\">\u4E8C</span><span class=\"tri\">\u4E09</span>"
      "<span class=\"box\">\u56F2</span><span class=\"short\">\u67A0</span>"
      "<span class=\"line\">\u4E0B</span><span class=\"gone\">\u7121</span>"
      "<span class=\"emw\">\u5E45</span><span class=\"rem\">\u6839</span>"
      "<span class=\"wide\">\u7DDA</span><span class=\"pairw\">\u5BFE</span>"
      "</body></html>";
  AtomBag bag;
  ts::FaceRule rules[24]{};
  uint16_t nRules = 0;
  ts::scanStyleTags(html, std::strlen(html), rules, nRules);
  ts::AtomSink sink{&bag, takeAtom, nullptr, rules, nRules, nullptr};
  check(ts::htmlToAtoms(html, std::strlen(html), sink), "one-side html");
  uint8_t emph = 0;
  uint8_t box = 0;
  uint8_t px = 0;
  const ts::Atom *top = findCp(bag, 0x4E0A);
  check(markOf(bag, 0x4E0A, emph, box, px) && emph == ts::kEmLine && box == 0 && px == 2 && top && top->ruleSides == 0,
        "solid none none none is a 2px side line");
  check(markOf(bag, 0x53F3, emph, box, px) && emph == ts::kEmLine && box == 0 && px == 1,
        "none solid none none is a side line");
  const ts::Atom *pair = findCp(bag, 0x4E8C);
  check(pair && (pair->emphasis & ts::kEmphasisMask) == 0 && pair->box != 0 && pair->rulePx == 0 &&
            pair->ruleSides == (ts::kBorderTop | ts::kBorderBottom),
        "two visible sides are the top and bottom of a box");
  const ts::Atom *tri = findCp(bag, 0x4E09);
  check(tri && (tri->emphasis & ts::kEmphasisMask) == 0 && tri->box != 0 && tri->rulePx == 0 &&
            tri->ruleSides == (ts::kBorderTop | ts::kBorderRight | ts::kBorderLeft),
        "three visible sides leave the bottom open");
  const ts::Atom *full = findCp(bag, 0x56F2);
  check(markOf(bag, 0x56F2, emph, box, px) && box != 0 && px == 1 && full && full->ruleSides == 0,
        "four solid sides are a 1px box");
  const ts::Atom *shorthand = findCp(bag, 0x67A0);
  check(markOf(bag, 0x67A0, emph, box, px) && box != 0 && px == 1 && shorthand && shorthand->ruleSides == 0,
        "border shorthand is a 1px box");
  check(markOf(bag, 0x4E0B, emph, box, px) && emph == ts::kEmLine && box == 0 && px == 1,
        "border-bottom stays a side line");
  check(markOf(bag, 0x7121, emph, box, px) && emph == 0 && box == 0, "border none starts no rule");
  const ts::Atom *emw = findCp(bag, 0x5E45);
  check(emw && (emw->emphasis & ts::kEmphasisMask) == ts::kEmLine && emw->box == 0 && emw->rulePx == 0 &&
            emw->ruleEm == 10 && emw->ruleRoot == 0,
        "0.1em is ten percent of this em");
  const ts::Atom *remw = findCp(bag, 0x6839);
  check(remw && (remw->emphasis & ts::kEmphasisMask) == ts::kEmLine && remw->box == 0 && remw->rulePx == 0 &&
            remw->ruleEm == 25 && remw->ruleRoot == 1,
        "0.25rem is twenty-five percent of the body em");
  const ts::Atom *wide = findCp(bag, 0x7DDA);
  check(wide && (wide->emphasis & ts::kEmphasisMask) == ts::kEmLine && wide->box == 0 && wide->rulePx == 2,
        "a width rule merges onto a one-side style");
  const ts::Atom *pairw = findCp(bag, 0x5BFE);
  check(pairw && (pairw->emphasis & ts::kEmphasisMask) == 0 && pairw->box != 0 && pairw->rulePx == 2 &&
            pairw->ruleSides == (ts::kBorderTop | ts::kBorderBottom),
        "a width rule merges onto a two-side style");
}

struct PlateGrab {
  uint32_t cp = 0;
  int nPic = 0;
};

bool takePlate(void *ctx, const ts::Atom &atom, uint32_t) {
  auto *g = static_cast<PlateGrab *>(ctx);
  if (atom.kind == ts::AtomKind::Picture) {
    g->cp = atom.cp;
    g->nPic++;
  }
  return true;
}

void testForcePlate() {
  const char *html = "<html><body><img src=\"a.jpg\"/></body></html>";
  PlateGrab g;
  ts::AtomSink sink;
  sink.ctx = &g;
  sink.emit = takePlate;
  sink.picture = [](void *, const char *) -> uint16_t { return 3; };
  sink.imageLongSide = [](void *, const char *) -> int { return 200; };
  sink.forcePlate = true;
  check(ts::htmlToAtoms(html, std::strlen(html), sink), "force plate html");
  check(g.nPic == 1 && g.cp == (3u | ts::kPictureForcePage), "a pre-paginated image is its own page");
  g = PlateGrab{};
  sink.ctx = &g;
  sink.forcePlate = false;
  check(ts::htmlToAtoms(html, std::strlen(html), sink), "column plate html");
  check(g.nPic == 1 && g.cp == 3, "the same image stays unmarked until layout");
}

void testLargeStyle() {
  std::string html = "<html><head><style>";
  html.append(70000, ' ');
  html += ".late{border-style:solid none none none;border-width:2px}</style></head><body>"
          "<span class=\"late\">\u3042</span></body></html>";
  AtomBag bag;
  ts::FaceRule rules[4]{};
  uint16_t nRules = 0;
  ts::scanStyleTags(html.data(), html.size(), rules, nRules);
  ts::AtomSink sink{&bag, takeAtom, nullptr, rules, nRules, nullptr};
  check(ts::htmlToAtoms(html.data(), html.size(), sink), "large style html");
  const ts::Atom *a = findCp(bag, 0x3042);
  check(a && a->box == 0 && a->rulePx == 2 && (a->emphasis & ts::kEmphasisMask) == ts::kEmLine,
        "a class past 64KB is a 2px side line");
}

void testRuleOnRun() {
  ts::PageLayouter lay;
  ts::LayoutOptions opt;
  opt.width = 200;
  opt.height = 80;
  opt.em = 20;
  opt.margin = 0;
  opt.mode = ts::WritingMode::HorizontalTb;
  lay.begin(opt);
  ts::Atom atom{};
  atom.cp = 0x3042;
  atom.emphasis = ts::kEmLine;
  atom.rulePx = 2;
  atom.ruleEm = 10;
  atom.ruleRoot = 1;
  atom.ruleSides = ts::kBorderTop | ts::kBorderBottom;
  lay.feed(atom, 0);
  check(lay.finish() && lay.pageGlyphCount() == 1, "rule width page");
  if (lay.pageGlyphCount() == 1) {
    const ts::GlyphRun &g = lay.page()[0];
    check(g.rulePx == 2 && g.ruleEm == 10 && g.ruleRoot == 1 && g.ruleSides == (ts::kBorderTop | ts::kBorderBottom) &&
              (g.emphasis & ts::kEmphasisMask) == ts::kEmLine,
          "the run keeps the css width");
  }
}

void put32be(std::vector<unsigned char> &b, uint32_t v) {
  b.push_back(static_cast<unsigned char>(v >> 24));
  b.push_back(static_cast<unsigned char>(v >> 16));
  b.push_back(static_cast<unsigned char>(v >> 8));
  b.push_back(static_cast<unsigned char>(v));
}

void put16le(std::vector<unsigned char> &b, uint16_t v) {
  b.push_back(static_cast<unsigned char>(v));
  b.push_back(static_cast<unsigned char>(v >> 8));
}

void put32le(std::vector<unsigned char> &b, uint32_t v) {
  b.push_back(static_cast<unsigned char>(v));
  b.push_back(static_cast<unsigned char>(v >> 8));
  b.push_back(static_cast<unsigned char>(v >> 16));
  b.push_back(static_cast<unsigned char>(v >> 24));
}

void putBe32At(std::vector<unsigned char> &b, size_t at, uint32_t v) {
  b[at] = static_cast<unsigned char>(v >> 24);
  b[at + 1] = static_cast<unsigned char>(v >> 16);
  b[at + 2] = static_cast<unsigned char>(v >> 8);
  b[at + 3] = static_cast<unsigned char>(v);
}

// Uncompressed KF8. PalmDOC text length is flow 0. A later flow is past 64KB.
bool writeLateKf8(const char *path) {
  const std::string html = "<html class=\"hltr\"><body><p><span class=\"late\">\u3042</span></p></body></html>";
  const std::string css1 = "p{font-weight:normal}";
  std::string raw = html + css1;
  raw.append(70000, ' ');
  raw += ".late{border-style:solid none none none;border-width:2px}";
  const uint32_t flow0 = static_cast<uint32_t>(html.size());
  const uint32_t flow1 = flow0 + static_cast<uint32_t>(css1.size());
  const uint32_t flow2 = static_cast<uint32_t>(raw.size());
  constexpr uint32_t kRec = 4096;
  const uint16_t textCount = static_cast<uint16_t>((raw.size() + kRec - 1) / kRec);
  const uint16_t nrec = static_cast<uint16_t>(textCount + 2);

  std::vector<unsigned char> head(16u + 0xF8u, 0);
  head[1] = 1;
  putBe32At(head, 4, flow0);
  head[8] = static_cast<unsigned char>(textCount >> 8);
  head[9] = static_cast<unsigned char>(textCount);
  head[10] = 0x10;
  std::memcpy(head.data() + 16, "MOBI", 4);
  putBe32At(head, 20, 0xF8);
  putBe32At(head, 24, 2);
  putBe32At(head, 28, 65001);
  putBe32At(head, 36, 8);
  putBe32At(head, 0x6c, 0xFFFFFFFFu);
  putBe32At(head, 0x70, 0xFFFFFFFFu);
  putBe32At(head, 0xc0, static_cast<uint32_t>(textCount + 1));
  putBe32At(head, 0xf8, 0xFFFFFFFFu);
  putBe32At(head, 0xfc, 0xFFFFFFFFu);

  std::vector<unsigned char> fdst;
  fdst.insert(fdst.end(), {'F', 'D', 'S', 'T', 0, 0, 0, 0});
  put32be(fdst, 3);
  put32be(fdst, 0);
  put32be(fdst, flow0);
  put32be(fdst, flow0);
  put32be(fdst, flow1);
  put32be(fdst, flow1);
  put32be(fdst, flow2);

  std::vector<unsigned char> file(78, 0);
  std::memcpy(file.data() + 60, "BOOK", 4);
  std::memcpy(file.data() + 64, "MOBI", 4);
  file[76] = static_cast<unsigned char>(nrec >> 8);
  file[77] = static_cast<unsigned char>(nrec);
  const uint32_t data0 = 78u + static_cast<uint32_t>(nrec) * 8u;
  file.resize(data0, 0);
  auto note = [&](uint16_t i, uint32_t off) {
    const size_t at = 78u + static_cast<size_t>(i) * 8u;
    file[at] = static_cast<unsigned char>(off >> 24);
    file[at + 1] = static_cast<unsigned char>(off >> 16);
    file[at + 2] = static_cast<unsigned char>(off >> 8);
    file[at + 3] = static_cast<unsigned char>(off);
    file[at + 7] = static_cast<unsigned char>(i);
  };
  note(0, data0);
  file.insert(file.end(), head.begin(), head.end());
  uint32_t off = data0 + static_cast<uint32_t>(head.size());
  for (uint16_t t = 0; t < textCount; ++t) {
    note(static_cast<uint16_t>(t + 1), off);
    const size_t a = static_cast<size_t>(t) * kRec;
    const size_t n = a + kRec < raw.size() ? kRec : raw.size() - a;
    file.insert(file.end(), raw.begin() + static_cast<std::ptrdiff_t>(a),
                raw.begin() + static_cast<std::ptrdiff_t>(a + n));
    off += static_cast<uint32_t>(n);
  }
  note(static_cast<uint16_t>(textCount + 1), off);
  file.insert(file.end(), fdst.begin(), fdst.end());
  return writeBytes(path, file.data(), file.size());
}

struct ZipPart {
  const char *name;
  const std::string *bytes;
};

bool writeStoredZip(const char *path, const ZipPart *parts, int n) {
  std::vector<unsigned char> out;
  std::vector<uint32_t> local;
  for (int i = 0; i < n; ++i) {
    local.push_back(static_cast<uint32_t>(out.size()));
    const uint16_t nameN = static_cast<uint16_t>(std::strlen(parts[i].name));
    const uint32_t len = static_cast<uint32_t>(parts[i].bytes->size());
    put32le(out, 0x04034b50);
    put16le(out, 20);
    put16le(out, 0);
    put16le(out, 0);
    put16le(out, 0);
    put16le(out, 0);
    put32le(out, 0);
    put32le(out, len);
    put32le(out, len);
    put16le(out, nameN);
    put16le(out, 0);
    out.insert(out.end(), parts[i].name, parts[i].name + nameN);
    out.insert(out.end(), parts[i].bytes->begin(), parts[i].bytes->end());
  }
  const uint32_t cd = static_cast<uint32_t>(out.size());
  for (int i = 0; i < n; ++i) {
    const uint16_t nameN = static_cast<uint16_t>(std::strlen(parts[i].name));
    const uint32_t len = static_cast<uint32_t>(parts[i].bytes->size());
    put32le(out, 0x02014b50);
    put16le(out, 20);
    put16le(out, 20);
    put16le(out, 0);
    put16le(out, 0);
    put16le(out, 0);
    put16le(out, 0);
    put32le(out, 0);
    put32le(out, len);
    put32le(out, len);
    put16le(out, nameN);
    put16le(out, 0);
    put16le(out, 0);
    put16le(out, 0);
    put16le(out, 0);
    put32le(out, 0);
    put32le(out, local[static_cast<size_t>(i)]);
    out.insert(out.end(), parts[i].name, parts[i].name + nameN);
  }
  const uint32_t cdN = static_cast<uint32_t>(out.size()) - cd;
  put32le(out, 0x06054b50);
  put16le(out, 0);
  put16le(out, 0);
  put16le(out, static_cast<uint16_t>(n));
  put16le(out, static_cast<uint16_t>(n));
  put32le(out, cdN);
  put32le(out, cd);
  put16le(out, 0);
  return writeBytes(path, out.data(), out.size());
}

bool atomHas(const char *path, uint32_t cp, uint8_t &emph, uint8_t &box, uint8_t &px) {
  ts::AtomReader reader;
  if (!reader.open(path)) {
    return false;
  }
  ts::Atom atom{};
  while (reader.next(atom)) {
    if (atom.kind == ts::AtomKind::Ch && atom.cp == cp) {
      emph = static_cast<uint8_t>(atom.emphasis & ts::kEmphasisMask);
      box = atom.box;
      px = atom.rulePx;
      return true;
    }
  }
  return false;
}

void testLateKf8() {
  char dir[] = "/tmp/yomu-kf8-XXXXXX";
  if (!mkdtemp(dir)) {
    check(false, "kf8 dir");
    return;
  }
  ts_hal_set_root(dir);
  char path[256];
  std::snprintf(path, sizeof path, "%s/late.mobi", dir);
  check(writeLateKf8(path), "write late kf8");
  ts::MobiBook book;
  const bool opened = book.open(path, "/.yomu/late.atom");
  check(opened, book.lastError()[0] ? book.lastError() : "open late kf8");
  uint8_t emph = 0;
  uint8_t box = 0;
  uint8_t px = 0;
  check(opened && atomHas("/.yomu/late.atom", 0x3042, emph, box, px) && emph == ts::kEmLine && box == 0 && px == 2,
        "a later kf8 flow past 64KB is a 2px side line");
}

void testFixedEpub() {
  char dir[] = "/tmp/yomu-epub-XXXXXX";
  if (!mkdtemp(dir)) {
    check(false, "epub dir");
    return;
  }
  ts_hal_set_root(dir);
  const std::string container =
      "<?xml version=\"1.0\"?><container version=\"1.0\"><rootfiles>"
      "<rootfile full-path=\"OEBPS/content.opf\" media-type=\"application/oebps-package+xml\"/>"
      "</rootfiles></container>";
  const std::string chapter =
      "<?xml version=\"1.0\"?><html><body><p>"
      "\u3042\u3044\u3046\u3048\u304A\u304B\u304D\u304F\u3051\u3053\u3055\u3057\u3059\u305B\u305D"
      "\u305F\u3061\u3064\u3066\u3068\u306A\u306B\u306C\u306D\u306E\u306F\u3072\u3075\u3078\u307B"
      "</p></body></html>";
  const std::string fixedOpf =
      "<?xml version=\"1.0\"?><package version=\"3.0\"><metadata>"
      "<meta property=\"rendition:layout\" content=\"pre-paginated\"/>"
      "</metadata><manifest>"
      "<item id=\"a\" href=\"a.xhtml\" media-type=\"application/xhtml+xml\"/>"
      "<item id=\"b\" href=\"b.xhtml\" media-type=\"application/xhtml+xml\"/>"
      "</manifest><spine>"
      "<itemref idref=\"a\" properties=\"rendition:layout-pre-paginated\"/>"
      "<itemref idref=\"b\" properties=\"rendition:layout-pre-paginated\"/>"
      "</spine></package>";
  const std::string mixedOpf =
      "<?xml version=\"1.0\"?><package version=\"3.0\"><metadata></metadata><manifest>"
      "<item id=\"a\" href=\"a.xhtml\" media-type=\"application/xhtml+xml\"/>"
      "<item id=\"b\" href=\"b.xhtml\" media-type=\"application/xhtml+xml\"/>"
      "</manifest><spine>"
      "<itemref idref=\"a\" properties=\"rendition:layout-pre-paginated\"/>"
      "<itemref idref=\"b\"/>"
      "</spine></package>";
  std::string css(70000, ' ');
  css += ".late{border-style:solid none none none;border-width:2px}";
  const std::string sheetChapter =
      "<?xml version=\"1.0\"?><html><head><link rel=\"stylesheet\" href=\"late.css\"/></head>"
      "<body><p><span class=\"late\">\u3044</span>"
      "\u3042\u3046\u3048\u304A\u304B\u304D\u304F\u3051\u3053\u3055\u3057\u3059\u305B\u305D"
      "\u305F\u3061\u3064\u3066\u3068\u306A\u306B\u306C\u306D\u306E\u306F\u3072\u3075\u3078\u307B"
      "</p></body></html>";
  const std::string sheetOpf =
      "<?xml version=\"1.0\"?><package version=\"3.0\"><metadata></metadata><manifest>"
      "<item id=\"c\" href=\"c.xhtml\" media-type=\"application/xhtml+xml\"/>"
      "<item id=\"s\" href=\"late.css\" media-type=\"text/css\"/>"
      "</manifest><spine><itemref idref=\"c\"/></spine></package>";

  char fixedPath[256];
  char mixedPath[256];
  char sheetPath[256];
  std::snprintf(fixedPath, sizeof fixedPath, "%s/fixed.epub", dir);
  std::snprintf(mixedPath, sizeof mixedPath, "%s/mixed.epub", dir);
  std::snprintf(sheetPath, sizeof sheetPath, "%s/sheet.epub", dir);
  const ZipPart fixedParts[] = {{"META-INF/container.xml", &container},
                                {"OEBPS/content.opf", &fixedOpf},
                                {"OEBPS/a.xhtml", &chapter},
                                {"OEBPS/b.xhtml", &chapter}};
  const ZipPart mixedParts[] = {{"META-INF/container.xml", &container},
                                {"OEBPS/content.opf", &mixedOpf},
                                {"OEBPS/a.xhtml", &chapter},
                                {"OEBPS/b.xhtml", &chapter}};
  const ZipPart sheetParts[] = {{"META-INF/container.xml", &container},
                                {"OEBPS/content.opf", &sheetOpf},
                                {"OEBPS/c.xhtml", &sheetChapter},
                                {"OEBPS/late.css", &css}};
  check(writeStoredZip(fixedPath, fixedParts, 4), "write fixed epub");
  check(writeStoredZip(mixedPath, mixedParts, 4), "write mixed epub");
  check(writeStoredZip(sheetPath, sheetParts, 4), "write sheet epub");

  ts::EpubBook fixed;
  check(!fixed.open(fixedPath, "/.yomu/fixed.atom") && std::strcmp(fixed.lastError(), "fixed layout") == 0,
        "an all pre-paginated spine is fixed layout");
  ts::EpubBook mixed;
  const bool mixedOk = mixed.open(mixedPath, "/.yomu/mixed.atom");
  check(mixedOk && mixed.spineCount() == 2, mixed.lastError()[0] ? mixed.lastError() : "a mixed spine stays open");
  ts::EpubBook sheet;
  const bool sheetOk = sheet.open(sheetPath, "/.yomu/sheet.atom");
  check(sheetOk, sheet.lastError()[0] ? sheet.lastError() : "open linked sheet");
  uint8_t emph = 0;
  uint8_t box = 0;
  uint8_t px = 0;
  check(sheetOk && atomHas("/.yomu/sheet.atom", 0x3044, emph, box, px) && emph == ts::kEmLine && box == 0 && px == 2,
        "a linked sheet past 64KB is a 2px side line");
}

void testLongPath() {
  char dir[] = "/tmp/yomu-long-XXXXXX";
  if (!mkdtemp(dir)) {
    check(false, "long path dir");
    return;
  }
  ts_hal_set_root(dir);
  std::string nest = dir;
  nest.push_back('/');
  nest.append(200, 'n');
  check(mkdir(nest.c_str(), 0755) == 0, "long path nest");
  std::string path = nest;
  path.push_back('/');
  path.append(80, 'm');
  path += ".epub";
  check(path.size() > 255 && path.size() < 1024, "fixture path sits between 256 and 1024");

  const std::string container =
      "<?xml version=\"1.0\"?><container version=\"1.0\"><rootfiles>"
      "<rootfile full-path=\"OEBPS/content.opf\" media-type=\"application/oebps-package+xml\"/>"
      "</rootfiles></container>";
  const std::string chapter =
      "<?xml version=\"1.0\"?><html><body><p>"
      "\u3042\u3044\u3046\u3048\u304A\u304B\u304D\u304F\u3051\u3053\u3055\u3057\u3059\u305B\u305D"
      "\u305F\u3061\u3064\u3066\u3068\u306A\u306B\u306C\u306D\u306E\u306F\u3072\u3075\u3078\u307B"
      "</p></body></html>";
  const std::string opf =
      "<?xml version=\"1.0\"?><package version=\"3.0\"><metadata></metadata><manifest>"
      "<item id=\"c\" href=\"c.xhtml\" media-type=\"application/xhtml+xml\"/>"
      "</manifest><spine><itemref idref=\"c\"/></spine></package>";
  const ZipPart parts[] = {{"META-INF/container.xml", &container},
                           {"OEBPS/content.opf", &opf},
                           {"OEBPS/c.xhtml", &chapter}};
  check(writeStoredZip(path.c_str(), parts, 3), "write long-path epub");

  ts::ZipArchive zip;
  const bool zipOk = zip.open(path.c_str());
  check(zipOk && zip.count() == 3, zip.lastError()[0] ? zip.lastError() : "zip a path past 255 bytes");
  zip.close();

  ts::EpubBook epub;
  const bool epubOk = epub.open(path.c_str(), "/.yomu/long.atom");
  check(epubOk, epub.lastError()[0] ? epub.lastError() : "epub a path past 255 bytes");

  const char *font = nullptr;
  const char *cands[] = {"/System/Library/Fonts/Supplemental/Arial Unicode.ttf",
                         "/Library/Fonts/Arial Unicode.ttf"};
  for (const char *cand : cands) {
    if (access(cand, R_OK) == 0) {
      font = cand;
      break;
    }
  }
  check(font != nullptr, "font for a long path");
  if (!font) {
    return;
  }
  TypesetBook book;
  const bool opened = book.open(path.c_str(), nullptr, nullptr, font);
  check(opened, book.lastError()[0] ? book.lastError() : "open a path past 255 bytes");
  check(opened && std::strcmp(book.path(), path.c_str()) == 0, "the stored path keeps its extension");
  book.close();

  std::string huge(1100, 'm');
  huge += ".epub";
  TypesetBook over;
  check(!over.open(huge.c_str(), nullptr, nullptr, font) && std::strcmp(over.lastError(), "path long") == 0,
        "a path past 1024 bytes is path long");
  ts::ZipArchive zipOver;
  check(!zipOver.open(huge.c_str()) && std::strcmp(zipOver.lastError(), "path long") == 0,
        "a zip path past 1024 bytes is path long");
}

}  // namespace

extern "C" int test_typeset_rules(void) {
  g_fails = 0;
  testKinsoku();
  testBracketPair();
  testLayout();
  testToc();
  testKoMidashi();
  testGfontCss();
  testAtomFace();
  testFace();
  testCollection();
  testColumnPicture();
  testPictures();
  testBoxPad();
  testAozora();
  testMarks();
  testSwatch();
  testOneSide();
  testForcePlate();
  testLargeStyle();
  testRuleOnRun();
  testLateKf8();
  testFixedEpub();
  testLongPath();
  return g_fails;
}
