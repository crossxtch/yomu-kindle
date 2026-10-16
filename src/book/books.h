#ifndef YOMU_BOOKS_H
#define YOMU_BOOKS_H

#include <stddef.h>

/* One book the library can open. label is the UTF-8 filename stem, or empty
 * when the row should say "本N". Only .epub and .txt are listed. */
typedef struct YomuBook {
  char *path;
  char *label;
} YomuBook;

int yomu_scan_books(const char *root, YomuBook **books, int *count, int *truncated);
void yomu_free_books(YomuBook *books, int count);

/* 1 when text has two ASCII letters or digits, or any other Unicode character. */
int yomu_text_useful(const char *text);
void yomu_book_caption(const YomuBook *book, int index, char *dst, size_t dst_n);

#endif
