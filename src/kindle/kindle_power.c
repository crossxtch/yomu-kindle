#include "kindle_power.h"

#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int64_t clock_ms(clockid_t id) {
  struct timespec ts;
  if (clock_gettime(id, &ts) != 0) {
    return -1;
  }
  return (int64_t)ts.tv_sec * 1000 + (int64_t)ts.tv_nsec / 1000000;
}

static void trim_line(char *buf) {
  size_t len = strlen(buf);
  while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r' || buf[len - 1] == ' ')) {
    buf[--len] = '\0';
  }
}

/* rc is the command status. buf holds the first line of combined output. */
static int lipc_line(const char *cmd, char *buf, size_t n) {
  if (buf && n > 0) {
    buf[0] = '\0';
  }
  FILE *fp = popen(cmd, "r");
  if (!fp) {
    return -1;
  }
  if (buf && n > 0 && !fgets(buf, (int)n, fp)) {
    buf[0] = '\0';
  }
  int rc = pclose(fp);
  if (buf && n > 0) {
    trim_line(buf);
  }
  if (rc == -1) {
    return -1;
  }
  if (WIFEXITED(rc)) {
    return WEXITSTATUS(rc);
  }
  return -1;
}

static int read_power_state(char *buf, size_t n) {
  return lipc_line("lipc-get-prop -s com.lab126.powerd state 2>&1", buf, n) == 0 && buf[0] != '\0';
}

static int state_active(const char *state) {
  return strcasecmp(state, "active") == 0;
}

/* PW3 and older leave the LED on when lipc flIntensity is 0. KOReader
 * writes these nodes in that case. Newer panels, including Paperwhite 11
 * (fp9966), honor flIntensity 0, and writing their backlight sysfs races
 * powerd. */
static const char *const k_fl_sysfs[] = {
    "/sys/class/backlight/max77696-bl/brightness",
    "/sys/devices/system/fl_tps6116x/fl_tps6116x0/fl_intensity",
};

static int g_fl_held;
static int g_fl_saved = -1;
static int g_fl_released;
static unsigned g_fl_sysfs_mask;

static int parse_level(const char *text, int *out) {
  char *end = NULL;
  long level;
  if (!text || !text[0]) {
    return 0;
  }
  level = strtol(text, &end, 10);
  if (end == text || *end != '\0' || level < 0 || level > 255) {
    return 0;
  }
  *out = (int)level;
  return 1;
}

static int read_int_file(const char *path, int *out) {
  FILE *fp = fopen(path, "r");
  char buf[32];
  if (!fp) {
    return 0;
  }
  if (!fgets(buf, (int)sizeof buf, fp)) {
    fclose(fp);
    return 0;
  }
  fclose(fp);
  trim_line(buf);
  return parse_level(buf, out);
}

static int write_int_file(const char *path, int level) {
  FILE *fp = fopen(path, "w");
  if (!fp) {
    return 0;
  }
  fprintf(fp, "%d\n", level);
  return fclose(fp) == 0;
}

static int read_fl_intensity(int *out) {
  char buf[80];
  int rc = lipc_line("lipc-get-prop -i com.lab126.powerd flIntensity 2>&1", buf, sizeof buf);
  if (rc != 0 || !parse_level(buf, out)) {
    yomu_log("frontlight unread rc %d %s", rc, buf[0] ? buf : "empty");
    return 0;
  }
  return 1;
}

static void set_fl_intensity(int level) {
  char cmd[96];
  char out[120];
  snprintf(cmd, sizeof cmd, "lipc-set-prop -i com.lab126.powerd flIntensity %d 2>&1", level);
  int rc = lipc_line(cmd, out, sizeof out);
  yomu_log("frontlight flIntensity %d rc %d %s", level, rc, out[0] ? out : "ok");
}

/* lipc 0 is a no-op on the legacy driver. If the node is still lit, zero it. */
static void sysfs_frontlight_off(void) {
  size_t i;
  for (i = 0; i < sizeof k_fl_sysfs / sizeof k_fl_sysfs[0]; i++) {
    int hw = 0;
    if (!read_int_file(k_fl_sysfs[i], &hw) || hw <= 0) {
      continue;
    }
    if (write_int_file(k_fl_sysfs[i], 0)) {
      g_fl_sysfs_mask |= 1u << i;
      yomu_log("frontlight sysfs %s %d -> 0", k_fl_sysfs[i], hw);
    }
  }
}

