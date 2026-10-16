/* Transfer screen. fileserver.c is the HTTP server behind it. */

#include "screen.h"

#include "draw.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int parse_port(const char *s, int *out) {
  if (!s || !s[0]) {
    return -1;
  }
  int v = 0;
  for (const char *p = s; *p; p++) {
    if (*p < '0' || *p > '9') {
      return -1;
    }
    v = v * 10 + (*p - '0');
    if (v > 65535) {
      return -1;
    }
  }
  if (v < 1) {
    return -1;
  }
  *out = v;
  return 0;
}

/* YOMU_PORT, when set, is the only port. Otherwise try 80, then 8080. */
int enter_xfer(State *st) {
  if (st->fs) {
    fileserver_stop(st->fs);
    st->fs = NULL;
  }
  st->screen = SCR_XFER;
  st->note[0] = '\0';
  const char *root = st->host->root;
  if (!root || !root[0]) {
    return 1;
  }
  int ports[2];
  int nports = 0;
  const char *env = getenv("YOMU_PORT");
  if (env && env[0]) {
    int port = 0;
    if (parse_port(env, &port) != 0) {
      return 1;
    }
    ports[nports++] = port;
  } else {
    ports[nports++] = 80;
    ports[nports++] = 8080;
  }
  for (int i = 0; i < nports; i++) {
    st->fs = fileserver_start(root, ports[i]);
    if (st->fs) {
      break;
    }
  }
  return 1;
}

void paint_xfer(State *st) {
  AppHost *host = st->host;
  Layout layout = layout_of(host->w, host->h);
  st->nhots = 0;
  draw_clear(host->gray, host->w, host->h, host->w, 255);
  paint_header(st, &layout, "転送", "");
  int y = layout.header_h + layout.pad;
  int step = 8 * layout.scale + layout.pad / 2;
  if (!st->fs) {
    draw_text(host->gray, host->w, host->h, host->w, layout.pad, y, layout.scale, "サーバを開始できません", 0);
  } else {
    char urls[2][80];
    int n = fileserver_urls(st->fs, urls, 2);
    if (n <= 0) {
      const char *lines[3];
      lines[0] = "Wi-Fiアドレスがありません";
      lines[1] = "Wi-Fiに接続してから";
      lines[2] = "この画面を開き直してください";
      for (int i = 0; i < 3; i++) {
        draw_text(host->gray, host->w, host->h, host->w, layout.pad, y, layout.scale, lines[i], 0);
        y += step;
      }
    } else {
      for (int i = 0; i < n; i++) {
        draw_text(host->gray, host->w, host->h, host->w, layout.pad, y, layout.scale, urls[i], 0);
        y += step;
      }
      draw_text(host->gray, host->w, host->h, host->w, layout.pad, y, layout.scale, "同じWi-Fiのブラウザで", 0);
      y += step;
      draw_text(host->gray, host->w, host->h, host->w, layout.pad, y, layout.scale, "このアドレスを開いてください", 0);
    }
  }
  Chrome chrome;
  list_chrome(&layout, host->h, 0, 0, 1, &chrome);
  paint_button(st, &layout, 0, chrome.bar_y, host->w, chrome.bar_h, "停止", HOT_BACK);
  paint_footer(st, &layout, "");
}

static int leave_xfer(State *st) {
  if (st->fs) {
    if (fileserver_stop(st->fs)) {
      st->rescan = 1;
    }
    st->fs = NULL;
  }
  return back_to_library(st);
}

int handle_xfer(State *st, const AppEvent *ev) {
  if (ev->kind == EV_TAP) {
    if (hit_hot(st, ev->x, ev->y) == HOT_BACK) {
      return leave_xfer(st);
    }
    return 0;
  }
  if (ev->kind == EV_BACK) {
    return leave_xfer(st);
  }
  if (ev->kind == EV_QUIT) {
    st->quit = 1;
    return 1;
  }
  return 0;
}
