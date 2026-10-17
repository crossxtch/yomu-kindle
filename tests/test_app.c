#include "app.h"
#include "books.h"
#include "clockpref.h"
#include "draw.h"
#include "font.h"
#include "progress.h"
#include "rotate.h"
#include "screen.h"
#include "tsbook.h"
#include "typeface.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int g_failed;

void expect(int cond, const char *msg) {
  if (!cond) {
    fprintf(stderr, "FAIL %s\n", msg);
    g_failed++;
  }
}

static void wr16(uint8_t *p, uint16_t v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
}

static void wr32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
  p[2] = (uint8_t)(v >> 16);
  p[3] = (uint8_t)(v >> 24);
}

static const char k_line[] = "吾輩は猫である。名前はまだ無い。どこで生れたかとんと見当がつかぬ。";

static int write_text(const char *path, const char *text) {
  FILE *fp = fopen(path, "wb");
  if (!fp) {
    return -1;
  }
  fputs(text ? text : "", fp);
  fclose(fp);
  return 0;
}

static int write_repeat(const char *path, int n) {
  FILE *fp = fopen(path, "wb");
  int i;
  if (!fp) {
    return -1;
  }
  for (i = 0; i < n; i++) {
    fputs(k_line, fp);
  }
  fclose(fp);
  return 0;
}

/* Repeats a sentence until the typesetter reports between lo and hi pages. */
static int write_fitted(const char *path, const char *home, int w, int h, int lo, int hi, int *pages) {
  int n;
  for (n = 1; n <= 40; n++) {
    TsBook *book;
    int got;
    if (write_repeat(path, n) != 0) {
      return -1;
    }
    book = ts_open(path, home, NULL, w, h);
    if (!book) {
      return -1;
    }
    got = ts_pages(book);
    ts_close(book);
    if (got >= lo && got <= hi) {
      if (pages) {
        *pages = got;
      }
      return 0;
    }
    if (got > hi) {
      return -1;
    }
  }
  return -1;
}

static void test_progress_and_scan(void) {
  char dir[] = "/tmp/yomu-lib-XXXXXX";
  expect(mkdtemp(dir) != NULL, "temp dir");
  char book_path[256];
  char bitmap_path[256];
  char system_path[256];
  char nested[256];
  snprintf(book_path, sizeof book_path, "%s/Demo.txt", dir);
  snprintf(bitmap_path, sizeof bitmap_path, "%s/Old.xtch", dir);
  snprintf(system_path, sizeof system_path, "%s/system", dir);
  snprintf(nested, sizeof nested, "%s/system/Hidden.txt", dir);
  mkdir(system_path, 0755);
  expect(write_text(book_path, k_line) == 0, "scan book");
  expect(write_text(bitmap_path, "not a book") == 0, "ignored bitmap");
  expect(write_text(nested, k_line) == 0, "hidden book");

  YomuBook *books = NULL;
  int count = 0, trunc = 0;
  expect(yomu_scan_books(dir, &books, &count, &trunc) == 0, "scan");
  expect(count == 1, "skips system/ and .xtch");
  expect(books && strcmp(books[0].label, "Demo") == 0, "ascii label");
  expect(books && strstr(books[0].path, ".xtch") == NULL, "bitmap is not listed");
  yomu_free_books(books, count);

  expect(progress_save(dir, book_path, 0) == 0, "save progress");
  int page = -1;
  expect(progress_load(dir, book_path, &page) == 0 && page == 0, "load progress");
  expect(progress_save(dir, book_path, 4) == 0, "update progress");
  expect(progress_load(dir, book_path, &page) == 0 && page == 4, "updated page");

  remove(book_path);
  remove(bitmap_path);
  remove(nested);
  remove(system_path);
  char prog[256];
  snprintf(prog, sizeof prog, "%s/progress.tsv", dir);
  remove(prog);
  rmdir(dir);
}

static int ink_count(const uint8_t *p, int n) {
  int c = 0;
  int i;
  for (i = 0; i < n; i++) {
    if (p[i] < 250) {
      c++;
    }
  }
  return c;
}

static void ink_box(const uint8_t *p, int w, int h, int *bw, int *bh) {
  int min_x = w;
  int min_y = h;
  int max_x = -1;
  int max_y = -1;
  int y;
  int x;
  for (y = 0; y < h; y++) {
    for (x = 0; x < w; x++) {
      if (p[(size_t)y * (size_t)w + (size_t)x] >= 250) {
        continue;
      }
      if (x < min_x) {
        min_x = x;
      }
      if (y < min_y) {
        min_y = y;
      }
      if (x > max_x) {
        max_x = x;
      }
      if (y > max_y) {
        max_y = y;
      }
    }
  }
  *bw = max_x >= min_x ? max_x - min_x + 1 : 0;
  *bh = max_y >= min_y ? max_y - min_y + 1 : 0;
}

static void test_japanese(void) {
  char dir[] = "/tmp/yomu-jp-XXXXXX";
  const char *kana = "\xe3\x81\x82";
  const char *magic = "\xe9\xad\x94\xe6\xb3\x95";
  const char *chapter_name = "\xe7\xac\xac\xe4\xb8\x80\xe7\xab\xa0";
  char book_path[256];
  char punct_path[256];
  YomuBook *books = NULL;
  int count = 0, trunc = 0;
  int found = 0;
  int i;
  const int w = 64;
  const int h = 48;
  uint8_t *glyph;
  uint8_t *mark;

  expect(mkdtemp(dir) != NULL, "jp dir");
  expect(yomu_text_useful("Demo"), "ascii title");
  expect(!yomu_text_useful("!"), "punctuation is not a title");
  expect(yomu_text_useful(kana), "kana title");
  expect(yomu_text_useful(chapter_name), "kanji chapter");
  expect(draw_text_width(2, "AB") == 32, "ascii width");

  snprintf(book_path, sizeof book_path, "%s/%s.txt", dir, magic);
  snprintf(punct_path, sizeof punct_path, "%s/!!.txt", dir);
  expect(write_text(book_path, k_line) == 0, "jp book");
  expect(write_text(punct_path, "!") == 0, "punct book");

  expect(yomu_scan_books(dir, &books, &count, &trunc) == 0, "jp scan");
  expect(count == 2, "jp and punctuation");
  for (i = 0; books && i < count; i++) {
    if (strcmp(books[i].label, magic) == 0) {
      found = 1;
    }
    if (strstr(books[i].path, "!!.txt")) {
      char caption[32];
      expect(books[i].label[0] == '\0', "punctuation has no label");
      yomu_book_caption(&books[i], i, caption, sizeof caption);
      expect(strcmp(caption, "本1") == 0 || strcmp(caption, "本2") == 0, "punctuation is 本N");
    }
  }
  expect(found, "japanese filename is the label");
  yomu_free_books(books, count);

  expect(yomu_font_open() == 1, "japanese font");
  glyph = malloc((size_t)w * (size_t)h);
  mark = malloc((size_t)w * (size_t)h);
  expect(glyph && mark, "glyph buffers");
  if (glyph && mark) {
    memset(glyph, 255, (size_t)w * (size_t)h);
    memset(mark, 255, (size_t)w * (size_t)h);
    draw_text(mark, w, h, w, 4, 8, 2, "?", 0);
    draw_text(glyph, w, h, w, 4, 8, 2, kana, 0);
    expect(ink_count(glyph, w * h) > 8, "hiragana has ink");
    expect(memcmp(glyph, mark, (size_t)w * (size_t)h) != 0, "hiragana is not a question mark");
    memset(glyph, 255, (size_t)w * (size_t)h);
    draw_text(glyph, w, h, w, 4, 8, 2, "\xe6\x9c\xac", 0);
    expect(ink_count(glyph, w * h) > 8, "kanji has ink");
    expect(memcmp(glyph, mark, (size_t)w * (size_t)h) != 0, "kanji is not a question mark");
    expect(draw_text_width(2, kana) >= 8, "kana advances");
    memset(glyph, 255, (size_t)w * (size_t)h);
    draw_text(glyph, w, h, w, 4, 8, 2, chapter_name, 0);
    expect(ink_count(glyph, w * h) > 20, "chapter name has ink");
    expect(memcmp(glyph, mark, (size_t)w * (size_t)h) != 0, "chapter name is not a question mark");
  }
  free(glyph);
  free(mark);

  remove(book_path);
  remove(punct_path);
  rmdir(dir);
}


static int g_reader_step;
static const char *g_reader_path;
static uint8_t *g_page0;
static uint8_t *g_page1;
static size_t g_page_n;
static int g_jump_pages;

static int page_match(const uint8_t *saved, const AppHost *host) {
  return saved && memcmp(saved, host->gray, g_page_n) == 0;
}

static int g_presents;

static void fake_present(AppHost *host, int x, int y, int w, int h, int full) {
  (void)host;
  (void)x;
  (void)y;
  (void)w;
  (void)h;
  (void)full;
  g_presents++;
}

static int fake_poll(AppHost *host, AppEvent *event) {
  (void)host;
  event->kind = EV_QUIT;
  return -1;
}

static int write_chapter_epub(const char *path);

static int reader_poll(AppHost *host, AppEvent *event) {
  memset(event, 0, sizeof *event);
  int page = -1;
  int have = progress_load(host->home, g_reader_path, &page) == 0;
  switch (g_reader_step++) {
    case 0:
      expect(ink_count(host->gray, (int)g_page_n) > 0, "reader opens on a page with ink");
      memcpy(g_page0, host->gray, g_page_n);
      event->kind = EV_TAP;
      return 1;
    case 1:
      expect(!page_match(g_page0, host), "tap shows the next page");
      expect(have && page == 1, "tap saves the next page");
      memcpy(g_page1, host->gray, g_page_n);
      event->kind = EV_SWIPE_LEFT;
      event->y0 = host->h / 2;
      return 1;
    case 2:
      expect(page_match(g_page1, host), "side swipe stays put");
      event->kind = EV_SWIPE_DOWN;
      event->y0 = host->h / 2;
      return 1;
    case 3:
      expect(page_match(g_page1, host), "middle swipe stays put");
      event->kind = EV_SWIPE_UP;
      event->y0 = 0;
      return 1;
    case 4:
      expect(page_match(g_page1, host), "upward swipe from the top stays put");
      event->kind = EV_SWIPE_UP;
      event->y0 = host->h - 1;
      return 1;
    case 5:
      expect(page_match(g_page0, host), "bottom swipe shows the previous page");
      expect(have && page == 0, "bottom swipe saves the previous page");
      event->kind = EV_TAP;
      return 1;
    case 6:
      expect(page_match(g_page1, host) && have && page == 1, "second tap goes forward again");
      event->kind = EV_SWIPE_DOWN;
      event->y0 = 0;
      return 1;
    case 7:
      expect(have && page == 1, "menu keeps the current page");
      expect(!page_match(g_page1, host), "menu covers the page");
      event->kind = EV_BACK;
      return 1;
    case 8:
      expect(page_match(g_page1, host), "back from the menu returns to the page");
      expect(have && page == 1, "menu did not turn the page");
      event->kind = EV_SWIPE_DOWN;
      event->y0 = 0;
      return 1;
    case 9:
      event->kind = EV_DOWN;
      return 1;
    case 10:
      event->kind = EV_DOWN;
      return 1;
    case 11:
      event->kind = EV_DOWN;
      return 1;
    case 12:
      event->kind = EV_CONFIRM;
      return 1;
    default:
      expect(0, "leave did not exit");
      event->kind = EV_QUIT;
      return -1;
  }
}

static void test_reader_gestures(void) {
  char dir[] = "/tmp/yomu-gest-XXXXXX";
  expect(mkdtemp(dir) != NULL, "gesture dir");
  char book_path[256];
  const int width = 120;
  const int height = 80;
  snprintf(book_path, sizeof book_path, "%s/Pages.txt", dir);
  expect(write_fitted(book_path, dir, width, height, 2, 8, NULL) == 0, "two page book");
  g_page_n = (size_t)width * (size_t)height;
  uint8_t *gray = malloc(g_page_n);
  g_page0 = malloc(g_page_n);
  g_page1 = malloc(g_page_n);
  expect(gray && g_page0 && g_page1, "gesture gray");
  if (!gray || !g_page0 || !g_page1) {
    free(gray);
    free(g_page0);
    free(g_page1);
    return;
  }
  memset(gray, 255, g_page_n);
  AppHost host;
  memset(&host, 0, sizeof host);
  host.w = width;
  host.h = height;
  host.gray = gray;
  host.full_every = 8;
  host.root = dir;
  host.home = dir;
  host.present = fake_present;
  host.poll = reader_poll;
  g_reader_step = 0;
  g_reader_path = book_path;
  expect(app_run(&host, book_path) == 0, "gesture run");
  expect(g_reader_step == 13, "leave row exits");
  free(gray);
  free(g_page0);
  free(g_page1);
  g_page0 = NULL;
  g_page1 = NULL;
  remove(book_path);
  char prog[256];
  snprintf(prog, sizeof prog, "%s/progress.tsv", dir);
  remove(prog);
  rmdir(dir);
}

static int g_sleeps;
static int g_sleep_step;
static int g_sleep_overlay;

static void fake_sleep(AppHost *host) {
  g_sleeps++;
  /* The sleep box is framed in black and is up before the device is asked to sleep. */
  g_sleep_overlay = host->gray && host->w > 0 && host->gray[0] == 0;
}

static int sleep_poll(AppHost *host, AppEvent *event) {
  (void)host;
  memset(event, 0, sizeof *event);
  switch (g_sleep_step++) {
    case 0:
      event->kind = EV_SLEEP;
      return 1;
    default:
      event->kind = EV_QUIT;
      return -1;
  }
}

