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
    {"/System/Library/Fonts/Supplemental/Arial Unicode.ttf", "Arial Unicode"},
    {"/Library/Fonts/Arial Unicode.ttf", "Arial Unicode"},
    {"/System/Library/Fonts/Hiragino Sans GB.ttc", "ヒラギノ"},
    {"/usr/share/fonts/opentype/noto/NotoSerifCJK-Regular.ttc", "Noto明朝"},
};

static int g_step;
static int g_index;
static char g_font[512];
static char g_home[768];

extern int ts_font_faces(const char *path, int *indexes, char labels[][48], int cap);

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

static int contains_fold(const char *hay, const char *needle) {
  size_t n;
  const char *p;
  if (!hay || !needle || !needle[0]) {
    return 0;
  }
  n = strlen(needle);
  for (p = hay; *p; p++) {
    size_t i = 0;
    while (i < n && p[i]) {
      char a = p[i];
      char b = needle[i];
      if (a >= 'A' && a <= 'Z') {
        a = (char)(a - 'A' + 'a');
      }
      if (b >= 'A' && b <= 'Z') {
        b = (char)(b - 'A' + 'a');
      }
      if (a != b) {
        break;
      }
      i++;
    }
    if (i == n) {
      return 1;
    }
  }
  return 0;
}

/* Code2000 has kana and no unified kanji. Gothic stays the UI face. */
static int is_code2000(const char *path) {
  const char *base = strrchr(path, '/');
  const char *p;
  base = base ? base + 1 : path;
  for (p = base; *p; p++) {
    size_t i = 0;
    while (p[i] && i < 8) {
      char a = p[i];
      if (a >= 'A' && a <= 'Z') {
        a = (char)(a - 'A' + 'a');
      }
      if (a != "code2000"[i]) {
        break;
      }
      i++;
    }
    if (i == 8) {
      return 1;
    }
  }
  return 0;
}

static int is_gothic(const char *s) { return s && (contains_fold(s, "goth") || strstr(s, "ゴシック") != NULL); }

static int already(const TypeFace *out, int n, const char *path, int index) {
  int i;
  for (i = 0; i < n; i++) {
    if (out[i].index == index && strcmp(out[i].path, path) == 0) {
      return 1;
    }
  }
  return 0;
}

static int add_face(TypeFace *out, int n, int cap, const char *path, int index, const char *label) {
  if (!out || n >= cap || index < 0 || !regular_font(path) || already(out, n, path, index)) {
    return n;
  }
  snprintf(out[n].path, sizeof out[n].path, "%s", path);
  snprintf(out[n].label, sizeof out[n].label, "%s", label && label[0] ? label : "書体");
  out[n].index = index;
  return n + 1;
}

/* One row per face that has kana and kanji. A single face keeps fixed_label. */
static int add_file(TypeFace *out, int n, int cap, const char *path, const char *fixed_label) {
  int indexes[16];
  char styles[16][48];
  char labels[16][64];
  char stem[64];
  int count;
  int i;
  if (!regular_font(path) || n >= cap || is_code2000(path) || is_gothic(path) || is_gothic(fixed_label)) {
    return n;
  }
  count = ts_font_faces(path, indexes, styles, 16);
  if (count <= 0) {
    return n;
  }
  copy_stem(stem, sizeof stem, path);
  for (i = 0; i < count; i++) {
    const char *prefix = fixed_label && fixed_label[0] ? fixed_label : stem;
    if (count == 1 && fixed_label && fixed_label[0]) {
      snprintf(labels[i], sizeof labels[i], "%s", fixed_label);
    } else if (styles[i][0]) {
      snprintf(labels[i], sizeof labels[i], "%s", styles[i]);
    } else {
      snprintf(labels[i], sizeof labels[i], "%s %d", prefix, indexes[i] + 1);
    }
  }
  for (i = 0; i < count && n < cap; i++) {
    int j;
    for (j = 0; j < i; j++) {
      if (strcmp(labels[i], labels[j]) == 0) {
        char extra[80];
        snprintf(extra, sizeof extra, "%s %d", labels[i], indexes[i] + 1);
        snprintf(labels[i], sizeof labels[i], "%s", extra);
        break;
      }
    }
    if (is_gothic(labels[i])) {
      continue;
    }
    n = add_face(out, n, cap, path, indexes[i], labels[i]);
  }
  return n;
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
    int k;
    if (de->d_name[0] == '.' || !ends_font(de->d_name)) {
      continue;
    }
    k = snprintf(path, sizeof path, "%s/%s", dir, de->d_name);
    if (k < 0 || (size_t)k >= sizeof path) {
      continue;
    }
    en = add_file(extra, en, TYPE_FACE_MAX, path, NULL);
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
    n = add_face(out, n, cap, extra[i].path, extra[i].index, extra[i].label);
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
    fprintf(fp, "index %d\n", g_index);
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
  g_index = 0;
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
    } else if (strncmp(line, "index ", 6) == 0) {
      g_index = atoi(line + 6);
      if (g_index < 0) {
        g_index = 0;
      }
      if (g_index > 15) {
        g_index = 15;
      }
    }
  }
  fclose(fp);
  if (g_font[0] && !regular_font(g_font)) {
    g_font[0] = '\0';
    g_index = 0;
  }
}

