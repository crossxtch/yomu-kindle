#include "typeface.h"

#include "books.h"
#include "log.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

/* Steps sit around the fitted em. 0 is the size the page used before this setting. */
static const int k_pct[] = {70, 85, 90, 100, 115, 130, 150};
static const char *k_label[] = {"最小", "小さめ", "やや小", "標準", "やや大", "大きめ", "最大"};

static const struct {
  const char *path;
  const char *label;
} k_known[] = {
    {"/var/local/font/mnt/ja_font/fonts/TsukuMinPr5-Medium.ttf", "筑紫明朝"},
    {"/var/local/font/mnt/ja_font/fonts/TsukuMinPr5-Bold.ttf", "筑紫明朝 太字"},
    {"/usr/java/lib/fonts/TBMinchoMedium_213.ttf", "明朝"},
    {"/usr/java/lib/fonts/TBMinchoBold_213.ttf", "明朝 太字"},
    {"/usr/java/lib/fonts/TBGothicMed_213.ttf", "ゴシック"},
    {"/usr/java/lib/fonts/TBGothicBold_213.ttf", "ゴシック 太字"},
    {"/usr/java/lib/fonts/HeiSeiMaruGoth213_E.ttf", "丸ゴシック"},
    {"/usr/java/lib/fonts/HeiSeiMaruGoth213_E_Bold.ttf", "丸ゴシック 太字"},
    {"/usr/java/lib/fonts/code2000.ttf", "Code2000"},
    {"/usr/java/lib/fonts/Code2000.ttf", "Code2000"},
    {"/System/Library/Fonts/Supplemental/Arial Unicode.ttf", "Arial Unicode"},
    {"/Library/Fonts/Arial Unicode.ttf", "Arial Unicode"},
    {"/System/Library/Fonts/Hiragino Sans GB.ttc", "ヒラギノ"},
    {"/usr/share/fonts/opentype/noto/NotoSerifCJK-Regular.ttc", "Noto明朝"},
    {"/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc", "Notoゴシック"},
    {"/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc", "Notoゴシック"},
    {"/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc", "Notoゴシック"},
};

static int g_step;
static char g_font[512];
static char g_home[768];

static int clamp_step(int step) {
  if (step < TYPE_STEP_MIN) {
    return TYPE_STEP_MIN;
  }
  if (step > TYPE_STEP_MAX) {
    return TYPE_STEP_MAX;
  }
  return step;
}

static int step_index(void) { return clamp_step(g_step) - TYPE_STEP_MIN; }

static int regular_font(const char *path) {
  struct stat st;
  if (!path || !path[0] || stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
    return 0;
  }
  if (st.st_size <= 0 || st.st_size > 80LL * 1024 * 1024) {
    return 0;
  }
  return 1;
}

static int ends_font(const char *name) {
  size_t n = strlen(name);
  if (n < 4) {
    return 0;
  }
  return strcasecmp(name + n - 4, ".ttf") == 0 || strcasecmp(name + n - 4, ".ttc") == 0 ||
         strcasecmp(name + n - 4, ".otf") == 0 || (n >= 4 && strcasecmp(name + n - 4, ".otc") == 0);
}

static void copy_stem(char *dst, size_t dst_n, const char *path) {
  const char *base = strrchr(path, '/');
  char *dot;
  base = base ? base + 1 : path;
  snprintf(dst, dst_n, "%s", base);
  dot = strrchr(dst, '.');
  if (dot && ends_font(dst)) {
    *dot = '\0';
  }
  if (!yomu_text_useful(dst) && dst_n > 0) {
    snprintf(dst, dst_n, "書体");
  }
}

static int already(const TypeFace *out, int n, const char *path) {
  int i;
  for (i = 0; i < n; i++) {
    if (strcmp(out[i].path, path) == 0) {
      return 1;
    }
  }
  return 0;
}

static int add_face(TypeFace *out, int n, int cap, const char *path, const char *label) {
  if (!out || n >= cap || !regular_font(path) || already(out, n, path)) {
    return n;
  }
  snprintf(out[n].path, sizeof out[n].path, "%s", path);
  snprintf(out[n].label, sizeof out[n].label, "%s", label && label[0] ? label : "書体");
  return n + 1;
}

static int scan_dir(const char *dir, TypeFace *out, int n, int cap) {
  DIR *dp;
  struct dirent *de;
  TypeFace extra[TYPE_FACE_MAX];
  int en = 0;
  int i;
  if (!dir || !dir[0] || n >= cap) {
    return n;
  }
  dp = opendir(dir);
  if (!dp) {
    return n;
  }
  while ((de = readdir(dp)) != NULL && en < TYPE_FACE_MAX) {
    char path[512];
    char label[64];
    int k;
    if (de->d_name[0] == '.' || !ends_font(de->d_name)) {
      continue;
    }
    k = snprintf(path, sizeof path, "%s/%s", dir, de->d_name);
    if (k < 0 || (size_t)k >= sizeof path) {
      continue;
    }
    copy_stem(label, sizeof label, path);
    en = add_face(extra, en, TYPE_FACE_MAX, path, label);
  }
  closedir(dp);
  for (i = 1; i < en; i++) {
    TypeFace key = extra[i];
    int j = i;
    while (j > 0 && strcmp(extra[j - 1].label, key.label) > 0) {
      extra[j] = extra[j - 1];
      j--;
    }
    extra[j] = key;
  }
  for (i = 0; i < en && n < cap; i++) {
    n = add_face(out, n, cap, extra[i].path, extra[i].label);
  }
  return n;
}

