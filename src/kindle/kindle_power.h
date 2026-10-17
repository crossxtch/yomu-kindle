#ifndef YOMU_KINDLE_POWER_H
#define YOMU_KINDLE_POWER_H

#include "kindle_input.h"

/* Wait until powerd has slept and woken, then return.
 * input may be NULL. An ungrabbed power key is left to powerd: this does
 * not send powerButton or wakeUp, and it does not continue awesome.
 * A grabbed key is forwarded once, because powerd never saw that press. */
void kindle_power_sleep(KindleInput *input);

/* Turn the frontlight off for this process and remember the lipc level.
 * Paperwhite 4 and later, including Paperwhite 11, go dark when
 * flIntensity is 0. Wake turns the light back on, so sleep calls this
 * again. kindle_frontlight_release puts the saved level back. */
void kindle_frontlight_hold_off(void);
void kindle_frontlight_release(void);

/* Wake turns the light back on. Wait briefly, then force it off again. */
void kindle_power_after_wake(void);

/* 1 when com.lab126.powerd state reads as active. */
int kindle_power_panel_active(void);

#endif
