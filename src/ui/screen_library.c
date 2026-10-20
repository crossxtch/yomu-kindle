/* Bookshelf: one row per book. Swipe down from the top to open the menu. */

#include "screen.h"

#include "clockpref.h"
#include "draw.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

static int library_count(const State *st) { return st->nbooks; }

int library_clock_box(const Layout *layout, int width, const char *count, int *x, int *y, int *w, int *h) {
  int scale;
  int pad;
  int title_w;
  int count_w;
  int clock_w;
  int count_x;
  int left;
  int right;
  int cx;
  if (!layout || width < 1 || !count) {
    return 0;
  }
  scale = layout->scale;
  pad = layout->pad;
  if (scale < 1) {
    return 0;
  }
  title_w = draw_text_width(scale, "yomu");
  count_w = draw_text_width(scale, count);
  clock_w = draw_text_width(scale, "00:00");
  count_x = width - pad - count_w;
  if (count_x < pad) {
    count_x = pad;
  }
  left = pad + title_w + pad;
  right = count_x - pad;
  if (clock_w < 1 || right - left < clock_w) {
    return 0;
  }
  cx = (width - clock_w) / 2;
  if (cx < left) {
    cx = left;
  }
  if (cx + clock_w > right) {
    cx = right - clock_w;
  }
  if (x) {
    *x = cx;
  }
  if (y) {
    *y = pad;
  }
  if (w) {
    *w = clock_w;
  }
  if (h) {
    *h = 8 * scale;
  }
  return 1;
}

static int clock_minute(char *buf, size_t n, int64_t *minute) {
  time_t now;
  now = time(NULL);
  if (now == (time_t)-1 || !clock_format(now, clock_offset_q(), buf, n)) {
    return 0;
  }
  if (minute) {
    *minute = (int64_t)now / 60;
  }
  return 1;
}

static void draw_library_clock(State *st) {
  char text[8];
  char count[32];
  int64_t minute = -1;
  int x = 0;
  int y = 0;
  int w = 0;
  int h = 0;
  Layout layout;
  AppHost *host = st->host;
  snprintf(count, sizeof count, "%d", st->nbooks);
  layout = layout_of(host->w, host->h);
  if (!clock_minute(text, sizeof text, &minute) ||
      !library_clock_box(&layout, host->w, count, &x, &y, &w, &h)) {
    st->clock_min = -1;
    return;
  }
  draw_fill(host->gray, host->w, host->h, host->w, x, y, w, h, 255);
  draw_text(host->gray, host->w, host->h, host->w, x, y, layout.scale, text, 0);
  st->clock_min = minute;
  st->clock_q = clock_offset_q();
}

int library_clock_wait_ms(const State *st) {
  struct timespec ts;
  int ms;
  if (!st || st->screen != SCR_LIB || st->clock_min < 0) {
    return -1;
  }
  if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
    return 60000;
  }
  ms = (int)(60000 - (ts.tv_sec % 60) * 1000 - ts.tv_nsec / 1000000L);
  if (ms < 1) {
    ms = 1;
  }
  if (ms > 60000) {
    ms = 60000;
  }
  return ms;
}

int refresh_library_clock(State *st) {
  char text[8];
  char count[32];
  int64_t minute = -1;
  int q;
  int x = 0;
  int y = 0;
  int w = 0;
  int h = 0;
  Layout layout;
  AppHost *host;
  if (!st || !st->host || st->screen != SCR_LIB || st->clock_min < 0) {
    return 0;
  }
  host = st->host;
  q = clock_offset_q();
  if (!clock_minute(text, sizeof text, &minute) || (minute == st->clock_min && q == st->clock_q)) {
    return 0;
  }
  snprintf(count, sizeof count, "%d", st->nbooks);
  layout = layout_of(host->w, host->h);
  if (!library_clock_box(&layout, host->w, count, &x, &y, &w, &h)) {
    st->clock_min = -1;
    return 0;
  }
  draw_fill(host->gray, host->w, host->h, host->w, x, y, w, h, 255);
  draw_text(host->gray, host->w, host->h, host->w, x, y, layout.scale, text, 0);
  st->clock_min = minute;
  st->clock_q = q;
  if (!host->present) {
    return 0;
  }
  host->present(host, x, y, w, h, 0);
  return 1;
}

