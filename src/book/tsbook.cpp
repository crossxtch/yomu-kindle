#include "tsbook.h"

#include "TypesetBook.h"
#include "typeface.h"

#include <BoardConfig.h>

extern "C" {
#include "progress.h"
}

#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

struct TsBook {
  TypesetBook book;
  TsProgressFn progress;
  void *progress_ctx;
};

static void spine_progress(void *ctx, uint16_t done, uint16_t total) {
  TsBook *ts = static_cast<TsBook *>(ctx);
  if (ts && ts->progress) {
    ts->progress(ts->progress_ctx, done, total);
  }
}

static char g_ts_error[160] = "not open";

static void set_error(const char *text) {
  snprintf(g_ts_error, sizeof g_ts_error, "%s", text && text[0] ? text : "open");
}

static int file_exists(const char *path) {
  struct stat st;
  return path && path[0] && stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static int font_error(const char *err) {
  if (!err || !err[0]) {
    return 1;
  }
  return strcmp(err, "font missing") == 0 || strcmp(err, "freetype") == 0 ||
         strcmp(err, "no japanese glyphs") == 0;
}

extern "C" TsBook *ts_open(const char *path, const char *home, const char *font, int w, int h) {
  return ts_open_progress(path, home, font, w, h, NULL, NULL);
}

extern "C" TsBook *ts_open_progress(const char *path, const char *home, const char *font, int w, int h,
                                    TsProgressFn progress, void *progress_ctx) {
  static const char *faces[] = {
      "/var/local/font/mnt/ja_font/fonts/TsukuMinPr5-Medium.ttf",
      "/var/local/font/mnt/ja_font/fonts/TsukuMinPr5-Bold.ttf",
      "/usr/java/lib/fonts/TBMinchoMedium_213.ttf",
      "/usr/java/lib/fonts/TBMinchoBold_213.ttf",
      "/System/Library/Fonts/Supplemental/Arial Unicode.ttf",
      "/Library/Fonts/Arial Unicode.ttf",
      "/System/Library/Fonts/Hiragino Sans GB.ttc",
      "/usr/share/fonts/opentype/noto/NotoSerifCJK-Regular.ttc",
      NULL,
  };
  const char *cands[24];
  int nc = 0;
  int i;
  int resume = 0;
  unsigned place_atom = 0;
  int saved_slot = -1;
  const char *fail = "font missing";
  int explicit_font;
  TsBook *ts;

  if (!path || !path[0] || w < 8 || h < 8) {
    set_error("page size");
    return NULL;
  }
  type_load(home);
  explicit_font = font && font[0];
  if (explicit_font) {
    cands[nc++] = font;
  } else {
    const char *saved = type_font();
    const char *env = getenv("YOMU_FONT");
    if (saved && saved[0] && nc < 24) {
      saved_slot = nc;
      cands[nc++] = saved;
    }
    if (env && env[0] && nc < 24) {
      cands[nc++] = env;
    }
    for (i = 0; faces[i] && nc < 24; i++) {
      cands[nc++] = faces[i];
    }
  }

  ts_hal_set_root(home);
  ts_board_set(w, h);
  ts_font_set_em(type_em(w, h));
  if (progress_load_place(home, path, &resume, &place_atom) != 0 || resume < 0) {
    resume = 0;
    place_atom = 0;
  }

  ts = new (std::nothrow) TsBook();
  if (!ts) {
    set_error("nomem");
    return NULL;
  }
  ts->progress = progress;
  ts->progress_ctx = progress_ctx;
  for (i = 0; i < nc; i++) {
    if (!file_exists(cands[i])) {
      continue;
    }
    const int face_index = i == saved_slot ? type_face_index() : -1;
    if (ts->book.open(path, progress ? spine_progress : nullptr, progress ? ts : nullptr, cands[i],
                      static_cast<uint32_t>(resume), place_atom, face_index)) {
      g_ts_error[0] = '\0';
      return ts;
    }
    fail = ts->book.lastError();
    if (explicit_font || !font_error(fail)) {
      break;
    }
  }
  set_error(fail);
  delete ts;
  return NULL;
}

extern "C" void ts_close(TsBook *book) { delete book; }

extern "C" int ts_pages(TsBook *book) {
  if (!book || !book->book.isOpen()) {
    return 0;
  }
  return book->book.pageCount();
}

extern "C" int ts_done(TsBook *book) { return book && book->book.isOpen() && book->book.buildFinished(); }

extern "C" int ts_spine(const TsBook *book, int *done, int *total) {
  if (!book || !book->book.isOpen()) {
    return 0;
  }
  if (done) {
    *done = book->book.spineDone();
  }
  if (total) {
    *total = book->book.spineTotal();
  }
  return 1;
}

extern "C" int ts_index_step(TsBook *book) {
  if (!book || !book->book.isOpen()) {
    return 0;
  }
  return book->book.indexStep();
}

extern "C" int ts_render(TsBook *book, int global_page, uint8_t *gray, int w, int h) {
  if (!book || !gray || global_page < 0) {
    return 0;
  }
  return book->book.renderGray(gray, w, h, static_cast<uint32_t>(global_page)) ? 1 : 0;
}

extern "C" int ts_show_at(const TsBook *book) {
  if (!book || !book->book.isOpen()) {
    return 0;
  }
  const uint32_t page = book->book.showAt();
  return page > 2147483647u ? 2147483647 : static_cast<int>(page);
}

extern "C" unsigned ts_page_atom(const TsBook *book, int page) {
  if (!book || !book->book.isOpen() || page < 0) {
    return 0;
  }
  const uint32_t front = book->book.pagesBefore();
  const uint32_t span = book->book.indexedSpan();
  if (static_cast<uint32_t>(page) < front || static_cast<uint32_t>(page) >= front + span) {
    return 0;
  }
  const uint32_t atom = book->book.atomForPage(static_cast<uint32_t>(page));
  return atom >= 4 ? atom : 0;
}

extern "C" int ts_chapter_count(TsBook *book) {
  if (!book || !book->book.isOpen()) {
    return 0;
  }
  const std::vector<ts::ChapterInfo> &chs = book->book.getChapters();
  int n = 0;
  for (size_t i = 0; i < chs.size(); i++) {
    if (!chs[i].name.empty()) {
      n++;
    }
  }
  return n;
}

static int chapter_mark(const std::vector<ts::ChapterInfo> &chs, int index) {
  int seen = 0;
  for (size_t i = 0; i < chs.size(); i++) {
    if (chs[i].name.empty()) {
      continue;
    }
    if (seen == index) {
      return static_cast<int>(i);
    }
    seen++;
  }
  return -1;
}

extern "C" int ts_chapter(TsBook *book, int index, char *name, size_t name_n, int *start, int *end) {
  if (!book || !book->book.isOpen() || index < 0) {
    return 0;
  }
  const std::vector<ts::ChapterInfo> &chs = book->book.getChapters();
  const int mark = chapter_mark(chs, index);
  if (mark < 0) {
    return 0;
  }
  const ts::ChapterInfo &ch = chs[static_cast<size_t>(mark)];
  if (name && name_n > 0) {
    snprintf(name, name_n, "%s", ch.name.c_str());
  }
  if (!ch.ready) {
    if (start) {
      *start = -1;
    }
    if (end) {
      *end = -1;
    }
    return 1;
  }
  if (start) {
    *start = ch.startPage;
  }
  if (end) {
    *end = ch.endKnown ? ch.endPage : -1;
  }
  return 1;
}

extern "C" int ts_open_chapter(TsBook *book, int index, int *page) {
  if (!book || !book->book.isOpen() || index < 0) {
    return 0;
  }
  const int mark = chapter_mark(book->book.getChapters(), index);
  if (mark < 0 || mark > 65535) {
    return 0;
  }
  uint32_t global = 0;
  if (!book->book.ensureChapter(static_cast<uint16_t>(mark), global)) {
    return 0;
  }
  if (page) {
    *page = global > 2147483647u ? 2147483647 : static_cast<int>(global);
  }
  return 1;
}

extern "C" int ts_rtl(TsBook *book) { return book && book->book.isOpen() && book->book.vertical(); }

extern "C" const char *ts_error(TsBook *book) {
  if (!book) {
    return g_ts_error;
  }
  const char *err = book->book.lastError();
  return err ? err : "";
}

extern "C" const char *ts_title(TsBook *book) {
  if (!book || !book->book.isOpen()) {
    return "";
  }
  const char *title = book->book.title();
  return title ? title : "";
}
