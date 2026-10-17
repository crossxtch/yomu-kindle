#include "FtFont.h"

#include "Kinsoku.h"

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_OUTLINE_H
#include FT_SFNT_NAMES_H
#include FT_TRUETYPE_TABLES_H
#include FT_TRUETYPE_TAGS_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {

int g_em = 50;

constexpr uint32_t kTagVert = 0x76657274u;  // vert
constexpr uint32_t kTagVrt2 = 0x76727432u;  // vrt2
constexpr int kMaxPairs = 4096;
constexpr int kMaxSubs = 8;

struct VertSet {
  struct Sub {
    uint16_t *from;
    uint16_t *to;
    uint16_t n;
  };
  Sub subs[kMaxSubs];
  int nsubs;
};

struct Pair {
  uint16_t from;
  uint16_t to;
};

struct View {
  const uint8_t *p;
  size_t n;

  bool u16(size_t off, uint16_t &o) const {
    if (off + 2 > n) {
      return false;
    }
    o = (uint16_t)((p[off] << 8) | p[off + 1]);
    return true;
  }

  bool i16(size_t off, int16_t &o) const {
    uint16_t u = 0;
    if (!u16(off, u)) {
      return false;
    }
    o = (int16_t)u;
    return true;
  }

  bool u32(size_t off, uint32_t &o) const {
    if (off + 4 > n) {
      return false;
    }
    o = ((uint32_t)p[off] << 24) | ((uint32_t)p[off + 1] << 16) | ((uint32_t)p[off + 2] << 8) | p[off + 3];
    return true;
  }
};

int cmpPair(const void *a, const void *b) {
  const uint16_t fa = static_cast<const Pair *>(a)->from;
  const uint16_t fb = static_cast<const Pair *>(b)->from;
  if (fa < fb) {
    return -1;
  }
  if (fa > fb) {
    return 1;
  }
  return 0;
}

void freeVert(VertSet *v) {
  int i;
  if (!v) {
    return;
  }
  for (i = 0; i < v->nsubs; i++) {
    free(v->subs[i].from);
    free(v->subs[i].to);
  }
  free(v);
}

bool pushPair(Pair *pairs, int &n, uint16_t from, uint16_t to) {
  if (from == 0 || from == to || n >= kMaxPairs) {
    return n < kMaxPairs;
  }
  pairs[n].from = from;
  pairs[n].to = to;
  n++;
  return true;
}

/* Coverage lists glyph ids in coverage-index order. `fn` is (gid, index). */
template <typename Fn>
bool eachCovered(const View &g, size_t off, Fn fn) {
  uint16_t fmt = 0;
  uint16_t count = 0;
  if (!g.u16(off, fmt) || !g.u16(off + 2, count)) {
    return false;
  }
  if (fmt == 1) {
    uint16_t i;
    for (i = 0; i < count; i++) {
      uint16_t gid = 0;
      if (!g.u16(off + 4 + (size_t)i * 2, gid)) {
        return false;
      }
      fn(gid, i);
    }
    return true;
  }
  if (fmt == 2) {
    uint16_t i;
    for (i = 0; i < count; i++) {
      size_t rec = off + 4 + (size_t)i * 6;
      uint16_t start = 0;
      uint16_t end = 0;
      uint16_t index = 0;
      uint32_t gid;
      if (!g.u16(rec, start) || !g.u16(rec + 2, end) || !g.u16(rec + 4, index)) {
        return false;
      }
      if (end < start) {
        return false;
      }
      for (gid = start; gid <= end; gid++) {
        fn((uint16_t)gid, (uint16_t)(index + (gid - start)));
        if (gid == 0xFFFFu) {
          break;
        }
      }
    }
    return true;
  }
  return false;
}

