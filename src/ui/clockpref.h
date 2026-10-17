#ifndef YOMU_CLOCKPREF_H
#define YOMU_CLOCKPREF_H

/* Shelf clock offset, stored in home/clock.txt.
 * Quarter-hours from UTC, biased by 48 so 48 is UTC+0.
 * 0 is UTC−12 and 104 is UTC+14, in 15-minute steps.
 * The Kindle libc zone is UTC. The transfer page sets this value. */

#include <stddef.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Missing file or a NULL home resets to UTC+0. */
void clock_offset_load(const char *home);

int clock_offset_q(void);

/* 0 on success. A value outside 0..104 is rejected.
 * Persists when a home was loaded. */
int clock_offset_set(int q);

/* HH:MM for utc plus offset q. 0 when q or the buffer is unusable. */
int clock_format(time_t utc, int q, char *buf, size_t n);

#ifdef __cplusplus
}
#endif

#endif
