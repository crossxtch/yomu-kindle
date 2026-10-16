#include "kindle_fb.h"

#include "log.h"
#include "rotate.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <linux/fb.h>

/* ioctl numbers from KOReader's mxcfb_kindle_h (FBInk). The size is part of
 * the command, so a mismatch returns EINVAL and does not refresh.
 */
#define KO_SEND_CLASSIC 1078478382u /* 72-byte mxcfb_update_data, PW2 era */
#define KO_SEND_REX 1079002670u     /* 80-byte, some Carta panels */
#define KO_SEND_ZELDA 1079526958u   /* 88-byte, Oasis 2 / PW4 / Oasis 3 */
#define KO_SEND_MTK 1080051246u     /* 96-byte, hwtcon on PW5 and newer */
#define KO_WAIT_SUBMISSION 1074021943u
#define KO_WAIT_COMPLETE 3221767727u
#define KO_WAIT_COMPLETE_PEARL 1074021935u

#define KO_WAVE_GC16 2u
#define KO_UPDATE_PARTIAL 0u
#define KO_UPDATE_FULL 1u
#define KO_TEMP_AUTO 0x1000

enum {
  DRV_NONE = 0,
  DRV_CLASSIC,
  DRV_REX,
  DRV_ZELDA,
  DRV_MTK
};

struct mxcfb_rect {
  uint32_t top;
  uint32_t left;
  uint32_t width;
  uint32_t height;
};

struct mxcfb_alt_buffer_data {
  uint32_t phys_addr;
  uint32_t width;
  uint32_t height;
  struct mxcfb_rect alt_update_region;
};

struct mxcfb_update_data {
  struct mxcfb_rect update_region;
  uint32_t waveform_mode;
  uint32_t update_mode;
  uint32_t update_marker;
  uint32_t hist_bw_waveform_mode;
  uint32_t hist_gray_waveform_mode;
  int32_t temp;
  uint32_t flags;
  struct mxcfb_alt_buffer_data alt_buffer_data;
};

struct mxcfb_update_data_rex {
  struct mxcfb_rect update_region;
  uint32_t waveform_mode;
  uint32_t update_mode;
  uint32_t update_marker;
  int32_t temp;
  uint32_t flags;
  int32_t dither_mode;
  int32_t quant_bit;
  struct mxcfb_alt_buffer_data alt_buffer_data;
  uint32_t hist_bw_waveform_mode;
  uint32_t hist_gray_waveform_mode;
};

struct mxcfb_update_data_zelda {
  struct mxcfb_rect update_region;
  uint32_t waveform_mode;
  uint32_t update_mode;
  uint32_t update_marker;
  int32_t temp;
  uint32_t flags;
  int32_t dither_mode;
  int32_t quant_bit;
  struct mxcfb_alt_buffer_data alt_buffer_data;
  uint32_t hist_bw_waveform_mode;
  uint32_t hist_gray_waveform_mode;
  uint32_t ts_pxp;
  uint32_t ts_epdc;
};

struct mxcfb_swipe_data {
  uint32_t direction;
  uint32_t steps;
};

struct mxcfb_update_data_mtk {
  struct mxcfb_rect update_region;
  uint32_t waveform_mode;
  uint32_t update_mode;
  uint32_t update_marker;
  int32_t temp;
  uint32_t flags;
  int32_t dither_mode;
  int32_t quant_bit;
  struct mxcfb_alt_buffer_data alt_buffer_data;
  struct mxcfb_swipe_data swipe_data;
  uint32_t hist_bw_waveform_mode;
  uint32_t hist_gray_waveform_mode;
  uint32_t ts_pxp;
  uint32_t ts_epdc;
};

_Static_assert(sizeof(struct mxcfb_update_data) == 72, "classic kindle update");
_Static_assert(sizeof(struct mxcfb_update_data_rex) == 80, "rex kindle update");
_Static_assert(sizeof(struct mxcfb_update_data_zelda) == 88, "zelda kindle update");
_Static_assert(sizeof(struct mxcfb_update_data_mtk) == 96, "mtk kindle update");

struct KindleFb {
  int fd;
  uint8_t *mem;
  size_t smem_len;
  int line_length;
  int bpp;
  int xoffset;
  int yoffset;
  int invert;
  int red_off, red_len, green_off, green_len, blue_off, blue_len, alpha_off, alpha_len;
  int view_x, view_y, view_w, view_h;
  int nat_w, nat_h;
  int rot;
  int driver;
  int prefer_mtk;
  uint32_t marker;
  uint32_t last_marker;
  char id[20];
};

