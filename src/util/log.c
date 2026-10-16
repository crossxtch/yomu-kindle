#include "log.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/stat.h>
#include <time.h>

static FILE *g_log;
static pthread_mutex_t g_log_mu = PTHREAD_MUTEX_INITIALIZER;

void yomu_log_open(const char *path) {
  pthread_mutex_lock(&g_log_mu);
  if (g_log) {
    fclose(g_log);
    g_log = NULL;
  }
  if (path && path[0]) {
    struct stat st;
    const char *mode = "a";
    if (stat(path, &st) == 0 && st.st_size > 200000) {
      mode = "w";
    }
    g_log = fopen(path, mode);
  }
  pthread_mutex_unlock(&g_log_mu);
}

void yomu_log(const char *fmt, ...) {
  char buf[640];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  pthread_mutex_lock(&g_log_mu);
  fprintf(stderr, "%s\n", buf);
  if (g_log) {
    fprintf(g_log, "%s\n", buf);
    fflush(g_log);
  }
  pthread_mutex_unlock(&g_log_mu);
}

int64_t yomu_ms(void) {
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
    return 0;
  }
  return (int64_t)ts.tv_sec * 1000 + (int64_t)ts.tv_nsec / 1000000;
}
