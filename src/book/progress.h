#ifndef YOMU_PROGRESS_H
#define YOMU_PROGRESS_H

/* Remembers the last 0-based page per book path under home/progress.tsv.
 * progress_load returns 0 and writes *page when the book is known.
 */
int progress_load(const char *home, const char *book_path, int *page);
int progress_save(const char *home, const char *book_path, int page);

#endif