bool addSub(VertSet *set, Pair *pairs, int n) {
  VertSet::Sub *sub;
  int w;
  int i;
  if (n <= 0 || set->nsubs >= kMaxSubs) {
    return false;
  }
  qsort(pairs, (size_t)n, sizeof(Pair), cmpPair);
  w = 0;
  for (i = 0; i < n; i++) {
    if (w > 0 && pairs[i].from == pairs[w - 1].from) {
      continue;
    }
    pairs[w++] = pairs[i];
  }
  sub = &set->subs[set->nsubs];
  sub->from = static_cast<uint16_t *>(malloc((size_t)w * sizeof(uint16_t)));
  sub->to = static_cast<uint16_t *>(malloc((size_t)w * sizeof(uint16_t)));
  if (!sub->from || !sub->to) {
    free(sub->from);
    free(sub->to);
    sub->from = nullptr;
    sub->to = nullptr;
    return false;
  }
  for (i = 0; i < w; i++) {
    sub->from[i] = pairs[i].from;
    sub->to[i] = pairs[i].to;
  }
  sub->n = (uint16_t)w;
  set->nsubs++;
  return true;
}

bool parseSingle(const View &g, size_t off, VertSet *set, Pair *pairs) {
  uint16_t fmt = 0;
  uint16_t cov = 0;
  int n = 0;
  if (!g.u16(off, fmt) || !g.u16(off + 2, cov)) {
    return false;
  }
  if (fmt == 1) {
    int16_t delta = 0;
    if (!g.i16(off + 4, delta)) {
      return false;
    }
    if (!eachCovered(g, off + cov, [&](uint16_t gid, uint16_t) {
          int to = (int)gid + (int)delta;
          if (to > 0 && to <= 0xFFFF) {
            pushPair(pairs, n, gid, (uint16_t)to);
          }
        })) {
      return false;
    }
  } else if (fmt == 2) {
    uint16_t count = 0;
    if (!g.u16(off + 4, count)) {
      return false;
    }
    if (!eachCovered(g, off + cov, [&](uint16_t gid, uint16_t index) {
          uint16_t to = 0;
          if (index < count && g.u16(off + 6 + (size_t)index * 2, to)) {
            pushPair(pairs, n, gid, to);
          }
        })) {
      return false;
    }
  } else {
    return false;
  }
  if (n > 0) {
    addSub(set, pairs, n);
  }
  return true;
}

void parseLookup(const View &g, size_t lookOff, VertSet *set, Pair *pairs) {
  uint16_t type = 0;
  uint16_t nsub = 0;
  uint16_t i;
  if (!g.u16(lookOff, type) || !g.u16(lookOff + 4, nsub)) {
    return;
  }
  for (i = 0; i < nsub && set->nsubs < kMaxSubs; i++) {
    uint16_t rel = 0;
    size_t sub;
    if (!g.u16(lookOff + 6 + (size_t)i * 2, rel)) {
      return;
    }
    sub = lookOff + rel;
    if (type == 1) {
      parseSingle(g, sub, set, pairs);
    } else if (type == 7) {
      uint16_t extFmt = 0;
      uint16_t extType = 0;
      uint32_t extOff = 0;
      if (!g.u16(sub, extFmt) || !g.u16(sub + 2, extType) || !g.u32(sub + 4, extOff)) {
        continue;
      }
      if (extFmt == 1 && extType == 1) {
        parseSingle(g, sub + extOff, set, pairs);
      }
    }
  }
}

