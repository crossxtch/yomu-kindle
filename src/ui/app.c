/* Event loop. Each screen lives in its own file. */

#include "screen.h"

#include "clockpref.h"
#include "draw.h"
#include "font.h"
#include "log.h"
#include "typeface.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *screen_name(int screen) {
  switch (screen) {
    case SCR_LIB:
      return "library";
    case SCR_READ:
      return "read";
    case SCR_MENU:
      return "menu";
    case SCR_CHAP:
      return "chapters";
    case SCR_XFER:
      return "transfer";
    case SCR_JUMP:
      return "jump";
    case SCR_TYPE:
      return "type";
    default:
      return "library";
  }
}

/* paint is the CPU work before the panel. present is the framebuffer copy plus the refresh. */
static void log_frame(const State *st, const char *screen, int full, int64_t paint_ms, int64_t present_ms) {
  int hits = 0;
  int misses = 0;
  yomu_font_take_stats(&hits, &misses);
  if (st->ts && strcmp(screen, "read") == 0) {
    unsigned pages = (unsigned)ts_pages(st->ts);
    yomu_log("perf frame read %d/%u full %d every %d paint %lld present %lld glyphs %d miss %d", st->page + 1,
               pages, full, st->host->full_every, (long long)paint_ms, (long long)present_ms, hits + misses,
               misses);
    return;
  }
  yomu_log("perf frame %s full %d paint %lld present %lld glyphs %d miss %d", screen, full, (long long)paint_ms,
             (long long)present_ms, hits + misses, misses);
}

/* KOReader's own sleep screen leaves the current frame and draws a centered
 * message with a full refresh before the panel sleeps. The Kindle screensaver
 * stays out of it, so awesome is not continued. */
static void present_sleep_overlay(State *st) {
  AppHost *host = st->host;
  Layout layout = layout_of(host->w, host->h);
  const char *msg = "スリープ中";
  int scale = menu_text_scale(&layout);
  int gap = layout.pad;
  int border = layout.pad > 2 ? layout.pad : 2;
  int tw = draw_text_width(scale, msg);
  int th = 8 * scale;
  while (scale > 1 && (tw + (gap + border) * 2 > host->w || th + (gap + border) * 2 > host->h)) {
    scale--;
    gap = 4 * scale;
    border = scale > 2 ? scale : 2;
    tw = draw_text_width(scale, msg);
    th = 8 * scale;
  }
  int bw = tw + (gap + border) * 2;
  int bh = th + (gap + border) * 2;
  if (bw > host->w) {
    bw = host->w;
  }
  if (bh > host->h) {
    bh = host->h;
  }
  if (bw < 1) {
    bw = 1;
  }
  if (bh < 1) {
    bh = 1;
  }
  int x = (host->w - bw) / 2;
  int y = (host->h - bh) / 2;
  int64_t paint_at = yomu_ms();
  draw_fill(host->gray, host->w, host->h, host->w, x, y, bw, bh, 0);
  if (bw > border * 2 && bh > border * 2) {
    draw_fill(host->gray, host->w, host->h, host->w, x + border, y + border, bw - border * 2, bh - border * 2,
              255);
  }
  int tx = x + (bw - tw) / 2;
  int ty = y + (bh - th) / 2;
  if (tx < x) {
    tx = x;
  }
  if (ty < y) {
    ty = y;
  }
  draw_text(host->gray, host->w, host->h, host->w, tx, ty, scale, msg, 0);
  int64_t present_at = yomu_ms();
  host->present(host, 0, 0, 0, 0, 1);
  log_frame(st, "sleep", 1, present_at - paint_at, yomu_ms() - present_at);
  yomu_log("sleep overlay");
}

static int paint(State *st, int *full, int *rx, int *ry, int *rw, int *rh) {
  *full = 1;
  *rx = *ry = *rw = *rh = 0;
  switch (st->screen) {
    case SCR_READ:
      if (reading_open(st)) {
        paint_reader(st, full, rx, ry, rw, rh);
      } else {
        st->screen = SCR_LIB;
        paint_library(st);
      }
      break;
    case SCR_MENU:
      paint_menu(st);
      break;
    case SCR_CHAP:
      paint_chapters(st);
      break;
    case SCR_XFER:
      paint_xfer(st);
      break;
    case SCR_JUMP:
      paint_jump(st);
      break;
    case SCR_TYPE:
      paint_type(st);
      break;
    default:
      paint_library(st);
      break;
  }
  return 0;
}

