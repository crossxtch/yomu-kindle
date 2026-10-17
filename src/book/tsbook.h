#ifndef YOMU_TSBOOK_H
#define YOMU_TSBOOK_H

/* Japanese EPUB and plain text. Layout is the shared typesetter. Glyphs are
 * FreeType gray, 255 white and 0 black, in the caller's row-major buffer. */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct TsBook TsBook;

/* done is spine items already ingested. total is the spine length, or 0 before
 * that length is known. Called on this thread. */
typedef void (*TsProgressFn)(void *ctx, int done, int total);

/* font NULL tries the face saved from the 文字 screen, then YOMU_FONT, then
 * the Kindle mincho faces, then a few desktop faces. The saved
 * type size is applied either way. home holds the page index and type.txt.
 * w and h are the page. */
TsBook *ts_open(const char *path, const char *home, const char *font, int w, int h);
/* progress may be NULL. The same hook runs again when a later chapter is opened.
 * It is not called while the rest of the book is indexed in the background. */
TsBook *ts_open_progress(const char *path, const char *home, const char *font, int w, int h,
                         TsProgressFn progress, void *progress_ctx);
void ts_close(TsBook *book);

/* Page count grows as later chapters are indexed. 0 when book is NULL. */
int ts_pages(TsBook *book);
/* 1 when every page of the book is indexed. */
int ts_done(TsBook *book);
/* 1 when book is open. done and total may be NULL. total is 0 for a text file. */
int ts_spine(const TsBook *book, int *done, int *total);
/* Lay out or ingest one more step of the pages after the open. 1 when more
 * forward work remains. */
int ts_index_step(TsBook *book);
/* 1 on success. global_page is 0-based. */
int ts_render(TsBook *book, int global_page, uint8_t *gray, int w, int h);

int ts_chapter_count(TsBook *book);
/* name may be NULL. start and end are 0-based and inclusive.
 * start < 0: this chapter is not typeset yet. end < 0: the end page is not
 * known yet (the next chapter has no page, or the book is not finished). */
int ts_chapter(TsBook *book, int index, char *name, size_t name_n, int *start, int *end);
/* Typeset up to this chapter and return its 0-based page. 1 on success. */
int ts_open_chapter(TsBook *book, int index, int *page);
/* 1 when the book is vertical, so the left side of the screen is forward. */
int ts_rtl(TsBook *book);

const char *ts_error(TsBook *book);
const char *ts_title(TsBook *book);

#ifdef __cplusplus
}
#endif

#endif