static void remember_frontlight(int level) {
  FILE *fp;
  g_fl_saved = level;
  yomu_log("frontlight was %d", level);
  if (level <= 0) {
    return;
  }
  fp = fopen("/var/tmp/yomu-fl", "w");
  if (!fp) {
    yomu_log("frontlight save failed");
    return;
  }
  fprintf(fp, "%d\n", level);
  if (fclose(fp) != 0) {
    yomu_log("frontlight save failed");
  }
}

void kindle_frontlight_hold_off(void) {
  if (!g_fl_held) {
    int level = 0;
    if (read_fl_intensity(&level)) {
      remember_frontlight(level);
    }
    g_fl_held = 1;
  }
  set_fl_intensity(0);
  sysfs_frontlight_off();
}

void kindle_frontlight_release(void) {
  size_t i;
  if (g_fl_released || !g_fl_held) {
    return;
  }
  g_fl_released = 1;
  g_fl_held = 0;
  if (g_fl_saved > 0) {
    set_fl_intensity(g_fl_saved);
    for (i = 0; i < sizeof k_fl_sysfs / sizeof k_fl_sysfs[0]; i++) {
      if (g_fl_sysfs_mask & (1u << i)) {
        write_int_file(k_fl_sysfs[i], g_fl_saved);
      }
    }
    yomu_log("frontlight restore %d", g_fl_saved);
  }
  unlink("/var/tmp/yomu-fl");
}

/* powerd turns the light back on as the panel wakes. Wait so this write
 * lands after that restore, which is the race KOReader loses on purpose. */
static void frontlight_after_wake(void) {
  struct timespec pause = {0, 300000000L};
  if (!g_fl_held) {
    return;
  }
  nanosleep(&pause, NULL);
  kindle_frontlight_hold_off();
  yomu_log("frontlight held off after wake");
}

static int env_yes(const char *name) {
  const char *value = getenv(name);
  return value && strcmp(value, "yes") == 0;
}

/* One synthetic press. A second powerButton toggles the Kindle awake again.
 * preventScreenSaver swallows the press, so it is cleared and left off.
 * Idle sleep is held off with screenSaverTimeout in yomu.sh. */
static void request_sleep(void) {
  char out[120];
  int rc = lipc_line("lipc-set-prop -i com.lab126.powerd preventScreenSaver 0 2>&1", out, sizeof out);
  yomu_log("sleep preventScreenSaver %d %s", rc, out);
  rc = lipc_line("lipc-set-prop -i com.lab126.powerd powerButton 1 2>&1", out, sizeof out);
  yomu_log("sleep powerButton %d %s", rc, out);
  if (rc != 0) {
    rc = lipc_line("powerd_test -p 2>&1", out, sizeof out);
    yomu_log("sleep powerd_test %d %s", rc, out);
  }
}

/* lipc-set-prop returns before the CPU stops. Wait until powerd has left
 * the active state and come back, so the wake refresh does not run while
 * the panel is still going to sleep. The give-up applies only before sleep
 * starts. Monotonic time stops in suspend.
 * forward_wake is for a grabbed key, which powerd never sees. The Paperwhite
 * 11 key is not grabbed: powerd already toggles, and a wakeUp here cancels
 * the suspend. */
