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
    {"/system/fonts/NotoSerifCJKjp-Regular.otf", "明朝"},
    {"/system/fonts/NotoSerifCJK-Regular.ttc", "明朝"},
    {"/system/fonts/NotoSerifJP-Regular.otf", "明朝"},
    /* DPT-RP1 ships these Japanese faces and no mincho. */
    {"/system/fonts/DFHEI5-SONY.ttf", "中黒"},
    {"/system/fonts/SSTJpPro-Regular.otf", "SST"},
    {"/system/fonts/SSTJpPro-Bold.otf", "SST 太字"},
    {"/system/fonts/SSTJpPro-Light.otf", "SST 細"},
    {"/system/fonts/SSTJpCondPro-Regular.otf", "SST 狭"},
    {"/system/fonts/MTLmr3m.ttf", "丸"},
    {"/system/fonts/DroidSansFallback.ttf", "Droid"},
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

/* A Unicode cmap is enough to skip Latin faces. FreeType still names the faces we keep.
 * 1 has kana and kanji, 0 does not, -1 means the cmap could not be read. */
static unsigned ru16(const unsigned char *p) { return ((unsigned)p[0] << 8) | p[1]; }

static unsigned ru32(const unsigned char *p) {
  return ((unsigned)p[0] << 24) | ((unsigned)p[1] << 16) | ((unsigned)p[2] << 8) | p[3];
}

static int fmt4_has(const unsigned char *p, unsigned len, unsigned cp) {
  unsigned segCount;
  unsigned i;
  unsigned id_off;
  const unsigned char *endCode;
  const unsigned char *startCode;
  const unsigned char *idDelta;
  const unsigned char *idRange;
  if (len < 16 || ru16(p) != 4) {
    return -1;
  }
  if (ru16(p + 2) < 16 || ru16(p + 2) > len) {
    return -1;
  }
  len = ru16(p + 2);
  segCount = ru16(p + 6) / 2;
  if (segCount < 1 || segCount > (len - 16) / 8) {
    return -1;
  }
  endCode = p + 14;
  startCode = endCode + segCount * 2 + 2;
  idDelta = startCode + segCount * 2;
  idRange = idDelta + segCount * 2;
  id_off = (unsigned)(idRange - p);
  for (i = 0; i < segCount; i++) {
    unsigned end = ru16(endCode + i * 2);
    unsigned start = ru16(startCode + i * 2);
    unsigned delta = ru16(idDelta + i * 2);
    unsigned rangeOff = ru16(idRange + i * 2);
    unsigned gid;
    if (cp > end) {
      continue;
    }
    if (cp < start) {
      return 0;
    }
    if (rangeOff == 0) {
      return ((cp + delta) & 0xFFFFu) != 0;
    }
    if ((rangeOff & 1u) != 0) {
      return -1;
    }
    {
      unsigned units = rangeOff / 2 + (cp - start);
      unsigned pos = id_off + i * 2 + units * 2;
      if (units > len || pos > len - 2) {
        return -1;
      }
      gid = ru16(p + pos);
    }
    if (gid != 0) {
      gid = (gid + delta) & 0xFFFFu;
    }
    return gid != 0;
  }
  return 0;
}

static int fmt12_has(const unsigned char *p, unsigned len, unsigned cp, int constant) {
  unsigned sublen;
  unsigned nGroups;
  unsigned i;
  if (len < 16 || ru16(p) != (constant ? 13u : 12u)) {
    return -1;
  }
  sublen = ru32(p + 4);
  if (sublen < 16 || sublen > len) {
    return -1;
  }
  nGroups = ru32(p + 12);
  if (nGroups > (sublen - 16) / 12) {
    return -1;
  }
  for (i = 0; i < nGroups; i++) {
    const unsigned char *g = p + 16 + i * 12;
    unsigned start = ru32(g);
    unsigned end = ru32(g + 4);
    unsigned gid0 = ru32(g + 8);
    if (cp < start || cp > end) {
      continue;
    }
    if (constant) {
      return gid0 != 0;
    }
    return gid0 + (cp - start) != 0;
  }
  return 0;
}

