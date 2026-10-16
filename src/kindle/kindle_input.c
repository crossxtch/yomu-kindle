#include "kindle_input.h"

#include "log.h"
#include "rotate.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#ifndef KEY_BACK
#define KEY_BACK 158
#endif

/* 32-bit Kindle userspace delivers 16-byte input events (two 32-bit times).
 * A 64-bit layout would desynchronize the stream, so this is fixed here.
 */
struct KoEvent {
  int32_t sec;
  int32_t usec;
  uint16_t type;
  uint16_t code;
  int32_t value;
} __attribute__((packed));

_Static_assert(sizeof(struct KoEvent) == 16, "kindle evdev event");

#define DEV_MAX 16

typedef struct Dev {
  int fd;
  int min_x, max_x, min_y, max_y;
  int has_touch;
  int down;
  int need_origin;
  int grabbed;
  int sx, sy, cx, cy;
  char name[64];
} Dev;

struct KindleInput {
  Dev devs[DEV_MAX];
  int ndevs;
  int nat_w;
  int nat_h;
  int width;
  int height;
  int rot;
  int swap;
  int flip_x;
  int flip_y;
  int thresh;
  /* Drops the press that wakes the Kindle, and a second press in this many
   * awake milliseconds. Monotonic time stops during suspend, not while the
   * screensaver is still up, so sleep re-arms this when it returns. */
  int64_t power_ignore_until_ms;
  /* Set for the press that became EV_SLEEP. */
  int power_from_grabbed;
};

static volatile sig_atomic_t g_stop;

void kindle_input_stop(void) {
  g_stop = 1;
}

static int bit_set(const unsigned char *bits, int bit) {
  return (bits[bit / 8] >> (bit % 8)) & 1;
}

static int skip_name(const char *name) {
  if (!name || !name[0]) {
    return 0;
  }
  char lower[64];
  snprintf(lower, sizeof lower, "%s", name);
  for (char *p = lower; *p; p++) {
    if (*p >= 'A' && *p <= 'Z') {
      *p = (char)(*p - 'A' + 'a');
    }
  }
  return strstr(lower, "accel") || strstr(lower, "gyro") || strstr(lower, "hall") || strstr(lower, "als") ||
         strstr(lower, "light") || strstr(lower, "temp") || strstr(lower, "battery") || strstr(lower, "headset");
}

static int64_t mono_ms(void) {
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
    return 0;
  }
  return (int64_t)ts.tv_sec * 1000 + (int64_t)ts.tv_nsec / 1000000;
}

void kindle_input_ignore_power(KindleInput *input, int ms) {
  if (!input || ms <= 0) {
    return;
  }
  int64_t now = mono_ms();
  if (now > 0) {
    input->power_ignore_until_ms = now + ms;
  }
}

int kindle_input_power_from_grabbed(const KindleInput *input) {
  return !input || input->power_from_grabbed;
}

#ifndef KEY_SLEEP
#define KEY_SLEEP 142
#endif

static int is_power_key(int code) {
  return code == KEY_POWER || code == KEY_SLEEP;
}

/* KEY_POWER is 116 and KEY_SLEEP is 142, both inside the first 256 codes. */
static int device_has_power_key(int fd) {
  unsigned char bits[32];
  memset(bits, 0, sizeof bits);
  if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof bits), bits) < 0) {
    return 0;
  }
  return bit_set(bits, KEY_POWER) || bit_set(bits, KEY_SLEEP);
}

static int env_01(const char *name, int *out) {
  const char *v = getenv(name);
  if (!v || (v[0] != '0' && v[0] != '1')) {
    return 0;
  }
  *out = v[0] == '1';
  return 1;
}

static AppEvKind map_key(int code) {
  switch (code) {
    case KEY_LEFT:
      return EV_LEFT;
    case KEY_RIGHT:
      return EV_RIGHT;
    case KEY_UP:
      return EV_UP;
    case KEY_DOWN:
      return EV_DOWN;
    case KEY_PAGEUP:
      return EV_PAGE_PREV;
    case KEY_PAGEDOWN:
      return EV_PAGE_NEXT;
    case KEY_ENTER:
    case KEY_KPENTER:
    case KEY_MENU:
    case KEY_F1:
      return EV_CONFIRM;
    case KEY_HOME:
    case KEY_ESC:
    case KEY_BACK:
      return EV_BACK;
    default:
      return EV_NONE;
  }
}

