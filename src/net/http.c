#include "http.h"

#include "log.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define CONNECT_MS 30000
#define STALL_SEC 120

typedef struct Conn {
  int fd;
  int err_timeout;
  unsigned char in[4096];
  size_t in_len;
  size_t in_pos;
} Conn;

typedef struct Url {
  int https;
  char host[256];
  int port;
  char path[2048];
} Url;

typedef struct Sink {
  FILE *fp;
  unsigned char *mem;
  size_t len;
  size_t cap;
  size_t max;
  int64_t got;
  void (*progress)(void *ud, int64_t got);
  void *progress_ud;
} Sink;

static void set_err(char *err, size_t cap, const char *msg) {
  if (!err || cap == 0) {
    return;
  }
  snprintf(err, cap, "%s", msg ? msg : "");
}

static void ignore_pipe(void) {
  static int once = 0;
  if (!once) {
    signal(SIGPIPE, SIG_IGN);
    once = 1;
  }
}

static int parse_url(const char *url, Url *out, char *err, size_t errcap) {
  memset(out, 0, sizeof *out);
  const char *p = url ? url : "";
  if (strncmp(p, "https://", 8) == 0) {
    out->https = 1;
    out->port = 443;
    p += 8;
  } else if (strncmp(p, "http://", 7) == 0) {
    out->https = 0;
    out->port = 80;
    p += 7;
  } else {
    set_err(err, errcap, "need http://");
    return -1;
  }
  if (*p == '[') {
    const char *end = strchr(p, ']');
    if (!end || end == p + 1 || (size_t)(end - (p + 1)) >= sizeof out->host) {
      set_err(err, errcap, "bad URL");
      return -1;
    }
    memcpy(out->host, p + 1, (size_t)(end - (p + 1)));
    out->host[end - (p + 1)] = '\0';
    p = end + 1;
  } else {
    const char *end = p;
    while (*end && *end != ':' && *end != '/' && *end != '?' && *end != '#') {
      end++;
    }
    if (end == p || (size_t)(end - p) >= sizeof out->host || memchr(p, '@', (size_t)(end - p))) {
      set_err(err, errcap, "bad URL");
      return -1;
    }
    memcpy(out->host, p, (size_t)(end - p));
    out->host[end - p] = '\0';
    p = end;
  }
  if (*p == ':') {
    char *end = NULL;
    long port = strtol(p + 1, &end, 10);
    if (end == p + 1 || port <= 0 || port > 65535) {
      set_err(err, errcap, "bad port");
      return -1;
    }
    out->port = (int)port;
    p = end;
  }
  if (*p == 0 || *p == '#') {
    snprintf(out->path, sizeof out->path, "/");
  } else if (*p == '?') {
    if (snprintf(out->path, sizeof out->path, "/%s", p) >= (int)sizeof out->path) {
      set_err(err, errcap, "URL too long");
      return -1;
    }
  } else if (*p == '/') {
    size_t n = 0;
    while (p[n] && p[n] != '#' && n + 1 < sizeof out->path) {
      out->path[n] = p[n];
      n++;
    }
    if (p[n] && p[n] != '#') {
      set_err(err, errcap, "URL too long");
      return -1;
    }
    out->path[n] = '\0';
  } else {
    set_err(err, errcap, "bad URL");
    return -1;
  }
  char *hash = strchr(out->path, '#');
  if (hash) {
    *hash = '\0';
  }
  if (out->path[0] == '\0') {
    snprintf(out->path, sizeof out->path, "/");
  }
  if (strpbrk(out->path, " \r\n") || strpbrk(out->host, " \r\n")) {
    set_err(err, errcap, "bad URL");
    return -1;
  }
  return 0;
}

static int format_origin(const Url *url, char *out, size_t cap) {
  int defport = url->https ? 443 : 80;
  const char *scheme = url->https ? "https" : "http";
  int n;
  if (strchr(url->host, ':')) {
    if (url->port == defport) {
      n = snprintf(out, cap, "%s://[%s]", scheme, url->host);
    } else {
      n = snprintf(out, cap, "%s://[%s]:%d", scheme, url->host, url->port);
    }
  } else if (url->port == defport) {
    n = snprintf(out, cap, "%s://%s", scheme, url->host);
  } else {
    n = snprintf(out, cap, "%s://%s:%d", scheme, url->host, url->port);
  }
  return (n < 0 || (size_t)n >= cap) ? -1 : 0;
}