static void test_sleep_stays(void) {
  char dir[] = "/tmp/yomu-sleep-XXXXXX";
  expect(mkdtemp(dir) != NULL, "sleep dir");
  uint8_t *gray = malloc(16 * 16);
  expect(gray != NULL, "sleep gray");
  if (!gray) {
    return;
  }
  memset(gray, 255, 16 * 16);
  AppHost host;
  memset(&host, 0, sizeof host);
  host.w = 16;
  host.h = 16;
  host.gray = gray;
  host.full_every = 8;
  host.root = dir;
  host.home = dir;
  host.present = fake_present;
  host.poll = sleep_poll;
  host.sleep = fake_sleep;
  g_sleeps = 0;
  g_sleep_step = 0;
  g_sleep_overlay = 0;
  expect(app_run(&host, NULL) == 0, "sleep does not exit");
  expect(g_sleeps == 1, "sleep callback ran");
  expect(g_sleep_overlay == 1, "sleep overlay is up");
  expect(g_sleep_step == 2, "sleep returned to the loop");
  host.sleep = NULL;
  g_sleep_step = 0;
  expect(app_run(&host, NULL) == 0, "missing sleep callback stays up");
  expect(g_sleep_step == 2, "null sleep still continues");
  free(gray);
  rmdir(dir);
}

static int g_wake_step;
static int g_last_full;

static void wake_present(AppHost *host, int x, int y, int w, int h, int full) {
  (void)host;
  (void)x;
  (void)y;
  (void)w;
  (void)h;
  g_presents++;
  g_last_full = full;
}

static int wake_poll(AppHost *host, AppEvent *event) {
  memset(event, 0, sizeof *event);
  if (host->poll_ms == 0) {
    return 0;
  }
  switch (g_wake_step++) {
    case 0:
      g_last_full = -1;
      event->kind = EV_WAKE;
      return 1;
    case 1:
      expect(g_last_full == 1, "wake redraws the page with a full refresh");
      event->kind = EV_QUIT;
      return -1;
    default:
      event->kind = EV_QUIT;
      return -1;
  }
}

static void test_wake_repaints(void) {
  char dir[] = "/tmp/yomu-wake-XXXXXX";
  char book_path[256];
  uint8_t *gray;
  AppHost host;
  const int width = 120;
  const int height = 80;
  expect(mkdtemp(dir) != NULL, "wake dir");
  snprintf(book_path, sizeof book_path, "%s/Demo.txt", dir);
  expect(write_text(book_path, k_line) == 0, "wake book");
  gray = malloc((size_t)width * (size_t)height);
  expect(gray != NULL, "wake gray");
  if (!gray) {
    return;
  }
  memset(gray, 255, (size_t)width * (size_t)height);
  memset(&host, 0, sizeof host);
  host.w = width;
  host.h = height;
  host.gray = gray;
  host.full_every = 8;
  host.root = dir;
  host.home = dir;
  host.present = wake_present;
  host.poll = wake_poll;
  host.sleep = fake_sleep;
  g_sleeps = 0;
  g_presents = 0;
  g_last_full = 0;
  g_wake_step = 0;
  expect(app_run(&host, book_path) == 0, "wake does not exit");
  expect(g_sleeps == 0, "wake does not ask the device to sleep");
  expect(g_wake_step == 2, "wake returned to the loop");
  free(gray);
  remove(book_path);
  snprintf(book_path, sizeof book_path, "%s/progress.tsv", dir);
  remove(book_path);
  snprintf(book_path, sizeof book_path, "%s/ts", dir);
  {
    DIR *ts = opendir(book_path);
    if (ts) {
      struct dirent *ent;
      while ((ent = readdir(ts)) != NULL) {
        char path[512];
        if (ent->d_name[0] == '.') {
          continue;
        }
        snprintf(path, sizeof path, "%s/ts/%s", dir, ent->d_name);
        remove(path);
      }
      closedir(ts);
    }
  }
  rmdir(book_path);
  rmdir(dir);
}

static void test_ui(void) {
  char dir[] = "/tmp/yomu-ui-XXXXXX";
  expect(mkdtemp(dir) != NULL, "ui dir");
  char book_path[256];
  char other_path[256];
  YomuBook *books = NULL;
  int count = 0;
  int trunc = 0;
  const int width = 120;
  const int height = 80;
  snprintf(book_path, sizeof book_path, "%s/Demo.txt", dir);
  snprintf(other_path, sizeof other_path, "%s/Other.xtch", dir);
  expect(write_text(book_path, k_line) == 0, "ui book");
  expect(write_text(other_path, "bitmap") == 0, "ignored bitmap");
  expect(yomu_scan_books(dir, &books, &count, &trunc) == 0, "ui scan");
  expect(count == 1 && books && strstr(books[0].path, "Demo.txt") != NULL, "library skips .xtch");
  yomu_free_books(books, count);

  uint8_t *gray = malloc((size_t)width * (size_t)height);
  expect(gray != NULL, "ui gray");
  if (!gray) {
    return;
  }
  memset(gray, 255, (size_t)width * (size_t)height);
  AppHost host;
  memset(&host, 0, sizeof host);
  host.w = width;
  host.h = height;
  host.gray = gray;
  host.full_every = 8;
  host.root = dir;
  host.home = dir;
  host.present = fake_present;
  host.poll = fake_poll;
  g_presents = 0;
  expect(app_run(&host, book_path) == 0, "app run");
  expect(g_presents >= 2, "painted scan and page");
  expect(ink_count(gray, width * height) > 0, "page has ink");
  int page = -1;
  expect(progress_load(dir, book_path, &page) == 0 && page == 0, "ui progress");
  page = -1;
  expect(progress_load(dir, other_path, &page) != 0, "bitmap was not opened");
  free(gray);
  remove(other_path);
  remove(book_path);
  char prog[256];
  snprintf(prog, sizeof prog, "%s/progress.tsv", dir);
  remove(prog);
  rmdir(dir);
}

static int g_tap_step;

static int tap_poll(AppHost *host, AppEvent *event) {
  memset(event, 0, sizeof *event);
  int w = host->w;
  switch (g_tap_step++) {
    case 0:
      expect(host->gray[32 * w] == 0, "library cursor starts on the first row");
      memcpy(g_page0, host->gray, g_page_n);
      event->kind = EV_SWIPE_UP;
      return 1;
    case 1:
      expect(page_match(g_page0, host), "swipe does not move the list");
      event->kind = EV_TAP;
      event->x = 10;
      event->y = 140;
      return 1;
    case 2:
      expect(!page_match(g_page0, host), "down shows the next page");
      expect(host->gray[32 * w] == 0, "cursor stays on the first visible row");
      event->kind = EV_QUIT;
      return -1;
    default:
      expect(0, "down did not finish");
      event->kind = EV_QUIT;
      return -1;
  }
}

static int chap_poll(AppHost *host, AppEvent *event) {
  memset(event, 0, sizeof *event);
  switch (g_tap_step++) {
    case 0:
      expect(ink_count(host->gray, (int)g_page_n) > 0, "chapter book opens on its page");
      memcpy(g_page1, host->gray, g_page_n);
      event->kind = EV_CONFIRM;
      return 1;
    case 1:
      event->kind = EV_DOWN;
      return 1;
    case 2:
      event->kind = EV_CONFIRM;
      return 1;
    case 3:
      expect(host->gray[0] == 255, "chapter list is showing");
      event->kind = EV_SWIPE_UP;
      return 1;
    case 4:
      expect(host->gray[0] == 255, "swipe stays on the chapter list");
      event->kind = EV_TAP;
      event->x = 10;
      event->y = 220;
      return 1;
    case 5:
      event->kind = EV_TAP;
      event->x = 10;
      event->y = 279;
      return 1;
    case 6:
      expect(page_match(g_page1, host), "back returns to the page");
      event->kind = EV_QUIT;
      return -1;
    default:
      expect(0, "chapter back did not finish");
      event->kind = EV_QUIT;
      return -1;
  }
}

static int g_jump_step;
static const char *g_jump_path;

/* 200x200 is scale 2. The page line is y < 24. The keypad number is 32px
 * under the 32px header, then an 8px gap, so the grid starts at y 72.
 * Cells are 66x14. 2 is (66,72), 9 is (132,100), 消す is (0,114), 移動 is (132,114). */
static int jump_poll(AppHost *host, AppEvent *event) {
  memset(event, 0, sizeof *event);
  int page = -1;
  int have = progress_load(host->home, g_jump_path, &page) == 0;
  int border = host->gray[80 * host->w];
  switch (g_jump_step++) {
    case 0:
      expect(ink_count(host->gray, (int)g_page_n) > 0, "jump book opens on a page");
      memcpy(g_page0, host->gray, g_page_n);
      event->kind = EV_SWIPE_DOWN;
      event->y0 = 0;
      return 1;
    case 1:
      event->kind = EV_TAP;
      event->x = 10;
      event->y = 10;
      return 1;
    case 2:
      event->kind = EV_TAP;
      event->x = 20;
      event->y = 78;
      return 1;
    case 3:
      event->kind = EV_TAP;
      event->x = 20;
      event->y = 120;
      return 1;
    case 4:
      event->kind = EV_TAP;
      event->x = 160;
      event->y = 120;
      return 1;
    case 5:
      expect(host->gray[0] == 255 && border == 0, "empty entry stays on the keypad");
      event->kind = EV_TAP;
      event->x = 90;
      event->y = 78;
      return 1;
    case 6:
      event->kind = EV_TAP;
      event->x = 160;
      event->y = 120;
      return 1;
    case 7:
      expect(!page_match(g_page0, host), "jump opens the typed page");
      expect(have && page == 1, "jump saves that page");
      memcpy(g_page1, host->gray, g_page_n);
      event->kind = EV_SWIPE_DOWN;
      event->y0 = 0;
      return 1;
    case 8:
      event->kind = EV_TAP;
      event->x = 10;
      event->y = 10;
      return 1;
    case 9:
      event->kind = EV_TAP;
      event->x = 160;
      event->y = 106;
      return 1;
    case 10:
      event->kind = EV_TAP;
      event->x = 160;
      event->y = 120;
      return 1;
    case 11:
      expect(have && page == g_jump_pages - 1, "a page past the end clamps to the last page");
      expect(g_jump_pages != 2 || page_match(g_page1, host), "clamp keeps the last page");
      event->kind = EV_QUIT;
      return -1;
    default:
      expect(0, "jump script did not finish");
      event->kind = EV_QUIT;
      return -1;
  }
}

static void test_jump_page(void) {
  char dir[] = "/tmp/yomu-jump-XXXXXX";
  expect(mkdtemp(dir) != NULL, "jump dir");
  char book_path[256];
  const int side = 200;
  snprintf(book_path, sizeof book_path, "%s/Pages.txt", dir);
  expect(write_fitted(book_path, dir, side, side, 2, 8, &g_jump_pages) == 0, "jump book");
  g_page_n = (size_t)side * (size_t)side;
  uint8_t *gray = malloc(g_page_n);
  g_page0 = malloc(g_page_n);
  g_page1 = malloc(g_page_n);
  expect(gray && g_page0 && g_page1, "jump gray");
  if (!gray || !g_page0 || !g_page1) {
    free(gray);
    free(g_page0);
    free(g_page1);
    return;
  }
  memset(gray, 255, g_page_n);
  AppHost host;
  memset(&host, 0, sizeof host);
  host.w = side;
  host.h = side;
  host.gray = gray;
  host.full_every = 8;
  host.root = dir;
  host.home = dir;
  host.present = fake_present;
  host.poll = jump_poll;
  g_jump_step = 0;
  g_jump_path = book_path;
  expect(app_run(&host, book_path) == 0, "jump run");
  expect(g_jump_step == 12, "jump script finished");
  free(gray);
  free(g_page0);
  free(g_page1);
  g_page0 = NULL;
  g_page1 = NULL;
  remove(book_path);
  char prog[256];
  snprintf(prog, sizeof prog, "%s/progress.tsv", dir);
  remove(prog);
  rmdir(dir);
}

/* Library height 200 uses scale 2: header 32, row 48, footer 24, button bar at y 128.
 * The chapter run is 280 tall so five menu rows (panel 232) still leave a gap under the menu.
 * 戻る on that chapter list is the bar at y 208. */
static void test_tap_buttons(void) {
  char dir[] = "/tmp/yomu-tap-XXXXXX";
  expect(mkdtemp(dir) != NULL, "tap dir");
  const int width = 120;
  const int height = 200;
  const int chap_h = 280;
  char book_path[256];
  char extra_path[256];
  TsBook *opened;
  snprintf(book_path, sizeof book_path, "%s/Demo.epub", dir);
  snprintf(extra_path, sizeof extra_path, "%s/Extra.txt", dir);
  expect(write_chapter_epub(book_path) == 0, "tap book");
  expect(write_text(extra_path, k_line) == 0, "second book");
  opened = ts_open(book_path, dir, NULL, width, height);
  expect(opened != NULL, ts_error(NULL));
  if (opened) {
    expect(ts_chapter_count(opened) > 0, "epub has a chapter");
    expect(ts_pages(opened) > 0, "epub has a page");
    {
      int start = -2;
      int end = -2;
      expect(ts_chapter(opened, 0, NULL, 0, &start, &end) == 1, "epub chapter range");
      expect(start >= 0, "opened chapter has a page");
      if (ts_done(opened)) {
        expect(end >= start, "a finished book closes the last chapter");
      } else {
        expect(end < 0, "an unfinished book leaves the last chapter open");
      }
    }
    ts_close(opened);
  }

  g_page_n = (size_t)width * (size_t)height;
  uint8_t *gray = malloc((size_t)width * (size_t)chap_h);
  g_page0 = malloc((size_t)width * (size_t)chap_h);
  g_page1 = malloc((size_t)width * (size_t)chap_h);
  expect(gray && g_page0 && g_page1, "tap gray");
  if (!gray || !g_page0 || !g_page1) {
    free(gray);
    free(g_page0);
    free(g_page1);
    return;
  }
  memset(gray, 255, (size_t)width * (size_t)chap_h);
  AppHost host;
  memset(&host, 0, sizeof host);
  host.w = width;
  host.h = height;
  host.gray = gray;
  host.full_every = 8;
  host.root = dir;
  host.home = dir;
  host.present = fake_present;
  host.poll = tap_poll;
  g_tap_step = 0;
  expect(app_run(&host, NULL) == 0, "library buttons");
  expect(g_tap_step == 3, "down pages the library");

  host.h = chap_h;
  g_page_n = (size_t)width * (size_t)chap_h;
  memset(gray, 255, g_page_n);
  host.poll = chap_poll;
  g_tap_step = 0;
  expect(app_run(&host, book_path) == 0, "chapter back button");
  expect(g_tap_step == 7, "chapter script finished");

  free(gray);
  free(g_page0);
  free(g_page1);
  g_page0 = NULL;
  g_page1 = NULL;
  remove(book_path);
  remove(extra_path);
  char prog[256];
  snprintf(prog, sizeof prog, "%s/progress.tsv", dir);
  remove(prog);
  rmdir(dir);
}