VertSet *buildVert(FT_Face face) {
  FT_ULong len = 0;
  uint8_t *buf = nullptr;
  VertSet *set = nullptr;
  Pair *pairs = nullptr;
  View g{};
  uint16_t scriptOff = 0;
  uint16_t featOff = 0;
  uint16_t lookOff = 0;
  uint16_t featCount = 0;
  uint16_t lookCount = 0;
  int featIndex = -1;
  uint16_t fi;
  uint32_t fallback = 0;
  if (!face) {
    return nullptr;
  }
  if (FT_Load_Sfnt_Table(face, TTAG_GSUB, 0, nullptr, &len) != 0 || len < 10 || len > (8u << 20)) {
    return nullptr;
  }
  buf = static_cast<uint8_t *>(malloc(len));
  pairs = static_cast<Pair *>(malloc(sizeof(Pair) * kMaxPairs));
  set = static_cast<VertSet *>(calloc(1, sizeof(VertSet)));
  if (!buf || !pairs || !set || FT_Load_Sfnt_Table(face, TTAG_GSUB, 0, buf, &len) != 0) {
    free(buf);
    free(pairs);
    freeVert(set);
    return nullptr;
  }
  g.p = buf;
  g.n = len;
  if (!g.u16(4, scriptOff) || !g.u16(6, featOff) || !g.u16(8, lookOff) || !g.u16(featOff, featCount) ||
      !g.u16(lookOff, lookCount)) {
    free(buf);
    free(pairs);
    freeVert(set);
    return nullptr;
  }
  (void)scriptOff;
  /* vert when the face has it (Core Text vertical forms). vrt2 only if vert is absent. */
  for (fi = 0; fi < featCount; fi++) {
    size_t rec = (size_t)featOff + 2 + (size_t)fi * 6;
    uint32_t tag = 0;
    if (!g.u32(rec, tag)) {
      break;
    }
    if (tag == kTagVert) {
      featIndex = (int)fi;
      fallback = tag;
      break;
    }
    if (tag == kTagVrt2 && featIndex < 0) {
      featIndex = (int)fi;
      fallback = tag;
    }
  }
  if (featIndex >= 0 && fallback != 0) {
    size_t rec = (size_t)featOff + 2 + (size_t)featIndex * 6;
    uint16_t frel = 0;
    uint16_t nlook = 0;
    size_t feature;
    uint16_t li;
    if (g.u16(rec + 4, frel) && g.u16((size_t)featOff + frel + 2, nlook)) {
      feature = (size_t)featOff + frel;
      for (li = 0; li < nlook && set->nsubs < kMaxSubs; li++) {
        uint16_t which = 0;
        uint16_t lrel = 0;
        if (!g.u16(feature + 4 + (size_t)li * 2, which)) {
          break;
        }
        if (which >= lookCount || !g.u16((size_t)lookOff + 2 + (size_t)which * 2, lrel)) {
          continue;
        }
        parseLookup(g, (size_t)lookOff + lrel, set, pairs);
      }
    }
  }
  free(buf);
  free(pairs);
  if (set->nsubs == 0) {
    freeVert(set);
    return nullptr;
  }
  return set;
}

void blit(uint8_t *page, int pageW, int pageH, int x0, int y0, const uint8_t *bits, int bw, int bh) {
  int y;
  int x;
  if (!page || !bits || bw < 1 || bh < 1) {
    return;
  }
  for (y = 0; y < bh; y++) {
    int dy = y0 + y;
    if (dy < 0 || dy >= pageH) {
      continue;
    }
    for (x = 0; x < bw; x++) {
      int dx = x0 + x;
      uint8_t cov;
      uint8_t ink;
      uint8_t *dst;
      if (dx < 0 || dx >= pageW) {
        continue;
      }
      cov = bits[y * bw + x];
      if (cov == 0) {
        continue;
      }
      ink = (uint8_t)(255 - cov);
      dst = page + dy * pageW + dx;
      if (ink < *dst) {
        *dst = ink;
      }
    }
  }
}

const uint8_t *bitmapRow(const FT_Bitmap *bm, int y) {
  if (bm->pitch >= 0) {
    return bm->buffer + y * bm->pitch;
  }
  return bm->buffer + (bm->rows - 1 - y) * (-bm->pitch);
}

}  // namespace

extern "C" void ts_font_set_em(int em) {
  if (em < 8) {
    em = 8;
  }
  if (em > 96) {
    em = 96;
  }
  g_em = em;
}

static bool face_has_kana_kanji(FT_Face f) {
  return f && FT_Get_Char_Index(f, 0x3042) != 0 && FT_Get_Char_Index(f, 0x4E00) != 0;
}

