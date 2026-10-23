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

/* Path is the last field. Two fields are page and path. Three are page, atom, path. */
static int field_path(const char *line, size_t len, const char **path, size_t *path_len) {
  const char *tab = memchr(line, '\t', len);
  const char *rest;
  size_t rest_len;
  const char *tab2;
  if (!tab) {
    return 0;
  }
  rest = tab + 1;
  rest_len = len - (size_t)(rest - line);
  tab2 = memchr(rest, '\t', rest_len);
  if (tab2) {
    *path = tab2 + 1;
    *path_len = len - (size_t)(*path - line);
  } else {
    *path = rest;
    *path_len = rest_len;
  }
  return *path_len > 0;
}

static int parse_place(char *line, int *page, unsigned *atom, char **path) {
  char *tab = strchr(line, '\t');
  char *rest;
  char *tab2;
  if (!tab) {
    return 0;
  }
  *tab = '\0';
  rest = tab + 1;
  tab2 = strchr(rest, '\t');
  *page = atoi(line);
  if (!tab2) {
    *atom = 0;
    *path = rest;
    return 1;
  }
  *tab2 = '\0';
  *atom = (unsigned)strtoul(rest, NULL, 10);
  *path = tab2 + 1;
  return 1;
}

int progress_load_place(const char *home, const char *book_path, int *page, unsigned *atom) {
  if (!page || !atom || !path_ok(book_path)) {
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
  unsigned where = 0;
  while (fgets(line, sizeof line, fp)) {
    int got_page = 0;
    unsigned got_atom = 0;
    char *stored = NULL;
    size_t len = strlen(line);
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
      line[--len] = '\0';
    }
    if (!parse_place(line, &got_page, &got_atom, &stored) || !stored) {
      continue;
    }
    if (strcmp(stored, book_path) == 0) {
      value = got_page;
      where = got_atom;
      found = 1;
    }
  }
  fclose(fp);
  if (!found) {
    return -1;
  }
  *page = value;
  *atom = where;
  return 0;
}

int progress_load(const char *home, const char *book_path, int *page) {
  unsigned atom = 0;
  return progress_load_place(home, book_path, page, &atom);
}

int progress_save_place(const char *home, const char *book_path, int page, unsigned atom) {
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
        const char *stored = NULL;
        size_t path_len = 0;
        if (field_path(cursor, len, &stored, &path_len) && path_len == strlen(book_path) &&
            memcmp(stored, book_path, path_len) == 0) {
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
  fprintf(out, "%d\t%u\t%s\n", page, atom, book_path);
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

int progress_save(const char *home, const char *book_path, int page) {
  return progress_save_place(home, book_path, page, 0);
}