static void test_rotate(void) {
  int lw = 0;
  int lh = 0;
  yomu_logical_size(1236, 1648, 3, &lw, &lh);
  expect(lw == 1648 && lh == 1236, "landscape size");
  int nx = 0;
  int ny = 0;
  int lx = 0;
  int ly = 0;
  yomu_to_native(3, 1236, 1648, 0, 0, &nx, &ny);
  expect(nx == 1235 && ny == 0, "90 degree origin");
  yomu_to_logical(3, 1236, 1648, nx, ny, &lx, &ly);
  expect(lx == 0 && ly == 0, "90 degree inverse");
  yomu_to_native(1, 1236, 1648, 0, 0, &nx, &ny);
  expect(nx == 0 && ny == 1647, "270 degree origin");
  for (int rot = 0; rot < 4; rot++) {
    yomu_logical_size(1236, 1648, rot, &lw, &lh);
    int ox = 0;
    int oy = 0;
    int ow = 0;
    int oh = 0;
    yomu_rect_native(rot, 1236, 1648, 0, 0, lw, lh, &ox, &oy, &ow, &oh);
    expect(ox == 0 && oy == 0 && ow == 1236 && oh == 1648, "full rect covers panel");
    yomu_to_native(rot, 1236, 1648, lw / 2, lh / 2, &nx, &ny);
    yomu_to_logical(rot, 1236, 1648, nx, ny, &lx, &ly);
    expect(lx == lw / 2 && ly == lh / 2, "rotate roundtrip");
  }
}

static void zip_put(FILE *fp, const void *bytes, size_t n, uint32_t *off) {
  if (fwrite(bytes, 1, n, fp) == n) {
    *off += (uint32_t)n;
  }
}

static uint32_t zip_local(FILE *fp, const char *name, const void *data, uint32_t dlen, uint32_t *off) {
  uint8_t hdr[30];
  uint32_t local = *off;
  uint16_t nlen = (uint16_t)strlen(name);
  memset(hdr, 0, sizeof hdr);
  wr32(hdr, 0x04034b50u);
  wr16(hdr + 4, 20);
  wr32(hdr + 18, dlen);
  wr32(hdr + 22, dlen);
  wr16(hdr + 26, nlen);
  zip_put(fp, hdr, sizeof hdr, off);
  zip_put(fp, name, nlen, off);
  zip_put(fp, data, dlen, off);
  return local;
}

static void zip_central(FILE *fp, const char *name, uint32_t local, uint32_t dlen, uint32_t *off) {
  uint8_t hdr[46];
  uint16_t nlen = (uint16_t)strlen(name);
  memset(hdr, 0, sizeof hdr);
  wr32(hdr, 0x02014b50u);
  wr16(hdr + 4, 20);
  wr16(hdr + 6, 20);
  wr32(hdr + 20, dlen);
  wr32(hdr + 24, dlen);
  wr16(hdr + 28, nlen);
  wr32(hdr + 42, local);
  zip_put(fp, hdr, sizeof hdr, off);
  zip_put(fp, name, nlen, off);
}

struct ZipEnt {
  const char *name;
  const void *data;
  uint32_t len;
};

static int write_stored_parts(const char *path, const struct ZipEnt *ents, int count) {
  FILE *fp;
  uint32_t off = 0;
  uint32_t cd;
  uint32_t *locals;
  uint8_t end[22];
  int i;
  if (!path || !ents || count < 1 || count > 4096) {
    return -1;
  }
  locals = malloc((size_t)count * sizeof(*locals));
  if (!locals) {
    return -1;
  }
  fp = fopen(path, "wb");
  if (!fp) {
    free(locals);
    return -1;
  }
  for (i = 0; i < count; i++) {
    locals[i] = zip_local(fp, ents[i].name, ents[i].data, ents[i].len, &off);
  }
  cd = off;
  for (i = 0; i < count; i++) {
    zip_central(fp, ents[i].name, locals[i], ents[i].len, &off);
  }
  memset(end, 0, sizeof end);
  wr32(end, 0x06054b50u);
  wr16(end + 8, (uint16_t)count);
  wr16(end + 10, (uint16_t)count);
  wr32(end + 12, off - cd);
  wr32(end + 16, cd);
  zip_put(fp, end, sizeof end, &off);
  fclose(fp);
  free(locals);
  return 0;
}

static int write_stored_zip(const char *path, const char *n0, const char *d0, const char *n1, const char *d1,
                            const char *n2, const char *d2) {
  struct ZipEnt ents[3];
  ents[0].name = n0;
  ents[0].data = d0;
  ents[0].len = (uint32_t)strlen(d0);
  ents[1].name = n1;
  ents[1].data = d1;
  ents[1].len = (uint32_t)strlen(d1);
  ents[2].name = n2;
  ents[2].data = d2;
  ents[2].len = (uint32_t)strlen(d2);
  return write_stored_parts(path, ents, 3);
}

static int write_chapter_epub(const char *path) {
  const char *container =
      "<?xml version=\"1.0\"?>"
      "<container version=\"1.0\">"
      "<rootfiles>"
      "<rootfile full-path=\"OEBPS/content.opf\" media-type=\"application/oebps-package+xml\"/>"
      "</rootfiles>"
      "</container>";
  const char *opf =
      "<?xml version=\"1.0\"?>"
      "<package>"
      "<metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\">"
      "<dc:title>猫</dc:title>"
      "</metadata>"
      "<manifest>"
      "<item id=\"c1\" href=\"chap.xhtml\" media-type=\"application/xhtml+xml\"/>"
      "</manifest>"
      "<spine>"
      "<itemref idref=\"c1\"/>"
      "</spine>"
      "</package>";
  char xhtml[800];
  snprintf(xhtml, sizeof xhtml,
           "<?xml version=\"1.0\"?>"
           "<html xmlns=\"http://www.w3.org/1999/xhtml\"><body><p>%s%s</p></body></html>",
           k_line, k_line);
  return write_stored_zip(path, "META-INF/container.xml", container, "OEBPS/content.opf", opf, "OEBPS/chap.xhtml",
                          xhtml);
}

static int write_three_chapter_epub(const char *path) {
  const char *container =
      "<?xml version=\"1.0\"?>"
      "<container version=\"1.0\">"
      "<rootfiles>"
      "<rootfile full-path=\"OEBPS/content.opf\" media-type=\"application/oebps-package+xml\"/>"
      "</rootfiles>"
      "</container>";
  const char *opf =
      "<?xml version=\"1.0\"?>"
      "<package>"
      "<metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\">"
      "<dc:title>三章</dc:title>"
      "</metadata>"
      "<manifest>"
      "<item id=\"ncx\" href=\"toc.ncx\" media-type=\"application/x-dtbncx+xml\"/>"
      "<item id=\"c1\" href=\"c1.xhtml\" media-type=\"application/xhtml+xml\"/>"
      "<item id=\"c2\" href=\"c2.xhtml\" media-type=\"application/xhtml+xml\"/>"
      "<item id=\"c3\" href=\"c3.xhtml\" media-type=\"application/xhtml+xml\"/>"
      "</manifest>"
      "<spine toc=\"ncx\">"
      "<itemref idref=\"c1\"/>"
      "<itemref idref=\"c2\"/>"
      "<itemref idref=\"c3\"/>"
      "</spine>"
      "</package>";
  const char *ncx =
      "<ncx>"
      "<navPoint><navLabel><text>First</text></navLabel><content src=\"c1.xhtml\"/></navPoint>"
      "<navPoint><navLabel><text>Second</text></navLabel><content src=\"c2.xhtml\"/></navPoint>"
      "<navPoint><navLabel><text>Third</text></navLabel><content src=\"c3.xhtml\"/></navPoint>"
      "</ncx>";
  char body[240];
  struct ZipEnt ents[6];
  snprintf(body, sizeof body,
           "<?xml version=\"1.0\"?>"
           "<html xmlns=\"http://www.w3.org/1999/xhtml\"><body><p>%s</p></body></html>",
           k_line);
  ents[0].name = "META-INF/container.xml";
  ents[0].data = container;
  ents[0].len = (uint32_t)strlen(container);
  ents[1].name = "OEBPS/content.opf";
  ents[1].data = opf;
  ents[1].len = (uint32_t)strlen(opf);
  ents[2].name = "OEBPS/toc.ncx";
  ents[2].data = ncx;
  ents[2].len = (uint32_t)strlen(ncx);
  ents[3].name = "OEBPS/c1.xhtml";
  ents[3].data = body;
  ents[3].len = (uint32_t)strlen(body);
  ents[4].name = "OEBPS/c2.xhtml";
  ents[4].data = body;
  ents[4].len = (uint32_t)strlen(body);
  ents[5].name = "OEBPS/c3.xhtml";
  ents[5].data = body;
  ents[5].len = (uint32_t)strlen(body);
  return write_stored_parts(path, ents, 6);
}

struct IndexProg {
  int calls;
  int saw_total;
  int last_done;
  int last_total;
};

static void index_progress(void *ctx, int done, int total) {
  struct IndexProg *prog = ctx;
  prog->calls++;
  prog->last_done = done;
  prog->last_total = total;
  if (total == 3 && done >= 1) {
    prog->saw_total = 1;
  }
}

static void test_index_continues(void) {
  char dir[] = "/tmp/yomu-idx-XXXXXX";
  char book_path[256];
  struct IndexProg prog;
  TsBook *book;
  uint8_t *page;
  int done = 0;
  int total = 0;
  int steps = 0;
  const int w = 120;
  const int h = 80;
  memset(&prog, 0, sizeof prog);
  expect(mkdtemp(dir) != NULL, "index dir");
  snprintf(book_path, sizeof book_path, "%s/Three.epub", dir);
  expect(write_three_chapter_epub(book_path) == 0, "index epub");
  book = ts_open_progress(book_path, dir, NULL, w, h, index_progress, &prog);
  expect(book != NULL, ts_error(NULL));
  if (!book) {
    remove(book_path);
    rmdir(dir);
    return;
  }
  expect(prog.calls > 0 && prog.saw_total, "open reports spine progress");
  expect(ts_done(book) == 0, "open returns before the spine is finished");
  expect(ts_spine(book, &done, &total) == 1, "spine count");
  expect(total == 3 && done < total, "saved page is ready before the last spine");
  while (!ts_done(book) && steps < 40) {
    int more = ts_index_step(book);
    steps++;
    if (!more && !ts_done(book)) {
      break;
    }
  }
  expect(ts_done(book) == 1, "background steps finish the book");
  done = 0;
  total = 0;
  expect(ts_spine(book, &done, &total) == 1 && done == 3 && total == 3, "every spine item is ingested");
  expect(ts_index_step(book) == 0, "a finished book has no further step");
  page = malloc((size_t)w * (size_t)h);
  expect(page != NULL, "index page");
  if (page) {
    memset(page, 255, (size_t)w * (size_t)h);
    expect(ts_render(book, 0, page, w, h) == 1, "page 0 renders after indexing");
    expect(ink_count(page, w * h) > 8, "indexed page has ink");
    free(page);
  }
  ts_close(book);
  remove(book_path);
  rmdir(dir);
}

static int g_idle_step;

static int idle_poll(AppHost *host, AppEvent *event) {
  (void)host;
  memset(event, 0, sizeof *event);
  if (g_idle_step++ < 40) {
    event->kind = EV_NONE;
    return 0;
  }
  event->kind = EV_QUIT;
  return -1;
}

static void test_index_idle(void) {
  char dir[] = "/tmp/yomu-idle-XXXXXX";
  char book_path[256];
  uint8_t *gray;
  AppHost host;
  const int w = 120;
  const int h = 80;
  expect(mkdtemp(dir) != NULL, "idle dir");
  snprintf(book_path, sizeof book_path, "%s/Three.epub", dir);
  expect(write_three_chapter_epub(book_path) == 0, "idle epub");
  gray = malloc((size_t)w * (size_t)h);
  expect(gray != NULL, "idle gray");
  if (!gray) {
    remove(book_path);
    rmdir(dir);
    return;
  }
  memset(gray, 255, (size_t)w * (size_t)h);
  memset(&host, 0, sizeof host);
  host.w = w;
  host.h = h;
  host.gray = gray;
  host.full_every = 8;
  host.root = dir;
  host.home = dir;
  host.present = fake_present;
  host.poll = idle_poll;
  g_idle_step = 0;
  expect(app_run(&host, book_path) == 0, "idle indexing returns");
  expect(g_idle_step > 1, "idle poll ran");
  free(gray);
  remove(book_path);
  rmdir(dir);
}

static void test_chapter_ranges(void) {
  char dir[] = "/tmp/yomu-chap-XXXXXX";
  char book_path[256];
  char name[32];
  TsBook *book;
  int start = 0;
  int end = 0;
  int page = -1;
  int i;
  expect(mkdtemp(dir) != NULL, "chapter range dir");
  snprintf(book_path, sizeof book_path, "%s/Three.epub", dir);
  expect(write_three_chapter_epub(book_path) == 0, "three chapter epub");
  book = ts_open(book_path, dir, NULL, 120, 80);
  expect(book != NULL, ts_error(NULL));
  if (!book) {
    remove(book_path);
    rmdir(dir);
    return;
  }
  expect(ts_chapter_count(book) == 3, "partial book lists every chapter");
  expect(ts_done(book) == 0, "three chapters are not all indexed yet");
  expect(ts_chapter(book, 0, name, sizeof name, &start, &end) == 1, "first chapter");
  expect(strcmp(name, "First") == 0 && start >= 0 && end < 0, "first chapter is open");
  for (i = 1; i < 3; i++) {
    start = 0;
    end = 0;
    expect(ts_chapter(book, i, name, sizeof name, &start, &end) == 1, "later chapter is listed");
    expect(start < 0 && end < 0, "later chapter has no page yet");
  }
  expect(ts_chapter(book, 2, name, sizeof name, &start, &end) == 1, "third name");
  expect(strcmp(name, "Third") == 0, "third chapter keeps its name");
  expect(ts_open_chapter(book, 2, &page) == 1, "opening a later chapter typesets it");
  expect(page >= 0, "opened chapter has a page");
  expect(ts_chapter(book, 2, name, sizeof name, &start, &end) == 1, "third chapter after open");
  expect(start == page, "open lands on the chapter page");
  if (ts_done(book)) {
    expect(end >= start, "a finished book closes the opened chapter");
  } else {
    expect(end < 0, "an unfinished opened chapter stays open");
  }
  start = -1;
  end = -1;
  expect(ts_chapter(book, 0, NULL, 0, &start, &end) == 1, "first chapter after later open");
  expect(start >= 0 && end >= start, "the earlier chapter closes once the next one has a page");
  ts_close(book);
  remove(book_path);
  rmdir(dir);
}

