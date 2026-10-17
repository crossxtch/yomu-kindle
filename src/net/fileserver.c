#include "fileserver.h"

#include "filepage.h"
#include "log.h"
#include "mdns.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#include <poll.h>

#define HEADER_MAX 16384
#define FORM_MAX 8192
#define IO_MAX 16384
#define FS_PATH 1024
#define FS_NAME 255
#define LIST_MAX 400
#define DELETE_DEPTH 32
#define UPLOAD_MAX (1024ll * 1024ll * 1024ll)

struct FileServer {
  char root[FS_PATH];
  char ip[64];
  char urls[3][80];
  int nurls;
  int port;
  int listen_fd;
  int started;
  int firewall;
  atomic_int stop;
  atomic_int dirty;
  pthread_t thread;
  Mdns *mdns;
};

typedef struct Ent {
  char name[FS_NAME + 1];
  int64_t size;
  int is_dir;
} Ent;

typedef struct Req {
  char method[8];
  char path[FS_PATH];
  char query[FS_PATH];
  char file_path[FS_PATH];
  char file_name[FS_NAME + 1];
  int64_t content_length;
  char *buf;
  size_t header_end;
  size_t len;
} Req;

static void ignore_pipe(void) {
  static int once = 0;
  if (!once) {
    signal(SIGPIPE, SIG_IGN);
    once = 1;
  }
}

static int send_flags(void) {
#ifdef MSG_NOSIGNAL
  return MSG_NOSIGNAL;
#else
  return 0;
#endif
}

static int send_all(int fd, const void *data, size_t n) {
  const char *p = data;
  while (n > 0) {
    ssize_t w = send(fd, p, n, send_flags());
    if (w < 0 && errno == EINTR) {
      continue;
    }
    if (w <= 0) {
      return -1;
    }
    p += w;
    n -= (size_t)w;
  }
  return 0;
}

static int write_full(int fd, const void *data, size_t n) {
  const char *p = data;
  while (n > 0) {
    ssize_t w = write(fd, p, n);
    if (w < 0 && errno == EINTR) {
      continue;
    }
    if (w <= 0) {
      return -1;
    }
    p += w;
    n -= (size_t)w;
  }
  return 0;
}

static int reply(int fd, int status, const char *reason, const char *type, const char *extra, const void *body,
                 size_t len) {
  char hdr[640];
  int n = snprintf(hdr, sizeof hdr,
                   "HTTP/1.1 %d %s\r\n"
                   "Content-Type: %s\r\n"
                   "Content-Length: %zu\r\n"
                   "Connection: close\r\n"
                   "Cache-Control: no-store\r\n"
                   "%s"
                   "\r\n",
                   status, reason, type, len, extra ? extra : "");
  if (n < 0 || (size_t)n >= sizeof hdr) {
    return -1;
  }
  if (send_all(fd, hdr, (size_t)n) != 0) {
    return -1;
  }
  if (len && body) {
    return send_all(fd, body, len);
  }
  return 0;
}

static int reply_text(int fd, int status, const char *reason, const char *msg) {
  return reply(fd, status, reason, "text/plain; charset=utf-8", NULL, msg, strlen(msg));
}

static int hex_val(char c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  if (c >= 'A' && c <= 'F') {
    return c - 'A' + 10;
  }
  return -1;
}

/* Percent-encoding is longer than the decoded name. cap is the decoded size,
 * including the trailing NUL. A name that fits is kept whole; a name that does
 * not fit, or a bad % sequence, is rejected rather than cut down. */
static int url_decode_n(const char *src, size_t src_n, char *dst, size_t cap, int plus) {
  size_t n = 0;
  size_t i = 0;
  if (!dst || cap == 0) {
    return -1;
  }
  if (!src) {
    dst[0] = '\0';
    return 0;
  }
  while (i < src_n) {
    unsigned char c;
    if (src[i] == '%') {
      int hi;
      int lo;
      if (i + 2 >= src_n) {
        dst[0] = '\0';
        return -1;
      }
      hi = hex_val(src[i + 1]);
      lo = hex_val(src[i + 2]);
      if (hi < 0 || lo < 0) {
        dst[0] = '\0';
        return -1;
      }
      c = (unsigned char)((hi << 4) | lo);
      i += 3;
    } else if (plus && src[i] == '+') {
      c = ' ';
      i++;
    } else {
      c = (unsigned char)src[i++];
    }
    if (n + 1 >= cap) {
      dst[0] = '\0';
      return -1;
    }
    dst[n++] = (char)c;
  }
  dst[n] = '\0';
  return 0;
}

static int segment_ok(const char *s) {
  size_t n;
  if (!s || !s[0] || strcmp(s, ".") == 0 || strcmp(s, "..") == 0) {
    return 0;
  }
  if (s[0] == '.' || strcmp(s, "System Volume Information") == 0) {
    return 0;
  }
  n = strlen(s);
  if (n > FS_NAME) {
    return 0;
  }
  for (size_t i = 0; i < n; i++) {
    unsigned char c = (unsigned char)s[i];
    if (c < 0x20 || c == 0x7f || c == '/' || c == '\\' || c == '"') {
      return 0;
    }
  }
  return 1;
}

static int root_reserved(const char *s) {
  return strcmp(s, "system") == 0 || strcmp(s, "lost+found") == 0 || strcmp(s, "extensions") == 0;
}

/* kind 0: any existing non-link. kind 1: existing directory. kind 2: a new or
 * existing regular file whose parent directory exists. */
