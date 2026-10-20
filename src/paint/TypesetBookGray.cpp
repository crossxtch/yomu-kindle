#include "TypesetBook.h"

#include "PicturePage.h"

#include <Logging.h>

#include <cstring>

/* Paint one laid-out page into an 8-bit gray buffer. Positions follow the
 * column grid. Glyphs are FreeType gray: vertical forms when the face has
 * them, body ink on the em, darker ink winning where glyphs overlap. */

namespace {

uint32_t fallbackCp(const uint32_t cp) {
  if (cp >= 0x2460 && cp <= 0x2468) {
    return static_cast<uint32_t>('1' + (cp - 0x2460));
  }
  if (cp == 0x2469) {
    return static_cast<uint32_t>('0');
  }
  return cp;
}

uint32_t pickCp(FtFont& font, const uint32_t cp) {
  if (font.glyphId(cp) != 0xFFFF) {
    return cp;
  }
  const uint32_t alt = fallbackCp(cp);
  if (alt != cp && font.glyphId(alt) != 0xFFFF) {
    return alt;
  }
  return 0;
}

void fillGray(uint8_t* gray, const int w, const int h, const int x, const int y, const int rw, const int rh,
              const uint8_t ink) {
  if (!gray || rw <= 0 || rh <= 0) {
    return;
  }
  for (int row = 0; row < rh; ++row) {
    const int dy = y + row;
    if (dy < 0 || dy >= h) {
      continue;
    }
    for (int col = 0; col < rw; ++col) {
      const int dx = x + col;
      if (dx < 0 || dx >= w) {
        continue;
      }
      uint8_t* dst = gray + static_cast<size_t>(dy) * static_cast<size_t>(w) + static_cast<size_t>(dx);
      if (ink < *dst) {
        *dst = ink;
      }
    }
  }
}

void inkBox(uint8_t* gray, const int w, const int h, const int x, const int y, const int rw, const int rh) {
  if (!gray || rw <= 0 || rh <= 0) {
    return;
  }
  for (int row = 0; row < rh; ++row) {
    const int dy = y + row;
    if (dy < 0 || dy >= h) {
      continue;
    }
    for (int col = 0; col < rw; ++col) {
      const int dx = x + col;
      if (dx < 0 || dx >= w) {
        continue;
      }
      gray[static_cast<size_t>(dy) * static_cast<size_t>(w) + static_cast<size_t>(dx)] = 0;
    }
  }
}

void inkDot(uint8_t* gray, const int w, const int h, const int cx, const int cy, const int rad) {
  const int r2 = rad * rad;
  for (int dy = -rad; dy <= rad; ++dy) {
    for (int dx = -rad; dx <= rad; ++dx) {
      if (dx * dx + dy * dy <= r2) {
        inkBox(gray, w, h, cx + dx, cy + dy, 1, 1);
      }
    }
  }
}

void inkRing(uint8_t* gray, const int w, const int h, const int cx, const int cy, int rad) {
  if (rad < 2) {
    rad = 2;
  }
  int thick = rad / 4;
  if (thick < 1) {
    thick = 1;
  }
  if (thick >= rad) {
    thick = rad - 1;
  }
  const int inner = rad - thick;
  const int outer2 = rad * rad;
  const int inner2 = inner * inner;
  for (int dy = -rad; dy <= rad; ++dy) {
    for (int dx = -rad; dx <= rad; ++dx) {
      const int d2 = dx * dx + dy * dy;
      if (d2 <= outer2 && d2 > inner2) {
        inkBox(gray, w, h, cx + dx, cy + dy, 1, 1);
      }
    }
  }
}

void inkTri(uint8_t* gray, const int w, const int h, const int cx, const int cy, int rad, const bool pointRight) {
  if (rad < 1) {
    rad = 1;
  }
  for (int dy = -rad; dy <= rad; ++dy) {
    for (int dx = -rad; dx <= rad; ++dx) {
      bool in = false;
      if (pointRight) {
        const int half = (rad - dx) / 2;
        in = dy >= -half && dy <= half;
      } else {
        const int half = (rad + dy) / 2;
        in = dx >= -half && dx <= half;
      }
      if (in) {
        inkBox(gray, w, h, cx + dx, cy + dy, 1, 1);
      }
    }
  }
}

void inkFrame(uint8_t* gray, const int w, const int h, const int x, const int y, const int rw, const int rh,
              const int thick, const int sides) {
  if (rw <= 0 || rh <= 0 || thick <= 0) {
    return;
  }
  const bool full = sides == 0 || (sides & 0x0F) == 0x0F;
  if (full && (thick * 2 >= rw || thick * 2 >= rh)) {
    inkBox(gray, w, h, x, y, rw, rh);
    return;
  }
  if (full || (sides & ts::kBorderTop) != 0) {
    inkBox(gray, w, h, x, y, rw, thick);
  }
  if (full || (sides & ts::kBorderBottom) != 0) {
    inkBox(gray, w, h, x, y + rh - thick, rw, thick);
  }
  if (full || (sides & ts::kBorderLeft) != 0) {
    inkBox(gray, w, h, x, y, thick, rh);
  }
  if (full || (sides & ts::kBorderRight) != 0) {
    inkBox(gray, w, h, x + rw - thick, y, thick, rh);
  }
}

// CSS px wins. An em or rem width is a percent of this glyph, or of the body em when ruleRoot is set.
int cssThick(const ts::GlyphRun& g, const int bodyEm) {
  if (g.rulePx > 0) {
    return g.rulePx;
  }
  if (g.ruleEm == 0) {
    return 0;
  }
  int ref = g.ruleRoot ? bodyEm : static_cast<int>(g.size);
  if (ref < 1) {
    ref = bodyEm > 0 ? bodyEm : 1;
  }
  int thick = ref * static_cast<int>(g.ruleEm) / 100;
  if (thick < 1) {
    thick = 1;
  }
  return thick;
}

int markRad(const int em, const int num, const int den) {
  int r = em * num / den;
  if (r < 1) {
    r = 1;
  }
  return r;
}

void paintMark(uint8_t* gray, const int w, const int h, const uint8_t kind, const int cx, const int cy, const int em,
               const bool vert) {
  const int sesame = markRad(em, 1, 10);
  const int dot = markRad(em, 8, 100);
  const int circle = markRad(em, 14, 100);
  switch (kind) {
    case ts::kEmSesameOpen:
      inkRing(gray, w, h, cx, cy, sesame);
      break;
    case ts::kEmDot:
      inkDot(gray, w, h, cx, cy, dot);
      break;
    case ts::kEmDotOpen:
      inkRing(gray, w, h, cx, cy, dot);
      break;
    case ts::kEmCircle:
      inkDot(gray, w, h, cx, cy, circle);
      break;
    case ts::kEmCircleOpen:
      inkRing(gray, w, h, cx, cy, circle);
      break;
    case ts::kEmDoubleCircle:
      inkRing(gray, w, h, cx, cy, circle);
      inkDot(gray, w, h, cx, cy, markRad(circle, 36, 100));
      break;
    case ts::kEmDoubleCircleOpen:
      inkRing(gray, w, h, cx, cy, circle < 3 ? 3 : circle);
      if (circle >= 4) {
        inkRing(gray, w, h, cx, cy, circle * 44 / 100);
      }
      break;
    case ts::kEmTriangle:
      inkTri(gray, w, h, cx, cy, markRad(em, 11, 100), vert);
      break;
    default:
      inkDot(gray, w, h, cx, cy, sesame);
      break;
  }
}

void paintShades(uint8_t* gray, const int w, const int h, const ts::GlyphRun* runs, const uint16_t n,
                 const int fallbackEm) {
  for (uint16_t i = 0; i < n; ++i) {
    const ts::GlyphRun& g = runs[i];
    if (g.shade < 2 || ts::runPicture(g)) {
      continue;
    }
    const bool vert = !ts::runRubyAbove(g);
    const int em = g.size > 0 ? static_cast<int>(g.size) : fallbackEm;
    const int adv = ts::runAdvance(g);
    int x = g.x;
    int y = g.y;
    int rw = vert ? em : adv;
    int rh = vert ? adv : em;
    if (g.box != 0 && em > 0) {
      int outset = em * 12 / 100;
      if (outset < 1) {
        outset = 1;
      }
      x -= outset;
      y -= outset;
      rw += outset * 2;
      rh += outset * 2;
    }
    fillGray(gray, w, h, x, y, rw, rh, g.shade);
  }
}

void paintBoxes(uint8_t* gray, const int w, const int h, const ts::GlyphRun* runs, const uint16_t n,
                const int fallbackEm) {
  for (uint16_t i = 0; i < n; ++i) {
    const ts::GlyphRun& g = runs[i];
    if (g.box == 0 || ts::runPicture(g)) {
      continue;
    }
    const bool vert = !ts::runRubyAbove(g);
    const int key = vert ? g.x : g.y;
    bool earlier = false;
    for (uint16_t j = 0; j < i; ++j) {
      const ts::GlyphRun& o = runs[j];
      if (o.box != g.box || ts::runPicture(o)) {
        continue;
      }
      const bool ov = !ts::runRubyAbove(o);
      if (ov == vert && (ov ? o.x : o.y) == key) {
        earlier = true;
        break;
      }
    }
    if (earlier) {
      continue;
    }
    int em = g.size > 0 ? static_cast<int>(g.size) : fallbackEm;
    int minX = g.x;
    int minY = g.y;
    int maxX = g.x + (vert ? (g.size > 0 ? static_cast<int>(g.size) : em) : ts::runAdvance(g));
    int maxY = g.y + (vert ? ts::runAdvance(g) : (g.size > 0 ? static_cast<int>(g.size) : em));
    for (uint16_t j = static_cast<uint16_t>(i + 1); j < n; ++j) {
      const ts::GlyphRun& o = runs[j];
      if (o.box != g.box || ts::runPicture(o)) {
        continue;
      }
      const bool ov = !ts::runRubyAbove(o);
      if (ov != vert || (ov ? o.x : o.y) != key) {
        continue;
      }
      const int oem = o.size > 0 ? static_cast<int>(o.size) : em;
      if (o.x < minX) {
        minX = o.x;
      }
      if (o.y < minY) {
        minY = o.y;
      }
      const int ox = o.x + (ov ? (o.size > 0 ? static_cast<int>(o.size) : oem) : ts::runAdvance(o));
      const int oy = o.y + (ov ? ts::runAdvance(o) : (o.size > 0 ? static_cast<int>(o.size) : oem));
      if (ox > maxX) {
        maxX = ox;
      }
      if (oy > maxY) {
        maxY = oy;
      }
      if (oem > em) {
        em = oem;
      }
    }
    int outset = em * 12 / 100;
    if (outset < 1) {
      outset = 1;
    }
    int thick = cssThick(g, fallbackEm);
    int sides = g.ruleSides;
    for (uint16_t j = static_cast<uint16_t>(i + 1); j < n && (thick == 0 || sides == 0); ++j) {
      const ts::GlyphRun& o = runs[j];
      if (o.box != g.box || ts::runPicture(o)) {
        continue;
      }
      if (thick == 0) {
        thick = cssThick(o, fallbackEm);
      }
      if (sides == 0 && o.ruleSides != 0) {
        sides = o.ruleSides;
      }
    }
    if (thick < 1) {
      thick = em * 6 / 100;
    }
    if (thick < 1) {
      thick = 1;
    }
    inkFrame(gray, w, h, minX - outset, minY - outset, (maxX - minX) + outset * 2, (maxY - minY) + outset * 2, thick,
             sides);
  }
}

}  // namespace

