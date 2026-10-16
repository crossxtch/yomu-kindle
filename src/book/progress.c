#include "progress.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int path_ok(const char *path) {
  if (!path || !path[0]) {
    return 0;
  }
  return strchr(path, '\t') == NULL && strchr(path, '\n') == NULL && strchr(path, '\r') == NULL;
}

static int ensure_dir(const char *path) {
  struct stat st;
  if (stat(path, &st) == 0) {
    return S_ISDIR(st.st_mode) ? 0 : -1;
  }
  if (mkdir(path, 0755) == 0 || errno == EEXIST) {
    return 0;
  }
  return -1;
}

static int progress_path(const char *home, char *out, size_t out_n) {
  if (!home || !home[0]) {
    return -1;
  }
  int n = snprintf(out, out_n, "%s/progress.tsv", home);
  if (n < 0 || (size_t)n >= out_n) {
    return -1;
  }
  return 0;
}

int progress_load(const char *home, const char *book_path, int *page) {
  if (!page || !path_ok(book_path)) {
    return -1;
  }
  char file[768];
  if (progress_path(home, file, sizeof file) != 0) {
    return -1;
  }
  FILE *fp = fopen(file, "r");
  if (!fp) {
    return -1;
  }
  char line[1200];
  int found = 0;
  int value = 0;
  while (fgets(line, sizeof line, fp)) {
    char *tab = strchr(line, '\t');
    if (!tab) {
      continue;
    }
    *tab = '\0';
    char *stored = tab + 1;
    size_t len = strlen(stored);
    while (len > 0 && (stored[len - 1] == '\n' || stored[len - 1] == '\r')) {
      stored[--len] = '\0';
    }
    if (strcmp(stored, book_path) == 0) {
      value = atoi(line);
      found = 1;
    }
  }
  fclose(fp);
  if (!found) {
    return -1;
  }
  *page = value;
  return 0;
}

int progress_save(const char *home, const char *book_path, int page) {
  if (!path_ok(book_path) || page < 0) {
    return -1;
  }
  if (ensure_dir(home) != 0) {
    return -1;
  }
  char file[768];
  char tmp[780];
  if (progress_path(home, file, sizeof file) != 0) {
    return -1;
  }
  int ntmp = snprintf(tmp, sizeof tmp, "%s.tmp", file);
  if (ntmp < 0 || (size_t)ntmp >= sizeof tmp) {
    return -1;
  }

  char *old = NULL;
  size_t old_n = 0;
  FILE *in = fopen(file, "r");
  if (in) {
    if (fseeko(in, 0, SEEK_END) == 0) {
      off_t sz = ftello(in);
      if (sz > 0 && sz < 1024 * 1024) {
        old = malloc((size_t)sz + 1);
        if (old && fseeko(in, 0, SEEK_SET) == 0) {
          old_n = fread(old, 1, (size_t)sz, in);
          old[old_n] = '\0';
        }
      }
    }
    fclose(in);
  }

  FILE *out = fopen(tmp, "w");
  if (!out) {
    free(old);
    return -1;
  }
  int replaced = 0;
  if (old) {
    char *cursor = old;
    while (*cursor) {
      char *nl = strchr(cursor, '\n');
      size_t len = nl ? (size_t)(nl - cursor) : strlen(cursor);
      char *tab = memchr(cursor, '\t', len);
      int skip = 0;
      if (tab) {
        size_t path_len = len - (size_t)(tab + 1 - cursor);
        if (path_len == strlen(book_path) && memcmp(tab + 1, book_path, path_len) == 0) {
          skip = 1;
          replaced = 1;
        }
      }
      if (!skip && len > 0) {
        fwrite(cursor, 1, len, out);
        fputc('\n', out);
      }
      if (!nl) {
        break;
      }
      cursor = nl + 1;
    }
  }
  fprintf(out, "%d\t%s\n", page, book_path);
  (void)replaced;
  if (fflush(out) != 0 || fsync(fileno(out)) != 0) {
    fclose(out);
    free(old);
    remove(tmp);
    return -1;
  }
  fclose(out);
  free(old);
  if (rename(tmp, file) != 0) {
    remove(tmp);
    return -1;
  }
  return 0;
}