static void to_screen(const KindleInput *input, const Dev *dev, int raw_x, int raw_y, int *sx, int *sy) {
  int xrange = dev->max_x - dev->min_x;
  int yrange = dev->max_y - dev->min_y;
  if (xrange < 1) {
    xrange = 1;
  }
  if (yrange < 1) {
    yrange = 1;
  }
  int x = (int)((long)(raw_x - dev->min_x) * input->nat_w / xrange);
  int y = (int)((long)(raw_y - dev->min_y) * input->nat_h / yrange);
  if (input->swap) {
    x = (int)((long)(raw_y - dev->min_y) * input->nat_w / yrange);
    y = (int)((long)(raw_x - dev->min_x) * input->nat_h / xrange);
  }
  if (input->flip_x) {
    x = input->nat_w - 1 - x;
  }
  if (input->flip_y) {
    y = input->nat_h - 1 - y;
  }
  if (x < 0) {
    x = 0;
  }
  if (y < 0) {
    y = 0;
  }
  if (x >= input->nat_w) {
    x = input->nat_w - 1;
  }
  if (y >= input->nat_h) {
    y = input->nat_h - 1;
  }
  int lx = 0;
  int ly = 0;
  yomu_to_logical(input->rot, input->nat_w, input->nat_h, x, y, &lx, &ly);
  if (lx < 0) {
    lx = 0;
  }
  if (ly < 0) {
    ly = 0;
  }
  if (lx >= input->width) {
    lx = input->width - 1;
  }
  if (ly >= input->height) {
    ly = input->height - 1;
  }
  *sx = lx;
  *sy = ly;
}

static int finish_gesture(KindleInput *input, Dev *dev, AppEvent *event) {
  int x0, y0, x1, y1;
  to_screen(input, dev, dev->sx, dev->sy, &x0, &y0);
  to_screen(input, dev, dev->cx, dev->cy, &x1, &y1);
  int dx = x1 - x0;
  int dy = y1 - y0;
  int adx = dx < 0 ? -dx : dx;
  int ady = dy < 0 ? -dy : dy;
  dev->down = 0;
  event->x0 = x0;
  event->y0 = y0;
  event->x = x1;
  event->y = y1;
  if (adx < input->thresh && ady < input->thresh) {
    event->kind = EV_TAP;
    return 1;
  }
  if (adx >= ady) {
    event->kind = dx < 0 ? EV_SWIPE_LEFT : EV_SWIPE_RIGHT;
  } else {
    event->kind = dy < 0 ? EV_SWIPE_UP : EV_SWIPE_DOWN;
  }
  return 1;
}

static int handle_event(KindleInput *input, Dev *dev, const struct KoEvent *raw, AppEvent *event) {
  if (raw->type == EV_KEY) {
    /* EVIOCGRAB hides this key from powerd. Only a press starts the wait.
     * powerd toggles an ungrabbed key itself. Repeats and the wake press do not. */
    if (is_power_key(raw->code)) {
      if (raw->value != 1) {
        yomu_log("power key %s code %d value %d grabbed %d", dev->name[0] ? dev->name : "?", raw->code,
                   raw->value, dev->grabbed);
        return 0;
      }
      int64_t now = mono_ms();
      if (now > 0 && now < input->power_ignore_until_ms) {
        yomu_log("power key ignored %s code %d for %lld ms", dev->name[0] ? dev->name : "?", raw->code,
                   (long long)(input->power_ignore_until_ms - now));
        return 0;
      }
      if (now > 0) {
        input->power_ignore_until_ms = now + 2000;
      }
      input->power_from_grabbed = dev->grabbed;
      yomu_log("power key %s code %d grabbed %d", dev->name[0] ? dev->name : "?", raw->code, dev->grabbed);
      event->kind = EV_SLEEP;
      return 1;
    }
    if (raw->value == 0) {
      return 0;
    }
    if (raw->code == BTN_TOUCH) {
      if (raw->value) {
        dev->down = 1;
        dev->need_origin = 1;
      } else if (dev->down) {
        return finish_gesture(input, dev, event);
      }
      return 0;
    }
    AppEvKind kind = map_key((int)raw->code);
    if (kind == EV_NONE) {
      return 0;
    }
    event->kind = kind;
    return 1;
  }
  /* Positions usually arrive after the tracking id. Snapshot the start on
   * the following frame so a swipe is not measured from the previous touch. */
  if (raw->type == EV_SYN && raw->code == SYN_REPORT) {
    if (dev->down && dev->need_origin) {
      dev->sx = dev->cx;
      dev->sy = dev->cy;
      dev->need_origin = 0;
    }
    return 0;
  }
  if (raw->type != EV_ABS) {
    return 0;
  }
  if (raw->code == ABS_MT_POSITION_X || raw->code == ABS_X) {
    dev->cx = raw->value;
  } else if (raw->code == ABS_MT_POSITION_Y || raw->code == ABS_Y) {
    dev->cy = raw->value;
  } else if (raw->code == ABS_MT_TRACKING_ID) {
    if (raw->value < 0 && dev->down) {
      return finish_gesture(input, dev, event);
    }
    if (raw->value >= 0) {
      dev->down = 1;
      dev->need_origin = 1;
    }
  }
  return 0;
}