static int cmap_jp(const unsigned char *cmap, unsigned len) {
  unsigned n;
  unsigned i;
  int known = 0;
  int bad = 0;
  if (len < 4) {
    return -1;
  }
  n = ru16(cmap + 2);
  if (n > 64 || 4 + n * 8 > len) {
    return -1;
  }
  for (i = 0; i < n; i++) {
    const unsigned char *rec = cmap + 4 + i * 8;
    unsigned platform = ru16(rec);
    unsigned encoding = ru16(rec + 2);
    unsigned off = ru32(rec + 4);
    unsigned fmt;
    int a;
    int b;
    if (platform != 0 && !(platform == 3 && (encoding == 1 || encoding == 10))) {
      continue;
    }
    if (off + 2 > len) {
      bad = 1;
      continue;
    }
    fmt = ru16(cmap + off);
    if (fmt == 4) {
      a = fmt4_has(cmap + off, len - off, 0x3042);
      b = fmt4_has(cmap + off, len - off, 0x4E00);
    } else if (fmt == 12 || fmt == 13) {
      a = fmt12_has(cmap + off, len - off, 0x3042, fmt == 13);
      b = fmt12_has(cmap + off, len - off, 0x4E00, fmt == 13);
    } else {
      bad = 1;
      continue;
    }
    if (a < 0 || b < 0) {
      bad = 1;
      continue;
    }
    known = 1;
    if (a && b) {
      return 1;
    }
  }
  if (known && !bad) {
    return 0;
  }
  return -1;
}

static unsigned char *read_span(FILE *fp, unsigned offset, unsigned length) {
  unsigned char *buf;
  if (length < 4 || length > 4u * 1024u * 1024u) {
    return NULL;
  }
  buf = malloc(length);
  if (!buf) {
    return NULL;
  }
  if (fseek(fp, (long)offset, SEEK_SET) != 0 || fread(buf, 1, length, fp) != length) {
    free(buf);
    return NULL;
  }
  return buf;
}

static int sfnt_jp_at(FILE *fp, unsigned face_off) {
  unsigned char hdr[12];
  unsigned char rec[16];
  unsigned scaler;
  unsigned num;
  unsigned i;
  unsigned cmap_off = 0;
  unsigned cmap_len = 0;
  unsigned char *cmap;
  int rc;
  if (fseek(fp, (long)face_off, SEEK_SET) != 0 || fread(hdr, 1, 12, fp) != 12) {
    return -1;
  }
  scaler = ru32(hdr);
  if (scaler != 0x00010000u && scaler != 0x4F54544Fu && scaler != 0x74727565u) {
    return -1;
  }
  num = ru16(hdr + 4);
  if (num < 1 || num > 128) {
    return -1;
  }
  for (i = 0; i < num; i++) {
    if (fread(rec, 1, 16, fp) != 16) {
      return -1;
    }
    if (rec[0] == 'c' && rec[1] == 'm' && rec[2] == 'a' && rec[3] == 'p') {
      cmap_off = ru32(rec + 8);
      cmap_len = ru32(rec + 12);
    }
  }
  cmap = read_span(fp, cmap_off, cmap_len);
  if (!cmap) {
    return -1;
  }
  rc = cmap_jp(cmap, cmap_len);
  free(cmap);
  return rc;
}

