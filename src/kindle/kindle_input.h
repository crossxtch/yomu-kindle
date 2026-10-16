#ifndef YOMU_KINDLE_INPUT_H
#define YOMU_KINDLE_INPUT_H

#include "app.h"

typedef struct KindleInput KindleInput;

/* width and height are the panel's native pixels. rot is 0..3. */
int kindle_input_open(KindleInput **out, int width, int height, int rot);
void kindle_input_close(KindleInput *input);

/* Wait up to timeout_ms for an event. -1 blocks. 0 returns at once.
 * Returns 1 on an event, 0 on timeout, or -1 when the process was asked to stop. */
int kindle_input_poll(KindleInput *input, AppEvent *event, int timeout_ms);

/* Wait up to timeout_ms. Returns 1 if a power press arrived, 0 on timeout,
 * or -1 when the process was asked to stop. Other events are dropped. */
int kindle_input_take_power(KindleInput *input, int timeout_ms);

/* Ignore power presses for ms of awake time. Monotonic time stops in suspend. */
void kindle_input_ignore_power(KindleInput *input, int ms);

/* 1 when the press that started this sleep was read from a grabbed device.
 * powerd did not see that press, so the caller has to forward it. */
int kindle_input_power_from_grabbed(const KindleInput *input);

/* Async-signal-safe. Wakes poll with -1. */
void kindle_input_stop(void);

#endif