static void refresh_books(State *st) {
  if (st->fs && fileserver_changed(st->fs)) {
    st->rescan = 1;
  }
  if (!st->rescan) {
    return;
  }
  yomu_free_books(st->books, st->nbooks);
  st->books = NULL;
  st->nbooks = 0;
  int trunc = 0;
  if (yomu_scan_books(st->host->root, &st->books, &st->nbooks, &trunc) != 0) {
    snprintf(st->note, sizeof st->note, "本のフォルダを開けません");
  } else {
    st->truncated = trunc;
  }
  int total = library_count(st);
  if (st->lib_cursor >= total) {
    st->lib_cursor = total - 1;
  }
  if (st->lib_cursor < 0) {
    st->lib_cursor = 0;
  }
  st->rescan = 0;
}

void paint_library(State *st) {
  refresh_books(st);
  AppHost *host = st->host;
  Layout layout = layout_of(host->w, host->h);
  st->nhots = 0;
  draw_clear(host->gray, host->w, host->h, host->w, 255);
  char right[32];
  snprintf(right, sizeof right, "%d", st->nbooks);
  paint_header(st, &layout, "yomu", right);
  draw_library_clock(st);
  int total = library_count(st);
  Chrome chrome;
  fit_list(&layout, host->h, total, &st->lib_cursor, &st->lib_top, 0, &chrome);
  layout.rows = chrome.rows;
  for (int row = 0; row < layout.rows; row++) {
    int index = st->lib_top + row;
    if (index >= total) {
      break;
    }
    int y = layout.header_h + row * layout.row_h;
    if (!row_fits(&chrome, &layout, host->h, y)) {
      break;
    }
    if (index == st->lib_cursor) {
      draw_fill(host->gray, host->w, host->h, host->w, 0, y, layout.pad, layout.row_h, 0);
    }
    const YomuBook *book = &st->books[index];
    char caption[256];
    yomu_book_caption(book, index, caption, sizeof caption);
    int text_x = layout.pad;
    int text_y = y + layout.pad;
    draw_text(host->gray, host->w, host->h, host->w, text_x, text_y, layout.scale, caption, 0);
    add_hot(st, 0, y, host->w, layout.row_h, index);
  }
  char hint[160];
  if (st->truncated) {
    snprintf(hint, sizeof hint, "行をタップ  一覧は途中まで");
  } else if (st->nbooks == 0) {
    snprintf(hint, sizeof hint, ".epub .txt .mobi .azw3の本はありません");
  } else {
    snprintf(hint, sizeof hint, "行をタップ");
  }
  paint_bar(st, &layout, &chrome);
  paint_footer(st, &layout, hint);
}

int handle_library(State *st, const AppEvent *ev) {
  Layout layout = layout_of(st->host->w, st->host->h);
  int total = library_count(st);
  Chrome chrome;
  fit_list(&layout, st->host->h, total, &st->lib_cursor, &st->lib_top, 0, &chrome);
  if (ev->kind == EV_SWIPE_DOWN && ev->y0 < edge_band(st->host->h)) {
    st->screen = SCR_MENU;
    st->menu_cursor = 0;
    return 1;
  }
  if (ev->kind == EV_TAP) {
    int id = hit_hot(st, ev->x, ev->y);
    if (id == HOT_UP || id == HOT_DOWN) {
      page_list(&st->lib_cursor, &st->lib_top, total, chrome.rows, id == HOT_UP ? -1 : 1);
      return 1;
    }
    if (id < 0 || id >= total) {
      return 0;
    }
    st->lib_cursor = id;
    open_book(st, st->books[id].path);
    return 1;
  }
  if (ev->kind == EV_CONFIRM) {
    if (st->lib_cursor < 0 || st->lib_cursor >= st->nbooks) {
      return 0;
    }
    open_book(st, st->books[st->lib_cursor].path);
    return 1;
  }
  if (ev->kind == EV_BACK || ev->kind == EV_QUIT) {
    st->quit = 1;
    return 1;
  }
  int delta = list_delta(ev->kind, chrome.rows);
  if (delta == 0) {
    return 0;
  }
  move_cursor(&st->lib_cursor, &st->lib_top, total, chrome.rows, delta);
  return 1;
}

int back_to_library(State *st) {
  st->screen = SCR_LIB;
  st->note[0] = '\0';
  refresh_books(st);
  return 1;
}
