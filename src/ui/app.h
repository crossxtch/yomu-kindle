#ifndef YOMU_APP_H
#define YOMU_APP_H

#include "books.h"

#include <stdint.h>

typedef enum AppEvKind {
  EV_NONE = 0,
  EV_TAP,
  EV_SWIPE_LEFT,
  EV_SWIPE_RIGHT,
  EV_SWIPE_UP,
  EV_SWIPE_DOWN,
  EV_LEFT,
  EV_RIGHT,
  EV_UP,
  EV_DOWN,
  EV_CONFIRM,
  EV_BACK,
  EV_QUIT,
  EV_PAGE_NEXT,
  EV_PAGE_PREV,
  EV_SLEEP,
  /* The panel went through the system screensaver without this process
   * asking. The screen underneath may have been overwritten. */
  EV_WAKE
} AppEvKind;

typedef struct AppEvent {
  AppEvKind kind;
  int x;  /* release point */
  int y;
  int x0; /* press point */
  int y0;
  int ms; /* how long the finger was down; 0 is a quick tap */
} AppEvent;

typedef struct AppHost {
  int w;
  int h;
  uint8_t *gray; /* w * h bytes, stride w */
  int full_every;
  const char *root;
  const char *home;
  void *user;
  /* w or h of 0 means the whole screen. full requests a flashing refresh. */
  void (*present)(struct AppHost *host, int x, int y, int w, int h, int full);
  /* 1 = event, 0 = retry, -1 = leave the app. */
  int (*poll)(struct AppHost *host, AppEvent *event);
  /* Optional. A power press asks the device to sleep. NULL keeps running. */
  void (*sleep)(struct AppHost *host);
  /* How long poll waits. -1 blocks. 0 returns when nothing is waiting.
   * The app sets this before each poll. */
  int poll_ms;
} AppHost;

/* initial_path may be NULL. Opens that book after the library scan when set. */
int app_run(AppHost *host, const char *initial_path);

#endif
