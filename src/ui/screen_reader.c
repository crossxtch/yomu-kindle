/* The open book, its page cache, and reader gestures. */

#include "screen.h"

#include "draw.h"
#include "log.h"
#include "progress.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static void drop_page_cache(State *st);

/* 1 when the typeset page has any ink. A failed render counts as ink so a
 * blank cover is the only thing we skip. */
static int typeset_has_ink(TsBook *ts, int page, int w, int h) {
  size_t n;
  size_t i;
  uint8_t *gray;
  if (w < 1 || h < 1) {
    return 1;
  }
  n = (size_t)w * (size_t)h;
  gray = malloc(n);
  if (!gray) {
    return 1;
  }
  if (!ts_render(ts, page, gray, w, h)) {
    free(gray);
    return 1;
  }
  for (i = 0; i < n; i++) {
    if (gray[i] < 250) {
      free(gray);
      return 1;
    }
  }
  free(gray);
  return 0;
}

static int path_is_text(const char *path) {
  size_t n;
  if (!path) {
    return 0;
  }
  n = strlen(path);
  if (n >= 5 && strcasecmp(path + (n - 5), ".epub") == 0) {
    return 1;
  }
  if (n >= 4 && strcasecmp(path + (n - 4), ".txt") == 0) {
    return 1;
  }
  return 0;
}

static void release_open(State *st) {
  if (st->ts) {
    progress_save(st->host->home, st->book_path, st->page);
  }
  if (st->ts) {
    ts_close(st->ts);
    st->ts = NULL;
  }
  drop_page_cache(st);
}

int reading_open(const State *st) { return st->ts != NULL; }

int reading_pages(const State *st) { return st->ts ? ts_pages(st->ts) : 0; }

int reading_rtl(const State *st) { return st->ts && ts_rtl(st->ts); }

int reading_chapter_count(const State *st) { return st->ts ? ts_chapter_count(st->ts) : 0; }

int reading_chapter(const State *st, int index, char *name, size_t name_n, int *start, int *end) {
  if (!st->ts) {
    return 0;
  }
  return ts_chapter(st->ts, index, name, name_n, start, end);
}

int reading_open_chapter(State *st, int index, int *page) {
  if (!st || !st->ts) {
    return 0;
  }
  return ts_open_chapter(st->ts, index, page);
}

void reading_drop_pages(State *st) { drop_page_cache(st); }

int open_book(State *st, const char *path) {
  if (!path_is_text(path)) {
    snprintf(st->note, sizeof st->note, "このファイルは開けません");
    yomu_log("open rejected %s", path ? path : "");
    return -1;
  }
  release_open(st);
  TsBook *ts = ts_open(path, st->host->home, NULL, st->host->w, st->host->h);
  if (!ts) {
    snprintf(st->note, sizeof st->note, "%s", ts_error(NULL));
    yomu_log("open failed %s (%s)", path, ts_error(NULL));
    st->book_path[0] = '\0';
    return -1;
  }
  st->ts = ts;
  snprintf(st->book_path, sizeof st->book_path, "%s", path);
  int page = 0;
  int pages = ts_pages(ts);
  if (progress_load(st->host->home, path, &page) != 0 || page < 0) {
    page = 0;
  }
  if (ts_done(ts) && pages > 0 && page >= pages) {
    page = pages - 1;
  }
  st->page = page;
  /* Leading xhtml in this library is often an empty cover. Stay on the
   * reader, but start at the first page that actually has ink. */
  if (st->page == 0 && pages > 1) {
    int guard = pages < 8 ? pages : 8;
    while (st->page + 1 < guard && !typeset_has_ink(ts, st->page, st->host->w, st->host->h)) {
      st->page++;
    }
  }
  st->since_full = 0;
  st->screen = SCR_READ;
  st->note[0] = '\0';
  yomu_log("open %s (%dx%d, %d pages) at %d rtl %d", path, st->host->w, st->host->h, pages, st->page + 1,
             ts_rtl(ts));
  return 0;
}

static void drop_page_cache(State *st) {
  free(st->cur_gray);
  free(st->ahead_gray);
  st->cur_gray = NULL;
  st->ahead_gray = NULL;
  st->cur_page = -1;
  st->ahead_page = -1;
}

void close_book(State *st) {
  release_open(st);
  st->book_path[0] = '\0';
}

/* The returned buffer belongs to the cache. Do not free it. */
static int hold_page(State *st, int page, uint8_t **gray, uint16_t *w, uint16_t *h) {
  int pw = st->host->w;
  int ph = st->host->h;
  size_t n = (size_t)pw * (size_t)ph;
  if (st->cur_gray && st->cur_page == page && st->cur_w == (uint16_t)pw && st->cur_h == (uint16_t)ph) {
    *gray = st->cur_gray;
    *w = st->cur_w;
    *h = st->cur_h;
    return 1;
  }
  if (st->ahead_gray && st->ahead_page == page && st->ahead_w == (uint16_t)pw && st->ahead_h == (uint16_t)ph) {
    free(st->cur_gray);
    st->cur_gray = st->ahead_gray;
    st->cur_page = st->ahead_page;
    st->cur_w = st->ahead_w;
    st->cur_h = st->ahead_h;
    st->ahead_gray = NULL;
    st->ahead_page = -1;
    *gray = st->cur_gray;
    *w = st->cur_w;
    *h = st->cur_h;
    return 1;
  }
  uint8_t *decoded = malloc(n);
  if (!decoded) {
    *gray = NULL;
    *w = 0;
    *h = 0;
    return 0;
  }
  if (!ts_render(st->ts, page, decoded, pw, ph)) {
    free(decoded);
    *gray = NULL;
    *w = 0;
    *h = 0;
    return 0;
  }
  free(st->cur_gray);
  st->cur_gray = decoded;
  st->cur_page = page;
  st->cur_w = (uint16_t)pw;
  st->cur_h = (uint16_t)ph;
  *gray = decoded;
  *w = st->cur_w;
  *h = st->cur_h;
  return 1;
}

