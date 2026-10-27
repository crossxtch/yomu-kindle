/* List layout and the buttons every screen shares. */

#include "screen.h"

#include "draw.h"

#include <string.h>

Layout layout_of(int width, int height) {
  Layout layout;
  int span = width > height ? width : height;
  if (span >= 1800) {
    layout.scale = 4;
  } else if (span >= 1400) {
    layout.scale = 3;
  } else {
    layout.scale = 2;
  }
  layout.pad = 4 * layout.scale;
  layout.header_h = 8 * layout.scale + layout.pad * 2;
  layout.footer_h = 8 * layout.scale + layout.pad;
  int body = height - layout.header_h - layout.footer_h;
  layout.row_h = 8 * layout.scale * 2 + layout.pad * 2;
  layout.rows = body > 0 ? body / layout.row_h : 1;
  if (layout.rows < 1) {
    layout.rows = 1;
  }
  if (layout.rows > HOT_MAX) {
    layout.rows = HOT_MAX;
  }
  return layout;
}

void move_cursor(int *cursor, int *top, int count, int window, int delta) {
  if (count <= 0 || delta == 0) {
    return;
  }
  *cursor += delta;
  if (*cursor < 0) {
    *cursor = 0;
  }
  if (*cursor >= count) {
    *cursor = count - 1;
  }
  if (*cursor < *top) {
    *top = *cursor;
  }
  if (*cursor >= *top + window) {
    *top = *cursor - window + 1;
  }
  if (*top < 0) {
    *top = 0;
  }
}

/* Keys only. A swipe is not a list move. Up and Down are buttons. */
int list_delta(AppEvKind kind, int window) {
  switch (kind) {
    case EV_UP:
      return -1;
    case EV_DOWN:
      return 1;
    case EV_PAGE_PREV:
    case EV_LEFT:
      return -window;
    case EV_PAGE_NEXT:
    case EV_RIGHT:
      return window;
    default:
      return 0;
  }
}

int edge_band(int height) {
  int edge = height / 8;
  if (edge < 1) {
    edge = 1;
  }
  return edge;
}

int grip_band(int width) {
  int band = width * 6 / 100;
  if (band < 1 && width > 0) {
    band = 1;
  }
  return band;
}

static int rows_for(const Layout *layout, int height, int with_bar) {
  int bottom = layout->footer_h + (with_bar ? layout->row_h : 0);
  int body = height - layout->header_h - bottom;
  int rows = body > 0 ? body / layout->row_h : 1;
  if (rows < 1) {
    rows = 1;
  }
  if (rows > HOT_MAX - 3) {
    rows = HOT_MAX - 3;
  }
  return rows;
}

void list_chrome(const Layout *layout, int height, int total, int top, int need_back, Chrome *out) {
  int rows = rows_for(layout, height, 0);
  int overflow = total > rows;
  int bar = need_back || overflow;
  if (bar) {
    rows = rows_for(layout, height, 1);
    overflow = total > rows;
    bar = need_back || overflow;
    if (!bar) {
      rows = rows_for(layout, height, 0);
    }
  }
  memset(out, 0, sizeof *out);
  out->rows = rows;
  out->bar = bar;
  out->bar_h = layout->row_h;
  out->bar_y = height - layout->footer_h - out->bar_h;
  out->show_back = need_back && bar;
  out->show_up = bar && top > 0;
  out->show_down = bar && (top + rows < total);
}

void fit_list(const Layout *layout, int height, int total, int *cursor, int *top, int need_back,
                     Chrome *out) {
  if (total <= 0) {
    *cursor = 0;
    *top = 0;
  } else {
    if (*cursor < 0) {
      *cursor = 0;
    }
    if (*cursor >= total) {
      *cursor = total - 1;
    }
    if (*top < 0) {
      *top = 0;
    }
    if (*top > *cursor) {
      *top = *cursor;
    }
  }
  list_chrome(layout, height, total, *top, need_back, out);
  if (total > 0) {
    if (*cursor >= *top + out->rows) {
      *top = *cursor - out->rows + 1;
    }
    int max_top = total > out->rows ? total - out->rows : 0;
    if (*top > max_top) {
      *top = max_top;
    }
    if (*top < 0) {
      *top = 0;
    }
  }
  list_chrome(layout, height, total, *top, need_back, out);
}

void page_list(int *cursor, int *top, int total, int window, int dir) {
  if (total <= 0 || window < 1 || dir == 0) {
    return;
  }
  int max_top = total > window ? total - window : 0;
  int next = *top + dir * window;
  if (next < 0) {
    next = 0;
  }
  if (next > max_top) {
    next = max_top;
  }
  *top = next;
  if (*cursor < *top) {
    *cursor = *top;
  }
  if (*cursor >= *top + window) {
    *cursor = *top + window - 1;
  }
  if (*cursor >= total) {
    *cursor = total - 1;
  }
  if (*cursor < 0) {
    *cursor = 0;
  }
}