static int resolve_path(const FileServer *fs, const char *urlpath, int kind, char *out, size_t cap, int *status) {
  const char *p = urlpath && urlpath[0] ? urlpath : "/";
  if (p[0] != '/' || strchr(p, '\\')) {
    *status = 403;
    return -1;
  }
  if (strlen(fs->root) + 1 >= cap) {
    *status = 414;
    return -1;
  }
  memcpy(out, fs->root, strlen(fs->root) + 1);
  if (strcmp(p, "/") == 0) {
    if (kind == 2) {
      *status = 400;
      return -1;
    }
    return 0;
  }
  int first = 1;
  const char *seg = p;
  while (*seg) {
    while (*seg == '/') {
      seg++;
    }
    if (!*seg) {
      break;
    }
    const char *slash = strchr(seg, '/');
    size_t n = slash ? (size_t)(slash - seg) : strlen(seg);
    char name[FS_NAME + 1];
    if (n == 0 || n > FS_NAME) {
      *status = 403;
      return -1;
    }
    memcpy(name, seg, n);
    name[n] = '\0';
    if (!segment_ok(name) || (first && root_reserved(name))) {
      *status = 403;
      return -1;
    }
    first = 0;
    size_t cur = strlen(out);
    if (cur + 1 + n + 1 > cap) {
      *status = 414;
      return -1;
    }
    out[cur] = '/';
    memcpy(out + cur + 1, name, n + 1);
    int last = !slash || slash[1] == '\0';
    struct stat st;
    if (lstat(out, &st) != 0) {
      if (kind == 2 && last && errno == ENOENT) {
        return 0;
      }
      *status = 404;
      return -1;
    }
    if (S_ISLNK(st.st_mode)) {
      *status = 403;
      return -1;
    }
    if (!last && !S_ISDIR(st.st_mode)) {
      *status = 404;
      return -1;
    }
    if (last && kind == 1 && !S_ISDIR(st.st_mode)) {
      *status = 404;
      return -1;
    }
    if (last && kind == 2 && S_ISDIR(st.st_mode)) {
      *status = 409;
      return -1;
    }
    if (last && kind == 2 && !S_ISREG(st.st_mode) && errno == 0 && lstat(out, &st) == 0 && !S_ISREG(st.st_mode)) {
      *status = 403;
      return -1;
    }
    if (!slash) {
      break;
    }
    seg = slash + 1;
  }
  return 0;
}

static int form_get(const char *body, const char *key, char *out, size_t cap) {
  size_t klen = strlen(key);
  const char *p = body ? body : "";
  out[0] = '\0';
  while (*p) {
    const char *amp = strchr(p, '&');
    size_t n = amp ? (size_t)(amp - p) : strlen(p);
    const char *eq = memchr(p, '=', n);
    if (eq && (size_t)(eq - p) == klen && memcmp(p, key, klen) == 0) {
      size_t vn = n - klen - 1;
      return url_decode_n(eq + 1, vn, out, cap, 1);
    }
    if (!amp) {
      break;
    }
    p = amp + 1;
  }
  return 1;
}

static int header_match(const char *name, const char *want) { return strcasecmp(name, want) == 0; }

/* The target may contain blanks ("My Book.epub"). The version token, when
 * present, is the last " HTTP/" on the line. */
static int request_target(const char *line, char *method, size_t method_cap, char *target, size_t target_cap) {
  const char *sp = line ? strchr(line, ' ') : NULL;
  if (!sp || sp == line) {
    return -1;
  }
  size_t mlen = (size_t)(sp - line);
  if (mlen + 1 > method_cap) {
    return -1;
  }
  memcpy(method, line, mlen);
  method[mlen] = '\0';
  const char *start = sp + 1;
  while (*start == ' ' || *start == '\t') {
    start++;
  }
  const char *version = NULL;
  for (const char *p = start; *p; p++) {
    if (*p == ' ' && strncmp(p + 1, "HTTP/", 5) == 0) {
      version = p;
    }
  }
  const char *end = version ? version : start + strlen(start);
  while (end > start && (end[-1] == ' ' || end[-1] == '\t')) {
    end--;
  }
  size_t n = (size_t)(end - start);
  if (n == 0 || n + 1 > target_cap) {
    return -1;
  }
  memcpy(target, start, n);
  target[n] = '\0';
  return 0;
}

static int read_request(int fd, Req *req) {
  memset(req, 0, sizeof *req);
  req->content_length = -1;
  req->buf = malloc(HEADER_MAX + 1);
  if (!req->buf) {
    return -1;
  }
  size_t len = 0;
  size_t header_end = 0;
  int found = 0;
  while (len < HEADER_MAX && !found) {
    ssize_t r = read(fd, req->buf + len, HEADER_MAX - len);
    if (r < 0 && errno == EINTR) {
      continue;
    }
    if (r <= 0) {
      return -1;
    }
    size_t start = len > 3 ? len - 3 : 0;
    len += (size_t)r;
    for (size_t i = start; i + 3 < len; i++) {
      if (req->buf[i] == '\r' && req->buf[i + 1] == '\n' && req->buf[i + 2] == '\r' && req->buf[i + 3] == '\n') {
        header_end = i + 4;
        found = 1;
        break;
      }
    }
  }
  if (!found) {
    reply_text(fd, 431, "Request Header Fields Too Large", "ヘッダが大きすぎます");
    return -1;
  }
  req->len = len;
  req->header_end = header_end;
  /* The blank line's CRLF is also the last header's CRLF, so the header
   * bytes run through header_end and the body starts there. */
  char *cursor = req->buf;
  char *end = req->buf + header_end;
  int first = 1;
  while (cursor + 1 < end) {
    char *nl = cursor;
    while (nl + 1 < end && !(nl[0] == '\r' && nl[1] == '\n')) {
      nl++;
    }
    if (nl + 1 >= end) {
      break;
    }
    if (nl == cursor) {
      break;
    }
    *nl = '\0';
    if (first) {
      char target[FS_PATH];
      if (request_target(cursor, req->method, sizeof req->method, target, sizeof target) != 0) {
        reply_text(fd, 400, "Bad Request", "リクエストが不正です");
        return -1;
      }
      char *qmark = strchr(target, '?');
      if (qmark) {
        *qmark = '\0';
        snprintf(req->query, sizeof req->query, "%s", qmark + 1);
      }
      snprintf(req->path, sizeof req->path, "%s", target);
      first = 0;
    } else {
      char *colon = strchr(cursor, ':');
      if (colon) {
        *colon = '\0';
        char *val = colon + 1;
        while (*val == ' ' || *val == '\t') {
          val++;
        }
        if (header_match(cursor, "Content-Length")) {
          char *bend = NULL;
          long long n = strtoll(val, &bend, 10);
          if (bend == val || n < 0) {
            reply_text(fd, 400, "Bad Request", "長さが不正です");
            return -1;
          }
          req->content_length = n;
        } else if (header_match(cursor, "X-File-Path")) {
          if (url_decode_n(val, strlen(val), req->file_path, sizeof req->file_path, 0) != 0) {
            reply_text(fd, 400, "Bad Request", "パスが不正です");
            return -1;
          }
        } else if (header_match(cursor, "X-File-Name")) {
          if (url_decode_n(val, strlen(val), req->file_name, sizeof req->file_name, 0) != 0) {
            reply_text(fd, 400, "Bad Request", "名前が不正です");
            return -1;
          }
        }
      }
    }
    cursor = nl + 2;
  }
  if (first) {
    reply_text(fd, 400, "Bad Request", "リクエストが不正です");
    return -1;
  }
  return 0;
}