static int utf16be_to_utf8(const unsigned char *src, unsigned len, char *dst, int dst_n) {
  unsigned i;
  int o = 0;
  if (dst_n < 1) {
    return 0;
  }
  for (i = 0; i + 1 < len; i += 2) {
    unsigned u = ((unsigned)src[i] << 8) | src[i + 1];
    unsigned char tmp[4];
    int n = 0;
    int k;
    if (u == 0) {
      break;
    }
    if (u >= 0xD800u && u <= 0xDBFFu && i + 3 < len) {
      unsigned lo = ((unsigned)src[i + 2] << 8) | src[i + 3];
      if (lo >= 0xDC00u && lo <= 0xDFFFu) {
        u = 0x10000u + (((u - 0xD800u) << 10) | (lo - 0xDC00u));
        i += 2;
      }
    }
    if (u < 0x80u) {
      tmp[0] = (unsigned char)u;
      n = 1;
    } else if (u < 0x800u) {
      tmp[0] = (unsigned char)(0xC0u | (u >> 6));
      tmp[1] = (unsigned char)(0x80u | (u & 0x3Fu));
      n = 2;
    } else if (u < 0x10000u) {
      tmp[0] = (unsigned char)(0xE0u | (u >> 12));
      tmp[1] = (unsigned char)(0x80u | ((u >> 6) & 0x3Fu));
      tmp[2] = (unsigned char)(0x80u | (u & 0x3Fu));
      n = 3;
    } else {
      tmp[0] = (unsigned char)(0xF0u | (u >> 18));
      tmp[1] = (unsigned char)(0x80u | ((u >> 12) & 0x3Fu));
      tmp[2] = (unsigned char)(0x80u | ((u >> 6) & 0x3Fu));
      tmp[3] = (unsigned char)(0x80u | (u & 0x3Fu));
      n = 4;
    }
    if (o + n >= dst_n) {
      break;
    }
    for (k = 0; k < n; k++) {
      dst[o++] = (char)tmp[k];
    }
  }
  dst[o] = '\0';
  return o;
}

static int copy_sfnt(FT_Face face, unsigned name_id, unsigned lang, char *dst, int dst_n) {
  FT_UInt count;
  FT_UInt i;
  dst[0] = '\0';
  if (!face || dst_n < 1) {
    return 0;
  }
  count = FT_Get_Sfnt_Name_Count(face);
  for (i = 0; i < count; i++) {
    FT_SfntName rec;
    if (FT_Get_Sfnt_Name(face, i, &rec) != 0) {
      continue;
    }
    if (rec.platform_id != 3 || rec.encoding_id != 1 || rec.name_id != name_id || rec.language_id != lang ||
        !rec.string || rec.string_len < 2) {
      continue;
    }
    return utf16be_to_utf8(rec.string, rec.string_len, dst, dst_n);
  }
  return 0;
}

static int plain_style(const char *style) {
  char buf[16];
  size_t i;
  if (!style || !style[0]) {
    return 1;
  }
  for (i = 0; style[i] && i + 1 < sizeof buf; i++) {
    char c = style[i];
    if (c >= 'A' && c <= 'Z') {
      c = (char)(c - 'A' + 'a');
    }
    buf[i] = c;
  }
  buf[i] = '\0';
  return strcmp(buf, "regular") == 0 || strcmp(buf, "normal") == 0 || strcmp(buf, "roman") == 0;
}

/* Japanese typographic names, then English, then the face's family and style. */
static void face_label(FT_Face face, char *dst, int dst_n) {
  char family[48];
  char style[24];
  const unsigned langs[] = {0x0411u, 0x0409u};
  int li;
  family[0] = '\0';
  style[0] = '\0';
  dst[0] = '\0';
  if (!face || dst_n < 1) {
    return;
  }
  for (li = 0; li < 2; li++) {
    if (!family[0]) {
      copy_sfnt(face, 16, langs[li], family, (int)sizeof family);
    }
    if (!style[0]) {
      copy_sfnt(face, 17, langs[li], style, (int)sizeof style);
    }
  }
  if (!family[0]) {
    snprintf(family, sizeof family, "%s", face->family_name ? face->family_name : "");
    snprintf(style, sizeof style, "%s", face->style_name ? face->style_name : "");
  }
  if (plain_style(style)) {
    style[0] = '\0';
  }
  if (family[0] && style[0]) {
    snprintf(dst, (size_t)dst_n, "%s %s", family, style);
  } else if (family[0]) {
    snprintf(dst, (size_t)dst_n, "%s", family);
  } else {
    snprintf(dst, (size_t)dst_n, "%s", style);
  }
}

