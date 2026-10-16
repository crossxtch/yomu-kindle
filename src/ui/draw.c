#include "draw.h"

#include "font.h"
#include "font8x8.h"
#include "utf8.h"

#include <stddef.h>

static void put_px(uint8_t *dst, int dst_w, int dst_h, int stride, int x, int y, uint8_t color) {
  if ((unsigned)x >= (unsigned)dst_w || (unsigned)y >= (unsigned)dst_h) {
    return;
  }
  dst[(size_t)y * (size_t)stride + (size_t)x] = color;
}

void draw_clear(uint8_t *dst, int width, int height, int stride, uint8_t color) {
  if (!dst || width <= 0 || height <= 0 || stride < width) {
    return;
  }
  for (int y = 0; y < height; y++) {
    uint8_t *row = dst + (size_t)y * (size_t)stride;
    for (int x = 0; x < width; x++) {
      row[x] = color;
    }
  }
}

void draw_fill(uint8_t *dst, int dst_w, int dst_h, int stride, int x, int y, int w, int h,
               uint8_t color) {
  if (!dst || w <= 0 || h <= 0) {
    return;
  }
  for (int yy = y; yy < y + h; yy++) {
    for (int xx = x; xx < x + w; xx++) {
      put_px(dst, dst_w, dst_h, stride, xx, yy, color);
    }
  }
}

static void blend_px(uint8_t *dst, int dst_w, int dst_h, int stride, int x, int y, uint8_t color,
                     int coverage) {
  uint8_t *p;
  int bg;
  if ((unsigned)x >= (unsigned)dst_w || (unsigned)y >= (unsigned)dst_h || coverage <= 0) {
    return;
  }
  p = dst + (size_t)y * (size_t)stride + (size_t)x;
  if (coverage >= 255) {
    *p = color;
    return;
  }
  bg = *p;
  *p = (uint8_t)((bg * (255 - coverage) + (int)color * coverage) / 255);
}

static void draw_cell(uint8_t *dst, int dst_w, int dst_h, int stride, int cx, int y, int scale,
                      int glyph, uint8_t color) {
  const unsigned char *rows;
  int row;
  if (glyph < 0 || glyph > 127) {
    glyph = '?';
  }
  rows = yomu_font8x8[glyph];
  for (row = 0; row < 8; row++) {
    unsigned bits = rows[row];
    int col;
    for (col = 0; col < 8; col++) {
      int sy;
      if ((bits & (1u << col)) == 0) {
        continue;
      }
      for (sy = 0; sy < scale; sy++) {
        int sx;
        int py = y + row * scale + sy;
        for (sx = 0; sx < scale; sx++) {
          put_px(dst, dst_w, dst_h, stride, cx + col * scale + sx, py, color);
        }
      }
    }
  }
}

void draw_text(uint8_t *dst, int dst_w, int dst_h, int stride, int x, int y, int scale,
               const char *text, uint8_t color) {
  int cx = x;
  int cp;
  if (!dst || !text || scale < 1) {
    return;
  }
  while ((cp = yomu_utf8_next(&text)) >= 0) {
    if (cp >= 128) {
      const uint8_t *bmp = NULL;
      int gw = 0, gh = 0, xoff = 0, yoff = 0, adv = 0;
      int px = 8 * scale;
      if (yomu_font_glyph(cp, px, &bmp, &gw, &gh, &xoff, &yoff, &adv)) {
        int row;
        if (adv < 1) {
          adv = px;
        }
        if (cx + adv > dst_w) {
          break;
        }
        for (row = 0; row < gh; row++) {
          int col;
          for (col = 0; col < gw; col++) {
            blend_px(dst, dst_w, dst_h, stride, cx + xoff + col, y + yoff + row, color,
                     bmp ? bmp[(size_t)row * (size_t)gw + (size_t)col] : 0);
          }
        }
        cx += adv;
        continue;
      }
      cp = '?';
    }
    if (cx + 8 * scale > dst_w) {
      break;
    }
    draw_cell(dst, dst_w, dst_h, stride, cx, y, scale, cp, color);
    cx += 8 * scale;
  }
}

int draw_text_width(int scale, const char *text) {
  int cx = 0;
  int cp;
  if (!text || scale < 1) {
    return 0;
  }
  while ((cp = yomu_utf8_next(&text)) >= 0) {
    if (cp >= 128) {
      const uint8_t *bmp = NULL;
      int gw = 0, gh = 0, xoff = 0, yoff = 0, adv = 0;
      if (yomu_font_glyph(cp, 8 * scale, &bmp, &gw, &gh, &xoff, &yoff, &adv)) {
        if (adv < 1) {
          adv = 8 * scale;
        }
        cx += adv;
        continue;
      }
    }
    cx += 8 * scale;
  }
  return cx;
}