static void discard_body(int fd, const Req *req) {
  if (req->content_length < 0) {
    return;
  }
  int64_t left = req->content_length;
  size_t have = req->len - req->header_end;
  if ((int64_t)have >= left) {
    return;
  }
  left -= (int64_t)have;
  char buf[4096];
  while (left > 0) {
    size_t n = left > (int64_t)sizeof buf ? sizeof buf : (size_t)left;
    ssize_t r = read(fd, buf, n);
    if (r < 0 && errno == EINTR) {
      continue;
    }
    if (r <= 0) {
      return;
    }
    left -= r;
  }
}

static int take_body(int fd, Req *req, char **out, size_t *out_len) {
  *out = NULL;
  *out_len = 0;
  if (req->content_length < 0) {
    reply_text(fd, 411, "Length Required", "長さが必要です");
    return -1;
  }
  if (req->content_length > FORM_MAX) {
    discard_body(fd, req);
    reply_text(fd, 413, "Payload Too Large", "本文が大きすぎます");
    return -1;
  }
  size_t need = (size_t)req->content_length;
  char *body = malloc(need + 1);
  if (!body) {
    return -1;
  }
  size_t have = req->len - req->header_end;
  if (have > need) {
    have = need;
  }
  if (have) {
    memcpy(body, req->buf + req->header_end, have);
  }
  size_t got = have;
  while (got < need) {
    ssize_t r = read(fd, body + got, need - got);
    if (r < 0 && errno == EINTR) {
      continue;
    }
    if (r <= 0) {
      free(body);
      return -1;
    }
    got += (size_t)r;
  }
  body[need] = '\0';
  *out = body;
  *out_len = need;
  return 0;
}

static int ent_cmp(const void *a, const void *b) {
  const Ent *ea = a;
  const Ent *eb = b;
  if (ea->is_dir != eb->is_dir) {
    return eb->is_dir - ea->is_dir;
  }
  return strcasecmp(ea->name, eb->name);
}

typedef struct Buf {
  char *data;
  size_t len;
  size_t cap;
} Buf;

static int buf_add(Buf *b, const char *s, size_t n) {
  if (b->len + n + 1 > b->cap) {
    size_t cap = b->cap ? b->cap : 256;
    while (cap < b->len + n + 1) {
      cap *= 2;
    }
    char *grown = realloc(b->data, cap);
    if (!grown) {
      return -1;
    }
    b->data = grown;
    b->cap = cap;
  }
  memcpy(b->data + b->len, s, n);
  b->len += n;
  b->data[b->len] = '\0';
  return 0;
}

static int buf_add_str(Buf *b, const char *s) { return buf_add(b, s, strlen(s)); }

static int buf_json_str(Buf *b, const char *s) {
  if (buf_add(b, "\"", 1) != 0) {
    return -1;
  }
  for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
    char esc[8];
    if (*p == '"' || *p == '\\') {
      esc[0] = '\\';
      esc[1] = (char)*p;
      if (buf_add(b, esc, 2) != 0) {
        return -1;
      }
    } else if (*p < 0x20) {
      int n = snprintf(esc, sizeof esc, "\\u%04x", *p);
      if (buf_add(b, esc, (size_t)n) != 0) {
        return -1;
      }
    } else if (buf_add(b, (const char *)p, 1) != 0) {
      return -1;
    }
  }
  return buf_add(b, "\"", 1);
}

