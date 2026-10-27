#include "app.h"
#include "log.h"

#include <android/bitmap.h>
#include <jni.h>

#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* The reader thread blocks inside present until the UI thread has copied
 * the gray buffer and asked the panel to refresh. onDestroy only sets
 * g_stop and wakes poll. It must not join this thread: present is waiting
 * for that same UI thread.
 */

static JavaVM *g_vm;
static JNIEnv *g_env;
static jobject g_activity;
static jobject g_bitmap;
static jmethodID g_present;
static jmethodID g_poll;
static jmethodID g_exit;
static pthread_mutex_t g_px = PTHREAD_MUTEX_INITIALIZER;
static pthread_t g_thread;
static int g_running;
static volatile sig_atomic_t g_stop;
static uint8_t *g_gray;
static int g_w;
static int g_h;
static char g_home[512];
static char g_root[768];
static char g_open[1024];

static void copy_chars(char *dst, size_t dst_n, JNIEnv *env, jstring src) {
  const char *chars;
  if (!dst || dst_n == 0) {
    return;
  }
  dst[0] = '\0';
  if (!src) {
    return;
  }
  chars = (*env)->GetStringUTFChars(env, src, NULL);
  if (!chars) {
    return;
  }
  snprintf(dst, dst_n, "%s", chars);
  (*env)->ReleaseStringUTFChars(env, src, chars);
}

static void blit(void *pixels, int stride, int bw, int bh, int x, int y, int w, int h) {
  int x0 = x;
  int y0 = y;
  int cw = w;
  int ch = h;
  int row;
  if (!g_gray || !pixels || stride < bw * 4) {
    return;
  }
  if (cw <= 0 || ch <= 0) {
    x0 = 0;
    y0 = 0;
    cw = g_w;
    ch = g_h;
  }
  if (x0 < 0) {
    cw += x0;
    x0 = 0;
  }
  if (y0 < 0) {
    ch += y0;
    y0 = 0;
  }
  if (x0 >= g_w || y0 >= g_h || cw <= 0 || ch <= 0) {
    return;
  }
  if (x0 + cw > g_w) {
    cw = g_w - x0;
  }
  if (y0 + ch > g_h) {
    ch = g_h - y0;
  }
  if (x0 >= bw || y0 >= bh) {
    return;
  }
  if (x0 + cw > bw) {
    cw = bw - x0;
  }
  if (y0 + ch > bh) {
    ch = bh - y0;
  }
  for (row = 0; row < ch; row++) {
    uint32_t *dst = (uint32_t *)((uint8_t *)pixels + (size_t)(y0 + row) * (size_t)stride) + x0;
    const uint8_t *src = g_gray + (size_t)(y0 + row) * (size_t)g_w + x0;
    int col;
    for (col = 0; col < cw; col++) {
      uint8_t v = (uint8_t)(src[col] & 0xF0u);
      v = (uint8_t)(v | (v >> 4));
      dst[col] = 0xFF000000u | ((uint32_t)v << 16) | ((uint32_t)v << 8) | (uint32_t)v;
    }
  }
}

static void present(AppHost *host, int x, int y, int w, int h, int full) {
  (void)host;
  if (g_stop || !g_env || !g_activity || !g_present) {
    return;
  }
  (*g_env)->CallVoidMethod(g_env, g_activity, g_present, x, y, w, h, full);
  if ((*g_env)->ExceptionCheck(g_env)) {
    (*g_env)->ExceptionDescribe(g_env);
    (*g_env)->ExceptionClear(g_env);
  }
}

static int poll_event(AppHost *host, AppEvent *event) {
  jintArray arr;
  jint tmp[6];
  jint rc;
  if (!event) {
    return -1;
  }
  memset(event, 0, sizeof *event);
  if (g_stop || !g_env || !g_activity || !g_poll) {
    event->kind = EV_QUIT;
    return 1;
  }
  arr = (*g_env)->NewIntArray(g_env, 6);
  if (!arr) {
    return 0;
  }
  rc = (*g_env)->CallIntMethod(g_env, g_activity, g_poll, host ? host->poll_ms : 0, arr);
  if ((*g_env)->ExceptionCheck(g_env)) {
    (*g_env)->ExceptionClear(g_env);
    (*g_env)->DeleteLocalRef(g_env, arr);
    return 0;
  }
  memset(tmp, 0, sizeof tmp);
  (*g_env)->GetIntArrayRegion(g_env, arr, 0, 6, tmp);
  (*g_env)->DeleteLocalRef(g_env, arr);
  event->kind = (AppEvKind)tmp[0];
  event->x = tmp[1];
  event->y = tmp[2];
  event->x0 = tmp[3];
  event->y0 = tmp[4];
  event->ms = tmp[5];
  return rc;
}

