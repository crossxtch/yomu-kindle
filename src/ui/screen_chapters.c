/* Chapter list. A chapter that is not typeset yet is opened from here. */

#include "screen.h"

#include "draw.h"
#include "progress.h"

#include <stdio.h>
#include <string.h>

/* Named chapter that contains page. A page before the first mark stays on chapter 0. */
static int chapter_for_page(const State *st, int page) {
  int count = reading_chapter_count(st);
  int found = 0;
  int i;
  if (page < 0) {
    page = 0;
  }
  for (i = 0; i < count; i++) {
    int start = -1;
    int end = -1;
    if (!reading_chapter(st, i, NULL, 0, &start, &end)) {
      break;
    }
    if (start < 0 || page < start) {
      break;
    }
    found = i;
    if (end >= 0 && page <= end) {
      break;
    }
  }
  return found;
}

void chapters_focus_current(State *st) {
  int index = chapter_for_page(st, st->page);
  st->chap_cursor = index;
  st->chap_top = index;
}

void paint_chapters(State *st) {
  AppHost *host = st->host;
  Layout layout = layout_of(host->w, host->h);
  st->nhots = 0;
  draw_clear(host->gray, host->w, host->h, host->w, 255);
  int count = reading_chapter_count(st);
  char right[32];
  snprintf(right, sizeof right, "%d", count);
  paint_header(st, &layout, "目次", right);
  Chrome chrome;
  fit_list(&layout, host->h, count, &st->chap_cursor, &st->chap_top, 1, &chrome);
  layout.rows = chrome.rows;
  for (int row = 0; row < layout.rows; row++) {
    int index = st->chap_top + row;
    if (index >= count) {
      break;
    }
    char chapter_name[96];
    int start = 0;
    int end = 0;
    if (!reading_chapter(st, index, chapter_name, sizeof chapter_name, &start, &end)) {
      break;
    }
    int y = layout.header_h + row * layout.row_h;
    if (!row_fits(&chrome, &layout, host->h, y)) {
      break;
    }
    if (index == st->chap_cursor) {
      draw_fill(host->gray, host->w, host->h, host->w, 0, y, layout.pad, layout.row_h, 0);
    }
    const char *shown = chapter_name;
    char fallback[32];
    if (!yomu_text_useful(shown)) {
      snprintf(fallback, sizeof fallback, "第%d章", index + 1);
      shown = fallback;
    }
    char range[40];
    range[0] = '\0';
    if (start >= 0 && end >= 0) {
      snprintf(range, sizeof range, "%u-%uページ", (unsigned)start + 1, (unsigned)end + 1);
    } else if (start >= 0) {
      snprintf(range, sizeof range, "%uページ〜", (unsigned)start + 1);
    }
    int text_x = layout.pad * 2;
    draw_text(host->gray, host->w, host->h, host->w, text_x, y + layout.pad / 2, layout.scale, shown, 0);
    if (range[0]) {
      draw_text(host->gray, host->w, host->h, host->w, text_x, y + layout.pad / 2 + 8 * layout.scale + 2,
                layout.scale, range, 80);
    }
    add_hot(st, 0, y, host->w, layout.row_h, index);
  }
  paint_bar(st, &layout, &chrome);
  paint_footer(st, &layout, count > 0 ? "章をタップ" : "");
}

static void show_opening(State *st) {
  st->index_shown = 0;
  st->index_done = -1;
  st->index_total = -1;
  paint_opening(st, 0, 0);
}

static int go_chapter(State *st, int index) {
  int start = 0;
  if (!reading_chapter(st, index, NULL, 0, &start, NULL)) {
    return 0;
  }
  if (start < 0) {
    int page = 0;
    show_opening(st);
    if (!reading_open_chapter(st, index, &page)) {
      snprintf(st->note, sizeof st->note, "この章は開けません");
      return 1;
    }
    reading_drop_pages(st);
    start = page;
  }
  st->note[0] = '\0';
  st->page = start;
  st->since_full = 0;
  st->screen = SCR_READ;
  progress_save(st->host->home, st->book_path, st->page);
  return 1;
}

int handle_chapters(State *st, const AppEvent *ev) {
  Layout layout = layout_of(st->host->w, st->host->h);
  int count = reading_chapter_count(st);
  Chrome chrome;
  fit_list(&layout, st->host->h, count, &st->chap_cursor, &st->chap_top, 1, &chrome);
  if (ev->kind == EV_TAP) {
    int id = hit_hot(st, ev->x, ev->y);
    if (id == HOT_BACK) {
      st->screen = SCR_MENU;
      return 1;
    }
    if (id == HOT_UP || id == HOT_DOWN) {
      page_list(&st->chap_cursor, &st->chap_top, count, chrome.rows, id == HOT_UP ? -1 : 1);
      return 1;
    }
    if (id < 0) {
      return 0;
    }
    return go_chapter(st, id);
  }
  if (ev->kind == EV_CONFIRM && count > 0) {
    return go_chapter(st, st->chap_cursor);
  }
  if (ev->kind == EV_BACK) {
    st->screen = SCR_MENU;
    return 1;
  }
  int delta = list_delta(ev->kind, chrome.rows);
  if (delta == 0 || count <= 0) {
    return 0;
  }
  move_cursor(&st->chap_cursor, &st->chap_top, count, chrome.rows, delta);
  return 1;
}
