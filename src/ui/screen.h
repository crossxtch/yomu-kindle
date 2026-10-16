#ifndef YOMU_SCREEN_H
#define YOMU_SCREEN_H

/* Screens behind app_run. The host and Kindle mains include app.h only. */

#include "app.h"
#include "fileserver.h"
#include "tsbook.h"

#include <stdint.h>

#define HOT_MAX 48

typedef struct Hot {
  int x, y, w, h, id;
} Hot;

typedef struct Layout {
  int scale;
  int pad;
  int header_h;
  int footer_h;
  int row_h;
  int rows;
} Layout;

typedef struct Chrome {
  int rows;
  int bar;
  int bar_y;
  int bar_h;
  int show_back;
  int show_up;
  int show_down;
} Chrome;

enum { SCR_LIB = 0, SCR_READ, SCR_MENU, SCR_CHAP, SCR_XFER, SCR_JUMP, SCR_TYPE };
enum { ACT_RESUME = 1, ACT_CHAPTERS, ACT_LIBRARY, ACT_TYPE, ACT_QUIT, ACT_JUMP };
enum { LIB_TRANSFER = 0, LIB_LEAVE = 1, LIB_BOOKS = 2 };
enum { HOT_BACK = -1, HOT_UP = -2, HOT_DOWN = -3, HOT_JUMP_GO = -4, HOT_JUMP_BS = -5, HOT_SMALL = -6, HOT_BIG = -7 };

typedef struct State {
  AppHost *host;
  YomuBook *books;
  int nbooks;
  int truncated;
  int screen;
  int lib_cursor, lib_top;
  int menu_cursor;
  int chap_cursor, chap_top;
  int type_cursor, type_top;
  TsBook *ts;
  char book_path[1024];
  int page;
  int jump_num; /* digits typed on the page keypad; 0 with jump_len 0 is empty */
  int jump_len;
  int since_full;
  int quit;
  char note[160];
  int rescan;
  FileServer *fs;
  Hot hots[HOT_MAX];
  int nhots;
  /* Decoded pages kept across turns. ahead is the following page. */
  uint8_t *cur_gray;
  uint8_t *ahead_gray;
  int cur_page;
  int ahead_page;
  uint16_t cur_w, cur_h, ahead_w, ahead_h;
  /* Opening-screen progress. index_shown is 1 after 開いています… is up.
   * index_done and index_total are the last spine pair painted, or -1.
   * index_stall stops a step that made no progress from spinning.
   * index_badge is 1 when the reader frame is carrying the spine line. */
  int index_shown;
  int index_done;
  int index_total;
  int index_stall;
  int index_badge;
} State;

Layout layout_of(int width, int height);
void move_cursor(int *cursor, int *top, int count, int window, int delta);
int list_delta(AppEvKind kind, int window);
void list_chrome(const Layout *layout, int height, int total, int top, int need_back, Chrome *out);
void fit_list(const Layout *layout, int height, int total, int *cursor, int *top, int need_back, Chrome *out);
void page_list(int *cursor, int *top, int total, int window, int dir);
int row_fits(const Chrome *chrome, const Layout *layout, int height, int y);
int hit_hot(const State *st, int x, int y);
void add_hot(State *st, int x, int y, int w, int h, int id);
void paint_button(State *st, const Layout *layout, int x, int y, int w, int h, const char *label, int id);
void paint_key(State *st, int x, int y, int w, int h, int scale, const char *label, int id);
void paint_bar(State *st, const Layout *layout, const Chrome *chrome);
void paint_header(State *st, const Layout *layout, const char *title, const char *right);
void paint_footer(State *st, const Layout *layout, const char *hint);
int menu_text_scale(const Layout *layout);

void paint_library(State *st);
int handle_library(State *st, const AppEvent *ev);
int back_to_library(State *st);

int open_book(State *st, const char *path);
void close_book(State *st);
int reading_open(const State *st);
int reading_pages(const State *st);
int reading_rtl(const State *st);
int reading_chapter_count(const State *st);
/* name may be NULL. start and end are 0-based and inclusive.
 * start < 0: not typeset yet. end < 0: the end page is not known yet. */
int reading_chapter(const State *st, int index, char *name, size_t name_n, int *start, int *end);
/* Typeset up to this chapter. page is 0-based. 1 on success. */
int reading_open_chapter(State *st, int index, int *page);
void reading_drop_pages(State *st);
void prefetch_next_page(State *st);
void paint_reader(State *st, int *full, int *rx, int *ry, int *rw, int *rh);
/* Status while the saved page is still being reached. done/total are spine items.
 * The first call fills the screen. Later calls with a new count redraw the line. */
void paint_opening(State *st, int done, int total);
/* Draw or clear the spine line on the current reader frame and partial-refresh it.
 * 0 means the page cache cannot restore the line, so the caller paints the page. */
int present_spine_badge(State *st);
void blit_page(State *st);
int handle_reader(State *st, const AppEvent *ev);

void paint_menu(State *st);
int handle_menu(State *st, const AppEvent *ev);
void paint_chapters(State *st);
int handle_chapters(State *st, const AppEvent *ev);
void paint_jump(State *st);
int handle_jump(State *st, const AppEvent *ev);

void enter_type(State *st);
void paint_type(State *st);
int handle_type(State *st, const AppEvent *ev);

int enter_xfer(State *st);
void paint_xfer(State *st);
int handle_xfer(State *st, const AppEvent *ev);

#endif