int test_typeset_rules(void);

static void test_typeset(void) {
  expect(test_typeset_rules() == 0, "typeset rules");
  const char *body =
      "吾輩は猫である。名前はまだ無い。どこで生れたかとんと見当がつかぬ。"
      "何でも薄暗いじめじめした所でニャーニャー泣いていた事だけは記憶している。"
      "吾輩はここで始めて人間というものを見た。";
  char dir[] = "/tmp/yomu-ts-XXXXXX";
  char txt[256];
  char epub[256];
  const int w = 120;
  const int h = 80;
  uint8_t *page0;
  uint8_t *page1;
  TsBook *book;
  int pages;
  expect(mkdtemp(dir) != NULL, "typeset dir");
  snprintf(txt, sizeof txt, "%s/wagahai.txt", dir);
  snprintf(epub, sizeof epub, "%s/wagahai.epub", dir);
  {
    FILE *fp = fopen(txt, "wb");
    expect(fp != NULL, "typeset txt");
    if (fp) {
      fputs(body, fp);
      fputs(body, fp);
      fclose(fp);
    }
  }
  book = ts_open(txt, dir, NULL, w, h);
  expect(book != NULL, ts_error(NULL));
  if (!book) {
    return;
  }
  pages = ts_pages(book);
  expect(pages >= 2, "typeset lays out at least two pages");
  expect(ts_rtl(book) == 1, "plain text is vertical");
  page0 = malloc((size_t)w * (size_t)h);
  page1 = malloc((size_t)w * (size_t)h);
  expect(page0 && page1, "typeset page buffers");
  if (page0 && page1) {
    memset(page0, 255, (size_t)w * (size_t)h);
    memset(page1, 255, (size_t)w * (size_t)h);
    expect(ts_render(book, 0, page0, w, h) == 1, "render page 0");
    expect(ink_count(page0, w * h) > 20, "page 0 has ink");
    expect(ts_render(book, 1, page1, w, h) == 1, "render page 1");
    expect(memcmp(page0, page1, (size_t)w * (size_t)h) != 0, "page 1 differs");
  }
  free(page0);
  free(page1);
  ts_close(book);

  {
    const char *container =
        "<?xml version=\"1.0\"?>"
        "<container version=\"1.0\">"
        "<rootfiles>"
        "<rootfile full-path=\"OEBPS/content.opf\" media-type=\"application/oebps-package+xml\"/>"
        "</rootfiles>"
        "</container>";
    const char *opf =
        "<?xml version=\"1.0\"?>"
        "<package>"
        "<metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\">"
        "<dc:title>猫</dc:title>"
        "</metadata>"
        "<manifest>"
        "<item id=\"c1\" href=\"chap.xhtml\" media-type=\"application/xhtml+xml\"/>"
        "</manifest>"
        "<spine>"
        "<itemref idref=\"c1\"/>"
        "</spine>"
        "</package>";
    char xhtml[1200];
    snprintf(xhtml, sizeof xhtml,
             "<?xml version=\"1.0\"?>"
             "<html xmlns=\"http://www.w3.org/1999/xhtml\"><body><p>%s%s</p></body></html>",
             body, body);
    expect(write_stored_zip(epub, "META-INF/container.xml", container, "OEBPS/content.opf", opf, "OEBPS/chap.xhtml",
                            xhtml) == 0,
           "write epub");
  }
  book = ts_open(epub, dir, NULL, w, h);
  expect(book != NULL, ts_error(NULL));
  if (!book) {
    return;
  }
  expect(strcmp(ts_title(book), "猫") == 0, "epub title");
  expect(ts_pages(book) >= 1, "epub has a page");
  page0 = malloc((size_t)w * (size_t)h);
  if (page0) {
    memset(page0, 255, (size_t)w * (size_t)h);
    expect(ts_render(book, 0, page0, w, h) == 1, "render epub");
    expect(ink_count(page0, w * h) > 20, "epub page has ink");
    free(page0);
  }
  ts_close(book);

  {
    /* 80x40 black PNG. A leading img must become a page, and the text after it stays. */
    static const unsigned char fig_png[] = {
        0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52,
        0x00, 0x00, 0x00, 0x50, 0x00, 0x00, 0x00, 0x28, 0x08, 0x00, 0x00, 0x00, 0x00, 0x4c, 0x0b, 0x22,
        0xdd, 0x00, 0x00, 0x00, 0x1a, 0x49, 0x44, 0x41, 0x54, 0x78, 0xda, 0xed, 0xc1, 0x31, 0x01, 0x00,
        0x00, 0x00, 0xc2, 0xa0, 0xf5, 0x4f, 0x6d, 0x0d, 0x0f, 0xa0, 0x00, 0x00, 0x00, 0xbe, 0x0c, 0x0c,
        0xa8, 0x00, 0x01, 0xef, 0x6c, 0xcf, 0xea, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae,
        0x42, 0x60, 0x82};
    const char *container =
        "<?xml version=\"1.0\"?>"
        "<container version=\"1.0\">"
        "<rootfiles>"
        "<rootfile full-path=\"OEBPS/content.opf\" media-type=\"application/oebps-package+xml\"/>"
        "</rootfiles>"
        "</container>";
    const char *opf =
        "<?xml version=\"1.0\"?>"
        "<package>"
        "<metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\">"
        "<dc:title>図</dc:title>"
        "</metadata>"
        "<manifest>"
        "<item id=\"c1\" href=\"chap.xhtml\" media-type=\"application/xhtml+xml\"/>"
        "<item id=\"fig\" href=\"fig.png\" media-type=\"image/png\"/>"
        "</manifest>"
        "<spine>"
        "<itemref idref=\"c1\"/>"
        "</spine>"
        "</package>";
    const char *xhtml =
        "<?xml version=\"1.0\"?>"
        "<html xmlns=\"http://www.w3.org/1999/xhtml\"><body>"
        "<img src=\"fig.png\" alt=\"図\"/>"
        "<p>あ</p>"
        "</body></html>";
    char pic_epub[256];
    struct ZipEnt ents[4];
    uint8_t *page;
    int heavy = 0;
    int textish = 0;
    int i;
    snprintf(pic_epub, sizeof pic_epub, "%s/fig.epub", dir);
    ents[0].name = "META-INF/container.xml";
    ents[0].data = container;
    ents[0].len = (uint32_t)strlen(container);
    ents[1].name = "OEBPS/content.opf";
    ents[1].data = opf;
    ents[1].len = (uint32_t)strlen(opf);
    ents[2].name = "OEBPS/chap.xhtml";
    ents[2].data = xhtml;
    ents[2].len = (uint32_t)strlen(xhtml);
    ents[3].name = "OEBPS/fig.png";
    ents[3].data = fig_png;
    ents[3].len = (uint32_t)sizeof fig_png;
    expect(write_stored_parts(pic_epub, ents, 4) == 0, "write picture epub");
    book = ts_open(pic_epub, dir, NULL, w, h);
    expect(book != NULL, ts_error(NULL));
    if (!book) {
      return;
    }
    page = malloc((size_t)w * (size_t)h);
    expect(page != NULL, "picture page buffer");
    if (page) {
      const int edge = h > 32 ? 16 : 0;
      const int inner = w * (h - edge * 2);
      int edges_clear = 0;
      for (i = 0; i < 6; i++) {
        int ink;
        int y;
        int x;
        int clear = 1;
        memset(page, 255, (size_t)w * (size_t)h);
        if (ts_render(book, i, page, w, h) != 1) {
          break;
        }
        ink = ink_count(page, w * h);
        if (ink > inner / 2) {
          heavy = 1;
          for (y = 0; y < edge; y++) {
            for (x = 0; x < w; x++) {
              if (page[(size_t)y * (size_t)w + (size_t)x] < 250 ||
                  page[(size_t)(h - 1 - y) * (size_t)w + (size_t)x] < 250) {
                clear = 0;
              }
            }
          }
          if (clear) {
            edges_clear = 1;
          }
        } else if (ink > 8) {
          textish = 1;
        }
      }
      expect(heavy, "picture page is mostly ink");
      expect(edges_clear, "picture keeps the top and bottom clear");
      expect(textish, "text beside the picture still has a page");
      free(page);
    }
    ts_close(book);

    /* The same 80x40 image on a larger page fits in the column. */
    book = ts_open(pic_epub, dir, NULL, 240, 320);
    expect(book != NULL, ts_error(NULL));
    if (book) {
      uint8_t *wide = malloc(240u * 320u);
      expect(wide != NULL, "column picture buffer");
      if (wide) {
        int solid = 0;
        int min_y = 320;
        int max_y = 0;
        int y;
        int x;
        memset(wide, 255, 240u * 320u);
        expect(ts_render(book, 0, wide, 240, 320) == 1, "render column picture");
        for (y = 0; y < 320; y++) {
          for (x = 0; x < 240; x++) {
            uint8_t px = wide[(size_t)y * 240u + (size_t)x];
            if (px < 32) {
              solid++;
            }
            if (px < 250) {
              if (y < min_y) {
                min_y = y;
              }
              if (y > max_y) {
                max_y = y;
              }
            }
          }
        }
        expect(solid > 20 && solid < 2000, "small picture stays in the column");
        expect(max_y - min_y >= 16, "text follows the picture on the page");
        free(wide);
      }
      ts_close(book);
    }
  }
}

void test_fileserver(void);

static int g_type_step;
static char g_type_face[512];

/* 200x400 is scale 2. The size row is y 32..80. 大 is the right third, x 132..200.
 * Face rows start at y 80. The second face is y 128..176. */
static int type_poll(AppHost *host, AppEvent *event) {
  memset(event, 0, sizeof *event);
  switch (g_type_step++) {
    case 0:
      expect(ink_count(host->gray, (int)g_page_n) > 0, "type book opens on a page");
      event->kind = EV_CONFIRM;
      return 1;
    case 1:
    case 2:
    case 6:
    case 7:
      event->kind = EV_DOWN;
      return 1;
    case 3:
    case 5:
    case 8:
      if (g_type_step == 6) {
        char file[768];
        snprintf(file, sizeof file, "%s/type.txt", host->home);
        expect(type_step() == 1, "larger type is saved");
        expect(type_em(1648, 1236) == 57, "saved step is 57px on paperwhite");
        FILE *fp = fopen(file, "r");
        char buf[64];
        expect(fp != NULL, "type.txt exists");
        if (fp) {
          expect(fgets(buf, sizeof buf, fp) != NULL && strncmp(buf, "step 1", 6) == 0, "type.txt records the step");
          fclose(fp);
        }
        expect(ink_count(host->gray, (int)g_page_n) > 0, "page is drawn at the new size");
      }
      event->kind = EV_CONFIRM;
      return 1;
    case 4:
      event->kind = EV_TAP;
      event->x = 166;
      event->y = 56;
      return 1;
    case 9:
      event->kind = EV_TAP;
      event->x = 40;
      event->y = 150;
      return 1;
    case 10:
      expect(strcmp(type_font(), g_type_face) == 0, "chosen face is saved");
      expect(ink_count(host->gray, (int)g_page_n) > 0, "page is drawn in the chosen face");
      event->kind = EV_QUIT;
      return -1;
    default:
      expect(0, "type script did not finish");
      event->kind = EV_QUIT;
      return -1;
  }
}

extern int ts_font_faces(const char *path, int *indexes, char labels[][48], int cap);

static int face_named(char labels[][64], int n, const char *want) {
  int i;
  for (i = 0; i < n && i < 8; i++) {
    if (strcmp(labels[i], want) == 0) {
      return 1;
    }
  }
  return 0;
}

static int contains_fold(const char *hay, const char *needle) {
  size_t n = strlen(needle);
  const char *p;
  if (!hay || n == 0) {
    return 0;
  }
  for (p = hay; *p; p++) {
    size_t i = 0;
    while (i < n && p[i]) {
      char a = p[i];
      char b = needle[i];
      if (a >= 'A' && a <= 'Z') {
        a = (char)(a - 'A' + 'a');
      }
      if (b >= 'A' && b <= 'Z') {
        b = (char)(b - 'A' + 'a');
      }
      if (a != b) {
        break;
      }
      i++;
    }
    if (i == n) {
      return 1;
    }
  }
  return 0;
}

/* Code2000 stays off the list. A collection is one row per face, and the
 * chosen index survives type.txt. A file with no index line is face 0. */
