#ifndef YOMU_KINDLE_FB_H
#define YOMU_KINDLE_FB_H

#include <stdint.h>

/* Kindle framebuffer. The UI buffer is 8-bit gray in screen space.
 * present() copies it into /dev/fb0 and asks the panel to refresh.
 * Learned from KOReader's mxcfb / hwtcon path: one ioctl family per
 * SoC, probed at runtime because the command number includes the struct size.
 */

typedef struct KindleFb KindleFb;

int kindle_fb_open(KindleFb **out);
void kindle_fb_close(KindleFb *fb);

int kindle_fb_width(const KindleFb *fb);
int kindle_fb_height(const KindleFb *fb);
int kindle_fb_native_width(const KindleFb *fb);
int kindle_fb_native_height(const KindleFb *fb);
int kindle_fb_rotation(const KindleFb *fb);

/* x,y,w,h are in the UI buffer. w or h of 0 refreshes the whole screen.
 * full requests a flashing GC16 update; otherwise a partial GC16 update.
 */
void kindle_fb_present(KindleFb *fb, const uint8_t *gray, int x, int y, int w, int h, int full);

#endif