static void load_axis(int fd, int code, int *min_v, int *max_v) {
  struct input_absinfo info;
  memset(&info, 0, sizeof info);
  if (ioctl(fd, EVIOCGABS(code), &info) != 0) {
    return;
  }
  *min_v = info.minimum;
  *max_v = info.maximum;
  if (*max_v <= *min_v) {
    *max_v = *min_v + 1;
  }
}

int kindle_input_open(KindleInput **out, int width, int height, int rot) {
  if (out) {
    *out = NULL;
  }
  KindleInput *input = calloc(1, sizeof(*input));
  if (!input) {
    return -1;
  }
  input->nat_w = width > 0 ? width : 1;
  input->nat_h = height > 0 ? height : 1;
  input->rot = rot & 3;
  yomu_logical_size(input->nat_w, input->nat_h, input->rot, &input->width, &input->height);
  input->thresh = input->width / 25;
  if (input->thresh < 24) {
    input->thresh = 24;
  }
  env_01("YOMU_TOUCH_FLIP_X", &input->flip_x);
  env_01("YOMU_TOUCH_FLIP_Y", &input->flip_y);

  for (int i = 0; i < 32 && input->ndevs < DEV_MAX; i++) {
    char path[64];
    snprintf(path, sizeof path, "/dev/input/event%d", i);
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) {
      continue;
    }
    char name[64];
    memset(name, 0, sizeof name);
    if (ioctl(fd, EVIOCGNAME(sizeof name - 1), name) < 0) {
      name[0] = '\0';
    }
    if (skip_name(name)) {
      int power = device_has_power_key(fd);
      yomu_log("skip input %s (%s) power %d", path, name[0] ? name : "?", power);
      close(fd);
      continue;
    }
    unsigned char evbits[(EV_MAX + 7) / 8];
    memset(evbits, 0, sizeof evbits);
    if (ioctl(fd, EVIOCGBIT(0, sizeof evbits), evbits) < 0) {
      close(fd);
      continue;
    }
    int has_key = bit_set(evbits, EV_KEY);
    int has_abs = bit_set(evbits, EV_ABS);
    int has_touch = 0;
    int min_x = 0, max_x = 1, min_y = 0, max_y = 1;
    if (has_abs) {
      unsigned char absbits[(ABS_MAX + 7) / 8];
      memset(absbits, 0, sizeof absbits);
      ioctl(fd, EVIOCGBIT(EV_ABS, sizeof absbits), absbits);
      if (bit_set(absbits, ABS_MT_POSITION_X)) {
        has_touch = 1;
        load_axis(fd, ABS_MT_POSITION_X, &min_x, &max_x);
        load_axis(fd, ABS_MT_POSITION_Y, &min_y, &max_y);
      } else if (bit_set(absbits, ABS_X) && bit_set(absbits, ABS_Y)) {
        has_touch = 1;
        load_axis(fd, ABS_X, &min_x, &max_x);
        load_axis(fd, ABS_Y, &min_y, &max_y);
      }
    }
    if (!has_touch && !has_key) {
      close(fd);
      continue;
    }
    int has_power = has_key && device_has_power_key(fd);
    int grabbed = 0;
    /* Grabbing the power key hides it from powerd, and preventScreenSaver
     * cannot be used to make up for that: the flag swallows the button.
     * A touch device is still grabbed so the UI does not see taps. */
    if (has_power && !has_touch) {
      yomu_log("leave %s (%s) for powerd", path, name[0] ? name : "?");
    } else if (ioctl(fd, EVIOCGRAB, 1) != 0) {
      yomu_log("grab %s (%s): %s", path, name, strerror(errno));
    } else {
      grabbed = 1;
    }
    Dev *dev = &input->devs[input->ndevs++];
    dev->fd = fd;
    dev->has_touch = has_touch;
    dev->grabbed = grabbed;
    dev->min_x = min_x;
    dev->max_x = max_x;
    dev->min_y = min_y;
    dev->max_y = max_y;
    snprintf(dev->name, sizeof dev->name, "%s", name);
    yomu_log("input %s %s touch %d power %d grabbed %d range %d..%d %d..%d", path, name[0] ? name : "?",
               has_touch, has_power, grabbed, min_x, max_x, min_y, max_y);
    if (has_touch && !env_01("YOMU_TOUCH_SWAP", &input->swap)) {
      int xrange = max_x - min_x;
      int yrange = max_y - min_y;
      int native = abs(xrange - input->nat_w) + abs(yrange - input->nat_h);
      int swapped = abs(xrange - input->nat_h) + abs(yrange - input->nat_w);
      if (swapped + xrange / 10 < native) {
        input->swap = 1;
      }
    }
  }
  if (input->ndevs == 0) {
    yomu_log("no input devices");
  } else {
    yomu_log("touch swap %d flip %d,%d", input->swap, input->flip_x, input->flip_y);
  }
  *out = input;
  return 0;
}