static int resolve_location(const char *current, const char *location, char *out, size_t cap) {
  while (*location == ' ' || *location == '\t') {
    location++;
  }
  size_t n = strlen(location);
  while (n > 0 && (location[n - 1] == ' ' || location[n - 1] == '\t' || location[n - 1] == '\r')) {
    n--;
  }
  char loc[2048];
  if (n == 0 || n >= sizeof loc) {
    return -1;
  }
  memcpy(loc, location, n);
  loc[n] = '\0';
  if (strncmp(loc, "https://", 8) == 0 || strncmp(loc, "http://", 7) == 0) {
    if (n + 1 > cap) {
      return -1;
    }
    memcpy(out, loc, n + 1);
    return 0;
  }
  Url url;
  if (parse_url(current, &url, NULL, 0) != 0) {
    return -1;
  }
  char origin[768];
  if (format_origin(&url, origin, sizeof origin) != 0) {
    return -1;
  }
  if (loc[0] == '/') {
    return snprintf(out, cap, "%s%s", origin, loc) >= (int)cap ? -1 : 0;
  }
  char path[2048];
  snprintf(path, sizeof path, "%s", url.path);
  char *query = strchr(path, '?');
  if (query) {
    *query = '\0';
  }
  char *slash = strrchr(path, '/');
  if (slash) {
    slash[1] = '\0';
  } else {
    snprintf(path, sizeof path, "/");
  }
  return snprintf(out, cap, "%s%s%s", origin, path, loc) >= (int)cap ? -1 : 0;
}

static int same_origin(const char *a, const char *b) {
  Url ua, ub;
  if (parse_url(a, &ua, NULL, 0) != 0 || parse_url(b, &ub, NULL, 0) != 0) {
    return 0;
  }
  return ua.https == ub.https && ua.port == ub.port && strcasecmp(ua.host, ub.host) == 0;
}

static void conn_close(Conn *conn) {
  if (conn->fd >= 0) {
    close(conn->fd);
    conn->fd = -1;
  }
}

static int set_stall(int fd) {
  struct timeval tv;
  tv.tv_sec = STALL_SEC;
  tv.tv_usec = 0;
  if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv) != 0) {
    return -1;
  }
  if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv) != 0) {
    return -1;
  }
  return 0;
}

static int connect_host(const Url *url, Conn *conn, char *err, size_t errcap) {
  char port[16];
  snprintf(port, sizeof port, "%d", url->port);
  struct addrinfo hints;
  memset(&hints, 0, sizeof hints);
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_family = AF_UNSPEC;
  struct addrinfo *res = NULL;
  int gai = getaddrinfo(url->host, port, &hints, &res);
  if (gai != 0) {
    yomu_log("dns %s: %s", url->host, gai_strerror(gai));
    set_err(err, errcap, "DNS failed");
    return -1;
  }
  int failed_timeout = 0;
  for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
    int fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (fd < 0) {
      continue;
    }
#ifdef SO_NOSIGPIPE
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
      close(fd);
      continue;
    }
    int rc = connect(fd, ai->ai_addr, ai->ai_addrlen);
    if (rc < 0 && errno != EINPROGRESS) {
      close(fd);
      continue;
    }
    if (rc < 0) {
      struct pollfd pfd;
      pfd.fd = fd;
      pfd.events = POLLOUT;
      int pr;
      do {
        pr = poll(&pfd, 1, CONNECT_MS);
      } while (pr < 0 && errno == EINTR);
      if (pr == 0) {
        failed_timeout = 1;
        close(fd);
        continue;
      }
      if (pr < 0) {
        close(fd);
        continue;
      }
      int soerr = 0;
      socklen_t sl = sizeof soerr;
      if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &sl) != 0 || soerr != 0) {
        close(fd);
        continue;
      }
    }
    if (fcntl(fd, F_SETFL, flags) != 0 || set_stall(fd) != 0) {
      close(fd);
      continue;
    }
    conn->fd = fd;
    freeaddrinfo(res);
    return 0;
  }
  freeaddrinfo(res);
  set_err(err, errcap, failed_timeout ? "timed out" : "connection failed");
  return -1;
}