static int handle_list(FileServer *fs, int fd, const Req *req) {
  char rel[FS_PATH];
  int missing = form_get(req->query, "path", rel, sizeof rel);
  if (missing < 0) {
    return reply_text(fd, 400, "Bad Request", "パスが不正です");
  }
  if (missing > 0) {
    snprintf(rel, sizeof rel, "/");
  }
  char full[FS_PATH];
  int status = 404;
  if (resolve_path(fs, rel, 1, full, sizeof full, &status) != 0) {
    return reply(fd, status == 403 ? 403 : 404, status == 403 ? "Forbidden" : "Not Found", "application/json", NULL, "[]",
                 2);
  }
  DIR *dir = opendir(full);
  if (!dir) {
    return reply(fd, 404, "Not Found", "application/json", NULL, "[]", 2);
  }
  int at_root = strcmp(full, fs->root) == 0;
  Ent *ents = calloc(LIST_MAX, sizeof *ents);
  if (!ents) {
    closedir(dir);
    return reply_text(fd, 500, "Internal Server Error", "メモリ不足です");
  }
  int n = 0;
  struct dirent *de;
  while ((de = readdir(dir)) != NULL && n < LIST_MAX) {
    const char *name = de->d_name;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
      continue;
    }
    if (!segment_ok(name) || (at_root && root_reserved(name))) {
      continue;
    }
    char child[FS_PATH];
    if (snprintf(child, sizeof child, "%s/%s", full, name) >= (int)sizeof child) {
      continue;
    }
    struct stat st;
    if (lstat(child, &st) != 0 || S_ISLNK(st.st_mode)) {
      continue;
    }
    if (!S_ISDIR(st.st_mode) && !S_ISREG(st.st_mode)) {
      continue;
    }
    snprintf(ents[n].name, sizeof ents[n].name, "%s", name);
    ents[n].is_dir = S_ISDIR(st.st_mode) ? 1 : 0;
    ents[n].size = ents[n].is_dir ? 0 : (int64_t)st.st_size;
    n++;
  }
  closedir(dir);
  if (n > 1) {
    qsort(ents, (size_t)n, sizeof *ents, ent_cmp);
  }
  Buf json = {0};
  if (buf_add(&json, "[", 1) != 0) {
    free(ents);
    free(json.data);
    return reply_text(fd, 500, "Internal Server Error", "メモリ不足です");
  }
  for (int i = 0; i < n; i++) {
    char size[32];
    snprintf(size, sizeof size, "%lld", (long long)ents[i].size);
    if ((i && buf_add(&json, ",", 1) != 0) || buf_add_str(&json, "{\"name\":") != 0 ||
        buf_json_str(&json, ents[i].name) != 0 || buf_add_str(&json, ",\"size\":") != 0 || buf_add_str(&json, size) != 0 ||
        buf_add_str(&json, ",\"isDirectory\":") != 0 ||
        buf_add_str(&json, ents[i].is_dir ? "true}" : "false}") != 0) {
      free(ents);
      free(json.data);
      return reply_text(fd, 500, "Internal Server Error", "メモリ不足です");
    }
  }
  free(ents);
  if (buf_add(&json, "]", 1) != 0) {
    free(json.data);
    return reply_text(fd, 500, "Internal Server Error", "メモリ不足です");
  }
  int rc = reply(fd, 200, "OK", "application/json", NULL, json.data, json.len);
  free(json.data);
  return rc;
}

static int append_pct(char *out, size_t cap, size_t *used, unsigned char c) {
  static const char hex[] = "0123456789ABCDEF";
  if (*used + 3 >= cap) {
    return -1;
  }
  out[(*used)++] = '%';
  out[(*used)++] = hex[c >> 4];
  out[(*used)++] = hex[c & 0xf];
  return 0;
}

/* Quoted filename keeps a blank. filename* is percent-encoded UTF-8 for
 * clients that cut the quoted name at the first blank. */
static int disposition(const char *name, char *out, size_t cap) {
  int n = snprintf(out, cap, "Content-Disposition: attachment; filename=\"");
  if (n < 0 || (size_t)n >= cap) {
    return -1;
  }
  size_t used = (size_t)n;
  for (const unsigned char *p = (const unsigned char *)(name ? name : ""); *p; p++) {
    if (*p == '\\' || *p == '"' || used + 1 >= cap) {
      return -1;
    }
    out[used++] = (char)*p;
  }
  n = snprintf(out + used, cap - used, "\"; filename*=UTF-8''");
  if (n < 0 || (size_t)n >= cap - used) {
    return -1;
  }
  used += (size_t)n;
  for (const unsigned char *p = (const unsigned char *)(name ? name : ""); *p; p++) {
    if (append_pct(out, cap, &used, *p) != 0) {
      return -1;
    }
  }
  if (used + 3 > cap) {
    return -1;
  }
  out[used++] = '\r';
  out[used++] = '\n';
  out[used] = '\0';
  return 0;
}

static int handle_download(FileServer *fs, int fd, const Req *req) {
  char rel[FS_PATH];
  int missing = form_get(req->query, "path", rel, sizeof rel);
  if (missing != 0) {
    return reply_text(fd, missing < 0 ? 400 : 400, "Bad Request", "パスがありません");
  }
  char full[FS_PATH];
  int status = 404;
  if (resolve_path(fs, rel, 0, full, sizeof full, &status) != 0) {
    if (status == 403) {
      return reply_text(fd, 403, "Forbidden", "保護されたファイルです");
    }
    return reply_text(fd, 404, "Not Found", "ファイルが見つかりません");
  }
  int file = open(full, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (file < 0) {
    return reply_text(fd, 404, "Not Found", "ファイルが見つかりません");
  }
  struct stat st;
  if (fstat(file, &st) != 0 || !S_ISREG(st.st_mode)) {
    close(file);
    return reply_text(fd, 404, "Not Found", "ファイルが見つかりません");
  }
  const char *base = strrchr(full, '/');
  base = base ? base + 1 : full;
  char extra[1200];
  if (disposition(base, extra, sizeof extra) != 0) {
    close(file);
    return reply_text(fd, 500, "Internal Server Error", "ファイルを取得できませんでした");
  }
  char hdr[1600];
  int n = snprintf(hdr, sizeof hdr,
                   "HTTP/1.1 200 OK\r\n"
                   "Content-Type: application/octet-stream\r\n"
                   "Content-Length: %lld\r\n"
                   "Connection: close\r\n"
                   "Cache-Control: no-store\r\n"
                   "%s"
                   "\r\n",
                   (long long)st.st_size, extra);
  if (n < 0 || (size_t)n >= sizeof hdr || send_all(fd, hdr, (size_t)n) != 0) {
    close(file);
    return -1;
  }
  int64_t left = (int64_t)st.st_size;
  char buf[IO_MAX];
  while (left > 0) {
    if (atomic_load(&fs->stop)) {
      close(file);
      return -1;
    }
    size_t chunk = left > (int64_t)sizeof buf ? sizeof buf : (size_t)left;
    ssize_t r = read(file, buf, chunk);
    if (r < 0 && errno == EINTR) {
      continue;
    }
    if (r <= 0 || send_all(fd, buf, (size_t)r) != 0) {
      close(file);
      return -1;
    }
    left -= r;
  }
  close(file);
  return 0;
}

static int delete_children(const char *path, int depth) {
  if (depth > DELETE_DEPTH) {
    return -1;
  }
  DIR *dir = opendir(path);
  if (!dir) {
    return -1;
  }
  int rc = 0;
  struct dirent *de;
  while ((de = readdir(dir)) != NULL) {
    if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) {
      continue;
    }
    char child[FS_PATH];
    if (snprintf(child, sizeof child, "%s/%s", path, de->d_name) >= (int)sizeof child) {
      rc = -1;
      break;
    }
    struct stat st;
    if (lstat(child, &st) != 0) {
      rc = -1;
      break;
    }
    if (S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode)) {
      if (delete_children(child, depth + 1) != 0 || rmdir(child) != 0) {
        rc = -1;
        break;
      }
    } else if (unlink(child) != 0) {
      rc = -1;
      break;
    }
  }
  closedir(dir);
  return rc;
}

