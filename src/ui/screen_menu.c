/* Page menu over the open book. */

#include "screen.h"

#include "draw.h"

#include <stdio.h>
#include <string.h>

enum { MENU_CAP = 8 };

static int menu_items(const State *st, int *ids, const char **labels, int cap) {
  int n = 0;
  if (n < cap) {
    ids[n] = ACT_RESUME;
    labels[n++] = "再開";
  }
  if (reading_chapter_count(st) > 0 && n < cap) {
    ids[n] = ACT_CHAPTERS;
    labels[n++] = "目次";
  }
  if (n < cap) {
    ids[n] = ACT_LIBRARY;
    labels[n++] = "本棚";
  }
  if (n < cap) {
    ids[n] = ACT_TYPE;
    labels[n++] = "文字";
  }
  if (n < cap) {
    ids[n] = ACT_QUIT;
    labels[n++] = "終了";
  }
  return n;
}

static int menu_row_h(const Layout *layout) { return 8 * menu_text_scale(layout) + layout->pad; }

static int menu_pos_scale(const Layout *layout) {
  int scale = menu_text_scale(layout) / 2;
  return scale < 1 ? 1 : scale;
}

/* 1 when dst holds "current / total ページ". Pages on screen are 1-based. */
static int menu_pos(const State *st, char *dst, size_t n) {
  if (!dst || n == 0) {
    return 0;
  }
  dst[0] = '\0';
  if (!reading_open(st)) {
    return 0;
  }
  int pages = reading_pages(st);
  if (pages <= 0) {
    return 0;
  }
  snprintf(dst, n, "%d / %u ページ", st->page + 1, (unsigned)pages);
  return 1;
}

static int menu_panel_h(const State *st) {
  Layout layout = layout_of(st->host->w, st->host->h);
  int ids[MENU_CAP];
  const char *labels[MENU_CAP];
  int n = menu_items(st, ids, labels, MENU_CAP);
  char pos[64];
  int pos_h = menu_pos(st, pos, sizeof pos) ? 8 * menu_pos_scale(&layout) : 0;
  int panel_h = layout.pad + pos_h + (n > 0 ? n : 1) * menu_row_h(&layout) + layout.pad;
  if (panel_h > st->host->h) {
    panel_h = st->host->h;
  }
  return panel_h;
}

void paint_menu(State *st) {
  AppHost *host = st->host;
  Layout layout = layout_of(host->w, host->h);
  st->nhots = 0;
  blit_page(st);
  int ids[MENU_CAP];
  const char *labels[MENU_CAP];
  int n = menu_items(st, ids, labels, MENU_CAP);
  if (n <= 0) {
    return;
  }
  if (st->menu_cursor >= n) {
    st->menu_cursor = n - 1;
  }
  if (st->menu_cursor < 0) {
    st->menu_cursor = 0;
  }
  int row_h = menu_row_h(&layout);
  int panel_h = menu_panel_h(st);
  for (int y = panel_h; y < host->h; y++) {
    uint8_t *row = host->gray + (size_t)y * (size_t)host->w;
    for (int x = 0; x < host->w; x++) {
      row[x] = (uint8_t)(row[x] / 2 + 128);
    }
  }
  draw_fill(host->gray, host->w, host->h, host->w, 0, 0, host->w, panel_h, 255);
  if (panel_h >= 2 && panel_h < host->h) {
    draw_fill(host->gray, host->w, host->h, host->w, 0, panel_h - 2, host->w, 2, 0);
  }
  int text_scale = menu_text_scale(&layout);
  int text_h = 8 * text_scale;
  int y = layout.pad;
  char pos[64];
  if (menu_pos(st, pos, sizeof pos)) {
    int pos_h = 8 * menu_pos_scale(&layout);
    draw_text(host->gray, host->w, host->h, host->w, layout.pad * 2, y, menu_pos_scale(&layout), pos, 0);
    /* The band is the existing page line, including the pad above it.
     * Rows start at the bottom edge, so this hot does not cover 再開. */
    add_hot(st, 0, 0, host->w, y + pos_h, ACT_JUMP);
    y += pos_h;
  }
  for (int i = 0; i < n; i++) {
    if (y + row_h > panel_h) {
      break;
    }
    int text_y = y + (row_h - text_h) / 2;
    if (i == st->menu_cursor) {
      draw_fill(host->gray, host->w, host->h, host->w, layout.pad, y, host->w - layout.pad * 2, row_h, 0);
      draw_text(host->gray, host->w, host->h, host->w, layout.pad * 2, text_y, text_scale, labels[i], 255);
    } else {
      draw_text(host->gray, host->w, host->h, host->w, layout.pad * 2, text_y, text_scale, labels[i], 0);
    }
    add_hot(st, 0, y, host->w, row_h, ids[i]);
    y += row_h;
  }
}

static int activate_menu(State *st, int id) {
  switch (id) {
    case ACT_RESUME:
      st->screen = SCR_READ;
      st->since_full = 0;
      return 1;
    case ACT_CHAPTERS:
      st->screen = SCR_CHAP;
      st->chap_cursor = 0;
      st->chap_top = 0;
      return 1;
    case ACT_LIBRARY:
      close_book(st);
      st->screen = SCR_LIB;
      st->note[0] = '\0';
      return 1;
    case ACT_TYPE:
      enter_type(st);
      return 1;
    case ACT_QUIT:
      st->quit = 1;
      return 1;
    case ACT_JUMP:
      st->jump_num = 0;
      st->jump_len = 0;
      st->screen = SCR_JUMP;
      return 1;
    default:
      return 0;
  }
}

int handle_menu(State *st, const AppEvent *ev) {
  int ids[MENU_CAP];
  const char *labels[MENU_CAP];
  int n = menu_items(st, ids, labels, MENU_CAP);
  if (ev->kind == EV_TAP) {
    int id = hit_hot(st, ev->x, ev->y);
    if (id >= 0) {
      return activate_menu(st, id);
    }
    if (ev->y >= menu_panel_h(st)) {
      st->screen = SCR_READ;
      st->since_full = 0;
      return 1;
    }
    return 0;
  }
  if (ev->kind == EV_CONFIRM) {
    if (n <= 0) {
      return 0;
    }
    return activate_menu(st, ids[st->menu_cursor]);
  }
  if (ev->kind == EV_BACK) {
    st->screen = SCR_READ;
    st->since_full = 0;
    return 1;
  }
  int delta = list_delta(ev->kind, n > 0 ? n : 1);
  if (delta == 0) {
    return 0;
  }
  int top = 0;
  move_cursor(&st->menu_cursor, &top, n, n > 0 ? n : 1, delta);
  return 1;
}
