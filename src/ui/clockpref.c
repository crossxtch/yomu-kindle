#include "clockpref.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char g_home[768];
static atomic_int g_q = 48;

static int q_ok(int q) { return q >= 0 && q <= 104; }

static int clock_path(char *out, size_t out_n) {
  int n;
  if (!g_home[0]) {
    return -1;
  }
  n = snprintf(out, out_n, "%s/clock.txt", g_home);
  if (n < 0 || (size_t)n >= out_n) {
    return -1;
  }
  return 0;
}

static int clock_save(void) {
  char file[800];
  char tmp[800];
  FILE *fp;
  if (clock_path(file, sizeof file) != 0) {
    return -1;
  }
  snprintf(tmp, sizeof tmp, "%s.tmp", file);
  fp = fopen(tmp, "w");
  if (!fp) {
    return -1;
  }
  fprintf(fp, "offset %d\n", atomic_load(&g_q));
  if (fclose(fp) != 0) {
    remove(tmp);
    return -1;
  }
  if (rename(tmp, file) != 0) {
    remove(tmp);
    return -1;
  }
  return 0;
}

void clock_offset_load(const char *home) {
  char file[800];
  FILE *fp;
  atomic_store(&g_q, 48);
  g_home[0] = '\0';
  if (!home || !home[0]) {
    return;
  }
  snprintf(g_home, sizeof g_home, "%s", home);
  if (clock_path(file, sizeof file) != 0) {
    g_home[0] = '\0';
    return;
  }
  fp = fopen(file, "r");
  if (!fp) {
    return;
  }
  char line[64];
  while (fgets(line, sizeof line, fp)) {
    int q = 48;
    if (sscanf(line, "offset %d", &q) == 1 && q_ok(q)) {
      atomic_store(&g_q, q);
    }
  }
  fclose(fp);
}

int clock_offset_q(void) { return atomic_load(&g_q); }

int clock_offset_set(int q) {
  int prev;
  if (!q_ok(q)) {
    return -1;
  }
  prev = atomic_load(&g_q);
  atomic_store(&g_q, q);
  if (g_home[0] && clock_save() != 0) {
    atomic_store(&g_q, prev);
    return -1;
  }
  return 0;
}

int clock_format(time_t utc, int q, char *buf, size_t n) {
  time_t local;
  struct tm tm;
  if (!buf || n < 6 || !q_ok(q)) {
    return 0;
  }
  local = utc + (time_t)(q - 48) * 15 * 60;
  if (gmtime_r(&local, &tm) == NULL) {
    return 0;
  }
  if (strftime(buf, n, "%H:%M", &tm) == 0) {
    return 0;
  }
  return 1;
}