int row_fits(const Chrome *chrome, const Layout *layout, int height, int y) {
  int limit = chrome->bar ? chrome->bar_y : height - layout->footer_h;
  return y + layout->row_h <= limit;
}

int hit_hot(const State *st, int x, int y) {
  for (int i = st->nhots - 1; i >= 0; i--) {
    const Hot *hot = &st->hots[i];
    if (x >= hot->x && y >= hot->y && x < hot->x + hot->w && y < hot->y + hot->h) {
      return hot->id;
    }
  }
  return -1;
}

void add_hot(State *st, int x, int y, int w, int h, int id) {
  if (st->nhots >= HOT_MAX) {
    return;
  }
  st->hots[st->nhots++] = (Hot){x, y, w, h, id};
}

void paint_button(State *st, const Layout *layout, int x, int y, int w, int h, const char *label, int id) {
  AppHost *host = st->host;
  if (w < 1 || h < 1) {
    return;
  }
  draw_fill(host->gray, host->w, host->h, host->w, x, y, w, h, 0);
  if (w > 4 && h > 4) {
    draw_fill(host->gray, host->w, host->h, host->w, x + 2, y + 2, w - 4, h - 4, 255);
  }
  int tw = draw_text_width(layout->scale, label);
  int tx = x + (w - tw) / 2;
  int ty = y + (h - 8 * layout->scale) / 2;
  draw_text(host->gray, host->w, host->h, host->w, tx, ty, layout->scale, label, 0);
  add_hot(st, x, y, w, h, id);
}

void paint_key(State *st, int x, int y, int w, int h, int scale, const char *label, int id) {
  AppHost *host = st->host;
  if (w < 1 || h < 1 || scale < 1) {
    return;
  }
  draw_fill(host->gray, host->w, host->h, host->w, x, y, w, h, 0);
  if (w > 4 && h > 4) {
    draw_fill(host->gray, host->w, host->h, host->w, x + 2, y + 2, w - 4, h - 4, 255);
  }
  int tw = draw_text_width(scale, label);
  int tx = x + (w - tw) / 2;
  int ty = y + (h - 8 * scale) / 2;
  if (tx < x) {
    tx = x;
  }
  if (ty < y) {
    ty = y;
  }
  draw_text(host->gray, host->w, host->h, host->w, tx, ty, scale, label, 0);
  add_hot(st, x, y, w, h, id);
}

void paint_bar(State *st, const Layout *layout, const Chrome *chrome) {
  if (!chrome->bar) {
    return;
  }
  const char *labels[3];
  int ids[3];
  int n = 0;
  if (chrome->show_back) {
    labels[n] = "戻る";
    ids[n++] = HOT_BACK;
  }
  if (chrome->show_up) {
    labels[n] = "上";
    ids[n++] = HOT_UP;
  }
  if (chrome->show_down) {
    labels[n] = "下";
    ids[n++] = HOT_DOWN;
  }
  if (n <= 0) {
    return;
  }
  int x = 0;
  int bw = st->host->w / n;
  for (int i = 0; i < n; i++) {
    int w = (i == n - 1) ? st->host->w - x : bw;
    paint_button(st, layout, x, chrome->bar_y, w, chrome->bar_h, labels[i], ids[i]);
    x += w;
  }
}

void paint_header(State *st, const Layout *layout, const char *title, const char *right) {
  AppHost *host = st->host;
  draw_text(host->gray, host->w, host->h, host->w, layout->pad, layout->pad, layout->scale, title, 0);
  if (right && right[0]) {
    int tw = draw_text_width(layout->scale, right);
    int x = host->w - layout->pad - tw;
    if (x < layout->pad) {
      x = layout->pad;
    }
    draw_text(host->gray, host->w, host->h, host->w, x, layout->pad, layout->scale, right, 0);
  }
  draw_fill(host->gray, host->w, host->h, host->w, 0, layout->header_h - 2, host->w, 2, 0);
}

void paint_footer(State *st, const Layout *layout, const char *hint) {
  AppHost *host = st->host;
  int y = host->h - layout->footer_h + layout->pad / 2;
  const char *text = st->note[0] ? st->note : hint;
  draw_text(host->gray, host->w, host->h, host->w, layout->pad, y, layout->scale, text ? text : "", 0);
}

/* Twice the previous menu on Kindle-sized panels. Scale 2 stays at 2x.
 * Four reader rows still leave a gap on a 200px host. The chapter list
 * uses a 280px host so 戻る sits below that gap. */
int menu_text_scale(const Layout *layout) {
  return layout->scale < 3 ? layout->scale * 2 : layout->scale * 4;
}