static int wait_asleep_then_awake(KindleInput *input, int give_up_ms, int forward_wake) {
  int64_t mono0 = clock_ms(CLOCK_MONOTONIC);
  int64_t boot0 = clock_ms(CLOCK_BOOTTIME);
  int saw_down = 0;
  int missed = 0;
  int logged_suspend = 0;
  int64_t down_at = -1;
  char state[80];
  char last[80];
  state[0] = '\0';
  last[0] = '\0';
  for (;;) {
    int got = read_power_state(state, sizeof state);
    int64_t mono = clock_ms(CLOCK_MONOTONIC);
    int64_t boot = clock_ms(CLOCK_BOOTTIME);
    int64_t grew = -1;
    if (boot0 >= 0 && mono0 >= 0 && boot >= 0 && mono >= 0) {
      grew = (boot - mono) - (boot0 - mono0);
    }
    /* A second of boot time that monotonic did not count is a real suspend.
     * Two back-to-back clock reads cannot stall that long. */
    int suspended = grew > 1000;
    if (suspended && !logged_suspend) {
      yomu_log("sleep suspended boot +%lld ms", (long long)grew);
      logged_suspend = 1;
    }
    if (!got) {
      const char *shown = state[0] ? state : "unread";
      if (strcmp(last, shown) != 0) {
        yomu_log("sleep state %s", shown);
        snprintf(last, sizeof last, "%s", shown);
      }
      if ((saw_down || suspended) && ++missed > 50) {
        yomu_log("sleep state unread, giving up");
        return 1;
      }
    } else {
      missed = 0;
      if (strcmp(state, last) != 0) {
        yomu_log("sleep state %s", state);
        snprintf(last, sizeof last, "%s", state);
      }
      if (!state_active(state)) {
        if (!saw_down && mono >= 0) {
          down_at = mono;
        }
        saw_down = 1;
      } else if (saw_down || suspended) {
        yomu_log("sleep woke");
        return 1;
      }
    }
    if (!saw_down && !suspended && mono0 >= 0 && mono >= 0 && mono > mono0 + give_up_ms) {
      yomu_log("sleep still %s after %lld ms boot +%lld", got ? state : "unread",
                 (long long)(mono - mono0), (long long)grew);
      return 0;
    }
    int key = input ? kindle_input_take_power(input, 100) : 0;
    if (key < 0) {
      yomu_log("sleep wait interrupted down %d", saw_down || suspended);
      return saw_down || suspended;
    }
    if (!input) {
      struct timespec pause = {0, 100000000};
      nanosleep(&pause, NULL);
    }
    /* An ungrabbed key is powerd's. Forwarding it, or the same click's extra
     * press, is what turned screenSaver back into active on the Paperwhite.
     * A grabbed key is forwarded only after the screensaver has been up. */
    int held = suspended || (down_at >= 0 && mono >= 0 && mono >= down_at + 1500);
    if (key == 1 && forward_wake && held) {
      char out[120];
      int rc = lipc_line("lipc-set-prop -i com.lab126.powerd wakeUp 1 2>&1", out, sizeof out);
      yomu_log("sleep wakeUp %d %s", rc, out);
    } else if (key == 1) {
      yomu_log("sleep key ignored down %d", saw_down);
    }
  }
}

void kindle_power_sleep(KindleInput *input) {
  int grabbed = kindle_input_power_from_grabbed(input);
  char state[80];
  char prevent[80];
  char timeout[80];
  state[0] = prevent[0] = timeout[0] = '\0';
  lipc_line("lipc-get-prop -s com.lab126.powerd state 2>&1", state, sizeof state);
  lipc_line("lipc-get-prop -i com.lab126.powerd preventScreenSaver 2>&1", prevent, sizeof prevent);
  lipc_line("lipc-get-prop -i com.lab126.powerd screenSaverTimeout 2>&1", timeout, sizeof timeout);
  const char *awesome = getenv("AWESOME_STOPPED");
  const char *framework = getenv("STOP_FRAMEWORK");
  yomu_log("sleep begin grabbed %d state %s prevent %s timeout %s awesome %s framework %s", grabbed,
             state[0] ? state : "unread", prevent[0] ? prevent : "unread", timeout[0] ? timeout : "unread",
             awesome && awesome[0] ? awesome : "?", framework && framework[0] ? framework : "?");
  /* awesome stays stopped. Continuing it on this Paperwhite left screenSaver
   * and returned powerd to active. KOReader leaves it stopped unless the
   * stock ad screensaver has to draw. */
  int woke = 0;
  if (grabbed) {
    request_sleep();
    woke = wait_asleep_then_awake(input, 8000, 1);
  } else {
    woke = wait_asleep_then_awake(input, 8000, 0);
  }
  if (!woke) {
    yomu_log("sleep did not start");
    if (env_yes("STOP_FRAMEWORK")) {
      yomu_log("sleep framework is stopped");
    }
  }
  if (input) {
    kindle_input_ignore_power(input, 2000);
  }
  frontlight_after_wake();
}