static int type_path(char *out, size_t out_n) {
  int k;
  if (!g_home[0]) {
    return -1;
  }
  k = snprintf(out, out_n, "%s/type.txt", g_home);
  if (k < 0 || (size_t)k >= out_n) {
    return -1;
  }
  return 0;
}

static int type_save(void) {
  char file[800];
  char tmp[800];
  FILE *fp;
  if (type_path(file, sizeof file) != 0) {
    return -1;
  }
  snprintf(tmp, sizeof tmp, "%s.tmp", file);
  fp = fopen(tmp, "w");
  if (!fp) {
    return -1;
  }
  fprintf(fp, "step %d\n", g_step);
  if (g_font[0]) {
    fprintf(fp, "font %s\n", g_font);
  }
  if (fclose(fp) != 0) {
    remove(tmp);
    return -1;
  }
  if (rename(tmp, file) != 0) {
    remove(tmp);
    return -1;
  }
  return 0;
}

void type_load(const char *home) {
  char file[800];
  FILE *fp;
  g_step = 0;
  g_font[0] = '\0';
  g_home[0] = '\0';
  if (!home || !home[0]) {
    return;
  }
  snprintf(g_home, sizeof g_home, "%s", home);
  if (type_path(file, sizeof file) != 0) {
    g_home[0] = '\0';
    return;
  }
  fp = fopen(file, "r");
  if (!fp) {
    return;
  }
  char line[700];
  while (fgets(line, sizeof line, fp)) {
    size_t n = strlen(line);
    while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) {
      line[--n] = '\0';
    }
    if (strncmp(line, "step ", 5) == 0) {
      g_step = clamp_step(atoi(line + 5));
    } else if (strncmp(line, "font ", 5) == 0) {
      snprintf(g_font, sizeof g_font, "%s", line + 5);
    }
  }
  fclose(fp);
  if (g_font[0] && !regular_font(g_font)) {
    g_font[0] = '\0';
  }
}

int type_step(void) { return clamp_step(g_step); }

const char *type_step_label(void) { return k_label[step_index()]; }

const char *type_font(void) { return g_font; }

static int fitted_em(int w, int h) {
  int em = 50;
  if (h > 0) {
    int fit = h / 18;
    if (fit < 16) {
      fit = 16;
    }
    if (fit < em) {
      em = fit;
    }
  }
  if (w > 0 && em > w / 4) {
    em = w / 4;
    if (em < 12) {
      em = 12;
    }
  }
  return em;
}

/* One column and four characters still fit in either writing direction. */
static int clamp_em(int em, int w, int h) {
  int side;
  int limit;
  if (em < 12) {
    em = 12;
  }
  if (em > 96) {
    em = 96;
  }
  limit = em;
  if (w > 0 && limit > w / 4) {
    limit = w / 4;
  }
  if (h > 0 && limit > h / 4) {
    limit = h / 4;
  }
  if (limit < 12) {
    limit = 12;
  }
  side = w > 0 && h > 0 && w < h ? w : h;
  if (side < 1) {
    side = 1;
  }
  while (limit > 12 && limit + (limit + 1) / 2 > side) {
    limit--;
  }
  if (em > limit) {
    em = limit;
  }
  if (em < 12) {
    em = 12;
  }
  return em;
}

int type_em(int w, int h) {
  int em = fitted_em(w, h) * k_pct[step_index()] / 100;
  return clamp_em(em, w, h);
}

int type_set_step(int step) {
  int old;
  step = clamp_step(step);
  if (step == g_step) {
    return 0;
  }
  old = g_step;
  g_step = step;
  if (type_save() != 0) {
    g_step = old;
    return 0;
  }
  yomu_log("type size %s", type_step_label());
  return 1;
}

int type_set_font(const char *path) {
  char old[512];
  if (!path) {
    path = "";
  }
  if (strcmp(g_font, path) == 0) {
    return 0;
  }
  if (path[0] && !regular_font(path)) {
    return 0;
  }
  snprintf(old, sizeof old, "%s", g_font);
  snprintf(g_font, sizeof g_font, "%s", path);
  if (type_save() != 0) {
    snprintf(g_font, sizeof g_font, "%s", old);
    return 0;
  }
  yomu_log("type face %s", g_font[0] ? g_font : "default");
  return 1;
}

int type_faces(const char *home, TypeFace *out, int cap) {
  char dir[768];
  int n = 0;
  size_t i;
  if (!out || cap <= 0) {
    return 0;
  }
  out[0].path[0] = '\0';
  snprintf(out[0].label, sizeof out[0].label, "標準");
  n = 1;
  for (i = 0; i < sizeof k_known / sizeof k_known[0] && n < cap; i++) {
    n = add_face(out, n, cap, k_known[i].path, k_known[i].label);
  }
  if (home && home[0]) {
    int k = snprintf(dir, sizeof dir, "%s/fonts", home);
    if (k > 0 && (size_t)k < sizeof dir) {
      n = scan_dir(dir, out, n, cap);
    }
  }
  n = scan_dir("/mnt/us/yomu/fonts", out, n, cap);
  n = scan_dir("/mnt/us/fonts", out, n, cap);
  return n;
}
