#ifndef YOMU_ROTATE_H
#define YOMU_ROTATE_H

/* Linux fb rotate values. 0 is the panel's native portrait. 3 is 90°
 * clockwise and puts the bottom of the page on the panel's left edge.
 * 1 is 270° clockwise, with the bottom on the right edge.
 */

static inline void yomu_logical_size(int nat_w, int nat_h, int rot, int *lw, int *lh) {
  if ((rot & 3) == 1 || (rot & 3) == 3) {
    *lw = nat_h;
    *lh = nat_w;
  } else {
    *lw = nat_w;
    *lh = nat_h;
  }
}

static inline void yomu_to_native(int rot, int nat_w, int nat_h, int lx, int ly, int *nx, int *ny) {
  switch (rot & 3) {
    case 1:
      *nx = ly;
      *ny = nat_h - 1 - lx;
      break;
    case 2:
      *nx = nat_w - 1 - lx;
      *ny = nat_h - 1 - ly;
      break;
    case 3:
      *nx = nat_w - 1 - ly;
      *ny = lx;
      break;
    default:
      *nx = lx;
      *ny = ly;
      break;
  }
}

static inline void yomu_to_logical(int rot, int nat_w, int nat_h, int nx, int ny, int *lx, int *ly) {
  switch (rot & 3) {
    case 1:
      *lx = nat_h - 1 - ny;
      *ly = nx;
      break;
    case 2:
      *lx = nat_w - 1 - nx;
      *ly = nat_h - 1 - ny;
      break;
    case 3:
      *lx = ny;
      *ly = nat_w - 1 - nx;
      break;
    default:
      *lx = nx;
      *ly = ny;
      break;
  }
}

/* The native rectangle that covers a logical one. w and h are at least 1. */
static inline void yomu_rect_native(int rot, int nat_w, int nat_h, int x, int y, int w, int h, int *ox,
                                       int *oy, int *ow, int *oh) {
  int x0, y0, x1, y1;
  yomu_to_native(rot, nat_w, nat_h, x, y, &x0, &y0);
  yomu_to_native(rot, nat_w, nat_h, x + w - 1, y + h - 1, &x1, &y1);
  int nx = x0 < x1 ? x0 : x1;
  int ny = y0 < y1 ? y0 : y1;
  *ox = nx;
  *oy = ny;
  *ow = (x0 > x1 ? x0 : x1) - nx + 1;
  *oh = (y0 > y1 ? y0 : y1) - ny + 1;
}

#endif