static void test_font_faces(void) {
  char dir[] = "/tmp/yomu-faces-XXXXXX";
  char fonts[800];
  char link[900];
  char file[800];
  TypeFace faces[TYPE_FACE_MAX];
  const char *hiragino =
      "/System/Library/Fonts/\xe3\x83\x92\xe3\x83\xa9\xe3\x82\xad\xe3\x82\x99\xe3\x83\x8e\xe6\x98\x8e\xe6\x9c\x9d ProN.ttc";
  int n;
  int i;
  FILE *fp;
  expect(mkdtemp(dir) != NULL, "face dir");
  type_load(dir);
  n = type_faces(dir, faces, TYPE_FACE_MAX);
  expect(n >= 1 && faces[0].path[0], "a book face is installed");
  expect(strcmp(faces[0].label, "標準") != 0, "face rows are real fonts");
  {
    char kept[512];
    const char *env = getenv("YOMU_FONT");
    kept[0] = '\0';
    if (env) {
      snprintf(kept, sizeof kept, "%s", env);
    }
    unsetenv("YOMU_FONT");
    expect(type_used_row(faces, n) == 0, "an unsaved face uses the first real font");
    if (kept[0]) {
      setenv("YOMU_FONT", kept, 1);
    }
  }
  for (i = 0; i < n; i++) {
    expect(!contains_fold(faces[i].path, "code2000"), "body list has no code2000 path");
    expect(!contains_fold(faces[i].label, "code2000"), "body list has no code2000 label");
    expect(!contains_fold(faces[i].path, "goth") && !contains_fold(faces[i].label, "goth"), "body list has no gothic path");
    expect(strstr(faces[i].label, "ゴシック") == NULL && strstr(faces[i].path, "ゴシック") == NULL,
           "body list has no gothic label");
  }
  if (n >= 2) {
    snprintf(file, sizeof file, "%s/type.txt", dir);
    fp = fopen(file, "w");
    expect(fp != NULL, "write a type.txt without an index");
    if (fp) {
      fprintf(fp, "step 2\nfont %s\n", faces[1].path);
      fclose(fp);
    }
    type_load(dir);
    expect(type_step() == 2, "step survives a file with no index");
    expect(strcmp(type_font(), faces[1].path) == 0, "font path survives a file with no index");
    expect(type_face_index() == 0, "a missing index is face 0");
    expect(type_set_font(faces[1].path, 3) == 1, "save a collection face");
    type_load(dir);
    expect(type_face_index() == 3, "saved face index reloads");
    expect(strcmp(type_font(), faces[1].path) == 0, "saved face path reloads");
    expect(type_set_font("", 9) == 1, "clear the face");
    type_load(dir);
    expect(type_font()[0] == '\0' && type_face_index() == 0, "標準 clears the index");
  }
  if (access(hiragino, R_OK) == 0) {
    int indexes[16];
    char styles[16][48];
    int count = ts_font_faces(hiragino, indexes, styles, 16);
    int hits = 0;
    int got_index[4] = {0, 0, 0, 0};
    char seen_label[8][64];
    expect(count == 4, "hiragino lists four faces");
    expect(indexes[0] == 0 && indexes[3] == 3, "faces keep their collection index");
    expect(strcmp(styles[0], "ヒラギノ明朝 ProN W3") == 0, "listed name is ProN W3");
    snprintf(fonts, sizeof fonts, "%s/fonts", dir);
    snprintf(link, sizeof link, "%s/HiraginoMinchoProN.ttc", fonts);
    expect(mkdir(fonts, 0755) == 0, "fonts dir");
    expect(symlink(hiragino, link) == 0, "link the collection");
    n = type_faces(dir, faces, TYPE_FACE_MAX);
    for (i = 0; i < n; i++) {
      expect(!contains_fold(faces[i].path, "code2000"), "scanned list has no code2000");
      expect(!contains_fold(faces[i].path, "goth") && !contains_fold(faces[i].label, "goth"),
             "scanned list has no gothic");
      expect(strstr(faces[i].label, "ゴシック") == NULL, "scanned list has no gothic label");
      if (strcmp(faces[i].path, link) != 0) {
        continue;
      }
      if (hits < 8) {
        if (faces[i].index >= 0 && faces[i].index < 4) {
          got_index[faces[i].index] = 1;
        }
        snprintf(seen_label[hits], sizeof seen_label[hits], "%s", faces[i].label);
      }
      hits++;
    }
    expect(hits == 4 && hits == count, "each hiragino face is its own row");
    expect(got_index[0] && got_index[1] && got_index[2] && got_index[3], "rows cover face 0 through 3");
    expect(face_named(seen_label, hits, "ヒラギノ明朝 ProN W3"), "row ProN W3");
    expect(face_named(seen_label, hits, "ヒラギノ明朝 Pro W3"), "row Pro W3");
    expect(face_named(seen_label, hits, "ヒラギノ明朝 ProN W6"), "row ProN W6");
    expect(face_named(seen_label, hits, "ヒラギノ明朝 Pro W6"), "row Pro W6");
    unlink(link);
    rmdir(fonts);
  }
  snprintf(file, sizeof file, "%s/type.txt", dir);
  remove(file);
  type_load(NULL);
  rmdir(dir);
}

/* 200x260 is scale 2 and shows two face rows. 下 sits in the right half of the bar. */
static int type_page_poll(AppHost *host, AppEvent *event) {
  memset(event, 0, sizeof *event);
  switch (g_type_step++) {
    case 0:
      event->kind = EV_CONFIRM;
      return 1;
    case 1:
    case 2:
      event->kind = EV_DOWN;
      return 1;
    case 3:
      event->kind = EV_CONFIRM;
      return 1;
    case 4:
      expect(host->gray[80 * host->w] == 255, "the face above the used one is plain");
      expect(host->gray[128 * host->w] == 0, "the used face has the bar");
      event->kind = EV_TAP;
      event->x = 150;
      event->y = 208;
      return 1;
    case 5:
      expect(host->gray[80 * host->w] == 0, "the next page shows its first row");
      expect(host->gray[128 * host->w] == 255, "the paged row below is plain");
      event->kind = EV_QUIT;
      return -1;
    default:
      expect(0, "type page script did not finish");
      event->kind = EV_QUIT;
      return -1;
  }
}

static void test_type_page(void) {
  char dir[] = "/tmp/yomu-typepage-XXXXXX";
  char book_path[768];
  TypeFace faces[TYPE_FACE_MAX];
  int n;
  int side = 200;
  int small_pages = 0;
  uint8_t *gray;
  AppHost host;
  expect(mkdtemp(dir) != NULL, "type page dir");
  type_load(dir);
  n = type_faces(dir, faces, TYPE_FACE_MAX);
  if (n < 3) {
    type_load(NULL);
    rmdir(dir);
    return;
  }
  expect(type_set_font(faces[1].path, faces[1].index) == 1, "save the second face");
  expect(type_used_row(faces, n) == 1, "the saved face is the highlighted row");
  snprintf(book_path, sizeof book_path, "%s/Pages.txt", dir);
  expect(write_fitted(book_path, dir, side, 260, 2, 4, &small_pages) == 0, "type page book");
  g_page_n = (size_t)side * 260;
  gray = malloc(g_page_n);
  expect(gray != NULL, "type page gray");
  if (!gray) {
    return;
  }
  memset(gray, 255, g_page_n);
  memset(&host, 0, sizeof host);
  host.w = side;
  host.h = 260;
  host.gray = gray;
  host.full_every = 8;
  host.root = dir;
  host.home = dir;
  host.present = fake_present;
  host.poll = type_page_poll;
  g_type_step = 0;
  expect(app_run(&host, book_path) == 0, "type page run");
  expect(g_type_step == 6, "type page script finished");
  free(gray);
  remove(book_path);
  snprintf(book_path, sizeof book_path, "%s/type.txt", dir);
  remove(book_path);
  snprintf(book_path, sizeof book_path, "%s/progress.tsv", dir);
  remove(book_path);
  type_load(NULL);
  rmdir(dir);
}

static void test_type(void) {
  char dir[] = "/tmp/yomu-type-XXXXXX";
  char book_path[256];
  TypeFace faces[TYPE_FACE_MAX];
  TsBook *book;
  int nfaces;
  int small_pages = 0;
  const int side = 200;
  expect(mkdtemp(dir) != NULL, "type dir");
  type_load(dir);
  expect(type_step() == 0 && strcmp(type_step_label(), "標準") == 0, "type defaults to standard");
  expect(type_em(1648, 1236) == 50, "paperwhite standard em");
  expect(type_set_step(1) == 1, "step up");
  expect(type_em(1648, 1236) == 57, "one step larger");
  expect(type_set_step(TYPE_STEP_MAX) == 1, "largest step");
  expect(type_em(1648, 1236) == 75 && strcmp(type_step_label(), "最大") == 0, "largest em");
  expect(type_set_step(TYPE_STEP_MIN) == 1, "smallest step");
  expect(type_em(1648, 1236) == 35 && strcmp(type_step_label(), "最小") == 0, "smallest em");
  expect(type_em(120, 80) == 12, "small page clamps the em");
  expect(type_set_step(0) == 1, "back to standard");

  snprintf(book_path, sizeof book_path, "%s/Pages.txt", dir);
  expect(write_fitted(book_path, dir, side, side, 2, 4, &small_pages) == 0, "type fitted book");
  expect(type_set_step(TYPE_STEP_MAX) == 1, "fit at the largest step");
  book = ts_open(book_path, dir, NULL, side, side);
  expect(book != NULL, ts_error(NULL));
  if (book) {
    expect(ts_pages(book) > small_pages, "larger type breaks more pages");
    ts_close(book);
  }
  expect(type_set_step(0) == 1, "standard for the screen test");

  nfaces = type_faces(dir, faces, TYPE_FACE_MAX);
  expect(nfaces >= 2 && faces[0].path[0] && faces[1].path[0], "a book face is installed");
  expect(strcmp(faces[0].label, "標準") != 0, "the face list has no 標準 row");
  snprintf(g_type_face, sizeof g_type_face, "%s", nfaces >= 2 ? faces[1].path : "");

  g_page_n = (size_t)side * 400;
  uint8_t *gray = malloc(g_page_n);
  expect(gray != NULL, "type gray");
  if (!gray) {
    return;
  }
  memset(gray, 255, g_page_n);
  AppHost host;
  memset(&host, 0, sizeof host);
  host.w = side;
  host.h = 400;
  host.gray = gray;
  host.full_every = 8;
  host.root = dir;
  host.home = dir;
  host.present = fake_present;
  host.poll = type_poll;
  g_type_step = 0;
  expect(app_run(&host, book_path) == 0, "type screen run");
  expect(g_type_step == 11, "type script finished");
  free(gray);
  remove(book_path);
  char prog[768];
  snprintf(prog, sizeof prog, "%s/progress.tsv", dir);
  remove(prog);
  snprintf(prog, sizeof prog, "%s/type.txt", dir);
  remove(prog);
  type_load(NULL);
  rmdir(dir);
}

/* Package meta says horizontal. The body class in the stylesheet is vertical.
 * An hltr tag stays horizontal. A later untitled chapter stays vertical. */
static void test_writing_mode(void) {
  const char *container =
      "<?xml version=\"1.0\"?>"
      "<container version=\"1.0\" xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\">"
      "<rootfiles>"
      "<rootfile full-path=\"OEBPS/content.opf\" media-type=\"application/oebps-package+xml\"/>"
      "</rootfiles>"
      "</container>";
  const char *css =
      "/* writing-mode: horizontal-tb; */\n"
      ".vrtl { writing-mode: horizontal-tb; }\n"
      ".calibre2 { writing-mode: horizontal-tb; }\n"
      "@media amzn-kf8 {\n"
      "  .calibre { -webkit-writing-mode: vertical-rl; writing-mode: vertical-rl; }\n"
      "}\n"
      ".other { writing-mode: horizontal-tb; }\n";
  const char *opf_css =
      "<?xml version=\"1.0\"?>"
      "<package>"
      "<metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\">"
      "<dc:title>方向</dc:title>"
      "<meta name=\"primary-writing-mode\" content=\"horizontal-lr\"/>"
      "</metadata>"
      "<manifest>"
      "<item id=\"c1\" href=\"chap.xhtml\" media-type=\"application/xhtml+xml\"/>"
      "<item id=\"css\" href=\"stylesheet.css\" media-type=\"text/css\"/>"
      "</manifest>"
      "<spine><itemref idref=\"c1\"/></spine>"
      "</package>";
  const char *opf_two =
      "<?xml version=\"1.0\"?>"
      "<package>"
      "<metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\">"
      "<dc:title>方向</dc:title>"
      "<meta name=\"primary-writing-mode\" content=\"horizontal-lr\"/>"
      "</metadata>"
      "<manifest>"
      "<item id=\"c1\" href=\"chap.xhtml\" media-type=\"application/xhtml+xml\"/>"
      "<item id=\"c2\" href=\"chap2.xhtml\" media-type=\"application/xhtml+xml\"/>"
      "<item id=\"css\" href=\"stylesheet.css\" media-type=\"text/css\"/>"
      "</manifest>"
      "<spine><itemref idref=\"c1\"/><itemref idref=\"c2\"/></spine>"
      "</package>";
  const char *vertical_xhtml =
      "<?xml version=\"1.0\"?>"
      "<html xmlns=\"http://www.w3.org/1999/xhtml\">"
      "<head><link rel=\"stylesheet\" type=\"text/css\" href=\"stylesheet.css\"/></head>"
      "<body class=\"calibre\"><p>あいう</p></body></html>";
  const char *horizontal_xhtml =
      "<?xml version=\"1.0\"?>"
      "<html xmlns=\"http://www.w3.org/1999/xhtml\" class=\"hltr\">"
      "<head><link rel=\"stylesheet\" type=\"text/css\" href=\"stylesheet.css\"/></head>"
      "<body class=\"calibre\"><p>あいう</p></body></html>";
  const char *bare_xhtml =
      "<?xml version=\"1.0\"?>"
      "<html xmlns=\"http://www.w3.org/1999/xhtml\"><body><p>あいう</p></body></html>";
  char dir[] = "/tmp/yomu-mode-XXXXXX";
  char path[256];
  const int w = 240;
  const int h = 240;
  uint8_t *page;
  int bw;
  int bh;
  int guard;
  struct ZipEnt ents[5];
  TsBook *book;

  expect(mkdtemp(dir) != NULL, "writing mode dir");
  page = malloc((size_t)w * (size_t)h);
  expect(page != NULL, "writing mode page");
  if (!page) {
    return;
  }

  snprintf(path, sizeof path, "%s/vertical.epub", dir);
  ents[0].name = "META-INF/container.xml";
  ents[0].data = container;
  ents[0].len = (uint32_t)strlen(container);
  ents[1].name = "OEBPS/content.opf";
  ents[1].data = opf_two;
  ents[1].len = (uint32_t)strlen(opf_two);
  ents[2].name = "OEBPS/stylesheet.css";
  ents[2].data = css;
  ents[2].len = (uint32_t)strlen(css);
  ents[3].name = "OEBPS/chap.xhtml";
  ents[3].data = vertical_xhtml;
  ents[3].len = (uint32_t)strlen(vertical_xhtml);
  ents[4].name = "OEBPS/chap2.xhtml";
  ents[4].data = bare_xhtml;
  ents[4].len = (uint32_t)strlen(bare_xhtml);
  expect(write_stored_parts(path, ents, 5) == 0, "write vertical css epub");
  book = ts_open(path, dir, NULL, w, h);
  expect(book != NULL, ts_error(NULL));
  if (book) {
    expect(ts_rtl(book) == 1, "stylesheet class is vertical");
    memset(page, 255, (size_t)w * (size_t)h);
    expect(ts_render(book, 0, page, w, h) == 1, "render stylesheet chapter");
    ink_box(page, w, h, &bw, &bh);
    expect(bw > 0 && bh > bw, "stylesheet chapter runs down the column");
    guard = 0;
    while (ts_done(book) != 1 && guard < 40) {
      ts_index_step(book);
      guard++;
    }
    expect(ts_done(book) == 1, "stylesheet book finished");
    expect(ts_rtl(book) == 1, "a later chapter stays vertical");
    expect(ts_pages(book) >= 2, "untitled chapter has a page");
    memset(page, 255, (size_t)w * (size_t)h);
    expect(ts_render(book, ts_pages(book) - 1, page, w, h) == 1, "render untitled chapter");
    ink_box(page, w, h, &bw, &bh);
    expect(bw > 0 && bh > bw, "untitled chapter stays in the column");
    ts_close(book);
  }

  snprintf(path, sizeof path, "%s/horizontal.epub", dir);
  ents[1].data = opf_css;
  ents[1].len = (uint32_t)strlen(opf_css);
  ents[3].data = horizontal_xhtml;
  ents[3].len = (uint32_t)strlen(horizontal_xhtml);
  expect(write_stored_parts(path, ents, 4) == 0, "write hltr epub");
  book = ts_open(path, dir, NULL, w, h);
  expect(book != NULL, ts_error(NULL));
  if (book) {
    expect(ts_rtl(book) == 0, "hltr tag stays horizontal");
    memset(page, 255, (size_t)w * (size_t)h);
    expect(ts_render(book, 0, page, w, h) == 1, "render hltr chapter");
    ink_box(page, w, h, &bw, &bh);
    expect(bh > 0 && bw > bh, "hltr chapter runs along the line");
    ts_close(book);
  }
  free(page);
}

