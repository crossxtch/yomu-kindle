/* Book type size and face. Opened from the reader menu. */

#include "screen.h"

#include "draw.h"
#include "typeface.h"

#include <stdio.h>
#include <string.h>

static TypeFace g_faces[TYPE_FACE_MAX];

static int face_count(const State *st) {
  return type_faces(st->host->home, g_faces, TYPE_FACE_MAX);
}

static int selected_row(int n) {
  const char *cur = type_font();
  int i;
  for (i = 0; i < n; i++) {
    if (strcmp(g_faces[i].path, cur) == 0) {
      return i;
    }
  }
  return 0;
}

static void list_layout(const Layout *layout, Layout *out) {
  *out = *layout;
  out->header_h = layout->header_h + layout->row_h;
}

static void show_working(State *st) {
  AppHost *host = st->host;
  Layout layout = layout_of(host->w, host->h);
  draw_clear(host->gray, host->w, host->h, host->w, 255);
  draw_text(host->gray, host->w, host->h, host->w, layout.pad, layout.pad, layout.scale, "組み直しています…", 0);
  host->present(host, 0, 0, 0, 0, 1);
}

/* The new choice is already saved. On failure, put the previous choice back. */
static int reopen_type(State *st, int old_step, const char *old_font) {
  char path[1024];
  if (!st->book_path[0]) {
    return 1;
  }
  snprintf(path, sizeof path, "%s", st->book_path);
  show_working(st);
  if (open_book(st, path) == 0) {
    st->screen = SCR_READ;
    st->since_full = 0;
    return 1;
  }
  type_set_step(old_step);
  type_set_font(old_font ? old_font : "");
  if (open_book(st, path) == 0) {
    st->screen = SCR_TYPE;
    snprintf(st->note, sizeof st->note, "この書体は使えません");
    return 1;
  }
  st->screen = SCR_LIB;
  return 1;
}

static int change_step(State *st, int delta) {
  int before;
  int old_step;
  char old_font[512];
  if (delta == 0) {
    return 0;
  }
  old_step = type_step();
  snprintf(old_font, sizeof old_font, "%s", type_font());
  before = type_em(st->host->w, st->host->h);
  if (!type_set_step(old_step + delta)) {
    return 0;
  }
  if (type_em(st->host->w, st->host->h) == before) {
    return 1;
  }
  return reopen_type(st, old_step, old_font);
}

static int change_font(State *st, int index, int n) {
  int old_step;
  char old_font[512];
  if (index < 0 || index >= n) {
    return 0;
  }
  old_step = type_step();
  snprintf(old_font, sizeof old_font, "%s", type_font());
  if (!type_set_font(g_faces[index].path)) {
    return 0;
  }
  return reopen_type(st, old_step, old_font);
}

void enter_type(State *st) {
  int n = face_count(st);
  st->note[0] = '\0';
  st->type_cursor = selected_row(n);
  st->type_top = 0;
  st->screen = SCR_TYPE;
}

void paint_type(State *st) {
  AppHost *host = st->host;
  Layout layout = layout_of(host->w, host->h);
  Layout list;
  Chrome chrome;
  char em[16];
  int n;
  int bw;
  int text_h;
  int i;
  st->nhots = 0;
  draw_clear(host->gray, host->w, host->h, host->w, 255);
  snprintf(em, sizeof em, "%d", type_em(host->w, host->h));
  paint_header(st, &layout, "文字", em);
  n = face_count(st);
  list_layout(&layout, &list);
  fit_list(&list, host->h, n, &st->type_cursor, &st->type_top, 1, &chrome);
  bw = host->w / 3;
  if (bw < 1) {
    bw = 1;
  }
  paint_button(st, &layout, 0, layout.header_h, bw, layout.row_h, "小", HOT_SMALL);
  paint_button(st, &layout, bw * 2, layout.header_h, host->w - bw * 2, layout.row_h, "大", HOT_BIG);
  {
    const char *label = type_step_label();
    int tw = draw_text_width(layout.scale, label);
    int tx = bw + (bw - tw) / 2;
    int ty = layout.header_h + (layout.row_h - 8 * layout.scale) / 2;
    if (tx < bw) {
      tx = bw;
    }
    draw_text(host->gray, host->w, host->h, host->w, tx, ty, layout.scale, label, 0);
  }
  text_h = 8 * layout.scale;
  for (i = 0; i < chrome.rows; i++) {
    int index = st->type_top + i;
    int y;
    int text_x;
    int text_y;
    if (index >= n) {
      break;
    }
    y = list.header_h + i * layout.row_h;
    if (!row_fits(&chrome, &list, host->h, y)) {
      break;
    }
    if (index == st->type_cursor) {
      draw_fill(host->gray, host->w, host->h, host->w, 0, y, layout.pad, layout.row_h, 0);
    }
    text_x = layout.pad * 2;
    text_y = y + layout.pad / 2;
    draw_text(host->gray, host->w, host->h, host->w, text_x, text_y, layout.scale, g_faces[index].label, 0);
    if (index == selected_row(n)) {
      draw_text(host->gray, host->w, host->h, host->w, text_x, text_y + text_h + 2, layout.scale, "使用中", 80);
    }
    add_hot(st, 0, y, host->w, layout.row_h, index);
  }
  paint_bar(st, &list, &chrome);
  paint_footer(st, &layout, "書体をタップ");
}

static int on_back_bar(State *st, int y) {
  Layout layout = layout_of(st->host->w, st->host->h);
  Layout list;
  Chrome chrome;
  int n = face_count(st);
  list_layout(&layout, &list);
  fit_list(&list, st->host->h, n, &st->type_cursor, &st->type_top, 1, &chrome);
  return chrome.bar && y >= chrome.bar_y && y < chrome.bar_y + chrome.bar_h;
}

int handle_type(State *st, const AppEvent *ev) {
  Layout layout = layout_of(st->host->w, st->host->h);
  Layout list;
  Chrome chrome;
  int n = face_count(st);
  list_layout(&layout, &list);
  fit_list(&list, st->host->h, n, &st->type_cursor, &st->type_top, 1, &chrome);
  if (ev->kind == EV_TAP) {
    int id = hit_hot(st, ev->x, ev->y);
    if (id == HOT_SMALL) {
      return change_step(st, -1);
    }
    if (id == HOT_BIG) {
      return change_step(st, 1);
    }
    if (id == HOT_BACK) {
      if (!on_back_bar(st, ev->y)) {
        return 0;
      }
      st->screen = SCR_MENU;
      return 1;
    }
    if (id >= 0) {
      return change_font(st, id, n);
    }
    return 0;
  }
  if (ev->kind == EV_CONFIRM) {
    return change_font(st, st->type_cursor, n);
  }
  if (ev->kind == EV_BACK) {
    st->screen = SCR_MENU;
    return 1;
  }
  if (ev->kind == EV_LEFT) {
    return change_step(st, -1);
  }
  if (ev->kind == EV_RIGHT) {
    return change_step(st, 1);
  }
  {
    int delta = list_delta(ev->kind, chrome.rows > 0 ? chrome.rows : 1);
    if (delta == 0) {
      return 0;
    }
    move_cursor(&st->type_cursor, &st->type_top, n, chrome.rows > 0 ? chrome.rows : 1, delta);
    return 1;
  }
}
