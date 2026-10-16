#ifndef YOMU_DRAW_H
#define YOMU_DRAW_H

#include <stdint.h>

/* Surfaces are 8-bit gray, 255 white, 0 black, row-major, stride in bytes. */

void draw_clear(uint8_t *dst, int width, int height, int stride, uint8_t color);

void draw_fill(uint8_t *dst, int dst_w, int dst_h, int stride, int x, int y, int w, int h,
               uint8_t color);

/* Draws until the next glyph would pass dst_w. ASCII uses the 8x8 face.
 * Other characters use the Japanese font from yomu_font_open. A missing
 * glyph is '?'.
 */
void draw_text(uint8_t *dst, int dst_w, int dst_h, int stride, int x, int y, int scale,
               const char *text, uint8_t color);

int draw_text_width(int scale, const char *text);

#endif