static int cmap_has_kana_kanji(const char *path) {
  FILE *fp;
  unsigned char hdr[12];
  unsigned tag;
  int rc;
  fp = fopen(path, "rb");
  if (!fp) {
    return -1;
  }
  if (fread(hdr, 1, 12, fp) != 12) {
    fclose(fp);
    return -1;
  }
  tag = ru32(hdr);
  if (tag == 0x74746366u) {
    unsigned n = ru32(hdr + 8);
    unsigned offsets[16];
    unsigned nread;
    unsigned i;
    int bad = 0;
    if (n < 1) {
      fclose(fp);
      return -1;
    }
    nread = n > 16 ? 16 : n;
    for (i = 0; i < nread; i++) {
      unsigned char b[4];
      if (fread(b, 1, 4, fp) != 4) {
        fclose(fp);
        return -1;
      }
      offsets[i] = ru32(b);
    }
    for (i = 0; i < nread; i++) {
      int one = sfnt_jp_at(fp, offsets[i]);
      if (one == 1) {
        fclose(fp);
        return 1;
      }
      if (one < 0) {
        bad = 1;
      }
    }
    fclose(fp);
    return bad ? -1 : 0;
  }
  rc = sfnt_jp_at(fp, 0);
  fclose(fp);
  return rc;
}

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
  /* Latin faces never reach FreeType. An unreadable cmap still does. */
  if (cmap_has_kana_kanji(path) == 0) {
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
    /* A gothic typographic name still has kana and kanji. Show the file stem. */
    if (is_gothic(labels[i])) {
      snprintf(labels[i], sizeof labels[i], "%s", stem);
    }
    if (is_gothic(labels[i])) {
      continue;
    }
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

#define SCAN_STAMP_MAX 32

typedef struct ScanStamp {
  char path[512];
  int64_t mtime_ns;
  int count;
  int exists;
} ScanStamp;

static ScanStamp g_stamp[SCAN_STAMP_MAX];
static int g_stamp_n;
static int g_stamp_overflow;
static TypeFace g_scan[TYPE_FACE_MAX];
static int g_scan_n;
static int g_cache_ok;
static char g_cache_home[768];

static int64_t mtime_ns(const struct stat *st) {
#if defined(__APPLE__)
  return (int64_t)st->st_mtimespec.tv_sec * 1000000000LL + (int64_t)st->st_mtimespec.tv_nsec;
#else
  return (int64_t)st->st_mtim.tv_sec * 1000000000LL + (int64_t)st->st_mtim.tv_nsec;
#endif
}

static int dir_info(const char *dir, int64_t *mtime, int *count) {
  DIR *dp;
  struct dirent *de;
  struct stat st;
  int n = 0;
  if (!dir || !dir[0] || stat(dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
    return 0;
  }
  *mtime = mtime_ns(&st);
  dp = opendir(dir);
  if (!dp) {
    return 0;
  }
  while ((de = readdir(dp)) != NULL) {
    if (de->d_name[0] != '.') {
      n++;
    }
  }
  closedir(dp);
  *count = n;
  return 1;
}

static int stamp_has(const char *dir) {
  int i;
  for (i = 0; i < g_stamp_n; i++) {
    if (strcmp(g_stamp[i].path, dir) == 0) {
      return 1;
    }
  }
  return 0;
}

static void stamp_dir(const char *dir) {
  int64_t mt = 0;
  int count = 0;
  int exists;
  if (!dir || !dir[0] || stamp_has(dir)) {
    return;
  }
  if (g_stamp_n >= SCAN_STAMP_MAX) {
    g_stamp_overflow = 1;
    return;
  }
  exists = dir_info(dir, &mt, &count);
  snprintf(g_stamp[g_stamp_n].path, sizeof g_stamp[g_stamp_n].path, "%s", dir);
  g_stamp[g_stamp_n].mtime_ns = exists ? mt : 0;
  g_stamp[g_stamp_n].count = exists ? count : 0;
  g_stamp[g_stamp_n].exists = exists;
  g_stamp_n++;
}

static int cache_fresh(const char *home) {
  int i;
  if (!g_cache_ok || !home || strcmp(g_cache_home, home) != 0) {
    return 0;
  }
  for (i = 0; i < g_stamp_n; i++) {
    int64_t mt = 0;
    int count = 0;
    int exists = dir_info(g_stamp[i].path, &mt, &count);
    if (!exists) {
      mt = 0;
      count = 0;
    }
    if (exists != g_stamp[i].exists || mt != g_stamp[i].mtime_ns || count != g_stamp[i].count) {
      return 0;
    }
  }
  return 1;
}

static int collect_tree(const char *dir, int depth, TypeFace *extra, int en, int cap) {
  DIR *dp;
  struct dirent *de;
  stamp_dir(dir);
  if (!dir || !dir[0] || en >= cap) {
    return en;
  }
  dp = opendir(dir);
  if (!dp) {
    return en;
  }
  while ((de = readdir(dp)) != NULL && en < cap) {
    char path[512];
    struct stat st;
    int k;
    if (de->d_name[0] == '.') {
      continue;
    }
    k = snprintf(path, sizeof path, "%s/%s", dir, de->d_name);
    if (k < 0 || (size_t)k >= sizeof path) {
      continue;
    }
    if (lstat(path, &st) != 0) {
      continue;
    }
    if (S_ISLNK(st.st_mode)) {
      if (stat(path, &st) == 0 && S_ISREG(st.st_mode) && ends_font(de->d_name)) {
        en = add_file(extra, en, cap, path, NULL);
      }
      continue;
    }
    if (S_ISDIR(st.st_mode)) {
      if (depth < 2) {
        en = collect_tree(path, depth + 1, extra, en, cap);
      }
    } else if (S_ISREG(st.st_mode) && ends_font(de->d_name)) {
      en = add_file(extra, en, cap, path, NULL);
    }
  }
  closedir(dp);
  return en;
}

static int scan_dir(const char *dir, TypeFace *out, int n, int cap) {
  TypeFace *extra;
  int en;
  int i;
  stamp_dir(dir);
  if (!dir || !dir[0] || !out || n >= cap) {
    return n;
  }
  extra = calloc((size_t)TYPE_FACE_MAX, sizeof *extra);
  if (!extra) {
    return n;
  }
  en = collect_tree(dir, 0, extra, 0, TYPE_FACE_MAX);
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
  free(extra);
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
  static const char *k_device_dirs[] = {
      "/system/fonts",
      "/usr/java/lib/fonts",
      "/var/local/font/mnt/ja_font/fonts",
      "/opt/amazon/ebook/fonts",
      "/mnt/us/yomu/fonts",
      "/mnt/us/fonts",
  };
  char dir[768];
  int n = 0;
  int copy;
  size_t i;
  int64_t t0;
  if (!out || cap <= 0) {
    return 0;
  }
  if (!home) {
    home = "";
  }
  if (cache_fresh(home)) {
    copy = g_scan_n < cap ? g_scan_n : cap;
    if (copy > 0) {
      memcpy(out, g_scan, (size_t)copy * sizeof *out);
    }
    return copy;
  }
  g_cache_ok = 0;
  g_stamp_n = 0;
  g_stamp_overflow = 0;
  t0 = yomu_ms();
  for (i = 0; i < sizeof k_known / sizeof k_known[0] && n < TYPE_FACE_MAX; i++) {
    n = add_file(g_scan, n, TYPE_FACE_MAX, k_known[i].path, k_known[i].label);
  }
  for (i = 0; i < sizeof k_device_dirs / sizeof k_device_dirs[0]; i++) {
    n = scan_dir(k_device_dirs[i], g_scan, n, TYPE_FACE_MAX);
  }
  if (home[0]) {
    int k = snprintf(dir, sizeof dir, "%s/fonts", home);
    if (k > 0 && (size_t)k < sizeof dir && !stamp_has(dir)) {
      n = scan_dir(dir, g_scan, n, TYPE_FACE_MAX);
    }
  }
  g_scan_n = n;
  if (!g_stamp_overflow) {
    snprintf(g_cache_home, sizeof g_cache_home, "%s", home);
    g_cache_ok = 1;
  }
  yomu_log("type faces %d in %lld ms", n, (long long)(yomu_ms() - t0));
  if (n >= TYPE_FACE_MAX) {
    yomu_log("type face list is full");
  }
  copy = n < cap ? n : cap;
  if (copy > 0) {
    memcpy(out, g_scan, (size_t)copy * sizeof *out);
  }
  return copy;
}