static int env_force(const char *name, int *out) {
  const char *v = getenv(name);
  if (!v || !v[0]) {
    return 0;
  }
  *out = v[0] != '0';
  return 1;
}

/* The page is 90° clockwise. YOMU_ROTATE is not a switch. */
static int env_rotate(void) {
  const char *v = getenv("YOMU_ROTATE");
  if (v && v[0] && !(v[0] == '3' && v[1] == '\0')) {
    yomu_log("ignored YOMU_ROTATE=%s", v);
  }
  return 3;
}

static void fill_rect(struct mxcfb_rect *rect, int x, int y, int w, int h) {
  rect->top = (uint32_t)y;
  rect->left = (uint32_t)x;
  rect->width = (uint32_t)w;
  rect->height = (uint32_t)h;
}

static int ioctl_retry(int fd, unsigned long request, void *data) {
  for (int i = 0; i < 3; i++) {
    if (ioctl(fd, request, data) == 0) {
      return 0;
    }
    if (errno != EINTR) {
      return -1;
    }
  }
  return -1;
}

static void wait_submission(int fd, uint32_t marker) {
  if (marker == 0) {
    return;
  }
  ioctl_retry(fd, KO_WAIT_SUBMISSION, &marker);
}

static void wait_complete(int fd, uint32_t marker) {
  struct {
    uint32_t update_marker;
    uint32_t collision_test;
  } data;
  data.update_marker = marker;
  data.collision_test = 0;
  if (ioctl_retry(fd, KO_WAIT_COMPLETE, &data) != 0) {
    ioctl_retry(fd, KO_WAIT_COMPLETE_PEARL, &marker);
  }
}

static const char *driver_name(int driver) {
  switch (driver) {
    case DRV_CLASSIC:
      return "classic";
    case DRV_REX:
      return "rex";
    case DRV_ZELDA:
      return "zelda";
    case DRV_MTK:
      return "mtk";
    default:
      return "none";
  }
}

static int send_one(KindleFb *fb, int driver, int x, int y, int w, int h, int full, uint32_t marker) {
  uint32_t mode = full ? KO_UPDATE_FULL : KO_UPDATE_PARTIAL;
  switch (driver) {
    case DRV_CLASSIC: {
      struct mxcfb_update_data data;
      memset(&data, 0, sizeof data);
      fill_rect(&data.update_region, x, y, w, h);
      data.waveform_mode = KO_WAVE_GC16;
      data.update_mode = mode;
      data.update_marker = marker;
      data.temp = KO_TEMP_AUTO;
      return ioctl_retry(fb->fd, KO_SEND_CLASSIC, &data);
    }
    case DRV_REX: {
      struct mxcfb_update_data_rex data;
      memset(&data, 0, sizeof data);
      fill_rect(&data.update_region, x, y, w, h);
      data.waveform_mode = KO_WAVE_GC16;
      data.update_mode = mode;
      data.update_marker = marker;
      data.temp = KO_TEMP_AUTO;
      return ioctl_retry(fb->fd, KO_SEND_REX, &data);
    }
    case DRV_ZELDA: {
      struct mxcfb_update_data_zelda data;
      memset(&data, 0, sizeof data);
      fill_rect(&data.update_region, x, y, w, h);
      data.waveform_mode = KO_WAVE_GC16;
      data.update_mode = mode;
      data.update_marker = marker;
      data.temp = KO_TEMP_AUTO;
      return ioctl_retry(fb->fd, KO_SEND_ZELDA, &data);
    }
    case DRV_MTK: {
      struct mxcfb_update_data_mtk data;
      memset(&data, 0, sizeof data);
      fill_rect(&data.update_region, x, y, w, h);
      data.waveform_mode = KO_WAVE_GC16;
      data.update_mode = mode;
      data.update_marker = marker;
      data.temp = KO_TEMP_AUTO;
      return ioctl_retry(fb->fd, KO_SEND_MTK, &data);
    }
    default:
      return -1;
  }
}

/* Grow to a multiple of 8 when that still fits. A 1236-wide panel cannot
 * grow, and the EPDC driver aligns the rectangle itself. Returning 0 means
 * the caller's rectangle should be sent unchanged. */
