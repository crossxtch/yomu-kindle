/* Page-number keypad opened from the menu's page line. */

#include "screen.h"

#include "draw.h"

#include <stdio.h>
#include <string.h>

typedef struct JumpGeom {
  int num_scale;
  int num_y;
  int grid_y;
  int cell_w;
  int cell_h;
  int key_scale;
} JumpGeom;

/* The typed page keeps menu size when four key rows still fit. */
static void jump_geom(const Layout *layout, int width, int height, int bar_y, JumpGeom *geom) {
  int top = layout->header_h;
  int bot = bar_y > top ? bar_y : height - layout->footer_h;
  int body = bot - top;
  if (body < 1) {
    body = 1;
  }
  int num_scale = menu_text_scale(layout);
  int gap = layout->pad;
  while (num_scale > 1 && 8 * num_scale + gap + 40 > body) {
    num_scale--;
  }
  int num_h = 8 * num_scale;
  if (num_h + gap > body - 4) {
    gap = body - 4 - num_h;
    if (gap < 0) {
      gap = 0;
    }
  }
  int grid = body - num_h - gap;
  if (grid < 4) {
    grid = 4;
  }
  int cell_h = grid / 4;
  if (cell_h < 1) {
    cell_h = 1;
  }
  int cell_w = width / 3;
  if (cell_w < 1) {
    cell_w = 1;
  }
  int key_scale = 1;
  int limit = menu_text_scale(layout);
  for (int s = 1; s <= limit; s++) {
    if (8 * s + 4 > cell_h) {
      break;
    }
    int wide = draw_text_width(s, "移動");
    int wipe = draw_text_width(s, "消す");
    if (wipe > wide) {
      wide = wipe;
    }
    if (wide + 4 > cell_w) {
      break;
    }
    key_scale = s;
  }
  geom->num_scale = num_scale;
  geom->num_y = top;
  geom->grid_y = top + num_h + gap;
  geom->cell_w = cell_w;
  geom->cell_h = cell_h;
  geom->key_scale = key_scale;
}

static int jump_push(State *st, int digit) {
  if (digit < 0 || digit > 9 || st->jump_len >= 5) {
    return 0;
  }
  int next = st->jump_num * 10 + digit;
  if (next > 65535) {
    return 0;
  }
  st->jump_num = next;
  st->jump_len++;
  return 1;
}

static int jump_backspace(State *st) {
  if (st->jump_len <= 0) {
    return 0;
  }
  st->jump_len--;
  st->jump_num /= 10;
  return 1;
}

static int jump_go(State *st) {
  if (!reading_open(st) || st->jump_len <= 0 || st->jump_num < 1) {
    return 0;
  }
  int pages = reading_pages(st);
  if (pages <= 0) {
    return 0;
  }
  int page = st->jump_num;
  if (page > pages) {
    page = pages;
  }
  st->page = page - 1;
  st->since_full = 0;
  st->screen = SCR_READ;
  reading_save(st);
  return 1;
}

void paint_jump(State *st) {
  AppHost *host = st->host;
  Layout layout = layout_of(host->w, host->h);
  st->nhots = 0;
  draw_clear(host->gray, host->w, host->h, host->w, 255);
  paint_header(st, &layout, "ページ", "");
  Chrome chrome;
  list_chrome(&layout, host->h, 0, 0, 1, &chrome);
  JumpGeom geom;
  jump_geom(&layout, host->w, host->h, chrome.bar_y, &geom);
  char num[16];
  if (st->jump_len <= 0) {
    snprintf(num, sizeof num, "—");
  } else {
    snprintf(num, sizeof num, "%d", st->jump_num);
  }
  int tw = draw_text_width(geom.num_scale, num);
  int tx = (host->w - tw) / 2;
  if (tx < layout.pad) {
    tx = layout.pad;
  }
  draw_text(host->gray, host->w, host->h, host->w, tx, geom.num_y, geom.num_scale, num, 0);
  static const char *labels[4][3] = {
      {"1", "2", "3"},
      {"4", "5", "6"},
      {"7", "8", "9"},
      {"消す", "0", "移動"},
  };
  static const int ids[4][3] = {
      {1, 2, 3},
      {4, 5, 6},
      {7, 8, 9},
      {HOT_JUMP_BS, 0, HOT_JUMP_GO},
  };
  for (int row = 0; row < 4; row++) {
    int y = geom.grid_y + row * geom.cell_h;
    int x = 0;
    for (int col = 0; col < 3; col++) {
      int w = (col == 2) ? host->w - x : geom.cell_w;
      paint_key(st, x, y, w, geom.cell_h, geom.key_scale, labels[row][col], ids[row][col]);
      x += geom.cell_w;
    }
  }
  paint_bar(st, &layout, &chrome);
  char hint[40];
  hint[0] = '\0';
  if (reading_open(st)) {
    int pages = reading_pages(st);
    if (pages > 0) {
      snprintf(hint, sizeof hint, "1-%uページ", (unsigned)pages);
    }
  }
  paint_footer(st, &layout, hint);
}

/* hit_hot reports a miss as -1, which is also HOT_BACK. Only the bar counts. */
static int jump_tapped_back(const State *st, int y) {
  Layout layout = layout_of(st->host->w, st->host->h);
  Chrome chrome;
  list_chrome(&layout, st->host->h, 0, 0, 1, &chrome);
  return chrome.bar && y >= chrome.bar_y && y < chrome.bar_y + chrome.bar_h;
}

int handle_jump(State *st, const AppEvent *ev) {
  if (ev->kind == EV_TAP) {
    int id = hit_hot(st, ev->x, ev->y);
    if (id == HOT_JUMP_BS) {
      return jump_backspace(st);
    }
    if (id == HOT_JUMP_GO) {
      return jump_go(st);
    }
    if (id >= 0 && id <= 9) {
      return jump_push(st, id);
    }
    if (id == HOT_BACK && jump_tapped_back(st, ev->y)) {
      st->screen = SCR_MENU;
      return 1;
    }
    return 0;
  }
  if (ev->kind == EV_CONFIRM) {
    return jump_go(st);
  }
  if (ev->kind == EV_BACK) {
    st->screen = SCR_MENU;
    return 1;
  }
  return 0;
}