void prefetch_next_page(State *st) {
  int next;
  int count;
  size_t n;
  uint8_t *decoded;
  if (st->screen != SCR_READ || !st->ts) {
    return;
  }
  next = st->page + 1;
  count = ts_pages(st->ts);
  if (next >= count) {
    return;
  }
  if ((st->ahead_gray && st->ahead_page == next) || (st->cur_gray && st->cur_page == next)) {
    return;
  }
  n = (size_t)st->host->w * (size_t)st->host->h;
  decoded = malloc(n);
  if (!decoded) {
    return;
  }
  if (!ts_render(st->ts, next, decoded, st->host->w, st->host->h)) {
    free(decoded);
    return;
  }
  free(st->ahead_gray);
  st->ahead_gray = decoded;
  st->ahead_page = next;
  st->ahead_w = (uint16_t)st->host->w;
  st->ahead_h = (uint16_t)st->host->h;
}

static void reader_refresh_advance(State *st, int full) {
  if (st->host->full_every == 1) {
    st->since_full = 0;
    return;
  }
  if (full) {
    st->since_full = 1;
    return;
  }
  if (st->host->full_every > 1) {
    st->since_full++;
    if (st->since_full >= st->host->full_every) {
      st->since_full = 0;
    }
  }
}

void paint_reader(State *st, int *full, int *rx, int *ry, int *rw, int *rh) {
  AppHost *host = st->host;
  *full = st->since_full <= 0;
  *rx = 0;
  *ry = 0;
  *rw = 0;
  *rh = 0;
  draw_clear(host->gray, host->w, host->h, host->w, 255);
  if (!reading_open(st)) {
    *full = 1;
    reader_refresh_advance(st, 1);
    return;
  }
  uint8_t *page = NULL;
  uint16_t pw = 0, ph = 0;
  if (!hold_page(st, st->page, &page, &pw, &ph) || !page) {
    Layout layout = layout_of(host->w, host->h);
    const char *err = ts_error(st->ts);
    draw_text(host->gray, host->w, host->h, host->w, layout.pad, layout.header_h, layout.scale,
              "このページは開けません", 0);
    if (err && err[0]) {
      draw_text(host->gray, host->w, host->h, host->w, layout.pad, layout.header_h + 8 * layout.scale + layout.pad,
                layout.scale, err, 0);
    }
    char pos[64];
    snprintf(pos, sizeof pos, "%d / %u ページ", st->page + 1, (unsigned)reading_pages(st));
    draw_text(host->gray, host->w, host->h, host->w, layout.pad,
              layout.header_h + 2 * (8 * layout.scale + layout.pad), layout.scale, pos, 0);
    *full = 1;
    reader_refresh_advance(st, 1);
    return;
  }
  memcpy(host->gray, page, (size_t)pw * (size_t)ph);
  reader_refresh_advance(st, *full);
}

void blit_page(State *st) {
  AppHost *host = st->host;
  draw_clear(host->gray, host->w, host->h, host->w, 255);
  if (!reading_open(st)) {
    return;
  }
  uint8_t *page = NULL;
  uint16_t pw = 0;
  uint16_t ph = 0;
  if (hold_page(st, st->page, &page, &pw, &ph) && page) {
    memcpy(host->gray, page, (size_t)pw * (size_t)ph);
  }
}

static int turn_page(State *st, int delta) {
  if (!reading_open(st) || delta == 0) {
    return 0;
  }
  int count = reading_pages(st);
  int next = st->page + delta;
  if (next < 0) {
    return 0;
  }
  if (next >= count) {
    /* The count is an estimate until the last page is indexed. */
    if (delta < 0 || ts_done(st->ts) || next > count) {
      return 0;
    }
  }
  st->page = next;
  progress_save(st->host->home, st->book_path, st->page);
  return 1;
}

/* Top and bottom eighth of the screen. A swipe has to start there. */
static int edge_band(int h) {
  int edge = h / 8;
  if (edge < 1) {
    edge = 1;
  }
  return edge;
}

static int leave_reader(State *st) {
  close_book(st);
  st->screen = SCR_LIB;
  st->note[0] = '\0';
  return 1;
}

int handle_reader(State *st, const AppEvent *ev) {
  if (!reading_open(st)) {
    st->screen = SCR_LIB;
    return 1;
  }
  int rtl = reading_rtl(st);
  int edge = edge_band(st->host->h);
  switch (ev->kind) {
    case EV_TAP:
      return turn_page(st, 1);
    case EV_SWIPE_DOWN:
      if (ev->y0 < edge) {
        st->screen = SCR_MENU;
        st->menu_cursor = 0;
        return 1;
      }
      return 0;
    case EV_SWIPE_UP:
      if (ev->y0 >= st->host->h - edge) {
        return turn_page(st, -1);
      }
      return 0;
    case EV_SWIPE_LEFT:
    case EV_SWIPE_RIGHT:
      return 0;
    case EV_CONFIRM:
      st->screen = SCR_MENU;
      st->menu_cursor = 0;
      return 1;
    case EV_BACK:
      return leave_reader(st);
    case EV_PAGE_NEXT:
      return turn_page(st, 1);
    case EV_PAGE_PREV:
      return turn_page(st, -1);
    case EV_RIGHT:
      return turn_page(st, rtl ? -1 : 1);
    case EV_LEFT:
      return turn_page(st, rtl ? 1 : -1);
    case EV_DOWN:
      return turn_page(st, 1);
    case EV_UP:
      return turn_page(st, -1);
    default:
      return 0;
  }
}