static int handle_delete(FileServer *fs, int fd, const char *body) {
  char rel[FS_PATH];
  if (form_get(body, "path", rel, sizeof rel) != 0) {
    return reply_text(fd, 400, "Bad Request", "パスがありません");
  }
  char full[FS_PATH];
  int status = 404;
  if (resolve_path(fs, rel, 0, full, sizeof full, &status) != 0) {
    if (status == 403) {
      return reply_text(fd, 403, "Forbidden", "保護されたファイルです");
    }
    return reply_text(fd, 404, "Not Found", "削除できませんでした");
  }
  if (strcmp(full, fs->root) == 0) {
    return reply_text(fd, 403, "Forbidden", "ルートは削除できません");
  }
  struct stat st;
  if (lstat(full, &st) != 0 || S_ISLNK(st.st_mode)) {
    return reply_text(fd, 403, "Forbidden", "保護されたファイルです");
  }
  int ok;
  const char *fail = "削除できませんでした";
  if (S_ISDIR(st.st_mode)) {
    ok = delete_children(full, 0) == 0 && rmdir(full) == 0;
    fail = "フォルダを削除できませんでした";
  } else {
    ok = unlink(full) == 0;
  }
  if (!ok) {
    yomu_log("transfer delete failed %s", full);
    return reply_text(fd, 500, "Internal Server Error", fail);
  }
  atomic_store(&fs->dirty, 1);
  yomu_log("transfer deleted %s", full);
  return reply_text(fd, 200, "OK", "OK");
}

static int handle_mkdir(FileServer *fs, int fd, const char *body) {
  char name[FS_NAME + 1];
  char rel[FS_PATH];
  if (form_get(body, "name", name, sizeof name) != 0 || !name[0]) {
    return reply_text(fd, 400, "Bad Request", "名前がありません");
  }
  if (!segment_ok(name)) {
    return reply_text(fd, 403, "Forbidden", "保護されたファイルです");
  }
  int parent_missing = form_get(body, "path", rel, sizeof rel);
  if (parent_missing < 0) {
    return reply_text(fd, 400, "Bad Request", "パスが不正です");
  }
  if (parent_missing > 0) {
    snprintf(rel, sizeof rel, "/");
  }
  char parent[FS_PATH];
  int status = 404;
  if (resolve_path(fs, rel, 1, parent, sizeof parent, &status) != 0) {
    return reply_text(fd, status == 403 ? 403 : 500, status == 403 ? "Forbidden" : "Internal Server Error",
                      status == 403 ? "保護されたファイルです" : "フォルダを作成できませんでした");
  }
  if (strcmp(parent, fs->root) == 0 && root_reserved(name)) {
    return reply_text(fd, 403, "Forbidden", "保護されたファイルです");
  }
  char full[FS_PATH];
  if (snprintf(full, sizeof full, "%s/%s", parent, name) >= (int)sizeof full) {
    return reply_text(fd, 414, "URI Too Long", "URLが長すぎます");
  }
  if (mkdir(full, 0755) != 0) {
    return reply_text(fd, 500, "Internal Server Error", "フォルダを作成できませんでした");
  }
  atomic_store(&fs->dirty, 1);
  return reply_text(fd, 200, "OK", "OK");
}

static int handle_rename(FileServer *fs, int fd, const char *body) {
  char rel[FS_PATH];
  char name[FS_NAME + 1];
  if (form_get(body, "path", rel, sizeof rel) != 0 || form_get(body, "name", name, sizeof name) != 0) {
    return reply_text(fd, 400, "Bad Request", "パスか名前がありません");
  }
  if (!segment_ok(name)) {
    return reply_text(fd, 403, "Forbidden", "保護されたファイルです");
  }
  char oldp[FS_PATH];
  int status = 404;
  if (resolve_path(fs, rel, 0, oldp, sizeof oldp, &status) != 0) {
    return reply_text(fd, status == 403 ? 403 : 500, status == 403 ? "Forbidden" : "Internal Server Error",
                      status == 403 ? "保護されたファイルです" : "名前を変更できませんでした");
  }
  if (strcmp(oldp, fs->root) == 0) {
    return reply_text(fd, 403, "Forbidden", "保護されたファイルです");
  }
  char parent[FS_PATH];
  snprintf(parent, sizeof parent, "%s", oldp);
  char *slash = strrchr(parent, '/');
  if (!slash) {
    return reply_text(fd, 500, "Internal Server Error", "名前を変更できませんでした");
  }
  *slash = '\0';
  if (strcmp(parent, fs->root) == 0 && root_reserved(name)) {
    return reply_text(fd, 403, "Forbidden", "保護されたファイルです");
  }
  char neu[FS_PATH];
  if (snprintf(neu, sizeof neu, "%s/%s", parent, name) >= (int)sizeof neu) {
    return reply_text(fd, 414, "URI Too Long", "URLが長すぎます");
  }
  if (rename(oldp, neu) != 0) {
    return reply_text(fd, 500, "Internal Server Error", "名前を変更できませんでした");
  }
  atomic_store(&fs->dirty, 1);
  return reply_text(fd, 200, "OK", "OK");
}