static int align8(const KindleFb *fb, int *x, int *y, int *w, int *h) {
  int ax = *x & ~7;
  int ay = *y & ~7;
  int ax2 = (*x + *w + 7) & ~7;
  int ay2 = (*y + *h + 7) & ~7;
  if (ax < 0 || ay < 0 || ax2 > fb->nat_w || ay2 > fb->nat_h || ax2 <= ax || ay2 <= ay) {
    return 0;
  }
  *x = ax;
  *y = ay;
  *w = ax2 - ax;
  *h = ay2 - ay;
  return 1;
}

/* Crop to the largest 8-aligned rectangle inside the request. */
static int crop8(int *x, int *y, int *w, int *h) {
  int ax = *x & ~7;
  int ay = *y & ~7;
  int ax2 = (*x + *w) & ~7;
  int ay2 = (*y + *h) & ~7;
  if (ax2 <= ax || ay2 <= ay) {
    return 0;
  }
  *x = ax;
  *y = ay;
  *w = ax2 - ax;
  *h = ay2 - ay;
  return 1;
}

typedef struct PanelTime {
  int64_t wait_ms;
  int64_t send_ms;
  int64_t done_ms;
  int x, y, w, h;
  int cropped;
} PanelTime;

static int dispatch(KindleFb *fb, int x, int y, int w, int h, int full, PanelTime *pt) {
  int px = fb->view_x + x;
  int py = fb->view_y + y;
  int64_t t0 = yomu_ms();
  wait_submission(fb->fd, fb->last_marker);
  int64_t t1 = yomu_ms();
  uint32_t marker = fb->marker++;
  if (fb->marker == 0) {
    fb->marker = 1;
  }

  int drivers[4];
  int nd = 0;
  if (fb->driver != DRV_NONE) {
    drivers[nd++] = fb->driver;
  } else if (fb->prefer_mtk) {
    drivers[nd++] = DRV_MTK;
    drivers[nd++] = DRV_ZELDA;
    drivers[nd++] = DRV_REX;
    drivers[nd++] = DRV_CLASSIC;
  } else {
    drivers[nd++] = DRV_ZELDA;
    drivers[nd++] = DRV_REX;
    drivers[nd++] = DRV_CLASSIC;
    drivers[nd++] = DRV_MTK;
  }

  int worked = -1;
  for (int i = 0; i < nd; i++) {
    if (send_one(fb, drivers[i], px, py, w, h, full, marker) == 0) {
      worked = drivers[i];
      break;
    }
  }
  int64_t t2 = yomu_ms();
  if (worked < 0) {
    if (pt) {
      pt->wait_ms += t1 - t0;
      pt->send_ms += t2 - t1;
    }
    return -1;
  }
  if (fb->driver == DRV_NONE) {
    fb->driver = worked;
    yomu_log("eink driver %s", driver_name(worked));
  }
  fb->last_marker = marker;
  if (full) {
    wait_complete(fb->fd, marker);
  }
  int64_t t3 = yomu_ms();
  if (pt) {
    pt->wait_ms += t1 - t0;
    pt->send_ms += t2 - t1;
    pt->done_ms += t3 - t2;
    pt->x = x;
    pt->y = y;
    pt->w = w;
    pt->h = h;
  }
  return 0;
}

static int refresh(KindleFb *fb, int x, int y, int w, int h, int full, PanelTime *pt) {
  if (w <= 0 || h <= 0) {
    return -1;
  }
  if (x < 0) {
    w += x;
    x = 0;
  }
  if (y < 0) {
    h += y;
    y = 0;
  }
  if (x >= fb->nat_w || y >= fb->nat_h) {
    return 0;
  }
  if (x + w > fb->nat_w) {
    w = fb->nat_w - x;
  }
  if (y + h > fb->nat_h) {
    h = fb->nat_h - y;
  }
  if (w <= 1 || h <= 1) {
    return 0;
  }
  if (!full) {
    align8(fb, &x, &y, &w, &h);
  }
  /* ioctl coordinates match the pixels inside the visible frame. */
  if (dispatch(fb, x, y, w, h, full, pt) == 0) {
    return 0;
  }
  /* A strict driver can reject a width that is not a multiple of 8, such as
   * the Paperwhite 11's 1236. Crop and retry as a partial. Do not promote
   * the page to a full-screen flash. */
  if (!full && crop8(&x, &y, &w, &h) && dispatch(fb, x, y, w, h, 0, pt) == 0) {
    if (pt) {
      pt->cropped = 1;
    }
    return 0;
  }
  yomu_log("refresh failed (%s)", strerror(errno));
  return -1;
}