void kindle_input_close(KindleInput *input) {
  if (!input) {
    return;
  }
  for (int i = 0; i < input->ndevs; i++) {
    if (input->devs[i].fd >= 0) {
      ioctl(input->devs[i].fd, EVIOCGRAB, 0);
      close(input->devs[i].fd);
    }
  }
  free(input);
}

static int fill_pfds(const KindleInput *input, struct pollfd *pfds, int *map) {
  int np = 0;
  for (int i = 0; i < input->ndevs; i++) {
    if (input->devs[i].fd < 0) {
      continue;
    }
    pfds[np].fd = input->devs[i].fd;
    pfds[np].events = POLLIN;
    pfds[np].revents = 0;
    map[np] = i;
    np++;
  }
  return np;
}

int kindle_input_take_power(KindleInput *input, int timeout_ms) {
  if (g_stop) {
    return -1;
  }
  if (!input || input->ndevs == 0) {
    return 0;
  }
  if (timeout_ms < 0) {
    timeout_ms = 0;
  }
  struct pollfd pfds[DEV_MAX];
  int map[DEV_MAX];
  int np = fill_pfds(input, pfds, map);
  if (np == 0) {
    return 0;
  }
  int pr = poll(pfds, (nfds_t)np, timeout_ms);
  if (pr < 0) {
    if (errno == EINTR && g_stop) {
      return -1;
    }
    return 0;
  }
  if (pr == 0) {
    return 0;
  }
  int saw = 0;
  int consumed = 0;
  for (int i = 0; i < np; i++) {
    Dev *dev = &input->devs[map[i]];
    if ((pfds[i].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
      close(dev->fd);
      dev->fd = -1;
      continue;
    }
    if ((pfds[i].revents & POLLIN) == 0) {
      continue;
    }
    for (;;) {
      struct KoEvent raw;
      ssize_t n = read(dev->fd, &raw, sizeof raw);
      if (n < 0) {
        if (errno == EINTR) {
          continue;
        }
        break;
      }
      if (n != (ssize_t)sizeof raw) {
        break;
      }
      consumed = 1;
      if (raw.type == EV_KEY && is_power_key(raw.code)) {
        yomu_log("power wait %s code %d value %d", dev->name[0] ? dev->name : "?", raw.code, raw.value);
        if (raw.value == 1) {
          saw = 1;
        }
      }
    }
  }
  /* A gesture cut by suspend must not finish on the next touch. */
  if (consumed) {
    for (int i = 0; i < input->ndevs; i++) {
      input->devs[i].down = 0;
      input->devs[i].need_origin = 0;
    }
  }
  return saw ? 1 : 0;
}

int kindle_input_poll(KindleInput *input, AppEvent *event, int timeout_ms) {
  if (!input || !event) {
    return -1;
  }
  if (input->ndevs == 0) {
    return -1;
  }
  for (;;) {
    if (g_stop) {
      return -1;
    }
    struct pollfd pfds[DEV_MAX];
    int map[DEV_MAX];
    int np = fill_pfds(input, pfds, map);
    if (np == 0) {
      return -1;
    }
    int pr = poll(pfds, (nfds_t)np, timeout_ms < 0 ? -1 : timeout_ms);
    if (pr == 0) {
      memset(event, 0, sizeof *event);
      event->kind = EV_NONE;
      return 0;
    }
    if (pr < 0) {
      if (errno == EINTR) {
        if (g_stop) {
          return -1;
        }
        continue;
      }
      return -1;
    }
    for (int i = 0; i < np; i++) {
      Dev *dev = &input->devs[map[i]];
      if ((pfds[i].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
        close(dev->fd);
        dev->fd = -1;
        continue;
      }
      if ((pfds[i].revents & POLLIN) == 0) {
        continue;
      }
      for (;;) {
        struct KoEvent raw;
        ssize_t n = read(dev->fd, &raw, sizeof raw);
        if (n < 0) {
          if (errno == EINTR) {
            continue;
          }
          break;
        }
        if (n != (ssize_t)sizeof raw) {
          break;
        }
        if (handle_event(input, dev, &raw, event)) {
          return 1;
        }
      }
    }
  }
}