/* 1 = clean EOF, 0 = bytes in *got, -1 = error. */
static int transport_read(Conn *conn, unsigned char *buf, size_t cap, size_t *got) {
  *got = 0;
  for (;;) {
    ssize_t n = recv(conn->fd, buf, cap, 0);
    if (n < 0 && errno == EINTR) {
      continue;
    }
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == ETIMEDOUT)) {
      conn->err_timeout = 1;
      return -1;
    }
    if (n < 0) {
      return -1;
    }
    if (n == 0) {
      return 1;
    }
    *got = (size_t)n;
    return 0;
  }
}

static int pull(Conn *conn, unsigned char *buf, size_t cap, size_t *got, int *eof) {
  *got = 0;
  *eof = 0;
  if (cap == 0) {
    return 0;
  }
  if (conn->in_pos >= conn->in_len) {
    conn->in_pos = 0;
    conn->in_len = 0;
    size_t n = 0;
    int rc = transport_read(conn, conn->in, sizeof conn->in, &n);
    if (rc < 0) {
      return -1;
    }
    if (rc > 0) {
      *eof = 1;
      return 0;
    }
    conn->in_len = n;
  }
  if (conn->in_pos >= conn->in_len) {
    *eof = 1;
    return 0;
  }
  size_t n = conn->in_len - conn->in_pos;
  if (n > cap) {
    n = cap;
  }
  memcpy(buf, conn->in + conn->in_pos, n);
  conn->in_pos += n;
  *got = n;
  return 0;
}

static int get_byte(Conn *conn) {
  unsigned char b = 0;
  size_t got = 0;
  int eof = 0;
  if (pull(conn, &b, 1, &got, &eof) != 0) {
    return -1;
  }
  if (eof || got == 0) {
    return -2;
  }
  return (int)b;
}

static int read_full(Conn *conn, unsigned char *buf, size_t len) {
  size_t off = 0;
  while (off < len) {
    size_t got = 0;
    int eof = 0;
    if (pull(conn, buf + off, len - off, &got, &eof) != 0) {
      return -1;
    }
    if (eof || got == 0) {
      return -1;
    }
    off += got;
  }
  return 0;
}

static int write_all(Conn *conn, const void *data, size_t len) {
  const unsigned char *p = data;
  int flags = 0;
#ifdef MSG_NOSIGNAL
  flags = MSG_NOSIGNAL;
#endif
  size_t off = 0;
  while (off < len) {
    ssize_t n = send(conn->fd, p + off, len - off, flags);
    if (n < 0 && errno == EINTR) {
      continue;
    }
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == ETIMEDOUT)) {
      conn->err_timeout = 1;
      return -1;
    }
    if (n <= 0) {
      return -1;
    }
    off += (size_t)n;
  }
  return 0;
}