static void write_pixel(KindleFb *fb, int x, int y, uint8_t gray) {
  if (fb->invert) {
    gray = (uint8_t)(255 - gray);
  }
  size_t bytes = (size_t)(fb->bpp / 8);
  size_t off = (size_t)(fb->yoffset + y) * (size_t)fb->line_length + (size_t)(fb->xoffset + x) * bytes;
  if (bytes == 0 || off + bytes > fb->smem_len) {
    return;
  }
  uint8_t *p = fb->mem + off;
  if (fb->bpp == 8) {
    *p = gray;
    return;
  }
  if (fb->bpp == 16) {
    uint16_t pix = 0;
    if (fb->red_len) {
      pix |= (uint16_t)((gray >> (8 - fb->red_len)) << fb->red_off);
    }
    if (fb->green_len) {
      pix |= (uint16_t)((gray >> (8 - fb->green_len)) << fb->green_off);
    }
    if (fb->blue_len) {
      pix |= (uint16_t)((gray >> (8 - fb->blue_len)) << fb->blue_off);
    }
    memcpy(p, &pix, 2);
    return;
  }
  uint32_t pix = 0;
  if (fb->red_len) {
    pix |= (uint32_t)gray << fb->red_off;
  }
  if (fb->green_len) {
    pix |= (uint32_t)gray << fb->green_off;
  }
  if (fb->blue_len) {
    pix |= (uint32_t)gray << fb->blue_off;
  }
  if (fb->alpha_len > 0 && fb->alpha_len < 32) {
    pix |= ((1u << fb->alpha_len) - 1u) << fb->alpha_off;
  }
  memcpy(p, &pix, 4);
}

int kindle_fb_open(KindleFb **out) {
  if (out) {
    *out = NULL;
  }
  KindleFb *fb = calloc(1, sizeof(*fb));
  if (!fb) {
    return -1;
  }
  fb->fd = open("/dev/fb0", O_RDWR | O_CLOEXEC);
  if (fb->fd < 0) {
    yomu_log("open /dev/fb0: %s", strerror(errno));
    free(fb);
    return -1;
  }
  struct fb_fix_screeninfo finfo;
  struct fb_var_screeninfo vinfo;
  memset(&finfo, 0, sizeof finfo);
  memset(&vinfo, 0, sizeof vinfo);
  if (ioctl(fb->fd, FBIOGET_FSCREENINFO, &finfo) != 0 || ioctl(fb->fd, FBIOGET_VSCREENINFO, &vinfo) != 0) {
    yomu_log("framebuffer info: %s", strerror(errno));
    close(fb->fd);
    free(fb);
    return -1;
  }
  memcpy(fb->id, finfo.id, 16);
  fb->id[16] = '\0';
  fb->line_length = (int)finfo.line_length;
  fb->smem_len = finfo.smem_len;
  fb->bpp = (int)vinfo.bits_per_pixel;
  fb->xoffset = (int)vinfo.xoffset;
  fb->yoffset = (int)vinfo.yoffset;
  fb->red_off = (int)vinfo.red.offset;
  fb->red_len = (int)vinfo.red.length;
  fb->green_off = (int)vinfo.green.offset;
  fb->green_len = (int)vinfo.green.length;
  fb->blue_off = (int)vinfo.blue.offset;
  fb->blue_len = (int)vinfo.blue.length;
  fb->alpha_off = (int)vinfo.transp.offset;
  fb->alpha_len = (int)vinfo.transp.length;
  fb->nat_w = (int)vinfo.xres;
  fb->nat_h = (int)vinfo.yres;
  fb->marker = 1;
  fb->prefer_mtk = strncmp(fb->id, "hwtcon", 6) == 0;

  int detected_invert = 0;
  if (strncmp(fb->id, "eink_fb", 7) == 0 || vinfo.grayscale == 2) {
    detected_invert = 1;
  }
  int forced = 0;
  if (env_force("YOMU_FB_INVERT", &forced)) {
    fb->invert = forced;
  } else {
    fb->invert = detected_invert;
  }

  const char *view = getenv("YOMU_VIEW");
  if (view && view[0]) {
    int x = 0, y = 0, w = 0, h = 0;
    if (sscanf(view, "%d,%d,%d,%d", &x, &y, &w, &h) == 4 && w >= 16 && h >= 16) {
      fb->view_x = x;
      fb->view_y = y;
      fb->nat_w = w;
      fb->nat_h = h;
    } else {
      yomu_log("ignored YOMU_VIEW=%s", view);
    }
  }
  fb->rot = env_rotate();
  yomu_logical_size(fb->nat_w, fb->nat_h, fb->rot, &fb->view_w, &fb->view_h);

  if (fb->view_w < 16 || fb->view_h < 16 || (fb->bpp != 8 && fb->bpp != 16 && fb->bpp != 32)) {
    yomu_log("unsupported framebuffer %dx%d %dbpp id %s", fb->nat_w, fb->nat_h, fb->bpp, fb->id);
    close(fb->fd);
    free(fb);
    return -1;
  }
  fb->mem = mmap(NULL, fb->smem_len ? fb->smem_len : (size_t)fb->line_length * (size_t)vinfo.yres_virtual, PROT_READ | PROT_WRITE,
                 MAP_SHARED, fb->fd, 0);
  if (fb->mem == MAP_FAILED) {
    yomu_log("mmap framebuffer: %s", strerror(errno));
    close(fb->fd);
    free(fb);
    return -1;
  }
  if (fb->smem_len == 0) {
    fb->smem_len = (size_t)fb->line_length * (size_t)vinfo.yres_virtual;
  }
  yomu_log("fb %s %dx%d %dbpp line %d gray %u rotate %u invert %d view %d,%d %dx%d logical %dx%d rot %d", fb->id,
             vinfo.xres, vinfo.yres, fb->bpp, fb->line_length, vinfo.grayscale, vinfo.rotate, fb->invert, fb->view_x,
             fb->view_y, fb->nat_w, fb->nat_h, fb->view_w, fb->view_h, fb->rot);
  *out = fb;
  return 0;
}