static int handle_move(FileServer *fs, int fd, const char *body) {
  char rel[FS_PATH];
  char dest_rel[FS_PATH];
  if (form_get(body, "path", rel, sizeof rel) != 0 || form_get(body, "dest", dest_rel, sizeof dest_rel) != 0) {
    return reply_text(fd, 400, "Bad Request", "パスか移動先がありません");
  }
  char oldp[FS_PATH];
  char dest[FS_PATH];
  int status = 404;
  if (resolve_path(fs, rel, 0, oldp, sizeof oldp, &status) != 0) {
    return reply_text(fd, status == 403 ? 403 : 500, status == 403 ? "Forbidden" : "Internal Server Error",
                      status == 403 ? "保護されたファイルです" : "移動できませんでした");
  }
  if (strcmp(oldp, fs->root) == 0) {
    return reply_text(fd, 403, "Forbidden", "保護されたファイルです");
  }
  status = 404;
  if (resolve_path(fs, dest_rel, 1, dest, sizeof dest, &status) != 0) {
    return reply_text(fd, 500, "Internal Server Error", "移動できませんでした");
  }
  const char *base = strrchr(oldp, '/');
  base = base ? base + 1 : oldp;
  if (strcmp(dest, fs->root) == 0 && root_reserved(base)) {
    return reply_text(fd, 403, "Forbidden", "保護されたファイルです");
  }
  char neu[FS_PATH];
  if (snprintf(neu, sizeof neu, "%s/%s", dest, base) >= (int)sizeof neu) {
    return reply_text(fd, 414, "URI Too Long", "URLが長すぎます");
  }
  if (rename(oldp, neu) != 0) {
    return reply_text(fd, 500, "Internal Server Error", "移動できませんでした");
  }
  atomic_store(&fs->dirty, 1);
  return reply_text(fd, 200, "OK", "OK");
}

static int handle_upload(FileServer *fs, int fd, Req *req) {
  if (!req->file_name[0]) {
    discard_body(fd, req);
    return reply_text(fd, 400, "Bad Request", "名前がありません");
  }
  if (!segment_ok(req->file_name)) {
    discard_body(fd, req);
    return reply_text(fd, 403, "Forbidden", "保護されたファイルです");
  }
  if (req->content_length < 0) {
    return reply_text(fd, 411, "Length Required", "長さが必要です");
  }
  if (req->content_length > UPLOAD_MAX) {
    discard_body(fd, req);
    return reply_text(fd, 413, "Payload Too Large", "ファイルが大きすぎます");
  }
  const char *folder = req->file_path[0] ? req->file_path : "/";
  char parent[FS_PATH];
  int status = 404;
  if (resolve_path(fs, folder, 1, parent, sizeof parent, &status) != 0) {
    discard_body(fd, req);
    if (status == 403) {
      return reply_text(fd, 403, "Forbidden", "保護されたファイルです");
    }
    return reply_text(fd, 500, "Internal Server Error", "送信できませんでした");
  }
  if (strcmp(parent, fs->root) == 0 && root_reserved(req->file_name)) {
    discard_body(fd, req);
    return reply_text(fd, 403, "Forbidden", "保護されたファイルです");
  }
  char dest[FS_PATH];
  char partial[FS_PATH];
  if (snprintf(dest, sizeof dest, "%s/%s", parent, req->file_name) >= (int)sizeof dest ||
      snprintf(partial, sizeof partial, "%s/.yomu-upload", parent) >= (int)sizeof partial) {
    discard_body(fd, req);
    return reply_text(fd, 414, "URI Too Long", "URLが長すぎます");
  }
  int out = open(partial, O_CREAT | O_TRUNC | O_WRONLY | O_NOFOLLOW | O_CLOEXEC, 0644);
  if (out < 0) {
    discard_body(fd, req);
    return reply_text(fd, 500, "Internal Server Error", "送信できませんでした");
  }
  int failed = 0;
  int64_t left = req->content_length;
  size_t have = req->len - req->header_end;
  if ((int64_t)have > left) {
    have = (size_t)left;
  }
  if (have && write_full(out, req->buf + req->header_end, have) != 0) {
    failed = 1;
  }
  left -= (int64_t)have;
  char buf[IO_MAX];
  while (!failed && left > 0) {
    if (atomic_load(&fs->stop)) {
      failed = 1;
      break;
    }
    size_t chunk = left > (int64_t)sizeof buf ? sizeof buf : (size_t)left;
    ssize_t r = read(fd, buf, chunk);
    if (r < 0 && errno == EINTR) {
      continue;
    }
    if (r <= 0 || write_full(out, buf, (size_t)r) != 0) {
      failed = 1;
      break;
    }
    left -= r;
  }
  if (!failed && fsync(out) != 0) {
    failed = 1;
  }
  close(out);
  if (failed || left != 0 || rename(partial, dest) != 0) {
    unlink(partial);
    yomu_log("transfer upload failed %s", dest);
    if (!atomic_load(&fs->stop)) {
      return reply_text(fd, 500, "Internal Server Error", "送信できませんでした");
    }
    return -1;
  }
  atomic_store(&fs->dirty, 1);
  yomu_log("transfer uploaded %s (%lld bytes)", dest, (long long)req->content_length);
  char msg[320];
  snprintf(msg, sizeof msg, "送信しました: %s", req->file_name);
  return reply_text(fd, 200, "OK", msg);
}

static int handle_status(FileServer *fs, int fd) {
  char json[256];
  int n = snprintf(json, sizeof json, "{\"version\":\"yomu\",\"ip\":\"%s\",\"ssid\":\"\"}", fs->ip);
  if (n < 0 || (size_t)n >= sizeof json) {
    return reply_text(fd, 500, "Internal Server Error", "状態を取得できませんでした");
  }
  return reply(fd, 200, "OK", "application/json", NULL, json, (size_t)n);
}

