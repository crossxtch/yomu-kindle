#include "app.h"
#include "kindle_fb.h"
#include "kindle_input.h"
#include "kindle_power.h"
#include "log.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

typedef struct Plat {
  KindleFb *fb;
  KindleInput *input;
} Plat;

static void on_signal(int sig) {
  (void)sig;
  kindle_input_stop();
}

static void on_exit_frontlight(void) {
  kindle_frontlight_release();
}

static void present(AppHost *host, int x, int y, int w, int h, int full) {
  Plat *plat = host->user;
  kindle_fb_present(plat->fb, host->gray, x, y, w, h, full);
}

static int poll_event(AppHost *host, AppEvent *event) {
  Plat *plat = host->user;
  return kindle_input_poll(plat->input, event, host->poll_ms);
}

static void sleep_device(AppHost *host) {
  Plat *plat = host ? host->user : NULL;
  kindle_power_sleep(plat ? plat->input : NULL);
}

static int env_int(const char *name, int fallback) {
  const char *value = getenv(name);
  if (!value || !value[0]) {
    return fallback;
  }
  return atoi(value);
}

int main(int argc, char **argv) {
  signal(SIGTERM, on_signal);
  signal(SIGINT, on_signal);
  signal(SIGPIPE, SIG_IGN);

  const char *home = getenv("YOMU_HOME");
  if (!home || !home[0]) {
    home = "/mnt/us/yomu";
  }
  const char *root = getenv("YOMU_ROOT");
  if (!root || !root[0]) {
    root = "/mnt/us/yomu/books";
  }
  mkdir(home, 0755);
  mkdir(root, 0755);

  char log_path[512];
  snprintf(log_path, sizeof log_path, "%s/yomu.log", home);
  yomu_log_open(log_path);
  yomu_log("yomu start root %s", root);
  atexit(on_exit_frontlight);
  kindle_frontlight_hold_off();

  KindleFb *fb = NULL;
  if (kindle_fb_open(&fb) != 0) {
    yomu_log("framebuffer open failed; the jailbreak hotfix may not be installed");
    return 1;
  }
  KindleInput *input = NULL;
  if (kindle_input_open(&input, kindle_fb_native_width(fb), kindle_fb_native_height(fb), kindle_fb_rotation(fb)) !=
      0) {
    kindle_fb_close(fb);
    return 1;
  }

  int width = kindle_fb_width(fb);
  int height = kindle_fb_height(fb);
  uint8_t *gray = malloc((size_t)width * (size_t)height);
  if (!gray) {
    kindle_input_close(input);
    kindle_fb_close(fb);
    return 1;
  }
  memset(gray, 255, (size_t)width * (size_t)height);

  const char *initial = NULL;
  for (int i = 1; i < argc; i++) {
    if (argv[i][0] != '-') {
      initial = argv[i];
      break;
    }
  }

  Plat plat = {fb, input};
  AppHost host;
  memset(&host, 0, sizeof host);
  host.w = width;
  host.h = height;
  host.gray = gray;
  host.full_every = env_int("YOMU_FULL_EVERY", 20);
  host.root = root;
  host.home = home;
  host.user = &plat;
  host.present = present;
  host.poll = poll_event;
  host.sleep = sleep_device;

  int rc = app_run(&host, initial);
  free(gray);
  kindle_input_close(input);
  kindle_fb_close(fb);
  yomu_log("yomu exit %d", rc);
  return rc;
}