int app_run(AppHost *host, const char *initial_path) {
  if (!host || !host->gray || !host->present || !host->poll || host->w < 16 || host->h < 16) {
    return 1;
  }
  yomu_font_open();
  type_load(host->home);
  clock_offset_load(host->home);
  State *st = calloc(1, sizeof(*st));
  if (!st) {
    return 1;
  }
  st->host = host;
  Layout layout = layout_of(host->w, host->h);
  int64_t paint_at = yomu_ms();
  draw_clear(host->gray, host->w, host->h, host->w, 255);
  draw_text(host->gray, host->w, host->h, host->w, layout.pad, layout.pad, layout.scale, "探しています…", 0);
  if (host->root) {
    draw_text(host->gray, host->w, host->h, host->w, layout.pad, layout.pad + 8 * layout.scale + layout.pad,
              layout.scale, host->root, 0);
  }
  int64_t present_at = yomu_ms();
  host->present(host, 0, 0, 0, 0, 1);
  log_frame(st, "splash", 1, present_at - paint_at, yomu_ms() - present_at);

  int trunc = 0;
  int64_t scan_at = yomu_ms();
  if (yomu_scan_books(host->root, &st->books, &st->nbooks, &trunc) != 0) {
    snprintf(st->note, sizeof st->note, "本のフォルダを開けません");
    yomu_log("scan failed for %s in %lld ms", host->root ? host->root : "", (long long)(yomu_ms() - scan_at));
  } else {
    st->truncated = trunc;
    yomu_log("library %d book%s%s in %lld ms", st->nbooks, st->nbooks == 1 ? "" : "s",
               trunc ? " (truncated)" : "", (long long)(yomu_ms() - scan_at));
  }
  if (initial_path && initial_path[0]) {
    if (open_book(st, initial_path) != 0) {
      st->screen = SCR_LIB;
    }
  }

  int need_paint = 1;
  while (!st->quit) {
    if (need_paint) {
      int full = 1, rx = 0, ry = 0, rw = 0, rh = 0;
      int64_t paint_at = yomu_ms();
      paint(st, &full, &rx, &ry, &rw, &rh);
      int64_t present_at = yomu_ms();
      host->present(host, rx, ry, rw, rh, full);
      log_frame(st, screen_name(st->screen), full, present_at - paint_at, yomu_ms() - present_at);
      if (st->screen == SCR_READ) {
        prefetch_next_page(st);
      }
      need_paint = 0;
    }
    AppEvent ev;
    memset(&ev, 0, sizeof ev);
    /* Indexing steps immediately. The home clock waits until the next minute. */
    host->poll_ms = (st->ts && !st->index_stall && !ts_done(st->ts)) ? 0 : library_clock_wait_ms(st);
    int polled = host->poll(host, &ev);
    if (polled < 0 || ev.kind == EV_QUIT) {
      break;
    }
    if (polled == 0 || ev.kind == EV_NONE) {
      if (st->ts && !st->index_stall && !ts_done(st->ts)) {
        int done = 0;
        int total = 0;
        int done2 = 0;
        int total2 = 0;
        int more;
        ts_spine(st->ts, &done, &total);
        more = ts_index_step(st->ts);
        ts_spine(st->ts, &done2, &total2);
        if (!more) {
          if (ts_done(st->ts)) {
            yomu_log("index finished");
          } else {
            st->index_stall = 1;
          }
          if (st->screen == SCR_READ && st->index_badge) {
            if (!present_spine_badge(st)) {
              need_paint = 1;
            }
          }
        } else if (st->screen == SCR_READ && (done2 != done || total2 != total)) {
          int show = total2 > 0 && done2 < total2 && !ts_done(st->ts);
          if (show || st->index_badge) {
            if (!present_spine_badge(st)) {
              need_paint = 1;
            }
          }
        }
      }
      if (st->screen == SCR_LIB) {
        refresh_library_clock(st);
      }
      continue;
    }
    st->index_stall = 0;
    if (ev.kind == EV_SLEEP) {
      yomu_log("sleep event callback %d", host->sleep ? 1 : 0);
      if (host->sleep) {
        present_sleep_overlay(st);
        host->sleep(host);
      }
      st->since_full = 0;
      need_paint = 1;
      continue;
    }
    if (ev.kind == EV_WAKE) {
      /* Idle, cover, and power sleeps that powerd started itself. */
      yomu_log("screensaver wake");
      st->since_full = 0;
      need_paint = 1;
      continue;
    }
    int dirty = 0;
    switch (st->screen) {
      case SCR_READ:
        dirty = handle_reader(st, &ev);
        break;
      case SCR_MENU:
        dirty = handle_menu(st, &ev);
        break;
      case SCR_CHAP:
        dirty = handle_chapters(st, &ev);
        break;
      case SCR_XFER:
        dirty = handle_xfer(st, &ev);
        break;
      case SCR_JUMP:
        dirty = handle_jump(st, &ev);
        break;
      case SCR_TYPE:
        dirty = handle_type(st, &ev);
        break;
      default:
        dirty = handle_library(st, &ev);
        break;
    }
    if (st->quit) {
      break;
    }
    need_paint = dirty;
  }

  if (st->fs) {
    fileserver_stop(st->fs);
    st->fs = NULL;
  }
  close_book(st);
  yomu_free_books(st->books, st->nbooks);
  free(st);
  return 0;
}