int type_step(void) { return clamp_step(g_step); }

const char *type_step_label(void) { return k_label[step_index()]; }

const char *type_font(void) { return g_font; }

int type_face_index(void) { return g_font[0] ? g_index : 0; }

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

int type_set_font(const char *path, int index) {
  char old[512];
  int old_index;
  if (!path) {
    path = "";
  }
  if (index < 0) {
    index = 0;
  }
  if (index > 15) {
    index = 15;
  }
  if (!path[0]) {
    index = 0;
  }
  if (strcmp(g_font, path) == 0 && g_index == index) {
    return 0;
  }
  if (path[0] && !regular_font(path)) {
    return 0;
  }
  snprintf(old, sizeof old, "%s", g_font);
  old_index = g_index;
  snprintf(g_font, sizeof g_font, "%s", path);
  g_index = index;
  if (type_save() != 0) {
    snprintf(g_font, sizeof g_font, "%s", old);
    g_index = old_index;
    return 0;
  }
  yomu_log("type face %s #%d", g_font[0] ? g_font : "default", g_index);
  return 1;
}

static int row_matching(const TypeFace *faces, int n, const char *path, int index, int want_index) {
  int i;
  int first = -1;
  if (!path || !path[0]) {
    return -1;
  }
  for (i = 0; i < n; i++) {
    if (strcmp(faces[i].path, path) != 0) {
      continue;
    }
    if (first < 0) {
      first = i;
    }
    if (want_index && faces[i].index == index) {
      return i;
    }
  }
  return want_index ? -1 : first;
}

int type_used_row(const TypeFace *faces, int n) {
  const char *saved;
  const char *env;
  size_t i;
  int row;
  if (!faces || n <= 0) {
    return 0;
  }
  saved = type_font();
  if (saved && saved[0] && regular_font(saved)) {
    row = row_matching(faces, n, saved, type_face_index(), 1);
    if (row >= 0) {
      return row;
    }
    return 0;
  }
  env = getenv("YOMU_FONT");
  if (env && env[0] && regular_font(env)) {
    row = row_matching(faces, n, env, 0, 0);
    if (row >= 0) {
      return row;
    }
  }
  for (i = 0; i < sizeof k_known / sizeof k_known[0]; i++) {
    if (!regular_font(k_known[i].path)) {
      continue;
    }
    row = row_matching(faces, n, k_known[i].path, 0, 0);
    if (row >= 0) {
      return row;
    }
  }
  return 0;
}

int type_faces(const char *home, TypeFace *out, int cap) {
  char dir[768];
  int n = 0;
  size_t i;
  if (!out || cap <= 0) {
    return 0;
  }
  for (i = 0; i < sizeof k_known / sizeof k_known[0] && n < cap; i++) {
    n = add_file(out, n, cap, k_known[i].path, k_known[i].label);
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