extern "C" int ts_font_faces(const char *path, int *indexes, char labels[][48], int cap) {
  FT_Library lib = nullptr;
  int n = 0;
  int index;
  if (!path || !path[0] || !indexes || !labels || cap < 1) {
    return 0;
  }
  if (FT_Init_FreeType(&lib) != 0) {
    return 0;
  }
  for (index = 0; index < 16 && n < cap; index++) {
    FT_Face f = nullptr;
    if (FT_New_Face(lib, path, index, &f) != 0) {
      break;
    }
    if (face_has_kana_kanji(f)) {
      indexes[n] = index;
      face_label(f, labels[n], 48);
      n++;
    }
    FT_Done_Face(f);
  }
  FT_Done_FreeType(lib);
  return n;
}

bool FtFont::load(const char *path, int faceIndex) {
  FT_Library lib = nullptr;
  int index;
  close();
  error = "font missing";
  if (!path || !path[0]) {
    return false;
  }
  if (FT_Init_FreeType(&lib) != 0) {
    error = "freetype";
    return false;
  }
  library = lib;
  if (faceIndex >= 0) {
    FT_Face f = nullptr;
    if (FT_New_Face(lib, path, faceIndex, &f) != 0) {
      error = "font missing";
      close();
      return false;
    }
    if (!face_has_kana_kanji(f)) {
      FT_Done_Face(f);
      error = "no japanese glyphs";
      close();
      return false;
    }
    face = f;
    em = (uint8_t)g_em;
    ruby = (uint8_t)(g_em / 2);
    if (ruby < 8) {
      ruby = 8;
    }
    FT_Set_Pixel_Sizes(f, 0, em);
    loadVert();
    error = "";
    return true;
  }
  for (index = 0; index < 16; index++) {
    FT_Face f = nullptr;
    FT_Error err = FT_New_Face(lib, path, index, &f);
    if (err != 0) {
      break;
    }
    if (face_has_kana_kanji(f)) {
      face = f;
      em = (uint8_t)g_em;
      ruby = (uint8_t)(g_em / 2);
      if (ruby < 8) {
        ruby = 8;
      }
      FT_Set_Pixel_Sizes(f, 0, em);
      loadVert();
      error = "";
      return true;
    }
    FT_Done_Face(f);
  }
  error = "no japanese glyphs";
  close();
  return false;
}

void FtFont::clearVert() {
  freeVert(static_cast<VertSet *>(vert));
  vert = nullptr;
}

void FtFont::loadVert() { vert = buildVert(static_cast<FT_Face>(face)); }

uint32_t FtFont::vertOf(uint32_t gid) const {
  const VertSet *v = static_cast<const VertSet *>(vert);
  uint16_t g;
  int s;
  if (!v || gid == 0 || gid > 0xFFFFu) {
    return gid;
  }
  g = (uint16_t)gid;
  for (s = 0; s < v->nsubs; s++) {
    const VertSet::Sub &sub = v->subs[s];
    int lo = 0;
    int hi = sub.n;
    while (lo < hi) {
      const int mid = lo + (hi - lo) / 2;
      if (sub.from[mid] < g) {
        lo = mid + 1;
      } else {
        hi = mid;
      }
    }
    if (lo < sub.n && sub.from[lo] == g) {
      g = sub.to[lo];
    }
  }
  return g;
}

void FtFont::close() {
  int i;
  for (i = 0; i < 256; i++) {
    free(slots[i].bits);
    slots[i].bits = nullptr;
    slots[i].taken = 0;
  }
  clearVert();
  if (face) {
    FT_Done_Face(static_cast<FT_Face>(face));
    face = nullptr;
  }
  if (library) {
    FT_Done_FreeType(static_cast<FT_Library>(library));
    library = nullptr;
  }
}

