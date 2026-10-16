#pragma once

extern "C" {
#include "log.h"
}

#include <time.h>

/* crossjp logs through these macros. yomu writes them to the same log. */

#define LOG_ERR(tag, fmt, ...) yomu_log(tag ": " fmt, ##__VA_ARGS__)
#define LOG_INF(tag, fmt, ...) yomu_log(tag ": " fmt, ##__VA_ARGS__)
#define LOG_DBG(tag, fmt, ...) yomu_log(tag ": " fmt, ##__VA_ARGS__)

inline unsigned long millis(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (unsigned long)(ts.tv_sec * 1000ul + ts.tv_nsec / 1000000ul);
}

/* The typesetter yields between pages on the ESP32. Nothing else is waiting. */
inline void yield() {}