/* One kana on a 240 page. Linked CSS and an inline style set the weight and the em. */
static int render_face(const char *dir, const char *name, const char *container, const char *opf, const char *css,
                       const char *xhtml, int *ink, int *bw, int *bh, int *top_gap, int *bot_gap) {
  char path[256];
  struct ZipEnt ents[4];
  uint8_t *page;
  TsBook *book;
  const int w = 240;
  const int h = 240;

  snprintf(path, sizeof path, "%s/%s", dir, name);
  ents[0].name = "META-INF/container.xml";
  ents[0].data = container;
  ents[0].len = (uint32_t)strlen(container);
  ents[1].name = "OEBPS/content.opf";
  ents[1].data = opf;
  ents[1].len = (uint32_t)strlen(opf);
  ents[2].name = "OEBPS/stylesheet.css";
  ents[2].data = css;
  ents[2].len = (uint32_t)strlen(css);
  ents[3].name = "OEBPS/chap.xhtml";
  ents[3].data = xhtml;
  ents[3].len = (uint32_t)strlen(xhtml);
  if (write_stored_parts(path, ents, 4) != 0) {
    return -1;
  }
  book = ts_open(path, dir, NULL, w, h);
  if (!book) {
    fprintf(stderr, "FAIL face open %s: %s\n", name, ts_error(NULL));
    g_failed++;
    return -1;
  }
  page = malloc((size_t)w * (size_t)h);
  if (!page) {
    ts_close(book);
    return -1;
  }
  memset(page, 255, (size_t)w * (size_t)h);
  if (ts_render(book, 0, page, w, h) != 1) {
    fprintf(stderr, "FAIL face render %s\n", name);
    g_failed++;
    free(page);
    ts_close(book);
    return -1;
  }
  if (ink) {
    *ink = ink_count(page, w * h);
  }
  if (bw || bh) {
    int box_w = 0;
    int box_h = 0;
    ink_box(page, w, h, &box_w, &box_h);
    if (bw) {
      *bw = box_w;
    }
    if (bh) {
      *bh = box_h;
    }
  }
  if (top_gap || bot_gap) {
    int min_y = h;
    int max_y = -1;
    int y;
    int x;
    for (y = 0; y < h; y++) {
      for (x = 0; x < w; x++) {
        if (page[(size_t)y * (size_t)w + (size_t)x] >= 250) {
          continue;
        }
        if (y < min_y) {
          min_y = y;
        }
        if (y > max_y) {
          max_y = y;
        }
      }
    }
    if (top_gap) {
      *top_gap = max_y < 0 ? h : min_y;
    }
    if (bot_gap) {
      *bot_gap = max_y < 0 ? h : h - 1 - max_y;
    }
  }
  free(page);
  ts_close(book);
  return 0;
}

/* b and strong are bold. A class rule is bold. 2em is taller. 0.5em of 2em
 * comes back to the body. An inline normal clears the b. */
static void test_face_style(void) {
  const char *container =
      "<?xml version=\"1.0\"?>"
      "<container version=\"1.0\" xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\">"
      "<rootfiles>"
      "<rootfile full-path=\"OEBPS/content.opf\" media-type=\"application/oebps-package+xml\"/>"
      "</rootfiles>"
      "</container>";
  const char *css =
      "strong { font-weight: bold; }\n"
      ".bold { font-weight: bold; }\n"
      ".big { font-size: 2em; }\n"
      ".half { font-size: 0.5em; }\n";
  const char *opf =
      "<?xml version=\"1.0\"?>"
      "<package>"
      "<metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\">"
      "<dc:title>太さ</dc:title>"
      "</metadata>"
      "<manifest>"
      "<item id=\"c1\" href=\"chap.xhtml\" media-type=\"application/xhtml+xml\"/>"
      "<item id=\"css\" href=\"stylesheet.css\" media-type=\"text/css\"/>"
      "</manifest>"
      "<spine><itemref idref=\"c1\"/></spine>"
      "</package>";
  const char *kana = "\xe3\x81\x82";
  const char *head =
      "<?xml version=\"1.0\"?>"
      "<html xmlns=\"http://www.w3.org/1999/xhtml\">"
      "<head><link rel=\"stylesheet\" type=\"text/css\" href=\"stylesheet.css\"/></head><body>";
  char dir[] = "/tmp/yomu-face-XXXXXX";
  char xhtml[640];
  int plain_ink = 0;
  int plain_bh = 0;
  int top_gap = 0;
  int bot_gap = 0;
  int bold_ink = 0;
  int class_ink = 0;
  int strong_ink = 0;
  int big_bh = 0;
  int nest_bh = 0;
  int norm_ink = 0;

  expect(mkdtemp(dir) != NULL, "face style dir");
  snprintf(xhtml, sizeof xhtml, "%s<p>%s</p></body></html>", head, kana);
  expect(render_face(dir, "plain.epub", container, opf, css, xhtml, &plain_ink, NULL, &plain_bh, &top_gap, &bot_gap) ==
             0,
         "plain face page");
  snprintf(xhtml, sizeof xhtml, "%s<p><b>%s</b></p></body></html>", head, kana);
  expect(render_face(dir, "bold.epub", container, opf, css, xhtml, &bold_ink, NULL, NULL, NULL, NULL) == 0,
         "b face page");
  snprintf(xhtml, sizeof xhtml, "%s<p><span class=\"bold\">%s</span></p></body></html>", head, kana);
  expect(render_face(dir, "class.epub", container, opf, css, xhtml, &class_ink, NULL, NULL, NULL, NULL) == 0,
         "class face page");
  snprintf(xhtml, sizeof xhtml, "%s<p><strong>%s</strong></p></body></html>", head, kana);
  expect(render_face(dir, "strong.epub", container, opf, css, xhtml, &strong_ink, NULL, NULL, NULL, NULL) == 0,
         "strong face page");
  snprintf(xhtml, sizeof xhtml, "%s<p><span class=\"big\">%s</span></p></body></html>", head, kana);
  expect(render_face(dir, "big.epub", container, opf, css, xhtml, NULL, NULL, &big_bh, NULL, NULL) == 0,
         "big face page");
  snprintf(xhtml, sizeof xhtml, "%s<p><span class=\"big\"><span class=\"half\">%s</span></span></p></body></html>",
           head, kana);
  expect(render_face(dir, "nest.epub", container, opf, css, xhtml, NULL, NULL, &nest_bh, NULL, NULL) == 0,
         "nested face page");
  snprintf(xhtml, sizeof xhtml, "%s<p><b><span style=\"font-weight: normal\">%s</span></b></p></body></html>", head,
           kana);
  expect(render_face(dir, "normal.epub", container, opf, css, xhtml, &norm_ink, NULL, NULL, NULL, NULL) == 0,
         "normal face page");

  expect(plain_ink > 0 && plain_bh > 0, "plain kana has ink");
  expect(top_gap >= 16 && bot_gap >= 16, "top and bottom stay clear");
  expect(bold_ink > plain_ink, "b is heavier than the body");
  expect(class_ink > plain_ink, "class bold is heavier than the body");
  expect(strong_ink > plain_ink, "strong is heavier than the body");
  expect(big_bh > plain_bh * 3 / 2, "2em is taller than the body");
  expect(nest_bh < big_bh && nest_bh <= plain_bh + plain_bh / 4, "0.5em of 2em stays near the body");
  expect(norm_ink < (plain_ink + bold_ink) / 2, "font-weight normal clears bold");
}

/* 520 files, past the old 512 slot table. Each file is one short paragraph. */
static void test_long_spine(void) {
  const int n = 520;
  const char *container =
      "<?xml version=\"1.0\"?>"
      "<container version=\"1.0\" xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\">"
      "<rootfiles>"
      "<rootfile full-path=\"OEBPS/content.opf\" media-type=\"application/oebps-package+xml\"/>"
      "</rootfiles>"
      "</container>";
  const char *xhtml =
      "<?xml version=\"1.0\"?>"
      "<html xmlns=\"http://www.w3.org/1999/xhtml\"><body><p>A</p></body></html>";
  char dir[] = "/tmp/yomu-spine-XXXXXX";
  char path[256];
  char *opf;
  char *names;
  char *p;
  char *end;
  struct ZipEnt *ents;
  int i;
  int wrote;
  int done = 0;
  int total = 0;
  int guard;
  int page = 0;
  TsBook *book;
  const size_t opf_cap = 96 * 1024;

  expect(mkdtemp(dir) != NULL, "long spine dir");
  opf = malloc(opf_cap);
  names = calloc((size_t)n, 32);
  ents = calloc((size_t)n + 2, sizeof(*ents));
  expect(opf != NULL && names != NULL && ents != NULL, "long spine buffers");
  if (!opf || !names || !ents) {
    free(opf);
    free(names);
    free(ents);
    return;
  }

  p = opf;
  end = opf + opf_cap;
  wrote = snprintf(p, (size_t)(end - p),
                   "<?xml version=\"1.0\"?><package>"
                   "<metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\">"
                   "<dc:title>Long</dc:title></metadata><manifest>");
  expect(wrote > 0 && p + wrote < end, "opf head fits");
  if (wrote > 0 && p + wrote < end) {
    p += wrote;
  }
  for (i = 0; i < n; i++) {
    if (p >= end) {
      break;
    }
    wrote = snprintf(p, (size_t)(end - p),
                     "<item id=\"c%04d\" href=\"c%04d.xhtml\" media-type=\"application/xhtml+xml\"/>", i, i);
    if (wrote < 0 || p + wrote >= end) {
      break;
    }
    p += wrote;
    snprintf(names + (size_t)i * 32, 32, "OEBPS/c%04d.xhtml", i);
  }
  expect(i == n, "opf manifest fits");
  if (i == n && p < end) {
    wrote = snprintf(p, (size_t)(end - p), "</manifest><spine>");
    if (wrote > 0 && p + wrote < end) {
      p += wrote;
    }
  }
  for (i = 0; i < n && p < end; i++) {
    wrote = snprintf(p, (size_t)(end - p), "<itemref idref=\"c%04d\"/>", i);
    if (wrote < 0 || p + wrote >= end) {
      break;
    }
    p += wrote;
  }
  expect(i == n, "opf spine fits");
  if (i == n && p < end) {
    wrote = snprintf(p, (size_t)(end - p), "</spine></package>");
    expect(wrote > 0 && p + wrote < end, "opf tail fits");
  }

  ents[0].name = "META-INF/container.xml";
  ents[0].data = container;
  ents[0].len = (uint32_t)strlen(container);
  ents[1].name = "OEBPS/content.opf";
  ents[1].data = opf;
  ents[1].len = (uint32_t)strlen(opf);
  for (i = 0; i < n; i++) {
    ents[i + 2].name = names + (size_t)i * 32;
    ents[i + 2].data = xhtml;
    ents[i + 2].len = (uint32_t)strlen(xhtml);
  }
  snprintf(path, sizeof path, "%s/long.epub", dir);
  expect(write_stored_parts(path, ents, n + 2) == 0, "write long spine epub");
  book = ts_open(path, dir, NULL, 240, 240);
  expect(book != NULL, "long spine opens");
  if (book) {
    expect(ts_spine(book, &done, &total) == 1 && total == n, "spine longer than 512 is kept");
    guard = 0;
    while (!ts_done(book) && guard < n + 32) {
      if (!ts_index_step(book) && !ts_done(book)) {
        break;
      }
      guard++;
    }
    expect(ts_spine(book, &done, &total) == 1 && done == n && total == n, "every spine file is ingested");
    expect(ts_done(book) == 1, "long spine finishes");
    expect(ts_pages(book) > 512, "pages pass the old cap");
    expect(ts_pages(book) >= n, "each file has a page");
    expect(ts_chapter_count(book) == n, "each file is a chapter");
    expect(ts_open_chapter(book, n - 1, &page) == 1 && page > 512, "the last file opens");
    ts_close(book);
  }
  free(opf);
  free(names);
  free(ents);
}

