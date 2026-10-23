#ifndef YOMU_PROGRESS_H
#define YOMU_PROGRESS_H

/* Remembers the last 0-based page per book path under home/progress.tsv.
 * A line is "page\tpath" or "page\tatom\tpath". atom is the atom-file offset
 * of that page, or 0 when the page number is all that was saved.
 * progress_load returns 0 and writes *page when the book is known.
 */
int progress_load(const char *home, const char *book_path, int *page);
int progress_save(const char *home, const char *book_path, int page);
int progress_load_place(const char *home, const char *book_path, int *page, unsigned *atom);
int progress_save_place(const char *home, const char *book_path, int page, unsigned atom);

#endif