uint16_t FtFont::glyphId(uint32_t cp) const {
  FT_Face f = static_cast<FT_Face>(face);
  if (!f || FT_Get_Char_Index(f, cp) == 0) {
    return 0xFFFF;
  }
  return 1;
}

int16_t FtFont::advancePx(uint32_t cp, int px) {
  FT_Face f = static_cast<FT_Face>(face);
  int w;
  if (!f || px < 1) {
    return 0;
  }
  if (px > 128) {
    px = 128;
  }
  if (FT_Set_Pixel_Sizes(f, 0, (FT_UInt)px) != 0) {
    return 0;
  }
  if (FT_Load_Char(f, cp, FT_LOAD_ADVANCE_ONLY) != 0) {
    return 0;
  }
  w = (int)(f->glyph->advance.x >> 6);
  if (w < 1) {
    return 0;
  }
  if (w > 32767) {
    w = 32767;
  }
  return (int16_t)w;
}

const FtFont::Slot *FtFont::slotFor(uint32_t gid, int px, bool rotate, bool bold) {
  FT_Face f = static_cast<FT_Face>(face);
  int i;
  Slot *slot = nullptr;
  FT_GlyphSlot gs;
  FT_Bitmap *bm;
  int bw;
  int bh;
  int y;
  int x;
  uint8_t *tight = nullptr;
  const uint8_t key = (uint8_t)((bold ? 2 : 0) | (rotate ? 1 : 0));
  if (!f || gid == 0 || px < 1 || px > 128) {
    return nullptr;
  }
  for (i = 0; i < 256; i++) {
    if (slots[i].taken && slots[i].gid == gid && slots[i].em == (uint16_t)px && slots[i].rot == key) {
      return &slots[i];
    }
  }
  if (FT_Set_Pixel_Sizes(f, 0, (FT_UInt)px) != 0) {
    return nullptr;
  }
  if (bold) {
    if (FT_Load_Glyph(f, gid, FT_LOAD_NO_BITMAP | FT_LOAD_TARGET_NORMAL) != 0) {
      return nullptr;
    }
    gs = f->glyph;
    if (gs->format == FT_GLYPH_FORMAT_OUTLINE) {
      (void)FT_Outline_Embolden(&gs->outline, (FT_Pos)px * 4);
    }
    if (FT_Render_Glyph(gs, FT_RENDER_MODE_NORMAL) != 0) {
      return nullptr;
    }
  } else if (FT_Load_Glyph(f, gid, FT_LOAD_RENDER | FT_LOAD_TARGET_NORMAL) != 0) {
    return nullptr;
  }
  gs = f->glyph;
  bm = &gs->bitmap;
  bw = 0;
  bh = 0;
  if (bm->pixel_mode == FT_PIXEL_MODE_GRAY && bm->buffer != nullptr && bm->width > 0 && bm->rows > 0 &&
      bm->width <= 256 && bm->rows <= 256) {
    bw = (int)bm->width;
    bh = (int)bm->rows;
    tight = static_cast<uint8_t *>(calloc((size_t)bw * (size_t)bh, 1));
    if (!tight) {
      return nullptr;
    }
    for (y = 0; y < bh; y++) {
      memcpy(tight + y * bw, bitmapRow(bm, y), (size_t)bw);
    }
  }
  slot = &slots[clock % 256];
  clock++;
  free(slot->bits);
  slot->bits = nullptr;
  slot->taken = 1;
  slot->gid = gid;
  slot->em = (uint16_t)px;
  slot->rot = key;
  slot->left = (int16_t)gs->bitmap_left;
  slot->top = (int16_t)gs->bitmap_top;
  {
    int adv = (int)(gs->advance.x >> 6);
    int ascent = (int)(f->size->metrics.ascender >> 6);
    int descent = (int)(-(f->size->metrics.descender >> 6));
    if (adv < 1) {
      adv = px;
    }
    if (ascent < 0) {
      ascent = 0;
    }
    if (descent < 0) {
      descent = 0;
    }
    slot->advance = (int16_t)(adv > 32767 ? 32767 : adv);
    slot->ascent = (int16_t)(ascent > 32767 ? 32767 : ascent);
    slot->descent = (int16_t)(descent > 32767 ? 32767 : descent);
  }
  if (rotate && tight && bw > 0 && bh > 0) {
    int rw = bh;
    int rh = bw;
    uint8_t *turned = static_cast<uint8_t *>(calloc((size_t)rw * (size_t)rh, 1));
    if (!turned) {
      free(tight);
      slot->taken = 0;
      return nullptr;
    }
    for (y = 0; y < bh; y++) {
      for (x = 0; x < bw; x++) {
        /* 90 degrees clockwise, so a horizontal dash stands in the column. */
        int nx = bh - 1 - y;
        int ny = x;
        turned[ny * rw + nx] = tight[y * bw + x];
      }
    }
    free(tight);
    slot->bits = turned;
    slot->bw = (int16_t)rw;
    slot->bh = (int16_t)rh;
    slot->left = 0;
    slot->top = 0;
    return slot;
  }
  slot->bits = tight;
  slot->bw = (int16_t)bw;
  slot->bh = (int16_t)bh;
  return slot;
}