static void *run_app(void *arg) {
  AppHost host;
  char log_path[768];
  int rc;
  (void)arg;
  if ((*g_vm)->AttachCurrentThread(g_vm, &g_env, NULL) != 0) {
    g_env = NULL;
    g_running = 0;
    return NULL;
  }
  signal(SIGPIPE, SIG_IGN);
  mkdir(g_home, 0755);
  mkdir(g_root, 0755);
  snprintf(log_path, sizeof log_path, "%s/yomu.log", g_home);
  yomu_log_open(log_path);
  yomu_log("android panel %dx%d root %s", g_w, g_h, g_root);
  memset(g_gray, 255, (size_t)g_w * (size_t)g_h);
  memset(&host, 0, sizeof host);
  host.w = g_w;
  host.h = g_h;
  host.gray = g_gray;
  host.full_every = 20;
  {
    const char *full = getenv("YOMU_FULL_EVERY");
    if (full && full[0]) {
      host.full_every = atoi(full);
    }
  }
  host.root = g_root;
  host.home = g_home;
  host.present = present;
  host.poll = poll_event;
  rc = app_run(&host, g_open[0] ? g_open : NULL);
  yomu_log("android exit %d", rc);
  if (g_env && g_activity && g_exit && !g_stop) {
    (*g_env)->CallVoidMethod(g_env, g_activity, g_exit, rc);
    if ((*g_env)->ExceptionCheck(g_env)) {
      (*g_env)->ExceptionClear(g_env);
    }
  }
  pthread_mutex_lock(&g_px);
  free(g_gray);
  g_gray = NULL;
  if (g_env && g_bitmap) {
    (*g_env)->DeleteGlobalRef(g_env, g_bitmap);
    g_bitmap = NULL;
  }
  if (g_env && g_activity) {
    (*g_env)->DeleteGlobalRef(g_env, g_activity);
    g_activity = NULL;
  }
  pthread_mutex_unlock(&g_px);
  g_env = NULL;
  (*g_vm)->DetachCurrentThread(g_vm);
  g_running = 0;
  return NULL;
}

JNIEXPORT void JNICALL Java_dev_yomu_YomuNative_nativeLog(JNIEnv *env, jclass cls, jstring line) {
  const char *chars;
  (void)cls;
  if (!line) {
    return;
  }
  chars = (*env)->GetStringUTFChars(env, line, NULL);
  if (!chars) {
    return;
  }
  yomu_log("%s", chars);
  (*env)->ReleaseStringUTFChars(env, line, chars);
}

JNIEXPORT void JNICALL Java_dev_yomu_YomuNative_nativeBlit(JNIEnv *env, jclass cls, jint x, jint y, jint w, jint h) {
  AndroidBitmapInfo info;
  void *pixels = NULL;
  (void)cls;
  pthread_mutex_lock(&g_px);
  if (!g_gray || !g_bitmap) {
    pthread_mutex_unlock(&g_px);
    return;
  }
  if (AndroidBitmap_getInfo(env, g_bitmap, &info) != ANDROID_BITMAP_RESULT_SUCCESS ||
      info.format != ANDROID_BITMAP_FORMAT_RGBA_8888 ||
      AndroidBitmap_lockPixels(env, g_bitmap, &pixels) != ANDROID_BITMAP_RESULT_SUCCESS || !pixels) {
    pthread_mutex_unlock(&g_px);
    return;
  }
  blit(pixels, (int)info.stride, (int)info.width, (int)info.height, x, y, w, h);
  AndroidBitmap_unlockPixels(env, g_bitmap);
  pthread_mutex_unlock(&g_px);
}

JNIEXPORT void JNICALL Java_dev_yomu_YomuNative_nativeStop(JNIEnv *env, jclass cls) {
  (void)env;
  (void)cls;
  g_stop = 1;
}

JNIEXPORT void JNICALL Java_dev_yomu_YomuNative_nativeStart(JNIEnv *env, jclass cls, jobject activity, jint w, jint h,
                                                            jstring home, jstring root, jstring open_path,
                                                            jobject bitmap) {
  jclass activity_cls;
  (void)cls;
  if (g_running || !activity || !bitmap || w < 16 || h < 16 || w > 4096 || h > 4096) {
    return;
  }
  (*env)->GetJavaVM(env, &g_vm);
  activity_cls = (*env)->GetObjectClass(env, activity);
  g_present = (*env)->GetMethodID(env, activity_cls, "present", "(IIIII)V");
  g_poll = (*env)->GetMethodID(env, activity_cls, "poll", "(I[I)I");
  g_exit = (*env)->GetMethodID(env, activity_cls, "onAppExit", "(I)V");
  (*env)->DeleteLocalRef(env, activity_cls);
  if (!g_present || !g_poll || !g_exit) {
    (*env)->ExceptionClear(env);
    return;
  }
  g_gray = malloc((size_t)w * (size_t)h);
  if (!g_gray) {
    return;
  }
  g_w = w;
  g_h = h;
  g_stop = 0;
  copy_chars(g_home, sizeof g_home, env, home);
  copy_chars(g_root, sizeof g_root, env, root);
  copy_chars(g_open, sizeof g_open, env, open_path);
  if (!g_home[0] || !g_root[0]) {
    free(g_gray);
    g_gray = NULL;
    return;
  }
  g_activity = (*env)->NewGlobalRef(env, activity);
  g_bitmap = (*env)->NewGlobalRef(env, bitmap);
  if (!g_activity || !g_bitmap) {
    if (g_activity) {
      (*env)->DeleteGlobalRef(env, g_activity);
    }
    if (g_bitmap) {
      (*env)->DeleteGlobalRef(env, g_bitmap);
    }
    g_activity = NULL;
    g_bitmap = NULL;
    free(g_gray);
    g_gray = NULL;
    return;
  }
  g_running = 1;
  if (pthread_create(&g_thread, NULL, run_app, NULL) != 0) {
    g_running = 0;
    (*env)->DeleteGlobalRef(env, g_activity);
    (*env)->DeleteGlobalRef(env, g_bitmap);
    g_activity = NULL;
    g_bitmap = NULL;
    free(g_gray);
    g_gray = NULL;
  } else {
    pthread_detach(g_thread);
  }
}