static int g_here_step;

/* 120x280 is scale 2: header 32, row 48. The cursor is a black bar at x 0. */
static int here_poll(AppHost *host, AppEvent *event) {
  int w = host->w;
  int y0 = 32 * w;
  int y1 = (32 + 48) * w;
  int y2 = (32 + 48 * 2) * w;
  memset(event, 0, sizeof *event);
  switch (g_here_step++) {
    case 0:
      expect(ink_count(host->gray, host->w * host->h) > 0, "later chapter opens on its page");
      event->kind = EV_CONFIRM;
      return 1;
    case 1:
      event->kind = EV_DOWN;
      return 1;
    case 2:
      event->kind = EV_CONFIRM;
      return 1;
    case 3:
      expect(host->gray[0] == 255, "chapter list header stays clear");
      expect(host->gray[y2] == 0, "current chapter is highlighted");
      expect(host->gray[y0] == 255 && host->gray[y1] == 255, "earlier chapters stay clear");
      event->kind = EV_QUIT;
      return -1;
    default:
      expect(0, "current chapter script did not finish");
      event->kind = EV_QUIT;
      return -1;
  }
}

static void test_current_chapter(void) {
  char dir[] = "/tmp/yomu-here-XXXXXX";
  char book_path[256];
  TsBook *book;
  int page = -1;
  int start = -1;
  int end = -1;
  int first = -1;
  const int width = 120;
  const int height = 280;
  expect(mkdtemp(dir) != NULL, "current chapter dir");
  snprintf(book_path, sizeof book_path, "%s/Three.epub", dir);
  expect(write_three_chapter_epub(book_path) == 0, "current chapter epub");
  book = ts_open(book_path, dir, NULL, width, height);
  expect(book != NULL, ts_error(NULL));
  if (!book) {
    remove(book_path);
    rmdir(dir);
    return;
  }
  expect(ts_open_chapter(book, 2, &page) == 1, "third chapter page");
  expect(ts_chapter(book, 0, NULL, 0, &first, &end) == 1 && first >= 0, "first chapter page");
  expect(ts_chapter(book, 2, NULL, 0, &start, &end) == 1 && start == page, "third chapter starts there");
  expect(start > first, "third chapter is a later page");
  expect(progress_save(dir, book_path, page) == 0, "save the third chapter");
  ts_close(book);

  g_page_n = (size_t)width * (size_t)height;
  {
    uint8_t *gray = malloc(g_page_n);
    AppHost host;
    expect(gray != NULL, "current chapter gray");
    if (!gray) {
      remove(book_path);
      rmdir(dir);
      return;
    }
    memset(gray, 255, g_page_n);
    memset(&host, 0, sizeof host);
    host.w = width;
    host.h = height;
    host.gray = gray;
    host.full_every = 8;
    host.root = dir;
    host.home = dir;
    host.present = fake_present;
    host.poll = here_poll;
    g_here_step = 0;
    expect(app_run(&host, book_path) == 0, "current chapter run");
    expect(g_here_step == 4, "current chapter script finished");
    free(gray);
  }
  remove(book_path);
  {
    char prog[256];
    snprintf(prog, sizeof prog, "%s/progress.tsv", dir);
    remove(prog);
  }
  rmdir(dir);
}

static void be16p(uint8_t *p, uint16_t v) {
  p[0] = (uint8_t)(v >> 8);
  p[1] = (uint8_t)v;
}

static void be32p(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)(v >> 24);
  p[1] = (uint8_t)(v >> 16);
  p[2] = (uint8_t)(v >> 8);
  p[3] = (uint8_t)v;
}

static void put_vwi(uint8_t *dst, size_t *n, size_t cap, uint32_t v) {
  uint8_t tmp[5];
  int k = 0;
  tmp[k++] = (uint8_t)(v & 0x7f);
  v >>= 7;
  while (v && k < 5) {
    memmove(tmp + 1, tmp, (size_t)k);
    tmp[0] = (uint8_t)(v & 0x7f);
    k++;
    v >>= 7;
  }
  tmp[k - 1] |= 0x80;
  if (*n + (size_t)k > cap) {
    return;
  }
  memcpy(dst + *n, tmp, (size_t)k);
  *n += (size_t)k;
}

/* Bytes 0x01..0x08 and 0x80..0xFF go out as length-prefixed literals. */
static size_t palmdoc_store(const uint8_t *in, size_t n, uint8_t *out, size_t cap) {
  size_t i = 0;
  size_t o = 0;
  while (i < n) {
    uint8_t c = in[i];
    if (c == 0 || (c >= 0x09 && c <= 0x7f)) {
      if (o >= cap) {
        return 0;
      }
      out[o++] = c;
      i++;
      continue;
    }
    size_t run = 0;
    while (i + run < n && run < 8) {
      uint8_t d = in[i + run];
      if (d == 0 || (d >= 0x09 && d <= 0x7f)) {
        break;
      }
      run++;
    }
    if (run == 0 || o + 1 + run > cap) {
      return 0;
    }
    out[o++] = (uint8_t)run;
    memcpy(out + o, in + i, run);
    o += run;
    i += run;
  }
  return o;
}

static size_t append_exth(uint8_t *dst, size_t cap, const char *title, int fixed) {
  size_t title_n = title ? strlen(title) : 0;
  size_t need = 12;
  size_t p = 12;
  int nrec = 0;
  if (title_n) {
    need += 8 + title_n;
    nrec++;
  }
  if (fixed) {
    need += 12;
    nrec++;
  }
  if (nrec == 0 || need > cap) {
    return 0;
  }
  memcpy(dst, "EXTH", 4);
  be32p(dst + 4, (uint32_t)need);
  be32p(dst + 8, (uint32_t)nrec);
  if (title_n) {
    be32p(dst + p, 99);
    be32p(dst + p + 4, (uint32_t)(8 + title_n));
    memcpy(dst + p + 8, title, title_n);
    p += 8 + title_n;
  }
  if (fixed) {
    be32p(dst + p, 122);
    be32p(dst + p + 4, 12);
    memcpy(dst + p + 8, "true", 4);
    p += 12;
  }
  return p;
}

static int write_palm(const char *path, uint8_t **recs, size_t *lens, int n) {
  FILE *fp;
  uint8_t hdr[78];
  uint32_t data;
  int i;
  fp = fopen(path, "wb");
  if (!fp || n < 1 || n > 16) {
    if (fp) {
      fclose(fp);
    }
    return -1;
  }
  memset(hdr, 0, sizeof hdr);
  memcpy(hdr, "book", 4);
  memcpy(hdr + 60, "BOOK", 4);
  memcpy(hdr + 64, "MOBI", 4);
  be16p(hdr + 76, (uint16_t)n);
  if (fwrite(hdr, 1, 78, fp) != 78) {
    fclose(fp);
    return -1;
  }
  data = 78u + (uint32_t)n * 8u;
  for (i = 0; i < n; i++) {
    uint8_t ent[8];
    memset(ent, 0, sizeof ent);
    be32p(ent, data);
    if (fwrite(ent, 1, 8, fp) != 8) {
      fclose(fp);
      return -1;
    }
    data += (uint32_t)lens[i];
  }
  for (i = 0; i < n; i++) {
    if (lens[i] && fwrite(recs[i], 1, lens[i], fp) != lens[i]) {
      fclose(fp);
      return -1;
    }
  }
  fclose(fp);
  return 0;
}

/* Classic MOBI: uncompressed or PalmDOC, optional one-byte trailing extra. */
static int write_classic_mobi(const char *path, const char *html, const char *title, int compression,
                              int encryption, int trail, int fixed, int encoding) {
  uint8_t comp[4096];
  uint8_t exth[128];
  uint8_t *recs[2];
  size_t lens[2];
  size_t html_n;
  size_t comp_n;
  size_t exth_n;
  size_t title_n;
  size_t total;
  uint8_t *rec;
  uint8_t *text;
  int rc;
  const uint32_t head_len = 0xE8;
  html_n = strlen(html);
  if (html_n == 0 || html_n > 2000) {
    return -1;
  }
  if (compression == 2) {
    comp_n = palmdoc_store((const uint8_t *)html, html_n, comp, sizeof comp);
    if (comp_n == 0) {
      return -1;
    }
  } else {
    memcpy(comp, html, html_n);
    comp_n = html_n;
  }
  text = malloc(comp_n + 1);
  if (!text) {
    return -1;
  }
  memcpy(text, comp, comp_n);
  if (trail) {
    text[comp_n] = 0x81;
  }
  title_n = title ? strlen(title) : 0;
  exth_n = append_exth(exth, sizeof exth, title, fixed);
  total = 16u + head_len + exth_n + title_n;
  rec = calloc(1, total);
  if (!rec) {
    free(text);
    return -1;
  }
  be16p(rec + 0, (uint16_t)compression);
  be32p(rec + 4, (uint32_t)html_n);
  be16p(rec + 8, 1);
  be16p(rec + 10, 4096);
  be16p(rec + 12, (uint16_t)encryption);
  memcpy(rec + 16, "MOBI", 4);
  be32p(rec + 20, head_len);
  be32p(rec + 24, 2);
  be32p(rec + 28, (uint32_t)encoding);
  be32p(rec + 36, 6);
  if (title_n) {
    be32p(rec + 0x54, (uint32_t)(16u + head_len + exth_n));
    be32p(rec + 0x58, (uint32_t)title_n);
    memcpy(rec + 16 + head_len + exth_n, title, title_n);
  }
  be32p(rec + 0x6C, 0xFFFFFFFFu);
  if (exth_n) {
    be32p(rec + 0x80, 0x40);
    memcpy(rec + 16 + head_len, exth, exth_n);
  }
  be16p(rec + 0xC0, 1);
  if (trail) {
    be16p(rec + 0xF2, 2);
  }
  recs[0] = rec;
  lens[0] = total;
  recs[1] = text;
  lens[1] = comp_n + (trail ? 1 : 0);
  rc = write_palm(path, recs, lens, 2);
  free(rec);
  free(text);
  return rc;
}

struct IdxEntW {
  const char *label;
  int has1;
  uint32_t t1;
  int has6;
  uint32_t t6a;
  uint32_t t6b;
};

static int build_indx_main(uint8_t **out, size_t *out_n) {
  uint8_t *rec = calloc(1, 192 + 20);
  uint8_t *tag;
  if (!rec) {
    return -1;
  }
  memcpy(rec, "INDX", 4);
  be32p(rec + 4, 192);
  be32p(rec + 0x18, 1);
  tag = rec + 192;
  memcpy(tag, "TAGX", 4);
  be32p(tag + 4, 20);
  be32p(tag + 8, 1);
  tag[12] = 1;
  tag[13] = 1;
  tag[14] = 1;
  tag[15] = 0;
  tag[16] = 6;
  tag[17] = 2;
  tag[18] = 2;
  tag[19] = 0;
  *out = rec;
  *out_n = 212;
  return 0;
}

static int build_indx_data(const struct IdxEntW *ents, int count, uint8_t **out, size_t *out_n) {
  uint8_t body[512];
  uint16_t offs[4];
  size_t bn = 0;
  size_t idxt;
  size_t total;
  uint8_t *rec;
  int i;
  if (count < 1 || count > 4) {
    return -1;
  }
  for (i = 0; i < count; i++) {
    size_t lab = strlen(ents[i].label);
    size_t start = bn;
    if (lab > 40 || bn + lab + 24 > sizeof body) {
      return -1;
    }
    body[bn++] = (uint8_t)lab;
    memcpy(body + bn, ents[i].label, lab);
    bn += lab;
    body[bn++] = (uint8_t)((ents[i].has1 ? 1 : 0) | (ents[i].has6 ? 2 : 0));
    if (ents[i].has1) {
      put_vwi(body, &bn, sizeof body, ents[i].t1);
    }
    if (ents[i].has6) {
      put_vwi(body, &bn, sizeof body, ents[i].t6a);
      put_vwi(body, &bn, sizeof body, ents[i].t6b);
    }
    offs[i] = (uint16_t)(192 + start);
  }
  idxt = 192 + bn;
  total = idxt + 4 + (size_t)count * 2;
  rec = calloc(1, total);
  if (!rec) {
    return -1;
  }
  memcpy(rec, "INDX", 4);
  be32p(rec + 4, 192);
  be32p(rec + 0x14, (uint32_t)idxt);
  be32p(rec + 0x18, (uint32_t)count);
  memcpy(rec + 192, body, bn);
  memcpy(rec + idxt, "IDXT", 4);
  for (i = 0; i < count; i++) {
    be16p(rec + idxt + 4 + (size_t)i * 2, offs[i]);
  }
  *out = rec;
  *out_n = total;
  return 0;
}

