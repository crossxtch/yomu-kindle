/* Bookshelf: transfer, exit, and one row per book. */

#include "screen.h"

#include "draw.h"

#include <stdio.h>
#include <string.h>

static int library_count(const State *st) { return st->nbooks + LIB_BOOKS; }

void paint_library(State *st) {
  AppHost *host = st->host;
  Layout layout = layout_of(host->w, host->h);
  st->nhots = 0;
  draw_clear(host->gray, host->w, host->h, host->w, 255);
  char right[32];
  snprintf(right, sizeof right, "%d", st->nbooks);
  paint_header(st, &layout, "yomu", right);
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
    if (index == LIB_TRANSFER) {
      int text_x = layout.pad;
      int text_y = y + layout.pad;
      draw_text(host->gray, host->w, host->h, host->w, text_x, text_y, layout.scale, "転送", 0);
      draw_text(host->gray, host->w, host->h, host->w, text_x, text_y + 8 * layout.scale + 2, layout.scale,
                "ファイルを送る", 80);
      add_hot(st, 0, y, host->w, layout.row_h, LIB_TRANSFER);
      continue;
    }
    if (index == LIB_LEAVE) {
      int text_x = layout.pad;
      int text_y = y + layout.pad;
      draw_text(host->gray, host->w, host->h, host->w, text_x, text_y, layout.scale, "終了", 0);
      draw_text(host->gray, host->w, host->h, host->w, text_x, text_y + 8 * layout.scale + 2, layout.scale,
                "yomuを終了", 80);
      add_hot(st, 0, y, host->w, layout.row_h, LIB_LEAVE);
      continue;
    }
    const YomuBook *book = &st->books[index - LIB_BOOKS];
    char caption[256];
    yomu_book_caption(book, index - LIB_BOOKS, caption, sizeof caption);
    int text_x = layout.pad;
    int text_y = y + layout.pad;
    draw_text(host->gray, host->w, host->h, host->w, text_x, text_y, layout.scale, caption, 0);
    add_hot(st, 0, y, host->w, layout.row_h, index);
  }
  char hint[160];
  if (st->truncated) {
    snprintf(hint, sizeof hint, "行をタップ  一覧は途中まで");
  } else if (st->nbooks == 0) {
    snprintf(hint, sizeof hint, ".epub .txtの本はありません");
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
    if (id == LIB_TRANSFER) {
      return enter_xfer(st);
    }
    if (id == LIB_LEAVE) {
      st->quit = 1;
      return 1;
    }
    if (id < LIB_BOOKS || id - LIB_BOOKS >= st->nbooks) {
      return 0;
    }
    open_book(st, st->books[id - LIB_BOOKS].path);
    return 1;
  }
  if (ev->kind == EV_CONFIRM) {
    if (st->lib_cursor == LIB_TRANSFER) {
      return enter_xfer(st);
    }
    if (st->lib_cursor == LIB_LEAVE) {
      st->quit = 1;
      return 1;
    }
    if (st->lib_cursor < LIB_BOOKS || st->lib_cursor - LIB_BOOKS >= st->nbooks) {
      return 0;
    }
    open_book(st, st->books[st->lib_cursor - LIB_BOOKS].path);
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
  if (st->rescan) {
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
  return 1;
}
