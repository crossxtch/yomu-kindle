#include "books.h"

#include "utf8.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#define BOOK_MAX 400
#define LABEL_N 256

int yomu_text_useful(const char *text) {
  int alnum = 0;
  int cp;
  const char *p = text ? text : "";
  while ((cp = yomu_utf8_next(&p)) >= 0) {
    if (cp >= 128 && cp != 0xFFFD) {
      return 1;
    }
    if ((cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z') || (cp >= '0' && cp <= '9')) {
      alnum++;
    }
  }
  return alnum >= 2;
}

static void copy_visible(char *dst, size_t dst_n, const char *src) {
  size_t j = 0;
  const char *p = src ? src : "";
  const char *start = p;
  int cp;
  if (!dst || dst_n == 0) {
    return;
  }
  while ((cp = yomu_utf8_next(&p)) >= 0) {
    size_t n;
    if (cp < 32 || cp == 0x7F || cp == 0xFFFD) {
      start = p;
      continue;
    }
    n = (size_t)(p - start);
    if (j + n >= dst_n) {
      break;
    }
    memcpy(dst + j, start, n);
    j += n;
    start = p;
  }
  while (j > 0 && dst[j - 1] == ' ') {
    j--;
  }
  dst[j] = '\0';
}

void yomu_book_caption(const YomuBook *book, int index, char *dst, size_t dst_n) {
  if (book->label && book->label[0]) {
    snprintf(dst, dst_n, "%s", book->label);
  } else {
    snprintf(dst, dst_n, "本%d", index + 1);
  }
}

static int ends_with(const char *name, const char *ext) {
  size_t n = strlen(name);
  size_t e = strlen(ext);
  if (n < e) {
    return 0;
  }
  return strcasecmp(name + (n - e), ext) == 0;
}

static int ends_book(const char *name) { return ends_with(name, ".epub") || ends_with(name, ".txt"); }

static void stem_label(char *dst, size_t dst_n, const char *path) {
  const char *base = strrchr(path, '/');
  base = base ? base + 1 : path;
  char stem[LABEL_N];
  snprintf(stem, sizeof stem, "%s", base);
  size_t len = strlen(stem);
  if (len >= 5 && strcasecmp(stem + (len - 5), ".epub") == 0) {
    stem[len - 5] = '\0';
  } else if (len >= 4 && strcasecmp(stem + (len - 4), ".txt") == 0) {
    stem[len - 4] = '\0';
  }
  if (yomu_text_useful(stem)) {
    copy_visible(dst, dst_n, stem);
  } else if (dst_n > 0) {
    dst[0] = '\0';
  }
}

static int add_book(YomuBook **books, int *count, int *cap, const char *path, int *truncated) {
  if (*count >= BOOK_MAX) {
    *truncated = 1;
    return 0;
  }
  if (*count >= *cap) {
    int next = *cap == 0 ? 16 : *cap * 2;
    YomuBook *grown = realloc(*books, (size_t)next * sizeof(*grown));
    if (!grown) {
      return -1;
    }
    *books = grown;
    *cap = next;
  }
  YomuBook *book = &(*books)[*count];
  memset(book, 0, sizeof(*book));
  book->path = strdup(path);
  book->label = calloc(1, LABEL_N);
  if (!book->path || !book->label) {
    free(book->path);
    free(book->label);
    return -1;
  }
  stem_label(book->label, LABEL_N, path);
  (*count)++;
  return 0;
}

static int scan_dir(const char *dir, int depth, YomuBook **books, int *count, int *cap, int *truncated);

static int consider(const char *path, int depth, const char *name, YomuBook **books, int *count, int *cap,
                    int *truncated) {
  struct stat st;
  if (lstat(path, &st) != 0 || S_ISLNK(st.st_mode)) {
    return 0;
  }
  if (S_ISDIR(st.st_mode)) {
    if (depth == 0 && (strcmp(name, "system") == 0 || strcmp(name, "lost+found") == 0 ||
                       strcmp(name, "extensions") == 0)) {
      return 0;
    }
    if (*truncated) {
      return 0;
    }
    return scan_dir(path, depth + 1, books, count, cap, truncated);
  }
  if (S_ISREG(st.st_mode) && ends_book(name)) {
    return add_book(books, count, cap, path, truncated);
  }
  return 0;
}

static int scan_dir(const char *dir, int depth, YomuBook **books, int *count, int *cap, int *truncated) {
  if (depth > 8) {
    return 0;
  }
  DIR *dp = opendir(dir);
  if (!dp) {
    return 0;
  }
  struct dirent *de;
  int rc = 0;
  while ((de = readdir(dp)) != NULL) {
    if (de->d_name[0] == '.') {
      continue;
    }
    char path[1024];
    int n = snprintf(path, sizeof path, "%s/%s", dir, de->d_name);
    if (n < 0 || (size_t)n >= sizeof path) {
      continue;
    }
    rc = consider(path, depth, de->d_name, books, count, cap, truncated);
    if (rc != 0) {
      break;
    }
  }
  closedir(dp);
  return rc;
}

static int book_cmp(const void *a, const void *b) {
  const YomuBook *ba = a;
  const YomuBook *bb = b;
  const char *la = (ba->label && ba->label[0]) ? ba->label : ba->path;
  const char *lb = (bb->label && bb->label[0]) ? bb->label : bb->path;
  int c = strcasecmp(la, lb);
  if (c != 0) {
    return c;
  }
  return strcmp(ba->path, bb->path);
}

int yomu_scan_books(const char *root, YomuBook **books, int *count, int *truncated) {
  if (!books || !count) {
    return -1;
  }
  *books = NULL;
  *count = 0;
  if (truncated) {
    *truncated = 0;
  }
  if (!root || !root[0]) {
    return -1;
  }
  int trunc = 0;
  int cap = 0;
  struct stat st;
  if (lstat(root, &st) != 0) {
    return -1;
  }
  int rc = 0;
  if (S_ISREG(st.st_mode)) {
    const char *slash = strrchr(root, '/');
    const char *name = slash ? slash + 1 : root;
    if (ends_book(name)) {
      rc = add_book(books, count, &cap, root, &trunc);
    }
  } else if (S_ISDIR(st.st_mode)) {
    rc = scan_dir(root, 0, books, count, &cap, &trunc);
  } else {
    return -1;
  }
  if (rc != 0) {
    yomu_free_books(*books, *count);
    *books = NULL;
    *count = 0;
    return -1;
  }
  if (*count > 1) {
    qsort(*books, (size_t)*count, sizeof(**books), book_cmp);
  }
  if (truncated) {
    *truncated = trunc;
  }
  return 0;
}

void yomu_free_books(YomuBook *books, int count) {
  if (!books) {
    return;
  }
  for (int i = 0; i < count; i++) {
    free(books[i].path);
    free(books[i].label);
  }
  free(books);
}