bool FtFont::drawGray(uint8_t *page, int pageW, int pageH, int x, int y, int cellW, int cellH, uint32_t cp, int px,
                       bool vertical, bool bold) {
  FT_Face f = static_cast<FT_Face>(face);
  FT_UInt gid;
  bool slide = false;
  bool rotate = false;
  bool center = false;
  const Slot *slot;
  if (!f || !page || px < 1 || cellW < 1 || cellH < 1) {
    return false;
  }
  gid = FT_Get_Char_Index(f, cp);
  if (gid == 0) {
    return false;
  }
  if (vertical) {
    FT_UInt v = (FT_UInt)vertOf(gid);
    if (v != 0 && v != gid) {
      gid = v;
      slide = true;
    } else if (ts::shouldRotate(cp)) {
      rotate = true;
      center = true;
    } else if (ts::kinsokuYakumono(cp)) {
      center = true;
    }
  } else if (ts::kinsokuYakumono(cp)) {
    center = true;
  }
  slot = slotFor(gid, px, rotate, bold);
  if (!slot || slot->bw < 1 || slot->bh < 1 || !slot->bits) {
    return slot != nullptr;
  }
  if (center) {
    blit(page, pageW, pageH, x + (cellW - slot->bw) / 2, y + (cellH - slot->bh) / 2, slot->bits, slot->bw, slot->bh);
    return true;
  }
  /* Sit on the em: baseline plus sidebearings. A vertical-form glyph uses the
   * full em even when the cell is half wide; opening-bracket ink, which lives
   * in the lower half, slides up into that shorter cell. */
  {
    const int boxW = slide ? px : cellW;
    const int boxH = slide ? px : cellH;
    const int ascent = slot->ascent;
    const int descent = slot->descent;
    int adv = slot->advance > 0 ? slot->advance : px;
    int pad;
    int baseline;
    int penX;
    if (ascent + descent <= 0) {
      blit(page, pageW, pageH, x + (cellW - slot->bw) / 2, y + (cellH - slot->bh) / 2, slot->bits, slot->bw,
           slot->bh);
      return true;
    }
    pad = (boxH - (ascent + descent)) / 2;
    baseline = boxH - descent - pad;
    if (slide && cellH < px) {
      const int mid = slot->top - slot->bh / 2;
      const int centerY = (ascent - descent) / 2;
      if (mid < centerY) {
        baseline -= px - cellH;
      }
    }
    penX = (boxW - adv) / 2;
    blit(page, pageW, pageH, x + penX + slot->left, y + baseline - slot->top, slot->bits, slot->bw, slot->bh);
  }
  return true;
}
