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

uint32_t pickCp(XgfFont& font, const uint32_t cp) {
  if (font.glyphId(cp) != 0xFFFF) {
    return cp;
  }
  const uint32_t alt = fallbackCp(cp);
  if (alt != cp && font.glyphId(alt) != 0xFFFF) {
    return alt;
  }
  return 0;
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
    if (!ts::picturePaint(picturePath, loadedPicture, gray, w, h)) {
      LOG_INF("TS", "picture %u blank", static_cast<unsigned>(loadedPicture));
    }
    extendAhead(globalPage, 2);
    return true;
  }

  const int em = font.emPx();
  const int rubyEm = font.rubyEmPx();
  for (uint16_t i = 0; i < loadedCount; ++i) {
    const ts::GlyphRun& g = loadedGlyphs[i];
    const bool vert = !ts::runRubyAbove(g);
    const int adv = ts::runAdvance(g);
    const bool tcyRun = ts::runTcy(g) && g.rubyCount > 1;

    if (tcyRun) {
      const int n = g.rubyCount > ts::kMaxCluster ? ts::kMaxCluster : g.rubyCount;
      int size = em * 45 / 100;
      if (size < 1) {
        size = 1;
      }
      int widths[ts::kMaxCluster]{};
      uint32_t cps[ts::kMaxCluster]{};
      int total = 0;
      for (int r = 0; r < n; ++r) {
        cps[r] = pickCp(font, g.ruby[r]);
        int advW = cps[r] != 0 ? font.advancePx(cps[r], size) : size;
        if (advW < 1) {
          advW = size;
        }
        widths[r] = advW;
        total += advW;
      }
      int x0 = g.x + (em - total) / 2;
      const int y0 = g.y + (em - size) / 2;
      for (int r = 0; r < n; ++r) {
        if (cps[r] != 0) {
          font.drawGray(gray, w, h, x0, y0, widths[r], size, cps[r], size, false);
        }
        x0 += widths[r];
      }
    } else {
      const int emBox = g.size > 0 ? static_cast<int>(g.size) : em;
      const int cellW = vert ? emBox : adv;
      const int cellH = vert ? adv : emBox;
      const uint32_t cp = pickCp(font, g.cp);
      if (cp != 0 && cellW > 0 && cellH > 0) {
        font.drawGray(gray, w, h, g.x, g.y, cellW, cellH, cp, emBox > 0 ? emBox : 1, vert);
      }
    }

    if (g.emphasis != 0 && em > 0) {
      const bool hasRuby = !ts::runTcy(g) && g.rubyCount > 0;
      int gap = em / (hasRuby ? 10 : 4);
      if (gap < 1) {
        gap = 1;
      }
      if (g.emphasis == 2) {
        int thick = em / 14;
        if (thick < 1) {
          thick = 1;
        }
        if (vert) {
          inkBox(gray, w, h, g.x + em + gap, g.y, thick, adv);
        } else {
          inkBox(gray, w, h, g.x, g.y - gap - thick, adv, thick);
        }
      } else {
        int rad = em / 10;
        if (rad < 1) {
          rad = 1;
        }
        const int cx = vert ? g.x + em + gap : g.x + adv / 2;
        const int cy = vert ? g.y + adv / 2 : g.y - gap;
        inkDot(gray, w, h, cx, cy, rad);
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
      const uint32_t cp = pickCp(font, g.ruby[r]);
      if (cp == 0) {
        continue;
      }
      const int at = along.start + r * along.pitch;
      if (vert) {
        font.drawGray(gray, w, h, g.x + body + beside, at, rubyEm, pitch, cp, rubyEm, false);
      } else {
        font.drawGray(gray, w, h, at, g.y - rubyEm - beside, pitch, rubyEm, cp, rubyEm, false);
      }
    }
  }

  extendAhead(globalPage, 2);
  error = "";
  return true;
}