bool TypesetBook::renderGray(uint8_t* gray, const int w, const int h, const uint32_t globalPage) {
  if (!opened || !gray || w != layoutOpt.width || h != layoutOpt.height || w <= 0 || h <= 0) {
    error = "page size";
    return false;
  }
  if (!ensureGlobal(globalPage)) {
    return false;
  }
  const uint32_t local = localOf(globalPage);
  if (local == 0xFFFFFFFFu || !layoutPage(local)) {
    return false;
  }
  memset(gray, 255, static_cast<size_t>(w) * static_cast<size_t>(h));
  if (loadedPicture != 0) {
    const unsigned long tPic = millis();
    if (!ts::picturePaintInset(picturePath, loadedPicture, gray, w, h, layoutOpt.margin)) {
      LOG_INF("TS", "picture %u blank", static_cast<unsigned>(loadedPicture));
    }
    LOG_INF("TS", "time picture %u %lums page=%lu", static_cast<unsigned>(loadedPicture), millis() - tPic,
            static_cast<unsigned long>(globalPage + 1));
    extendAhead(globalPage, 2);
    return true;
  }

  const int em = font.emPx();
  const int rubyEm = font.rubyEmPx();
  paintShades(gray, w, h, loadedGlyphs, loadedCount, em);
  for (uint16_t i = 0; i < loadedCount; ++i) {
    const ts::GlyphRun& g = loadedGlyphs[i];
    if (ts::runPicture(g)) {
      const int bw = g.size;
      const int bh = ts::runAdvance(g);
      if (g.cp != 0 && bw > 0 && bh > 0) {
        ts::picturePaintBox(picturePath, static_cast<uint16_t>(g.cp), gray, w, h, g.x, g.y, bw, bh);
      }
      continue;
    }
    const bool vert = !ts::runRubyAbove(g);
    const int adv = ts::runAdvance(g);
    const bool tcyRun = ts::runTcy(g) && g.rubyCount > 1;
    FtFont* ink = &font;
    if ((g.flags & ts::kRunGothic) != 0 && gothic.loaded()) {
      ink = &gothic;
    }
    auto cpOf = [&](FtFont*& used, const uint32_t raw) -> uint32_t {
      uint32_t cp = pickCp(*used, raw);
      if (cp == 0 && used != &font) {
        used = &font;
        cp = pickCp(*used, raw);
      }
      return cp;
    };

    if (tcyRun) {
      const int n = g.rubyCount > ts::kMaxCluster ? ts::kMaxCluster : g.rubyCount;
      const int face = g.size > 0 ? static_cast<int>(g.size) : em;
      int size = face * 45 / 100;
      if (size < 1) {
        size = 1;
      }
      int widths[ts::kMaxCluster]{};
      uint32_t cps[ts::kMaxCluster]{};
      FtFont* faces[ts::kMaxCluster]{};
      int total = 0;
      for (int r = 0; r < n; ++r) {
        FtFont* used = ink;
        cps[r] = cpOf(used, g.ruby[r]);
        faces[r] = used;
        int advW = cps[r] != 0 ? used->advancePx(cps[r], size) : size;
        if (advW < 1) {
          advW = size;
        }
        widths[r] = advW;
        total += advW;
      }
      int x0 = g.x + (face - total) / 2;
      const int y0 = g.y + (face - size) / 2;
      const bool bold = (g.flags & ts::kRunBold) != 0;
      for (int r = 0; r < n; ++r) {
        if (cps[r] != 0) {
          faces[r]->drawGray(gray, w, h, x0, y0, widths[r], size, cps[r], size, false, bold);
        }
        x0 += widths[r];
      }
    } else {
      const int emBox = g.size > 0 ? static_cast<int>(g.size) : em;
      const int cellW = vert ? emBox : adv;
      const int cellH = vert ? adv : emBox;
      FtFont* used = ink;
      const uint32_t cp = cpOf(used, g.cp);
      const bool bold = (g.flags & ts::kRunBold) != 0;
      if (cp != 0 && cellW > 0 && cellH > 0) {
        used->drawGray(gray, w, h, g.x, g.y, cellW, cellH, cp, emBox > 0 ? emBox : 1, vert, bold);
      }
    }

    if (g.emphasis != 0 && em > 0) {
      const bool hasRuby = !ts::runTcy(g) && g.rubyCount > 0;
      int gap = em / (hasRuby ? 10 : 4);
      if (gap < 1) {
        gap = 1;
      }
      if (g.emphasis == ts::kEmLine) {
        int thick = cssThick(g, em);
        if (thick < 1) {
          thick = em / 14;
        }
        if (thick < 1) {
          thick = 1;
        }
        if (vert) {
          inkBox(gray, w, h, g.x + em + gap, g.y, thick, adv);
        } else {
          inkBox(gray, w, h, g.x, g.y - gap - thick, adv, thick);
        }
      } else {
        const int cx = vert ? g.x + em + gap : g.x + adv / 2;
        const int cy = vert ? g.y + adv / 2 : g.y - gap;
        paintMark(gray, w, h, g.emphasis, cx, cy, em, vert);
      }
    }

    if (tcyRun || g.rubyCount == 0 || rubyEm == 0) {
      continue;
    }

    const int lead = g.rubyLead;
    const int origin = (vert ? g.y : g.x) - lead;
    int lastEnd = (vert ? g.y : g.x) + adv;
    for (uint16_t j = static_cast<uint16_t>(i + 1); j < loadedCount; ++j) {
      const ts::GlyphRun& o = loadedGlyphs[j];
      if (!ts::runSticky(o)) {
        break;
      }
      if (vert) {
        if (o.x != g.x) {
          break;
        }
      } else if (o.y != g.y) {
        break;
      }
      lastEnd = (vert ? o.y : o.x) + ts::runAdvance(o);
    }
    const int span = lastEnd - origin + lead;
    if (span <= 0) {
      continue;
    }
    const int clusterLo = origin;
    const int clusterHi = origin + span;
    auto inside = [&](const ts::GlyphRun& o) -> bool {
      if (vert) {
        return o.x == g.x && o.y >= clusterLo && o.y < clusterHi;
      }
      return o.y == g.y && o.x >= clusterLo && o.x < clusterHi;
    };
    auto blocks = [&](const int dir) -> bool {
      const ts::GlyphRun* best = nullptr;
      int bestAbs = 0;
      for (uint16_t j = 0; j < loadedCount; ++j) {
        const ts::GlyphRun& o = loadedGlyphs[j];
        if (inside(o)) {
          continue;
        }
        if (vert) {
          if (o.x != g.x) {
            continue;
          }
        } else if (o.y != g.y) {
          continue;
        }
        const int d = (vert ? o.y : o.x) - origin;
        if ((dir < 0 && d >= 0) || (dir > 0 && d <= 0)) {
          continue;
        }
        const int ad = d < 0 ? -d : d;
        if (!best || ad < bestAbs) {
          best = &o;
          bestAbs = ad;
        }
      }
      return best && best->rubyCount > 0 && bestAbs <= span;
    };
    const ts::RubyAlong along = ts::placeRubyAlong(static_cast<int16_t>(origin), static_cast<int16_t>(span),
                                                   static_cast<int16_t>(rubyEm), g.rubyCount, !blocks(-1), !blocks(1));
    const int body = g.size > 0 ? static_cast<int>(g.size) : em;
    const int beside = g.emphasis ? body * 28 / 100 : 0;
    const int pitch = along.pitch > 0 ? along.pitch : rubyEm;
    for (uint8_t r = 0; r < g.rubyCount; ++r) {
      FtFont* used = ink;
      const uint32_t cp = cpOf(used, g.ruby[r]);
      if (cp == 0) {
        continue;
      }
      const int at = along.start + r * along.pitch;
      if (vert) {
        used->drawGray(gray, w, h, g.x + body + beside, at, rubyEm, pitch, cp, rubyEm, false, false);
      } else {
        used->drawGray(gray, w, h, at, g.y - rubyEm - beside, pitch, rubyEm, cp, rubyEm, false, false);
      }
    }
  }

  paintBoxes(gray, w, h, loadedGlyphs, loadedCount, em);
  extendAhead(globalPage, 2);
  error = "";
  return true;
}