static int handle_client(FileServer *fs, int fd) {
  /* accept() can hand back the listen socket's nonblocking flag. A read
   * before the request arrives would then fail instead of waiting. */
  int fl = fcntl(fd, F_GETFL, 0);
  if (fl >= 0) {
    fcntl(fd, F_SETFL, fl & ~O_NONBLOCK);
  }
  struct timeval tv;
  tv.tv_sec = 20;
  tv.tv_usec = 0;
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
#ifdef SO_NOSIGPIPE
  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
  Req req;
  if (read_request(fd, &req) != 0) {
    free(req.buf);
    return -1;
  }
  int rc = 0;
  if (strcmp(req.method, "GET") == 0 && strcmp(req.path, "/") == 0) {
    rc = reply(fd, 200, "OK", "text/html; charset=utf-8", NULL, filepage_html, filepage_html_len);
  } else if (strcmp(req.method, "GET") == 0 && strcmp(req.path, "/api/status") == 0) {
    rc = handle_status(fs, fd);
  } else if (strcmp(req.method, "GET") == 0 && strcmp(req.path, "/api/files") == 0) {
    rc = handle_list(fs, fd, &req);
  } else if (strcmp(req.method, "GET") == 0 &&
             (strcmp(req.path, "/download") == 0 || strncmp(req.path, "/download/", 10) == 0)) {
    rc = handle_download(fs, fd, &req);
  } else if (strcmp(req.method, "POST") == 0 && strcmp(req.path, "/upload") == 0) {
    rc = handle_upload(fs, fd, &req);
  } else if (strcmp(req.method, "POST") == 0 &&
             (strcmp(req.path, "/mkdir") == 0 || strcmp(req.path, "/rename") == 0 || strcmp(req.path, "/move") == 0 ||
              strcmp(req.path, "/delete") == 0)) {
    char *body = NULL;
    size_t body_len = 0;
    if (take_body(fd, &req, &body, &body_len) != 0) {
      free(req.buf);
      return -1;
    }
    if (strcmp(req.path, "/mkdir") == 0) {
      rc = handle_mkdir(fs, fd, body);
    } else if (strcmp(req.path, "/rename") == 0) {
      rc = handle_rename(fs, fd, body);
    } else if (strcmp(req.path, "/move") == 0) {
      rc = handle_move(fs, fd, body);
    } else {
      rc = handle_delete(fs, fd, body);
    }
    free(body);
  } else {
    discard_body(fd, &req);
    rc = reply_text(fd, 404, "Not Found", "見つかりません");
  }
  free(req.buf);
  return rc;
}

static void *serve_loop(void *arg) {
  FileServer *fs = arg;
  while (!atomic_load(&fs->stop)) {
    struct pollfd pfds[2];
    pfds[0].fd = fs->listen_fd;
    pfds[0].events = POLLIN;
    pfds[0].revents = 0;
    int np = 1;
    int wait = 200;
    if (fs->mdns) {
      pfds[1].fd = mdns_fd(fs->mdns);
      pfds[1].events = POLLIN;
      pfds[1].revents = 0;
      np = 2;
      int aw = mdns_announce_wait(fs->mdns);
      if (aw >= 0 && aw < wait) {
        wait = aw;
      }
    }
    int pr = poll(pfds, np, wait);
    if (pr < 0) {
      if (errno == EINTR) {
        continue;
      }
      break;
    }
    if (fs->mdns) {
      if (pfds[1].revents & POLLIN) {
        mdns_on_readable(fs->mdns);
      }
      mdns_announce_due(fs->mdns);
      if (pfds[1].revents & (POLLERR | POLLNVAL)) {
        mdns_stop(fs->mdns);
        fs->mdns = NULL;
      }
    }
    if (pr == 0 || (pfds[0].revents & POLLIN) == 0) {
      continue;
    }
    int fd = accept(fs->listen_fd, NULL, NULL);
    if (fd < 0) {
      if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
        continue;
      }
      break;
    }
    handle_client(fs, fd);
    close(fd);
  }
  return NULL;
}

static int if_score(const char *name) {
  if (!name) {
    return 5;
  }
  if (strstr(name, "wlan") || strstr(name, "wifi") || strstr(name, "mlan")) {
    return 0;
  }
  if (strncmp(name, "usb", 3) == 0) {
    return 3;
  }
  return 1;
}

static void add_mdns_url(FileServer *fs) {
  char url[80];
  int n;
  if (fs->port == 80) {
    n = snprintf(url, sizeof url, "http://yomu.local/");
  } else {
    n = snprintf(url, sizeof url, "http://yomu.local:%d/", fs->port);
  }
  if (n <= 0 || n >= (int)sizeof url) {
    return;
  }
  int keep = fs->nurls > 2 ? 2 : fs->nurls;
  for (int i = keep; i > 0; i--) {
    memcpy(fs->urls[i], fs->urls[i - 1], sizeof fs->urls[i]);
  }
  memcpy(fs->urls[0], url, (size_t)n + 1);
  fs->nurls = keep + 1;
}