static size_t base64_encode(const unsigned char *in, size_t len, char *out, size_t cap) {
  static const char tab[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  size_t need = ((len + 2) / 3) * 4 + 1;
  if (need > cap) {
    return 0;
  }
  size_t i = 0;
  size_t o = 0;
  while (i + 3 <= len) {
    unsigned n = ((unsigned)in[i] << 16) | ((unsigned)in[i + 1] << 8) | (unsigned)in[i + 2];
    out[o++] = tab[(n >> 18) & 63];
    out[o++] = tab[(n >> 12) & 63];
    out[o++] = tab[(n >> 6) & 63];
    out[o++] = tab[n & 63];
    i += 3;
  }
  if (i < len) {
    unsigned n = (unsigned)in[i] << 16;
    if (i + 1 < len) {
      n |= (unsigned)in[i + 1] << 8;
    }
    out[o++] = tab[(n >> 18) & 63];
    out[o++] = tab[(n >> 12) & 63];
    if (i + 1 < len) {
      out[o++] = tab[(n >> 6) & 63];
      out[o++] = '=';
    } else {
      out[o++] = '=';
      out[o++] = '=';
    }
  }
  out[o] = '\0';
  return o;
}

static int header_value(const char *headers, const char *key, char *out, size_t cap) {
  size_t klen = strlen(key);
  const char *p = strstr(headers, "\r\n");
  if (!p) {
    return -1;
  }
  p += 2;
  while (*p && !(p[0] == '\r' && p[1] == '\n')) {
    const char *endl = strstr(p, "\r\n");
    if (!endl) {
      break;
    }
    if ((size_t)(endl - p) > klen && strncasecmp(p, key, klen) == 0 && p[klen] == ':') {
      const char *v = p + klen + 1;
      while (v < endl && (*v == ' ' || *v == '\t')) {
        v++;
      }
      size_t n = (size_t)(endl - v);
      if (n + 1 > cap) {
        n = cap - 1;
      }
      memcpy(out, v, n);
      out[n] = '\0';
      return 0;
    }
    p = endl + 2;
  }
  return -1;
}

static int status_code(const char *headers) {
  const char *sp = strchr(headers, ' ');
  if (!sp) {
    return -1;
  }
  return atoi(sp + 1);
}

static int sink_write(Sink *sink, const unsigned char *data, size_t n) {
  if (n == 0) {
    return 0;
  }
  if (sink->max && (n > sink->max || sink->len > sink->max - n)) {
    return -1;
  }
  if (sink->fp) {
    if (fwrite(data, 1, n, sink->fp) != n) {
      return -1;
    }
  }
  if (sink->mem || !sink->fp) {
    if (!sink->fp) {
      size_t cap = sink->cap ? sink->cap : 4096;
      while (cap < sink->len + n + 1) {
        if (cap > (SIZE_MAX / 2)) {
          return -1;
        }
        cap *= 2;
      }
      if (cap != sink->cap) {
        unsigned char *grown = realloc(sink->mem, cap);
        if (!grown) {
          return -1;
        }
        sink->mem = grown;
        sink->cap = cap;
      }
      memcpy(sink->mem + sink->len, data, n);
    }
  }
  sink->len += n;
  if (sink->mem) {
    sink->mem[sink->len] = 0;
  }
  sink->got += (int64_t)n;
  if (sink->progress) {
    sink->progress(sink->progress_ud, sink->got);
  }
  return 0;
}

static int read_line(Conn *conn, char *buf, size_t cap) {
  size_t n = 0;
  for (;;) {
    int ch = get_byte(conn);
    if (ch < 0) {
      return -1;
    }
    if (ch == '\n') {
      break;
    }
    if (ch == '\r') {
      continue;
    }
    if (n + 1 >= cap) {
      return -1;
    }
    buf[n++] = (char)ch;
  }
  buf[n] = '\0';
  return (int)n;
}

static int read_headers(Conn *conn, char **out, int *status, char *err, size_t errcap) {
  size_t cap = 1024;
  size_t n = 0;
  char *buf = malloc(cap);
  if (!buf) {
    set_err(err, errcap, "out of memory");
    return -1;
  }
  for (;;) {
    int ch = get_byte(conn);
    if (ch == -1) {
      free(buf);
      set_err(err, errcap, conn->err_timeout ? "timed out" : "connection failed");
      return -1;
    }
    if (ch == -2) {
      free(buf);
      set_err(err, errcap, "connection failed");
      return -1;
    }
    if (n + 2 >= cap) {
      if (cap > 65536) {
        free(buf);
        set_err(err, errcap, "response too large");
        return -1;
      }
      size_t next = cap * 2;
      char *grown = realloc(buf, next);
      if (!grown) {
        free(buf);
        set_err(err, errcap, "out of memory");
        return -1;
      }
      buf = grown;
      cap = next;
    }
    buf[n++] = (char)ch;
    buf[n] = '\0';
    if (n >= 4 && buf[n - 4] == '\r' && buf[n - 3] == '\n' && buf[n - 2] == '\r' && buf[n - 1] == '\n') {
      break;
    }
  }
  *status = status_code(buf);
  if (*status < 100 || *status > 599) {
    free(buf);
    set_err(err, errcap, "bad response");
    return -1;
  }
  *out = buf;
  return 0;
}

static int read_body(Conn *conn, const char *headers, Sink *sink, char *err, size_t errcap) {
  char te[128];
  int chunked = 0;
  if (header_value(headers, "Transfer-Encoding", te, sizeof te) == 0) {
    for (char *p = te; *p; p++) {
      if (*p >= 'A' && *p <= 'Z') {
        *p = (char)(*p - 'A' + 'a');
      }
    }
    if (strstr(te, "chunked")) {
      chunked = 1;
    }
  }
  if (chunked) {
    for (;;) {
      char line[64];
      if (read_line(conn, line, sizeof line) < 0) {
        set_err(err, errcap, conn->err_timeout ? "timed out" : "connection failed");
        return -1;
      }
      char *end = NULL;
      unsigned long chunk = strtoul(line, &end, 16);
      if (end == line) {
        set_err(err, errcap, "bad response");
        return -1;
      }
      if (chunk == 0) {
        for (;;) {
          char trailer[128];
          int tn = read_line(conn, trailer, sizeof trailer);
          if (tn < 0) {
            set_err(err, errcap, conn->err_timeout ? "timed out" : "connection failed");
            return -1;
          }
          if (tn == 0) {
            break;
          }
        }
        return 0;
      }
      size_t left = (size_t)chunk;
      while (left > 0) {
        unsigned char tmp[8192];
        size_t n = left > sizeof tmp ? sizeof tmp : left;
        if (read_full(conn, tmp, n) != 0) {
          set_err(err, errcap, conn->err_timeout ? "timed out" : "connection failed");
          return -1;
        }
        if (sink_write(sink, tmp, n) != 0) {
          set_err(err, errcap, sink->fp ? "cannot save" : "response too large");
          return -1;
        }
        left -= n;
      }
      int c1 = get_byte(conn);
      if (c1 == '\r') {
        c1 = get_byte(conn);
      }
      if (c1 != '\n') {
        set_err(err, errcap, "bad response");
        return -1;
      }
    }
  }
  char cl[64];
  if (header_value(headers, "Content-Length", cl, sizeof cl) == 0) {
    char *end = NULL;
    long long length = strtoll(cl, &end, 10);
    if (end == cl || length < 0) {
      set_err(err, errcap, "bad response");
      return -1;
    }
    if (sink->max && (unsigned long long)length > sink->max) {
      set_err(err, errcap, "response too large");
      return -1;
    }
    unsigned long long left = (unsigned long long)length;
    while (left > 0) {
      unsigned char tmp[8192];
      size_t n = left > sizeof tmp ? sizeof tmp : (size_t)left;
      if (read_full(conn, tmp, n) != 0) {
        set_err(err, errcap, conn->err_timeout ? "timed out" : "connection failed");
        return -1;
      }
      if (sink_write(sink, tmp, n) != 0) {
        set_err(err, errcap, sink->fp ? "cannot save" : "response too large");
        return -1;
      }
      left -= n;
    }
    return 0;
  }
  for (;;) {
    unsigned char tmp[8192];
    size_t got = 0;
    int eof = 0;
    if (pull(conn, tmp, sizeof tmp, &got, &eof) != 0) {
      set_err(err, errcap, conn->err_timeout ? "timed out" : "connection failed");
      return -1;
    }
    if (got && sink_write(sink, tmp, got) != 0) {
      set_err(err, errcap, sink->fp ? "cannot save" : "response too large");
      return -1;
    }
    if (eof) {
      return 0;
    }
  }
}

static int http_once(const HttpRequest *req, const char *url_text, const char *user, const char *password,
                     int *status, char *location, size_t location_cap, Sink *sink, char *err, size_t errcap) {
  Url url;
  Conn conn;
  memset(&conn, 0, sizeof conn);
  conn.fd = -1;
  location[0] = '\0';
  *status = 0;
  if (parse_url(url_text, &url, err, errcap) != 0) {
    return -1;
  }
  if (url.https) {
    set_err(err, errcap, "need http://");
    return -1;
  }
  if (connect_host(&url, &conn, err, errcap) != 0) {
    return -1;
  }
  const char *pass = password ? password : "";
  const char *name = user ? user : "";
  int use_auth = name[0] || pass[0];
  char raw_auth[1024];
  char b64[((1024 + 2) / 3) * 4 + 1];
  b64[0] = '\0';
  if (use_auth) {
    int an = snprintf(raw_auth, sizeof raw_auth, "%s:%s", name, pass);
    if (an < 0 || (size_t)an >= sizeof raw_auth ||
        base64_encode((const unsigned char *)raw_auth, (size_t)an, b64, sizeof b64) == 0) {
      conn_close(&conn);
      set_err(err, errcap, "credentials too long");
      return -1;
    }
  }
  char hosthdr[320];
  int defport = url.https ? 443 : 80;
  int hn;
  if (strchr(url.host, ':')) {
    hn = url.port == defport ? snprintf(hosthdr, sizeof hosthdr, "Host: [%s]\r\n", url.host)
                             : snprintf(hosthdr, sizeof hosthdr, "Host: [%s]:%d\r\n", url.host, url.port);
  } else {
    hn = url.port == defport ? snprintf(hosthdr, sizeof hosthdr, "Host: %s\r\n", url.host)
                             : snprintf(hosthdr, sizeof hosthdr, "Host: %s:%d\r\n", url.host, url.port);
  }
  if (hn < 0 || (size_t)hn >= sizeof hosthdr) {
    conn_close(&conn);
    set_err(err, errcap, "bad URL");
    return -1;
  }
  size_t extra_len = req->extra_headers ? strlen(req->extra_headers) : 0;
  size_t cap = strlen(req->method) + strlen(url.path) + strlen(hosthdr) + strlen(b64) + extra_len +
               (req->content_type ? strlen(req->content_type) : 0) + req->body_len + 256;
  char *msg = malloc(cap);
  if (!msg) {
    conn_close(&conn);
    set_err(err, errcap, "out of memory");
    return -1;
  }
  size_t used = 0;
  int send_length = req->body_len > 0 || strcmp(req->method, "GET") != 0;
  int n = snprintf(msg, cap,
                   "%s %s HTTP/1.1\r\n"
                   "%s"
                   "User-Agent: yomu\r\n"
                   "Connection: close\r\n",
                   req->method, url.path, hosthdr);
  if (n < 0 || (size_t)n >= cap) {
    free(msg);
    conn_close(&conn);
    set_err(err, errcap, "URL too long");
    return -1;
  }
  used = (size_t)n;
  if (use_auth) {
    n = snprintf(msg + used, cap - used, "Authorization: Basic %s\r\n", b64);
    if (n < 0 || (size_t)n >= cap - used) {
      free(msg);
      conn_close(&conn);
      set_err(err, errcap, "credentials too long");
      return -1;
    }
    used += (size_t)n;
  }
  if (req->content_type && req->content_type[0]) {
    n = snprintf(msg + used, cap - used, "Content-Type: %s\r\n", req->content_type);
    if (n < 0 || (size_t)n >= cap - used) {
      free(msg);
      conn_close(&conn);
      set_err(err, errcap, "URL too long");
      return -1;
    }
    used += (size_t)n;
  }
  if (req->extra_headers && req->extra_headers[0]) {
    if (extra_len >= cap - used) {
      free(msg);
      conn_close(&conn);
      set_err(err, errcap, "URL too long");
      return -1;
    }
    memcpy(msg + used, req->extra_headers, extra_len);
    used += extra_len;
  }
  if (send_length) {
    n = snprintf(msg + used, cap - used, "Content-Length: %zu\r\n", req->body_len);
    if (n < 0 || (size_t)n >= cap - used) {
      free(msg);
      conn_close(&conn);
      set_err(err, errcap, "URL too long");
      return -1;
    }
    used += (size_t)n;
  }
  if (used + 2 >= cap) {
    free(msg);
    conn_close(&conn);
    set_err(err, errcap, "URL too long");
    return -1;
  }
  msg[used++] = '\r';
  msg[used++] = '\n';
  int failed = write_all(&conn, msg, used) != 0;
  if (!failed && req->body && req->body_len) {
    failed = write_all(&conn, req->body, req->body_len) != 0;
  }
  free(msg);
  if (failed) {
    conn_close(&conn);
    set_err(err, errcap, conn.err_timeout ? "timed out" : "connection failed");
    return -1;
  }
  char *headers = NULL;
  for (;;) {
    free(headers);
    headers = NULL;
    if (read_headers(&conn, &headers, status, err, errcap) != 0) {
      conn_close(&conn);
      return -1;
    }
    if (*status != 100) {
      break;
    }
  }
  header_value(headers, "Location", location, location_cap);
  int redirect = (*status == 301 || *status == 302 || *status == 303 || *status == 307 || *status == 308);
  int keep = (*status >= 200 && *status <= 299 && !redirect);
  int body_rc = 0;
  if (keep) {
    body_rc = read_body(&conn, headers, sink, err, errcap);
  }
  free(headers);
  int timed_out = conn.err_timeout;
  conn_close(&conn);
  if (body_rc != 0) {
    if (err && err[0] == '\0') {
      set_err(err, errcap, timed_out ? "timed out" : "connection failed");
    }
    return -1;
  }
  return 0;
}

int http_request(const HttpRequest *req, int *status, char *err, size_t errcap) {
  ignore_pipe();
  int status_local = 0;
  if (!status) {
    status = &status_local;
  }
  *status = 0;
  if (err && errcap) {
    err[0] = '\0';
  }
  if (!req || !req->method || !req->url) {
    set_err(err, errcap, "bad URL");
    return -1;
  }
  if (req->mem) {
    *req->mem = NULL;
  }
  if (req->mem_len) {
    *req->mem_len = 0;
  }
  char current[2048];
  if (strlen(req->url) >= sizeof current) {
    set_err(err, errcap, "URL too long");
    return -1;
  }
  memcpy(current, req->url, strlen(req->url) + 1);
  const char *user = req->user;
  const char *password = req->password;
  int hops = req->max_redirects < 0 ? 0 : req->max_redirects;
  for (int hop = 0;; hop++) {
    Sink sink;
    memset(&sink, 0, sizeof sink);
    sink.fp = req->dest;
    sink.max = req->max_body;
    sink.progress = req->progress;
    sink.progress_ud = req->progress_ud;
    int collect = req->mem != NULL;
    if (collect) {
      sink.mem = malloc(1);
      if (!sink.mem) {
        set_err(err, errcap, "out of memory");
        return -1;
      }
      sink.mem[0] = 0;
      sink.cap = 1;
    }
    int code = 0;
    char location[2048];
    int rc = http_once(req, current, user, password, &code, location, sizeof location, &sink, err, errcap);
    int redirect = (code == 301 || code == 302 || code == 303 || code == 307 || code == 308);
    int follow = redirect && strcmp(req->method, "GET") == 0;
    if (rc != 0 || !follow) {
      if (rc != 0) {
        free(sink.mem);
        return -1;
      }
      *status = code;
      if (code < 200 || code > 299) {
        free(sink.mem);
        return 0;
      }
      if (req->mem) {
        *req->mem = sink.mem;
        if (req->mem_len) {
          *req->mem_len = sink.len;
        }
      } else {
        free(sink.mem);
      }
      return 0;
    }
    free(sink.mem);
    if (hop >= hops) {
      set_err(err, errcap, "too many redirects");
      return -1;
    }
    if (location[0] == '\0') {
      set_err(err, errcap, "redirect without location");
      return -1;
    }
    char next[2048];
    if (resolve_location(current, location, next, sizeof next) != 0) {
      set_err(err, errcap, "bad redirect");
      return -1;
    }
    if (!same_origin(current, next)) {
      user = NULL;
      password = NULL;
    }
    memcpy(current, next, strlen(next) + 1);
    if (req->dest) {
      rewind(req->dest);
      if (ftruncate(fileno(req->dest), 0) != 0) {
        set_err(err, errcap, "cannot save");
        return -1;
      }
    }
  }
}
