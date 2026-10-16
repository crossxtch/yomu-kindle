#include "font.h"

#include "log.h"

#include "stb_truetype.h"

#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define CACHE_N 128
#define CAND_MAX 48

typedef struct Glyph {
  int cp;
  int px;
  int taken;
  int ok;
  int w, h;
  int xoff, yoff;
  int advance;
  uint8_t *bmp;
} Glyph;

typedef struct Cand {
  char path[512];
  int score;
} Cand;

static unsigned char *g_data;
static size_t g_size;
static stbtt_fontinfo g_info;
static int g_tried;
static int g_ready;
static int g_glyph_hit;
static int g_glyph_miss;
static char g_path[512];
static Glyph g_cache[CACHE_N];
static int g_clock;

static int contains_ci(const char *hay, const char *needle) {
  size_t n = strlen(needle);
  const char *p;
  if (n == 0) {
    return 1;
  }
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

static int ends_ci(const char *s, const char *suf) {
  size_t n = strlen(s);
  size_t m = strlen(suf);
  size_t i;
  if (n < m) {
    return 0;
  }
  for (i = 0; i < m; i++) {
    char a = s[n - m + i];
    char b = suf[i];
    if (a >= 'A' && a <= 'Z') {
      a = (char)(a - 'A' + 'a');
    }
    if (b >= 'A' && b <= 'Z') {
      b = (char)(b - 'A' + 'a');
    }
    if (a != b) {
      return 0;
    }
  }
  return 1;
}

static int font_score(const char *path) {
  const char *base = strrchr(path, '/');
  base = base ? base + 1 : path;
  if (contains_ci(base, "gothic") || contains_ci(base, "goth")) {
    return contains_ci(base, "bold") ? 90 : 100;
  }
  if (contains_ci(base, "heisei") || contains_ci(base, "maru")) {
    return 85;
  }
  if (contains_ci(base, "noto") && (contains_ci(base, "cjk") || contains_ci(base, "jp"))) {
    return 80;
  }
  if (contains_ci(base, "mincho") || contains_ci(base, "tsuku")) {
    return 70;
  }
  if (contains_ci(path, "ja_font")) {
    return 65;
  }
  if (contains_ci(base, "code2000")) {
    return 40;
  }
  if (contains_ci(base, "hiragino") || contains_ci(base, "heiti") || contains_ci(base, "arial unicode") ||
      contains_ci(base, "sourcehan") || contains_ci(base, "source han") || contains_ci(base, "droid")) {
    return 30;
  }
  return 0;
}

static int font_ext(const char *path) {
  return ends_ci(path, ".ttf") || ends_ci(path, ".ttc") || ends_ci(path, ".otf");
}

static int face_has_jp(const stbtt_fontinfo *info) {
  return stbtt_FindGlyphIndex(info, 0x3042) != 0 && stbtt_FindGlyphIndex(info, 0x4E00) != 0;
}

static int try_file(const char *path) {
  int fd;
  struct stat st;
  void *data;
  int i;
  int faces = 0;
  if (!path || !path[0] || !font_ext(path)) {
    return 0;
  }
  fd = open(path, O_RDONLY);
  if (fd < 0) {
    return 0;
  }
  if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 1024 ||
      st.st_size > (off_t)80 * 1024 * 1024) {
    close(fd);
    return 0;
  }
  data = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
  close(fd);
  if (data == MAP_FAILED) {
    return 0;
  }
  for (i = 0; i < 8; i++) {
    int off = stbtt_GetFontOffsetForIndex(data, i);
    stbtt_fontinfo info;
    if (off < 0) {
      break;
    }
    if (!stbtt_InitFont(&info, data, off)) {
      continue;
    }
    faces++;
    if (!face_has_jp(&info)) {
      continue;
    }
    g_info = info;
    g_data = data;
    g_size = (size_t)st.st_size;
    snprintf(g_path, sizeof g_path, "%s", path);
    g_ready = 1;
    return 1;
  }
  if (faces == 0) {
    stbtt_fontinfo info;
    if (stbtt_InitFont(&info, data, 0) && face_has_jp(&info)) {
      g_info = info;
      g_data = data;
      g_size = (size_t)st.st_size;
      snprintf(g_path, sizeof g_path, "%s", path);
      g_ready = 1;
      return 1;
    }
  }
  munmap(data, (size_t)st.st_size);
  return 0;
}