static void collect_urls(FileServer *fs, uint32_t *mdns_addr, unsigned *mdns_ifindex) {
  *mdns_addr = 0;
  *mdns_ifindex = 0;
  struct ifaddrs *list = NULL;
  if (getifaddrs(&list) != 0) {
    return;
  }
  typedef struct Cand {
    char ip[40];
    uint32_t addr;
    unsigned ifindex;
    int score;
  } Cand;
  Cand cands[8];
  int n = 0;
  for (struct ifaddrs *ifa = list; ifa && n < 8; ifa = ifa->ifa_next) {
    if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) {
      continue;
    }
    if (ifa->ifa_flags & IFF_LOOPBACK) {
      continue;
    }
    struct sockaddr_in *sin = (struct sockaddr_in *)ifa->ifa_addr;
    uint32_t host = ntohl(sin->sin_addr.s_addr);
    if (host == 0 || (host >> 24) == 127) {
      continue;
    }
    char ip[40];
    if (!inet_ntop(AF_INET, &sin->sin_addr, ip, sizeof ip)) {
      continue;
    }
    int dup = 0;
    for (int i = 0; i < n; i++) {
      if (strcmp(cands[i].ip, ip) == 0) {
        dup = 1;
      }
    }
    if (dup) {
      continue;
    }
    snprintf(cands[n].ip, sizeof cands[n].ip, "%s", ip);
    cands[n].addr = sin->sin_addr.s_addr;
    cands[n].ifindex = if_nametoindex(ifa->ifa_name);
    cands[n].score = if_score(ifa->ifa_name);
    n++;
  }
  freeifaddrs(list);
  for (int i = 0; i < n; i++) {
    for (int j = i + 1; j < n; j++) {
      if (cands[j].score < cands[i].score) {
        Cand tmp = cands[i];
        cands[i] = cands[j];
        cands[j] = tmp;
      }
    }
  }
  fs->nurls = n > 2 ? 2 : n;
  for (int i = 0; i < fs->nurls; i++) {
    if (fs->port == 80) {
      snprintf(fs->urls[i], sizeof fs->urls[i], "http://%s/", cands[i].ip);
    } else {
      snprintf(fs->urls[i], sizeof fs->urls[i], "http://%s:%d/", cands[i].ip, fs->port);
    }
  }
  if (fs->nurls > 0) {
    if (fs->port == 80) {
      snprintf(fs->ip, sizeof fs->ip, "%s", cands[0].ip);
    } else {
      snprintf(fs->ip, sizeof fs->ip, "%s:%d", cands[0].ip, fs->port);
    }
  }
  if (n > 0 && cands[0].score == 0 && cands[0].ifindex != 0) {
    *mdns_addr = cands[0].addr;
    *mdns_ifindex = cands[0].ifindex;
  }
}

static void firewall_open(FileServer *fs) {
  if (system("command -v iptables >/dev/null 2>&1") != 0) {
    return;
  }
  char check[160];
  char add[160];
  snprintf(check, sizeof check, "iptables -C INPUT -p tcp --dport %d -j ACCEPT >/dev/null 2>&1", fs->port);
  snprintf(add, sizeof add, "iptables -I INPUT 1 -p tcp --dport %d -j ACCEPT >/dev/null 2>&1", fs->port);
  if (system(check) == 0) {
    return;
  }
  if (system(add) == 0) {
    fs->firewall = 1;
  }
}

static void firewall_close(FileServer *fs) {
  if (!fs->firewall) {
    return;
  }
  char cmd[160];
  snprintf(cmd, sizeof cmd, "iptables -D INPUT -p tcp --dport %d -j ACCEPT >/dev/null 2>&1", fs->port);
  system(cmd);
  fs->firewall = 0;
}

FileServer *fileserver_start(const char *root, int port) {
  ignore_pipe();
  if (!root || !root[0] || port < 0 || port > 65535) {
    return NULL;
  }
  FileServer *fs = calloc(1, sizeof *fs);
  if (!fs) {
    return NULL;
  }
  fs->listen_fd = -1;
  char *rp = realpath(root, NULL);
  if (!rp || strlen(rp) >= sizeof fs->root || strcmp(rp, "/") == 0) {
    free(rp);
    free(fs);
    return NULL;
  }
  memcpy(fs->root, rp, strlen(rp) + 1);
  free(rp);
  struct stat st;
  if (stat(fs->root, &st) != 0 || !S_ISDIR(st.st_mode)) {
    free(fs);
    return NULL;
  }
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    free(fs);
    return NULL;
  }
  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof addr);
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons((uint16_t)port);
  if (bind(fd, (struct sockaddr *)&addr, sizeof addr) != 0 || listen(fd, 4) != 0) {
    close(fd);
    free(fs);
    return NULL;
  }
  socklen_t alen = sizeof addr;
  if (getsockname(fd, (struct sockaddr *)&addr, &alen) != 0) {
    close(fd);
    free(fs);
    return NULL;
  }
  fs->port = ntohs(addr.sin_port);
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags >= 0) {
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  }
  fs->listen_fd = fd;
  uint32_t mdns_addr = 0;
  unsigned mdns_ifindex = 0;
  collect_urls(fs, &mdns_addr, &mdns_ifindex);
  if (port != 0) {
    firewall_open(fs);
  }
  if (mdns_addr) {
    fs->mdns = mdns_start(mdns_addr, fs->port, mdns_ifindex);
    if (fs->mdns) {
      add_mdns_url(fs);
    }
  }
  if (pthread_create(&fs->thread, NULL, serve_loop, fs) != 0) {
    mdns_stop(fs->mdns);
    firewall_close(fs);
    close(fd);
    free(fs);
    return NULL;
  }
  fs->started = 1;
  yomu_log("transfer listening on %d for %s", fs->port, fs->root);
  return fs;
}

int fileserver_stop(FileServer *fs) {
  if (!fs) {
    return 0;
  }
  atomic_store(&fs->stop, 1);
  if (fs->started) {
    pthread_join(fs->thread, NULL);
  }
  if (fs->listen_fd >= 0) {
    close(fs->listen_fd);
  }
  mdns_stop(fs->mdns);
  fs->mdns = NULL;
  firewall_close(fs);
  int dirty = atomic_load(&fs->dirty);
  yomu_log("transfer stopped");
  free(fs);
  return dirty ? 1 : 0;
}

int fileserver_changed(FileServer *fs) {
  if (!fs) {
    return 0;
  }
  return atomic_exchange(&fs->dirty, 0) ? 1 : 0;
}

int fileserver_port(const FileServer *fs) { return fs ? fs->port : 0; }

int fileserver_urls(const FileServer *fs, char urls[][80], int max) {
  if (!fs || !urls || max <= 0) {
    return 0;
  }
  int n = fs->nurls < max ? fs->nurls : max;
  for (int i = 0; i < n; i++) {
    snprintf(urls[i], 80, "%s", fs->urls[i]);
  }
  return n;
}