/* KF8 with two parts. The first heading is a fragment inserted into an empty h1. */
static int write_kf8(const char *path) {
  char skel0[640];
  char skel1[640];
  char insert_label[16];
  uint8_t *raw;
  size_t skel0_n;
  size_t skel1_n;
  size_t raw_n;
  uint32_t insert_at;
  uint8_t *recs[7];
  size_t lens[7];
  uint8_t *head;
  uint8_t exth[64];
  size_t exth_n;
  const uint32_t head_len = 0xF0;
  struct IdxEntW skel_ents[2];
  struct IdxEntW frag_ent;
  int rc = -1;
  int i;
  const char *frag = "猫";
  snprintf(skel0, sizeof skel0,
           "<html><body><style>body{writing-mode:vertical-rl}</style><h1></h1><p>%s</p></body></html>", k_line);
  snprintf(skel1, sizeof skel1, "<html><body><h1>犬</h1><p>%s</p></body></html>", k_line);
  skel0_n = strlen(skel0);
  skel1_n = strlen(skel1);
  if (!strstr(skel0, "</h1>")) {
    return -1;
  }
  insert_at = (uint32_t)(strstr(skel0, "</h1>") - skel0);
  snprintf(insert_label, sizeof insert_label, "%u", insert_at);
  raw_n = skel0_n + 3 + skel1_n;
  raw = malloc(raw_n);
  if (!raw) {
    return -1;
  }
  memcpy(raw, skel0, skel0_n);
  memcpy(raw + skel0_n, frag, 3);
  memcpy(raw + skel0_n + 3, skel1, skel1_n);
  exth_n = append_exth(exth, sizeof exth, "猫", 0);
  head = calloc(1, 16u + head_len + exth_n);
  if (!head || exth_n == 0) {
    free(raw);
    free(head);
    return -1;
  }
  be16p(head + 0, 1);
  be32p(head + 4, (uint32_t)raw_n);
  be16p(head + 8, 1);
  be16p(head + 10, 4096);
  memcpy(head + 16, "MOBI", 4);
  be32p(head + 20, head_len);
  be32p(head + 24, 248);
  be32p(head + 28, 65001);
  be32p(head + 36, 8);
  be32p(head + 0x6C, 0xFFFFFFFFu);
  be32p(head + 0x80, 0x40);
  be32p(head + 0xC0, 2);
  be32p(head + 0xF8, 5);
  be32p(head + 0xFC, 3);
  memcpy(head + 16 + head_len, exth, exth_n);
  memset(recs, 0, sizeof recs);
  recs[0] = head;
  lens[0] = 16u + head_len + exth_n;
  recs[1] = raw;
  lens[1] = raw_n;
  recs[2] = calloc(1, 20);
  if (!recs[2]) {
    goto done;
  }
  memcpy(recs[2], "FDST", 4);
  be32p(recs[2] + 8, 1);
  be32p(recs[2] + 12, 0);
  be32p(recs[2] + 16, (uint32_t)raw_n);
  lens[2] = 20;
  if (build_indx_main(&recs[3], &lens[3]) != 0 || build_indx_main(&recs[5], &lens[5]) != 0) {
    goto done;
  }
  skel_ents[0].label = "0";
  skel_ents[0].has1 = 1;
  skel_ents[0].t1 = 1;
  skel_ents[0].has6 = 1;
  skel_ents[0].t6a = 0;
  skel_ents[0].t6b = (uint32_t)skel0_n;
  skel_ents[1].label = "1";
  skel_ents[1].has1 = 0;
  skel_ents[1].t1 = 0;
  skel_ents[1].has6 = 1;
  skel_ents[1].t6a = (uint32_t)(skel0_n + 3);
  skel_ents[1].t6b = (uint32_t)skel1_n;
  frag_ent.label = insert_label;
  frag_ent.has1 = 0;
  frag_ent.t1 = 0;
  frag_ent.has6 = 1;
  frag_ent.t6a = 0;
  frag_ent.t6b = 3;
  if (build_indx_data(skel_ents, 2, &recs[4], &lens[4]) != 0 || build_indx_data(&frag_ent, 1, &recs[6], &lens[6]) != 0) {
    goto done;
  }
  rc = write_palm(path, recs, lens, 7);
done:
  for (i = 0; i < 7; i++) {
    free(recs[i]);
  }
  return rc;
}

static int page_ink(TsBook *book, int w, int h) {
  uint8_t *page;
  int n;
  page = malloc((size_t)w * (size_t)h);
  if (!page) {
    return -1;
  }
  memset(page, 255, (size_t)w * (size_t)h);
  if (!ts_render(book, 0, page, w, h)) {
    free(page);
    return -1;
  }
  n = ink_count(page, w * h);
  free(page);
  return n;
}

static int book_label_is(YomuBook *books, int count, const char *label) {
  int i;
  for (i = 0; i < count; i++) {
    if (strcmp(books[i].label, label) == 0) {
      return 1;
    }
  }
  return 0;
}

static void expect_mobi_reject(const char *path, const char *home, const char *token, const char *note) {
  AppHost host;
  State st;
  TsBook *book = ts_open(path, home, NULL, 120, 80);
  expect(book == NULL, token);
  if (book) {
    ts_close(book);
  }
  expect(strcmp(ts_error(NULL), token) == 0, token);
  memset(&host, 0, sizeof host);
  host.home = home;
  host.w = 120;
  host.h = 80;
  memset(&st, 0, sizeof st);
  st.host = &host;
  expect(open_book(&st, path) != 0, note);
  expect(strcmp(st.note, note) == 0, note);
}

static void test_mobi(void) {
  char dir[] = "/tmp/yomu-mobi-XXXXXX";
  char html[512];
  char path[256];
  char name[32];
  YomuBook *books = NULL;
  int count = 0;
  int trunc = 0;
  int start = 0;
  int end = 0;
  int page = -1;
  TsBook *book;
  const int w = 120;
  const int h = 80;
  expect(mkdtemp(dir) != NULL, "mobi dir");
  snprintf(html, sizeof html, "<html><body><h1>吾輩</h1><p>%s</p></body></html>", k_line);

  snprintf(path, sizeof path, "%s/Neko.mobi", dir);
  expect(write_classic_mobi(path, html, "猫", 1, 0, 0, 0, 65001) == 0, "write uncompressed mobi");
  snprintf(path, sizeof path, "%s/Inu.azw3", dir);
  expect(write_text(path, "later") == 0, "azw3 placeholder");
  snprintf(path, sizeof path, "%s/Tori.azw", dir);
  expect(write_classic_mobi(path, html, "猫", 1, 0, 0, 0, 65001) == 0, "write azw");
  snprintf(path, sizeof path, "%s/Skip.xtch", dir);
  expect(write_text(path, "bitmap") == 0, "mobi scan xtch");
  expect(yomu_scan_books(dir, &books, &count, &trunc) == 0, "mobi scan");
  expect(count == 3, "library lists mobi azw3 and azw");
  expect(book_label_is(books, count, "Neko") && book_label_is(books, count, "Inu") &&
             book_label_is(books, count, "Tori"),
         "palm stems drop the extension");
  yomu_free_books(books, count);

  snprintf(path, sizeof path, "%s/Neko.mobi", dir);
  book = ts_open(path, dir, NULL, w, h);
  expect(book != NULL, ts_error(NULL));
  if (book) {
    expect(strcmp(ts_title(book), "猫") == 0, "mobi title");
    expect(ts_rtl(book) == 1, "mobi is vertical");
    expect(ts_chapter_count(book) == 1, "mobi chapter");
    expect(ts_chapter(book, 0, name, sizeof name, &start, &end) == 1 && strcmp(name, "吾輩") == 0, "mobi heading");
    expect(page_ink(book, w, h) > 20, "mobi page has ink");
    ts_close(book);
  }

  snprintf(path, sizeof path, "%s/Tori.azw", dir);
  book = ts_open(path, dir, NULL, w, h);
  expect(book != NULL, ts_error(NULL));
  if (book) {
    expect(strcmp(ts_title(book), "猫") == 0, "azw title");
    ts_close(book);
  }

  snprintf(path, sizeof path, "%s/Palm.mobi", dir);
  expect(write_classic_mobi(path, html, "猫", 2, 0, 1, 0, 65001) == 0, "write palmdoc mobi");
  book = ts_open(path, dir, NULL, w, h);
  expect(book != NULL, ts_error(NULL));
  if (book) {
    expect(ts_chapter(book, 0, name, sizeof name, &start, &end) == 1 && strcmp(name, "吾輩") == 0,
           "palmdoc heading survives the trailer");
    expect(page_ink(book, w, h) > 20, "palmdoc page has ink");
    ts_close(book);
  }

  snprintf(path, sizeof path, "%s/Inu.azw3", dir);
  expect(write_kf8(path) == 0, "write kf8");
  book = ts_open(path, dir, NULL, w, h);
  expect(book != NULL, ts_error(NULL));
  if (book) {
    expect(strcmp(ts_title(book), "猫") == 0, "kf8 title");
    expect(ts_rtl(book) == 1, "kf8 is vertical");
    expect(ts_chapter_count(book) == 2, "kf8 lists both chapters");
    expect(ts_done(book) == 0, "kf8 is not fully indexed yet");
    expect(ts_chapter(book, 0, name, sizeof name, &start, &end) == 1 && strcmp(name, "猫") == 0 && start >= 0,
           "kf8 first chapter");
    start = 0;
    expect(ts_chapter(book, 1, name, sizeof name, &start, &end) == 1 && strcmp(name, "犬") == 0 && start < 0,
           "kf8 second chapter waits");
    expect(page_ink(book, w, h) > 20, "kf8 page has ink");
    expect(ts_open_chapter(book, 1, &page) == 1 && page >= 0, "kf8 second chapter opens");
    expect(ts_chapter(book, 1, name, sizeof name, &start, &end) == 1 && start == page, "kf8 open lands on 犬");
    ts_close(book);
  }

  snprintf(path, sizeof path, "%s/Locked.azw3", dir);
  expect(write_classic_mobi(path, html, "猫", 1, 2, 0, 0, 65001) == 0, "write drm");
  expect_mobi_reject(path, dir, "drm", "この本は保護されています");
  snprintf(path, sizeof path, "%s/Fixed.azw3", dir);
  expect(write_classic_mobi(path, html, "猫", 1, 0, 0, 1, 65001) == 0, "write fixed");
  expect_mobi_reject(path, dir, "fixed layout", "この本は固定レイアウトです");
  snprintf(path, sizeof path, "%s/Utf16.mobi", dir);
  expect(write_classic_mobi(path, "x", "x", 1, 0, 0, 0, 65002) == 0, "write utf16");
  expect_mobi_reject(path, dir, "encoding", "この文字コードは未対応です");
}

static int g_clock_presents;
static int g_clock_full;
static int g_clock_x, g_clock_y, g_clock_w, g_clock_h;

static void clock_present(AppHost *host, int x, int y, int w, int h, int full) {
  (void)host;
  g_clock_presents++;
  g_clock_full = full;
  g_clock_x = x;
  g_clock_y = y;
  g_clock_w = w;
  g_clock_h = h;
}

static int rect_ink(const uint8_t *gray, int stride, int x, int y, int w, int h) {
  int n = 0;
  int yy;
  int xx;
  for (yy = y; yy < y + h; yy++) {
    for (xx = x; xx < x + w; xx++) {
      if (gray[(size_t)yy * (size_t)stride + (size_t)xx] < 250) {
        n++;
      }
    }
  }
  return n;
}

/* 400px is scale 2. HH:MM is 80px and sits between yomu and the count. */
static void test_library_clock(void) {
  const int width = 400;
  const int height = 200;
  int x = 0;
  int y = 0;
  int w = 0;
  int h = 0;
  int wait;
  Layout layout;
  uint8_t *gray;
  AppHost host;
  State st;
  uint8_t *narrow;
  AppHost small;
  State tiny;
  yomu_font_open();
  layout = layout_of(width, height);
  expect(library_clock_box(&layout, width, "0", &x, &y, &w, &h) == 1, "wide shelf has a clock");
  expect(w == 80 && h == 16 && y == 8 && x == 160, "clock is centered in the header");
  layout = layout_of(120, height);
  expect(library_clock_box(&layout, 120, "0", &x, &y, &w, &h) == 0, "narrow shelf keeps the title");

  gray = malloc((size_t)width * (size_t)height);
  expect(gray != NULL, "clock gray");
  if (!gray) {
    return;
  }
  memset(gray, 255, (size_t)width * (size_t)height);
  memset(&host, 0, sizeof host);
  host.w = width;
  host.h = height;
  host.gray = gray;
  host.present = clock_present;
  memset(&st, 0, sizeof st);
  st.host = &host;
  st.screen = SCR_LIB;
  g_clock_presents = 0;
  paint_library(&st);
  expect(g_clock_presents == 0, "painting the shelf does not refresh early");
  expect(st.clock_min > 0, "shelf remembers the minute");
  expect(rect_ink(gray, width, 160, 8, 80, 16) > 0, "header shows the time");
  expect(gray[8 * width + 159] == 255, "the clock stays clear of the title");
  wait = library_clock_wait_ms(&st);
  expect(wait >= 1 && wait <= 60000, "the clock waits until the next minute");

  st.clock_min -= 1;
  expect(refresh_library_clock(&st) == 1, "a new minute updates the clock");
  expect(g_clock_presents == 1 && g_clock_full == 0, "the minute is a partial refresh");
  expect(g_clock_x == 160 && g_clock_y == 8 && g_clock_w == 80 && g_clock_h == 16, "only the clock is refreshed");
  expect(refresh_library_clock(&st) == 0 && g_clock_presents == 1, "the same minute stays put");
  expect(clock_offset_set(84) == 0, "tokyo offset");
  expect(refresh_library_clock(&st) == 1 && g_clock_presents == 2, "a new offset redraws the clock");
  clock_offset_load(NULL);
  free(gray);

  narrow = malloc(120 * 200);
  expect(narrow != NULL, "narrow clock gray");
  if (!narrow) {
    return;
  }
  memset(narrow, 255, 120 * 200);
  memset(&small, 0, sizeof small);
  small.w = 120;
  small.h = height;
  small.gray = narrow;
  small.present = clock_present;
  memset(&tiny, 0, sizeof tiny);
  tiny.host = &small;
  tiny.screen = SCR_LIB;
  paint_library(&tiny);
  expect(tiny.clock_min == -1, "a narrow shelf hides the clock");
  expect(library_clock_wait_ms(&tiny) == -1, "a hidden clock does not wake the poll");
  free(narrow);
}

int main(void) {
  test_rotate();
  test_progress_and_scan();
  test_japanese();
  test_typeset();
  test_mobi();
  test_writing_mode();
  test_face_style();
  test_long_spine();
  test_ui();
  test_reader_gestures();
  test_sleep_stays();
  test_wake_repaints();
  test_tap_buttons();
  test_library_clock();
  test_chapter_ranges();
  test_current_chapter();
  test_index_continues();
  test_index_idle();
  test_jump_page();
  test_font_faces();
  test_type_page();
  test_type();
  test_fileserver();
  if (g_failed) {
    fprintf(stderr, "%d failed\n", g_failed);
    return 1;
  }
  printf("ok\n");
  return 0;
}