static int cand_cmp(const void *a, const void *b) {
  const Cand *ca = a;
  const Cand *cb = b;
  if (ca->score != cb->score) {
    return cb->score - ca->score;
  }
  return strcmp(ca->path, cb->path);
}

static void consider_file(Cand *cands, int *n, const char *path) {
  int score;
  int low;
  int i;
  if (!font_ext(path)) {
    return;
  }
  score = font_score(path);
  if (contains_ci(path, "ja_font") || contains_ci(path, "/mnt/us/fonts")) {
    if (score < 65) {
      score = 65;
    }
  } else if (score <= 0) {
    return;
  }
  if (*n < CAND_MAX) {
    snprintf(cands[*n].path, sizeof cands[*n].path, "%s", path);
    cands[*n].score = score;
    (*n)++;
    return;
  }
  low = 0;
  for (i = 1; i < *n; i++) {
    if (cands[i].score < cands[low].score) {
      low = i;
    }
  }
  if (score <= cands[low].score) {
    return;
  }
  snprintf(cands[low].path, sizeof cands[low].path, "%s", path);
  cands[low].score = score;
}

static void scan_dir(const char *dir, int depth, Cand *cands, int *n) {
  DIR *dp;
  struct dirent *de;
  if (depth > 2 || g_ready) {
    return;
  }
  dp = opendir(dir);
  if (!dp) {
    return;
  }
  while ((de = readdir(dp)) != NULL) {
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
      if (stat(path, &st) == 0 && S_ISREG(st.st_mode)) {
        consider_file(cands, n, path);
      }
      continue;
    }
    if (S_ISDIR(st.st_mode)) {
      scan_dir(path, depth + 1, cands, n);
    } else if (S_ISREG(st.st_mode)) {
      consider_file(cands, n, path);
    }
  }
  closedir(dp);
}

int yomu_font_ready(void) { return g_ready; }

int yomu_font_open(void) {
  static const char *preferred[] = {
      "/usr/java/lib/fonts/TBGothicMed_213.ttf",
      "/usr/java/lib/fonts/TBGothicBold_213.ttf",
      "/usr/java/lib/fonts/HeiSeiMaruGoth213_E.ttf",
      "/usr/java/lib/fonts/HeiSeiMaruGoth213_E_Bold.ttf",
      "/usr/java/lib/fonts/TBMinchoMedium_213.ttf",
      "/usr/java/lib/fonts/TBMinchoBold_213.ttf",
      "/var/local/font/mnt/ja_font/fonts/TsukuMinPr5-Medium.ttf",
      "/var/local/font/mnt/ja_font/fonts/TsukuMinPr5-Bold.ttf",
      "/usr/java/lib/fonts/code2000.ttf",
      "/usr/java/lib/fonts/Code2000.ttf",
      "/System/Library/Fonts/Supplemental/Arial Unicode.ttf",
      "/Library/Fonts/Arial Unicode.ttf",
      "/System/Library/Fonts/Hiragino Sans GB.ttc",
      "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
      "/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc",
      "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc",
      "/usr/share/fonts/truetype/droid/DroidSansFallbackFull.ttf",
      NULL};
  static const char *dirs[] = {
      "/var/local/font/mnt/ja_font/fonts", "/usr/java/lib/fonts", "/mnt/us/fonts",
      "/opt/amazon/ebook/fonts", NULL};
  const char *env;
  int i;
  if (g_tried) {
    return g_ready;
  }
  g_tried = 1;
  int64_t opened_at = yomu_ms();
  env = getenv("YOMU_FONT");
  if (env && env[0]) {
    if (try_file(env)) {
      yomu_log("japanese font %s in %lld ms", g_path, (long long)(yomu_ms() - opened_at));
      return 1;
    }
    yomu_log("YOMU_FONT %s has no Japanese glyphs", env);
  }
  for (i = 0; preferred[i]; i++) {
    if (try_file(preferred[i])) {
      yomu_log("japanese font %s in %lld ms", g_path, (long long)(yomu_ms() - opened_at));
      return 1;
    }
  }
  {
    Cand *cands = calloc(CAND_MAX, sizeof *cands);
    int n = 0;
    if (cands) {
      for (i = 0; dirs[i]; i++) {
        scan_dir(dirs[i], 0, cands, &n);
      }
      if (n > 1) {
        qsort(cands, (size_t)n, sizeof *cands, cand_cmp);
      }
      for (i = 0; i < n && !g_ready; i++) {
        try_file(cands[i].path);
      }
      free(cands);
    }
  }
  if (g_ready) {
    yomu_log("japanese font %s in %lld ms", g_path, (long long)(yomu_ms() - opened_at));
    return 1;
  }
  yomu_log("no japanese font in %lld ms; set YOMU_FONT to a .ttf or .ttc",
             (long long)(yomu_ms() - opened_at));
  return 0;
}

