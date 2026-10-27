#ifndef YOMU_FONT_H
#define YOMU_FONT_H

#include <stdint.h>

/* Maps a Japanese TrueType face that is already on the machine.
 * On a Kindle that is Gothic, then Mincho, then the Japanese font pack.
 * On Android the same search continues through /system/fonts.
 * YOMU_FONT overrides the path. Returns 1 when
 * the face has hiragana and a CJK ideograph.
 */
int yomu_font_open(void);
int yomu_font_ready(void);

/* Hits and misses since the previous take. Both counters are then cleared. */
void yomu_font_take_stats(int *hits, int *misses);

/* Rasterizes cp into an em box px pixels tall. *bmp is cache memory
 * and stays valid until the next call that misses the cache.
 * xoff and yoff are the bitmap's top-left relative to the em box.
 * Returns 1 when the face has the glyph.
 */
int yomu_font_glyph(int cp, int px, const uint8_t **bmp, int *w, int *h, int *xoff, int *yoff,
                      int *advance);

#endif
