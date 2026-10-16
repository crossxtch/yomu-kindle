#ifndef YOMU_LOG_H
#define YOMU_LOG_H

#include <stdint.h>

void yomu_log_open(const char *path);
void yomu_log(const char *fmt, ...);

/* Monotonic milliseconds. 0 if the clock cannot be read. */
int64_t yomu_ms(void);

#endif