void kindle_fb_close(KindleFb *fb) {
  if (!fb) {
    return;
  }
  if (fb->mem && fb->mem != MAP_FAILED) {
    munmap(fb->mem, fb->smem_len);
  }
  if (fb->fd >= 0) {
    close(fb->fd);
  }
  free(fb);
}

int kindle_fb_width(const KindleFb *fb) {
  return fb ? fb->view_w : 0;
}

int kindle_fb_height(const KindleFb *fb) {
  return fb ? fb->view_h : 0;
}

int kindle_fb_native_width(const KindleFb *fb) {
  return fb ? fb->nat_w : 0;
}

int kindle_fb_native_height(const KindleFb *fb) {
  return fb ? fb->nat_h : 0;
}

int kindle_fb_rotation(const KindleFb *fb) {
  return fb ? fb->rot : 0;
}

static int gray8_fits(const KindleFb *fb) {
  if (fb->bpp != 8 || fb->line_length <= 0 || fb->nat_w < 1 || fb->nat_h < 1) {
    return 0;
  }
  int x1 = fb->xoffset + fb->view_x + fb->nat_w - 1;
  int y1 = fb->yoffset + fb->view_y + fb->nat_h - 1;
  if (x1 < 0 || y1 < 0) {
    return 0;
  }
  size_t off = (size_t)y1 * (size_t)fb->line_length + (size_t)x1;
  return off < fb->smem_len;
}

static uint8_t gray_byte(const KindleFb *fb, uint8_t gray) {
  return fb->invert ? (uint8_t)(255 - gray) : gray;
}

/* Landscape rotation turns each logical row into a native column. A tile
 * keeps that column in a few cache lines instead of one store per pixel. */