static Glyph *cache_find(int cp, int px) {
  int i;
  for (i = 0; i < CACHE_N; i++) {
    if (g_cache[i].taken && g_cache[i].cp == cp && g_cache[i].px == px) {
      return &g_cache[i];
    }
  }
  return NULL;
}

static Glyph *cache_slot(void) {
  int i;
  Glyph *g;
  for (i = 0; i < CACHE_N; i++) {
    if (!g_cache[i].taken) {
      return &g_cache[i];
    }
  }
  g = &g_cache[g_clock % CACHE_N];
  g_clock++;
  stbtt_FreeBitmap(g->bmp, NULL);
  memset(g, 0, sizeof *g);
  return g;
}

int yomu_font_glyph(int cp, int px, const uint8_t **bmp, int *w, int *h, int *xoff, int *yoff,
                      int *advance) {
  Glyph *g;
  float scale;
  int ascent, descent, gap;
  int baseline;
  int adv, lsb;
  int gw = 0, gh = 0, bx = 0, by = 0;
  unsigned char *bits;
  if (!bmp || !w || !h || !xoff || !yoff || !advance || px < 1 || cp < 0) {
    return 0;
  }
  *bmp = NULL;
  *w = *h = *xoff = *yoff = *advance = 0;
  if (!yomu_font_open() || !g_data || g_size < 1024 || cp < 128) {
    return 0;
  }
  g = cache_find(cp, px);
  if (g) {
    g_glyph_hit++;
  } else {
    g_glyph_miss++;
    if (stbtt_FindGlyphIndex(&g_info, cp) == 0) {
      g = cache_slot();
      g->taken = 1;
      g->ok = 0;
      g->cp = cp;
      g->px = px;
    } else {
      scale = stbtt_ScaleForPixelHeight(&g_info, (float)px);
      stbtt_GetFontVMetrics(&g_info, &ascent, &descent, &gap);
      baseline = (int)((float)ascent * scale + 0.5f);
      stbtt_GetCodepointHMetrics(&g_info, cp, &adv, &lsb);
      (void)lsb;
      (void)descent;
      bits = stbtt_GetCodepointBitmap(&g_info, scale, scale, cp, &gw, &gh, &bx, &by);
      if ((gw > 0 || gh > 0) && !bits) {
        return 0;
      }
      if (gw > px * 4 || gh > px * 4 || gw < 0 || gh < 0) {
        stbtt_FreeBitmap(bits, NULL);
        return 0;
      }
      g = cache_slot();
      g->taken = 1;
      g->ok = 1;
      g->cp = cp;
      g->px = px;
      g->w = gw;
      g->h = gh;
      g->xoff = bx;
      g->yoff = baseline + by;
      g->advance = (int)((float)adv * scale + 0.5f);
      if (g->advance < 1) {
        g->advance = px;
      }
      if (g->advance > px * 4) {
        g->advance = px * 4;
      }
      g->bmp = bits;
    }
  }
  if (!g->ok) {
    return 0;
  }
  *bmp = g->bmp;
  *w = g->w;
  *h = g->h;
  *xoff = g->xoff;
  *yoff = g->yoff;
  *advance = g->advance;
  return 1;
}

void yomu_font_take_stats(int *hits, int *misses) {
  if (hits) {
    *hits = g_glyph_hit;
  }
  if (misses) {
    *misses = g_glyph_miss;
  }
  g_glyph_hit = 0;
  g_glyph_miss = 0;
}