static void blit_gray8(KindleFb *fb, const uint8_t *gray) {
  const int rot = fb->rot & 3;
  const int view_w = fb->view_w;
  const int view_h = fb->view_h;
  const int line = fb->line_length;
  const int x0 = fb->xoffset + fb->view_x;
  const int y0 = fb->yoffset + fb->view_y;
  uint8_t *base = fb->mem;
  if (rot == 0) {
    for (int ly = 0; ly < view_h; ly++) {
      const uint8_t *src = gray + (size_t)ly * (size_t)view_w;
      uint8_t *dst = base + (size_t)(y0 + ly) * (size_t)line + (size_t)x0;
      if (!fb->invert) {
        memcpy(dst, src, (size_t)view_w);
      } else {
        for (int lx = 0; lx < view_w; lx++) {
          dst[lx] = (uint8_t)(255 - src[lx]);
        }
      }
    }
    return;
  }
  if (rot == 2) {
    for (int ly = 0; ly < view_h; ly++) {
      const uint8_t *src = gray + (size_t)ly * (size_t)view_w;
      int ny = fb->nat_h - 1 - ly;
      uint8_t *dst = base + (size_t)(y0 + ny) * (size_t)line + (size_t)x0;
      for (int lx = 0; lx < view_w; lx++) {
        dst[fb->nat_w - 1 - lx] = gray_byte(fb, src[lx]);
      }
    }
    return;
  }
  enum { TILE = 64 };
  uint8_t tile[TILE * TILE];
  for (int ly0 = 0; ly0 < view_h; ly0 += TILE) {
    int th = view_h - ly0;
    if (th > TILE) {
      th = TILE;
    }
    for (int lx0 = 0; lx0 < view_w; lx0 += TILE) {
      int tw = view_w - lx0;
      if (tw > TILE) {
        tw = TILE;
      }
      for (int i = 0; i < th; i++) {
        const uint8_t *src = gray + (size_t)(ly0 + i) * (size_t)view_w + (size_t)lx0;
        uint8_t *dst = tile + (size_t)i * TILE;
        for (int j = 0; j < tw; j++) {
          dst[j] = gray_byte(fb, src[j]);
        }
      }
      if (rot == 3) {
        /* nx = nat_w - 1 - ly, ny = lx */
        for (int j = 0; j < tw; j++) {
          uint8_t *row = base + (size_t)(y0 + lx0 + j) * (size_t)line + (size_t)x0;
          int nx = fb->nat_w - 1 - ly0;
          for (int i = 0; i < th; i++) {
            row[nx - i] = tile[(size_t)i * TILE + (size_t)j];
          }
        }
      } else {
        /* rot 1: nx = ly, ny = nat_h - 1 - lx */
        for (int j = 0; j < tw; j++) {
          int ny = fb->nat_h - 1 - (lx0 + j);
          uint8_t *row = base + (size_t)(y0 + ny) * (size_t)line + (size_t)x0;
          for (int i = 0; i < th; i++) {
            row[ly0 + i] = tile[(size_t)i * TILE + (size_t)j];
          }
        }
      }
    }
  }
}

static void blit_slow(KindleFb *fb, const uint8_t *gray) {
  for (int ly = 0; ly < fb->view_h; ly++) {
    const uint8_t *row = gray + (size_t)ly * (size_t)fb->view_w;
    for (int lx = 0; lx < fb->view_w; lx++) {
      int nx = 0;
      int ny = 0;
      yomu_to_native(fb->rot, fb->nat_w, fb->nat_h, lx, ly, &nx, &ny);
      write_pixel(fb, fb->view_x + nx, fb->view_y + ny, row[lx]);
    }
  }
}

void kindle_fb_present(KindleFb *fb, const uint8_t *gray, int x, int y, int w, int h, int full) {
  if (!fb || !gray) {
    return;
  }
  int64_t blit_at = yomu_ms();
  if (gray8_fits(fb)) {
    blit_gray8(fb, gray);
  } else {
    blit_slow(fb, gray);
  }
  int64_t blit_ms = yomu_ms() - blit_at;
  if (w <= 0 || h <= 0) {
    x = 0;
    y = 0;
    w = fb->view_w;
    h = fb->view_h;
  }
  int nx = 0;
  int ny = 0;
  int nw = 0;
  int nh = 0;
  yomu_rect_native(fb->rot, fb->nat_w, fb->nat_h, x, y, w, h, &nx, &ny, &nw, &nh);
  PanelTime panel;
  memset(&panel, 0, sizeof panel);
  int rc = refresh(fb, nx, ny, nw, nh, full, &panel);
  yomu_log("perf present %s blit %lld wait %lld send %lld done %lld rect %d,%d %dx%d%s%s",
             full ? "full" : "partial", (long long)blit_ms, (long long)panel.wait_ms, (long long)panel.send_ms,
             (long long)panel.done_ms, panel.x, panel.y, panel.w, panel.h, panel.cropped ? " crop" : "",
             rc == 0 ? "" : " fail");
}
