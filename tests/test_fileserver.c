#include "app.h"
#include "fileserver.h"
#include "http.h"
#include "mdns.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

extern int g_failed;
void expect(int cond, const char *msg);

static int call(const char *method, const char *url, const char *type, const char *extra, const char *body,
                int *status, char **text) {
  unsigned char *mem = NULL;
  size_t len = 0;
  HttpRequest req;
  memset(&req, 0, sizeof req);
  req.method = method;
  req.url = url;
  req.content_type = type;
  req.extra_headers = extra;
  req.body = body;
  req.body_len = body ? strlen(body) : 0;
  req.mem = &mem;
  req.mem_len = &len;
  char err[160];
  err[0] = '\0';
  int rc = http_request(&req, status, err, sizeof err);
  if (text) {
    *text = (char *)mem;
  } else {
    free(mem);
  }
  return rc;
}

static void url_of(char *out, size_t cap, int port, const char *path) {
  snprintf(out, cap, "http://127.0.0.1:%d%s", port, path);
}

static int raw_exchange(int port, const char *req, char *out, size_t cap, int *status, const char **body) {
  if (cap < 16) {
    return -1;
  }
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    return -1;
  }
  struct timeval tv;
  tv.tv_sec = 5;
  tv.tv_usec = 0;
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof addr);
  addr.sin_family = AF_INET;
  addr.sin_port = htons((unsigned short)port);
  inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
  if (connect(fd, (struct sockaddr *)&addr, sizeof addr) != 0) {
    close(fd);
    return -1;
  }
  size_t n = strlen(req);
  size_t off = 0;
  while (off < n) {
    ssize_t w = send(fd, req + off, n - off, 0);
    if (w < 0 && errno == EINTR) {
      continue;
    }
    if (w <= 0) {
      close(fd);
      return -1;
    }
    off += (size_t)w;
  }
  size_t got = 0;
  while (got + 1 < cap) {
    ssize_t r = recv(fd, out + got, cap - 1 - got, 0);
    if (r < 0 && errno == EINTR) {
      continue;
    }
    if (r < 0 || r == 0) {
      break;
    }
    got += (size_t)r;
  }
  close(fd);
  out[got] = '\0';
  if (got + 1 >= cap) {
    return -1;
  }
  char *sp = strchr(out, ' ');
  char *sep = strstr(out, "\r\n\r\n");
  if (!sp || !sep || sp > sep) {
    return -1;
  }
  *status = atoi(sp + 1);
  *body = sep + 4;
  return *status > 0 ? 0 : -1;
}

/* encodeURIComponent: leave A-Z a-z 0-9 and -_.!~*'() as they are. */
static int pct_encode(const char *src, char *dst, size_t cap) {
  static const char hex[] = "0123456789ABCDEF";
  size_t n = 0;
  for (const unsigned char *p = (const unsigned char *)(src ? src : ""); *p; p++) {
    unsigned char c = *p;
    int plain = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
                c == '.' || c == '!' || c == '~' || c == '*' || c == '\'' || c == '(' || c == ')';
    size_t need = plain ? 1 : 3;
    if (n + need >= cap) {
      return -1;
    }
    if (plain) {
      dst[n++] = (char)c;
    } else {
      dst[n++] = '%';
      dst[n++] = hex[c >> 4];
      dst[n++] = hex[c & 0xf];
    }
  }
  if (n >= cap) {
    return -1;
  }
  dst[n] = '\0';
  return 0;
}

static int read_all(const char *path, char *buf, size_t cap) {
  FILE *fp = fopen(path, "rb");
  if (!fp || cap == 0) {
    if (fp) {
      fclose(fp);
    }
    return -1;
  }
  size_t n = fread(buf, 1, cap - 1, fp);
  fclose(fp);
  buf[n] = '\0';
  return (int)n;
}

static int exists_dir(const char *path) {
  struct stat st;
  return lstat(path, &st) == 0 && S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode);
}

static void rm_tree(const char *path) {
  DIR *dir = opendir(path);
  if (!dir) {
    unlink(path);
    return;
  }
  struct dirent *de;
  while ((de = readdir(dir)) != NULL) {
    if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) {
      continue;
    }
    char child[512];
    snprintf(child, sizeof child, "%s/%s", path, de->d_name);
    struct stat st;
    if (lstat(child, &st) == 0 && S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode)) {
      rm_tree(child);
    } else {
      unlink(child);
    }
  }
  closedir(dir);
  rmdir(path);
}

static int free_port(void) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    return -1;
  }
  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof addr);
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  if (bind(fd, (struct sockaddr *)&addr, sizeof addr) != 0) {
    close(fd);
    return -1;
  }
  socklen_t len = sizeof addr;
  if (getsockname(fd, (struct sockaddr *)&addr, &len) != 0) {
    close(fd);
    return -1;
  }
  int port = ntohs(addr.sin_port);
  close(fd);
  return port;
}

static int port_open(int port) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    return 0;
  }
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof addr);
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons((uint16_t)port);
  int rc = connect(fd, (struct sockaddr *)&addr, sizeof addr);
  close(fd);
  return rc == 0;
}

static int g_xfer_step;
static int g_xfer_port;
static int g_xfer_saw;
static int g_xfer_still;

static void xfer_present(AppHost *host, int x, int y, int w, int h, int full) {
  (void)host;
  (void)x;
  (void)y;
  (void)w;
  (void)h;
  (void)full;
}

static int xfer_poll(AppHost *host, AppEvent *event) {
  (void)host;
  memset(event, 0, sizeof *event);
  if (g_xfer_step == 0) {
    g_xfer_step++;
    event->kind = EV_CONFIRM;
    return 1;
  }
  if (g_xfer_step == 1) {
    g_xfer_step++;
    char url[80];
    url_of(url, sizeof url, g_xfer_port, "/");
    int status = 0;
    char *body = NULL;
    if (call("GET", url, NULL, NULL, NULL, &status, &body) == 0 && status == 200 && body && strstr(body, "yomu")) {
      g_xfer_saw = 1;
    }
    free(body);
    event->kind = EV_BACK;
    return 1;
  }
  if (g_xfer_step == 2) {
    g_xfer_step++;
    g_xfer_still = port_open(g_xfer_port);
    event->kind = EV_QUIT;
    return 1;
  }
  event->kind = EV_QUIT;
  return -1;
}

static void test_xfer_screen(const char *dir) {
  int port = free_port();
  expect(port > 0, "ui port");
  if (port <= 0) {
    return;
  }
  char text[16];
  snprintf(text, sizeof text, "%d", port);
  expect(setenv("YOMU_PORT", text, 1) == 0, "set port");
  uint8_t *gray = calloc(16, 16);
  expect(gray != NULL, "xfer gray");
  if (!gray) {
    unsetenv("YOMU_PORT");
    return;
  }
  AppHost host;
  memset(&host, 0, sizeof host);
  host.w = 16;
  host.h = 16;
  host.gray = gray;
  host.full_every = 8;
  host.root = dir;
  host.home = dir;
  host.present = xfer_present;
  host.poll = xfer_poll;
  g_xfer_step = 0;
  g_xfer_port = port;
  g_xfer_saw = 0;
  g_xfer_still = 0;
  expect(app_run(&host, NULL) == 0, "xfer screen run");
  expect(g_xfer_saw == 1, "transfer row serves the page");
  expect(g_xfer_still == 1, "back leaves the server running");
  expect(!port_open(port), "quit stops the server");
  unsetenv("YOMU_PORT");
  free(gray);
}

static uint16_t be16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }

static uint32_t be32(const uint8_t *p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static const uint8_t HOST_NAME[] = {4, 'y', 'o', 'm', 'u', 5, 'l', 'o', 'c', 'a', 'l', 0};
static const uint8_t HTTP_NAME[] = {5, '_', 'h', 't', 't', 'p', 4, '_', 't', 'c', 'p', 5, 'l', 'o', 'c', 'a', 'l', 0};
static const uint8_t INST_NAME[] = {4,   'y', 'o', 'm', 'u', 5,   '_', 'h', 't', 't', 'p', 4,   '_', 't',
                                    'c', 'p', 5,   'l', 'o', 'c', 'a', 'l', 0};
static const uint8_t SERVICES_NAME[] = {9,   '_', 's', 'e', 'r', 'v', 'i', 'c', 'e', 's', 7,   '_', 'd', 'n', 's',
                                        '-', 's', 'd', 4,   '_', 'u', 'd', 'p', 5,   'l', 'o', 'c', 'a', 'l', 0};

static int skip_name(const uint8_t *pkt, size_t len, size_t off, size_t *next) {
  size_t i = off;
  int jumps = 0;
  while (i < len && jumps < 64) {
    uint8_t lab = pkt[i];
    if (lab == 0) {
      *next = i + 1;
      return 0;
    }
    if ((lab & 0xC0) == 0xC0) {
      if (i + 1 >= len) {
        return -1;
      }
      *next = i + 2;
      return 0;
    }
    if ((lab & 0xC0) != 0 || i + 1u + lab > len) {
      return -1;
    }
    i += 1u + lab;
    jumps++;
  }
  return -1;
}

typedef struct Rr {
  uint16_t type;
  uint16_t class;
  uint32_t ttl;
  const uint8_t *rdata;
  size_t rdlen;
  int section;
} Rr;

static int find_rr(const uint8_t *pkt, size_t len, const uint8_t *name, size_t namelen, uint16_t type, Rr *out) {
  if (len < 12) {
    return 0;
  }
  uint16_t counts[4];
  for (int i = 0; i < 4; i++) {
    counts[i] = be16(pkt + 4 + i * 2);
  }
  size_t off = 12;
  for (uint16_t i = 0; i < counts[0]; i++) {
    size_t next = 0;
    if (skip_name(pkt, len, off, &next) != 0 || next + 4 > len) {
      return 0;
    }
    off = next + 4;
  }
  for (int sec = 1; sec <= 3; sec++) {
    for (uint16_t i = 0; i < counts[sec]; i++) {
      size_t name_off = off;
      size_t next = 0;
      if (skip_name(pkt, len, off, &next) != 0 || next + 10 > len) {
        return 0;
      }
      uint16_t rtype = be16(pkt + next);
      uint16_t rclass = be16(pkt + next + 2);
      uint32_t ttl = be32(pkt + next + 4);
      uint16_t rdlen = be16(pkt + next + 8);
      if (next + 10u + rdlen > len) {
        return 0;
      }
      int match = rtype == type && name_off + namelen <= len && memcmp(pkt + name_off, name, namelen) == 0;
      off = next + 10u + rdlen;
      if (match) {
        out->type = rtype;
        out->class = rclass;
        out->ttl = ttl;
        out->rdata = pkt + next + 10;
        out->rdlen = rdlen;
        out->section = sec;
        return 1;
      }
    }
  }
  return 0;
}

static size_t build_query(uint8_t *out, size_t cap, uint16_t id, uint16_t flags, const uint8_t *name, size_t nlen,
                          uint16_t qtype, uint16_t qclass) {
  if (cap < 12 + nlen + 4) {
    return 0;
  }
  memset(out, 0, 12);
  out[0] = (uint8_t)(id >> 8);
  out[1] = (uint8_t)id;
  out[2] = (uint8_t)(flags >> 8);
  out[3] = (uint8_t)flags;
  out[5] = 1;
  memcpy(out + 12, name, nlen);
  out[12 + nlen] = (uint8_t)(qtype >> 8);
  out[13 + nlen] = (uint8_t)qtype;
  out[14 + nlen] = (uint8_t)(qclass >> 8);
  out[15 + nlen] = (uint8_t)qclass;
  return 12 + nlen + 4;
}

static void test_mdns(void) {
  uint32_t addr = htonl(0xC0A80105);
  uint8_t ip[4];
  memcpy(ip, &addr, 4);
  expect(ip[0] == 192 && ip[1] == 168 && ip[2] == 1 && ip[3] == 5, "address wire order");

  uint8_t q[160];
  uint8_t out[768];
  int unicast = 1;
  Rr rr;
  size_t qn = build_query(q, sizeof q, 0x1234, 0, HOST_NAME, sizeof HOST_NAME, 1, 1);
  int n = mdns_reply(q, qn, addr, 80, 5353, out, sizeof out, &unicast);
  expect(n > 12, "A reply");
  expect(unicast == 0, "A reply is multicast");
  expect(out[0] == 0 && out[1] == 0, "multicast id is zero");
  expect(out[2] == 0x84 && out[3] == 0x00, "response is authoritative");
  expect(be16(out + 4) == 0, "multicast question section is empty");
  expect(find_rr(out, (size_t)n, HOST_NAME, sizeof HOST_NAME, 1, &rr), "A record");
  expect(rr.section == 1 && rr.class == 0x8001 && rr.ttl == 120 && rr.rdlen == 4 && memcmp(rr.rdata, ip, 4) == 0,
         "A record fields");

  qn = build_query(q, sizeof q, 0x1234, 0, HOST_NAME, sizeof HOST_NAME, 1, 0x8001);
  unicast = 0;
  n = mdns_reply(q, qn, addr, 80, 5353, out, sizeof out, &unicast);
  expect(n > 12 && unicast == 1, "QU bit asks for unicast");
  expect(be16(out) == 0x1234 && be16(out + 4) == 1, "unicast echoes id and question");

  qn = build_query(q, sizeof q, 7, 0, HOST_NAME, sizeof HOST_NAME, 1, 1);
  unicast = 0;
  n = mdns_reply(q, qn, addr, 8080, 53000, out, sizeof out, &unicast);
  expect(n > 12 && unicast == 1, "legacy source port is unicast");

  uint8_t other[] = {5, 'o', 't', 'h', 'e', 'r', 5, 'l', 'o', 'c', 'a', 'l', 0};
  qn = build_query(q, sizeof q, 1, 0, other, sizeof other, 1, 1);
  n = mdns_reply(q, qn, addr, 80, 5353, out, sizeof out, &unicast);
  expect(n == 0, "other name is ignored");

  qn = build_query(q, sizeof q, 1, 0x8000, HOST_NAME, sizeof HOST_NAME, 1, 1);
  n = mdns_reply(q, qn, addr, 80, 5353, out, sizeof out, &unicast);
  expect(n == 0, "response packet is ignored");

  qn = build_query(q, sizeof q, 1, 0x0800, HOST_NAME, sizeof HOST_NAME, 1, 1);
  n = mdns_reply(q, qn, addr, 80, 5353, out, sizeof out, &unicast);
  expect(n == 0, "nonzero opcode is ignored");

  qn = build_query(q, sizeof q, 1, 0, HOST_NAME, sizeof HOST_NAME, 28, 1);
  n = mdns_reply(q, qn, addr, 80, 5353, out, sizeof out, &unicast);
  expect(n == 0, "AAAA is ignored");

  uint8_t upper[] = {4, 'Y', 'O', 'M', 'U', 5, 'L', 'O', 'C', 'A', 'L', 0};
  qn = build_query(q, sizeof q, 1, 0, upper, sizeof upper, 1, 1);
  n = mdns_reply(q, qn, addr, 80, 5353, out, sizeof out, &unicast);
  expect(n > 12 && find_rr(out, (size_t)n, HOST_NAME, sizeof HOST_NAME, 1, &rr), "name match ignores case");

  uint8_t cq[64];
  memset(cq, 0, sizeof cq);
  cq[5] = 2;
  memcpy(cq + 12, HOST_NAME, sizeof HOST_NAME);
  size_t at = 12 + sizeof HOST_NAME;
  cq[at] = 0;
  cq[at + 1] = 1;
  cq[at + 2] = 0;
  cq[at + 3] = 1;
  at += 4;
  cq[at] = 0xC0;
  cq[at + 1] = 12;
  cq[at + 2] = 0;
  cq[at + 3] = 1;
  cq[at + 4] = 0;
  cq[at + 5] = 1;
  n = mdns_reply(cq, at + 6, addr, 80, 5353, out, sizeof out, &unicast);
  expect(n > 12 && be16(out + 6) == 1 && find_rr(out, (size_t)n, HOST_NAME, sizeof HOST_NAME, 1, &rr),
         "compressed name answers once");

  qn = build_query(q, sizeof q, 1, 0, HTTP_NAME, sizeof HTTP_NAME, 12, 1);
  n = mdns_reply(q, qn, addr, 8080, 5353, out, sizeof out, &unicast);
  expect(n > 12 && unicast == 0, "PTR reply");
  expect(find_rr(out, (size_t)n, HTTP_NAME, sizeof HTTP_NAME, 12, &rr) && rr.section == 1 && rr.class == 1 &&
             rr.rdlen == sizeof INST_NAME && memcmp(rr.rdata, INST_NAME, sizeof INST_NAME) == 0,
         "PTR target");
  expect(find_rr(out, (size_t)n, INST_NAME, sizeof INST_NAME, 33, &rr) && rr.section == 3 && rr.rdlen >= 6 &&
             rr.rdata[4] == 0x1F && rr.rdata[5] == 0x90,
         "SRV additional carries the port");
  expect(find_rr(out, (size_t)n, HOST_NAME, sizeof HOST_NAME, 1, &rr) && rr.section == 3 && memcmp(rr.rdata, ip, 4) == 0,
         "A additional");

  qn = build_query(q, sizeof q, 1, 0, SERVICES_NAME, sizeof SERVICES_NAME, 12, 1);
  n = mdns_reply(q, qn, addr, 80, 5353, out, sizeof out, &unicast);
  expect(n > 12 && find_rr(out, (size_t)n, SERVICES_NAME, sizeof SERVICES_NAME, 12, &rr) && rr.section == 1 &&
             rr.rdlen == sizeof HTTP_NAME && memcmp(rr.rdata, HTTP_NAME, sizeof HTTP_NAME) == 0,
         "service enumeration");

  n = mdns_announce(addr, 80, 120, out, sizeof out);
  expect(n > 12 && be16(out + 4) == 0 && be16(out + 6) == 4, "announcement has four answers");
  expect(find_rr(out, (size_t)n, HOST_NAME, sizeof HOST_NAME, 1, &rr) && rr.ttl == 120 && memcmp(rr.rdata, ip, 4) == 0,
         "announcement address");
  n = mdns_announce(addr, 80, 0, out, sizeof out);
  expect(n > 12 && find_rr(out, (size_t)n, HOST_NAME, sizeof HOST_NAME, 1, &rr) && rr.ttl == 0, "goodbye ttl is zero");
  expect(find_rr(out, (size_t)n, INST_NAME, sizeof INST_NAME, 33, &rr) && rr.ttl == 0, "goodbye srv");

  expect(mdns_reply(q, 11, addr, 80, 5353, out, sizeof out, &unicast) == 0, "short packet");
}

void test_fileserver(void) {
  test_mdns();
  expect(fileserver_start("/", 0) == NULL, "refuse root");
  char missing[] = "/tmp/yomu-fs-no-such";
  expect(fileserver_start(missing, 0) == NULL, "refuse missing");

  char dir[] = "/tmp/yomu-fs-XXXXXX";
  expect(mkdtemp(dir) != NULL, "fs dir");
  if (strstr(dir, "XXXXXX")) {
    return;
  }

  char file[512];
  snprintf(file, sizeof file, "%s/plain.txt", dir);
  FILE *fp = fopen(file, "w");
  expect(fp != NULL, "plain file");
  if (fp) {
    fputs("x", fp);
    fclose(fp);
  }
  expect(fileserver_start(file, 0) == NULL, "refuse file root");
  unlink(file);

  char reserved[3][512];
  const char *reserved_names[] = {"system", "lost+found", "extensions"};
  for (int i = 0; i < 3; i++) {
    snprintf(reserved[i], sizeof reserved[i], "%s/%s", dir, reserved_names[i]);
    expect(mkdir(reserved[i], 0755) == 0, "reserved dir");
  }

  FileServer *clean = fileserver_start(dir, 0);
  expect(clean != NULL, "clean start");
  expect(fileserver_stop(clean) == 0, "clean stop");

  FileServer *fs = fileserver_start(dir, 0);
  expect(fs != NULL, "start");
  int port = fileserver_port(fs);
  expect(port > 0, "ephemeral port");
  if (!fs || port <= 0) {
    rm_tree(dir);
    return;
  }

  char url[160];
  int status = 0;
  char *body = NULL;

  url_of(url, sizeof url, port, "/");
  expect(call("GET", url, NULL, NULL, NULL, &status, &body) == 0, "page rc");
  expect(status == 200 && body && strstr(body, "yomu"), "page has yomu");
  free(body);
  body = NULL;

  url_of(url, sizeof url, port, "/api/status");
  expect(call("GET", url, NULL, NULL, NULL, &status, &body) == 0, "status rc");
  expect(status == 200 && body && strstr(body, "\"version\":\"yomu\""), "status version");
  free(body);
  body = NULL;

  url_of(url, sizeof url, port, "/api/files?path=%2F");
  expect(call("GET", url, NULL, NULL, NULL, &status, &body) == 0, "empty list rc");
  expect(status == 200 && body && strcmp(body, "[]") == 0, "empty list hides reserved");
  free(body);
  body = NULL;

  url_of(url, sizeof url, port, "/upload");
  expect(call("POST", url, "application/octet-stream", "X-File-Path: %2F\r\nX-File-Name: Book.xtch\r\n", "hello-xtch!",
              &status, &body) == 0,
         "upload rc");
  expect(status == 200 && body && strstr(body, "送信しました: Book.xtch"), "upload body");
  free(body);
  body = NULL;
  char path[512];
  char got[64];
  snprintf(path, sizeof path, "%s/Book.xtch", dir);
  expect(read_all(path, got, sizeof got) == 11 && strcmp(got, "hello-xtch!") == 0, "upload bytes");

  url_of(url, sizeof url, port, "/download?path=%2FBook.xtch");
  expect(call("GET", url, NULL, NULL, NULL, &status, &body) == 0, "download rc");
  expect(status == 200 && body && strcmp(body, "hello-xtch!") == 0, "download body");
  free(body);
  body = NULL;

  url_of(url, sizeof url, port, "/mkdir");
  expect(call("POST", url, "application/x-www-form-urlencoded", NULL, "name=Sub&path=%2F", &status, &body) == 0,
         "mkdir rc");
  expect(status == 200 && body && strcmp(body, "OK") == 0, "mkdir ok");
  free(body);
  body = NULL;
  snprintf(path, sizeof path, "%s/Sub", dir);
  expect(exists_dir(path), "subdir exists");

  url_of(url, sizeof url, port, "/upload");
  expect(call("POST", url, "application/octet-stream", "X-File-Path: %2FSub\r\nX-File-Name: Nested.txt\r\n", "nested",
              &status, &body) == 0,
         "nested upload rc");
  expect(status == 200, "nested upload");
  free(body);
  body = NULL;
  snprintf(path, sizeof path, "%s/Sub/Nested.txt", dir);
  expect(read_all(path, got, sizeof got) == 6 && strcmp(got, "nested") == 0, "nested bytes");

  url_of(url, sizeof url, port, "/api/files?path=%2F");
  expect(call("GET", url, NULL, NULL, NULL, &status, &body) == 0, "list rc");
  expect(status == 200 && body && strstr(body, "\"name\":\"Sub\"") && strstr(body, "\"isDirectory\":true") &&
             strstr(body, "\"name\":\"Book.xtch\"") && strstr(body, ".yomu-upload") == NULL &&
             strstr(body, "system") == NULL,
         "list order and filter");
  if (body) {
    const char *sub = strstr(body, "Sub");
    const char *book = strstr(body, "Book.xtch");
    expect(sub && book && sub < book, "dirs before files");
  }
  free(body);
  body = NULL;

  url_of(url, sizeof url, port, "/rename");
  expect(call("POST", url, "application/x-www-form-urlencoded", NULL, "path=%2FBook.xtch&name=Renamed.xtch", &status,
              &body) == 0,
         "rename rc");
  expect(status == 200, "rename ok");
  free(body);
  body = NULL;
  snprintf(path, sizeof path, "%s/Renamed.xtch", dir);
  expect(read_all(path, got, sizeof got) == 11, "renamed file");

  url_of(url, sizeof url, port, "/move");
  expect(call("POST", url, "application/x-www-form-urlencoded", NULL, "path=%2FRenamed.xtch&dest=%2FSub", &status,
              &body) == 0,
         "move rc");
  expect(status == 200, "move ok");
  free(body);
  body = NULL;
  snprintf(path, sizeof path, "%s/Sub/Renamed.xtch", dir);
  expect(read_all(path, got, sizeof got) == 11 && strcmp(got, "hello-xtch!") == 0, "moved bytes");

  snprintf(path, sizeof path, "%s/Sub/.secret", dir);
  fp = fopen(path, "w");
  if (fp) {
    fputs("hide", fp);
    fclose(fp);
  }
  url_of(url, sizeof url, port, "/delete");
  expect(call("POST", url, "application/x-www-form-urlencoded", NULL, "path=%2FSub", &status, &body) == 0, "delete rc");
  expect(status == 200, "delete dir");
  free(body);
  body = NULL;
  snprintf(path, sizeof path, "%s/Sub", dir);
  expect(!exists_dir(path), "dir and children gone");
  snprintf(path, sizeof path, "%s/Sub/.secret", dir);
  expect(read_all(path, got, sizeof got) < 0, "hidden child gone");

  url_of(url, sizeof url, port, "/download?path=%2F%2e%2e%2Fetc%2Fpasswd");
  expect(call("GET", url, NULL, NULL, NULL, &status, &body) == 0, "dotdot rc");
  expect(status == 403, "dotdot is forbidden");
  free(body);
  body = NULL;

  url_of(url, sizeof url, port, "/upload");
  expect(call("POST", url, "application/octet-stream", "X-File-Path: %2F\r\nX-File-Name: .hid\r\n", "nope", &status,
              &body) == 0,
         "dotfile rc");
  expect(status == 403, "dotfile forbidden");
  free(body);
  body = NULL;
  snprintf(path, sizeof path, "%s/.hid", dir);
  expect(read_all(path, got, sizeof got) < 0, "dotfile not written");

  unlink("/tmp/evil.txt");
  expect(call("POST", url, "application/octet-stream", "X-File-Path: %2F%2e%2e\r\nX-File-Name: evil.txt\r\n", "nope",
              &status, &body) == 0,
         "escape rc");
  expect(status == 403, "escape forbidden");
  free(body);
  body = NULL;
  expect(read_all("/tmp/evil.txt", got, sizeof got) < 0, "escape did not write");

  snprintf(path, sizeof path, "%s/link", dir);
  expect(symlink("Book.xtch", path) == 0, "symlink");
  url_of(url, sizeof url, port, "/download?path=%2Flink");
  expect(call("GET", url, NULL, NULL, NULL, &status, &body) == 0, "symlink rc");
  expect(status == 403, "symlink forbidden");
  free(body);
  body = NULL;
  url_of(url, sizeof url, port, "/api/files?path=%2F");
  expect(call("GET", url, NULL, NULL, NULL, &status, &body) == 0, "symlink list rc");
  expect(status == 200 && body && strstr(body, "link") == NULL, "symlink not listed");
  free(body);
  body = NULL;

  url_of(url, sizeof url, port, "/upload");
  expect(call("POST", url, "application/octet-stream", "X-File-Path: %2F\r\nX-File-Name: My%20Book.epub\r\n", "spaced",
              &status, &body) == 0,
         "space upload rc");
  expect(status == 200 && body && strstr(body, "送信しました: My Book.epub"), "space upload body");
  free(body);
  body = NULL;
  snprintf(path, sizeof path, "%s/My Book.epub", dir);
  expect(read_all(path, got, sizeof got) == 6 && strcmp(got, "spaced") == 0, "space upload bytes");

  url_of(url, sizeof url, port, "/api/files?path=%2F");
  expect(call("GET", url, NULL, NULL, NULL, &status, &body) == 0, "space list rc");
  expect(status == 200 && body && strstr(body, "\"name\":\"My Book.epub\""), "space list name");
  free(body);
  body = NULL;

  url_of(url, sizeof url, port, "/download?path=%2FMy%20Book.epub");
  expect(call("GET", url, NULL, NULL, NULL, &status, &body) == 0, "space download rc");
  expect(status == 200 && body && strcmp(body, "spaced") == 0, "space download body");
  free(body);
  body = NULL;

  url_of(url, sizeof url, port, "/download/My%20Book.epub?path=%2FMy%20Book.epub");
  expect(call("GET", url, NULL, NULL, NULL, &status, &body) == 0, "space named download rc");
  expect(status == 200 && body && strcmp(body, "spaced") == 0, "space named download body");
  free(body);
  body = NULL;

  char resp[4096];
  const char *rbody = NULL;
  expect(raw_exchange(port,
                      "GET /download?path=/My Book.epub HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n",
                      resp, sizeof resp, &status, &rbody) == 0,
         "raw space rc");
  expect(status == 200 && rbody && strcmp(rbody, "spaced") == 0, "raw space body");
  expect(strstr(resp, "filename=\"My Book.epub\"") != NULL, "disposition quotes the blank");
  expect(strstr(resp, "filename*=UTF-8''%4D%79%20%42%6F%6F%6B%2E%65%70%75%62") != NULL,
         "disposition encodes the blank");

  url_of(url, sizeof url, port, "/mkdir");
  expect(call("POST", url, "application/x-www-form-urlencoded", NULL, "name=My%20Folder&path=%2F", &status, &body) == 0,
         "space mkdir rc");
  expect(status == 200, "space mkdir");
  free(body);
  body = NULL;
  snprintf(path, sizeof path, "%s/My Folder", dir);
  expect(exists_dir(path), "space dir");

  expect(call("POST", url, "application/x-www-form-urlencoded", NULL, "name=Plus+Name&path=%2F", &status, &body) == 0,
         "plus mkdir rc");
  expect(status == 200, "plus mkdir");
  free(body);
  body = NULL;
  snprintf(path, sizeof path, "%s/Plus Name", dir);
  expect(exists_dir(path), "plus dir");

  url_of(url, sizeof url, port, "/upload");
  expect(call("POST", url, "application/octet-stream",
              "X-File-Path: %2FMy%20Folder\r\nX-File-Name: Nested%20Name.txt\r\n", "nest", &status, &body) == 0,
         "space nested upload rc");
  expect(status == 200, "space nested upload");
  free(body);
  body = NULL;
  snprintf(path, sizeof path, "%s/My Folder/Nested Name.txt", dir);
  expect(read_all(path, got, sizeof got) == 4 && strcmp(got, "nest") == 0, "space nested bytes");

  expect(call("POST", url, "application/octet-stream", "X-File-Path: %2F\r\nX-File-Name: Raw Name.txt\r\n", "raw",
              &status, &body) == 0,
         "raw header upload rc");
  expect(status == 200 && body && strstr(body, "送信しました: Raw Name.txt"), "raw header upload");
  free(body);
  body = NULL;
  snprintf(path, sizeof path, "%s/Raw Name.txt", dir);
  expect(read_all(path, got, sizeof got) == 3 && strcmp(got, "raw") == 0, "raw header bytes");

  url_of(url, sizeof url, port, "/api/files?path=%2FMy%20Folder");
  expect(call("GET", url, NULL, NULL, NULL, &status, &body) == 0, "space folder list rc");
  expect(status == 200 && body && strstr(body, "\"name\":\"Nested Name.txt\""), "space folder list");
  free(body);
  body = NULL;

  expect(raw_exchange(port, "GET /api/files?path=/My Folder HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n",
                      resp, sizeof resp, &status, &rbody) == 0,
         "raw folder list rc");
  expect(status == 200 && rbody && strstr(rbody, "\"name\":\"Nested Name.txt\""), "raw folder list");

  url_of(url, sizeof url, port, "/download?path=%2FMy%20Folder%2FNested%20Name.txt");
  expect(call("GET", url, NULL, NULL, NULL, &status, &body) == 0, "space nested download rc");
  expect(status == 200 && body && strcmp(body, "nest") == 0, "space nested download");
  free(body);
  body = NULL;

  expect(raw_exchange(port,
                      "GET /download?path=/My Folder/Nested Name.txt HTTP/1.1\r\nHost: 127.0.0.1\r\n"
                      "Connection: close\r\n\r\n",
                      resp, sizeof resp, &status, &rbody) == 0,
         "raw nested rc");
  expect(status == 200 && rbody && strcmp(rbody, "nest") == 0, "raw nested body");

  url_of(url, sizeof url, port, "/rename");
  expect(call("POST", url, "application/x-www-form-urlencoded", NULL, "path=%2FMy%20Book.epub&name=Renamed%20Book.epub",
              &status, &body) == 0,
         "space rename rc");
  expect(status == 200, "space rename");
  free(body);
  body = NULL;
  snprintf(path, sizeof path, "%s/Renamed Book.epub", dir);
  expect(read_all(path, got, sizeof got) == 6 && strcmp(got, "spaced") == 0, "space renamed bytes");

  url_of(url, sizeof url, port, "/move");
  expect(call("POST", url, "application/x-www-form-urlencoded", NULL, "path=%2FRenamed%20Book.epub&dest=%2FMy%20Folder",
              &status, &body) == 0,
         "space move rc");
  expect(status == 200, "space move");
  free(body);
  body = NULL;
  snprintf(path, sizeof path, "%s/My Folder/Renamed Book.epub", dir);
  expect(read_all(path, got, sizeof got) == 6 && strcmp(got, "spaced") == 0, "space moved bytes");

  expect(raw_exchange(port,
                      "GET /download?path=/My Folder/../../etc/passwd HTTP/1.1\r\nHost: 127.0.0.1\r\n"
                      "Connection: close\r\n\r\n",
                      resp, sizeof resp, &status, &rbody) == 0,
         "space dotdot rc");
  expect(status == 403, "space dotdot forbidden");

  url_of(url, sizeof url, port, "/delete");
  expect(call("POST", url, "application/x-www-form-urlencoded", NULL, "path=%2FMy%20Folder", &status, &body) == 0,
         "space delete rc");
  expect(status == 200, "space delete");
  free(body);
  body = NULL;
  snprintf(path, sizeof path, "%s/My Folder", dir);
  expect(!exists_dir(path), "space dir gone");

  url_of(url, sizeof url, port, "/");
  expect(call("GET", url, NULL, NULL, NULL, &status, &body) == 0, "page again rc");
  expect(status == 200 && body && strstr(body, "/download/\" + encodeURIComponent(item.name)"), "page names the download");
  free(body);
  body = NULL;

  /* NFD title: U+30AF U+3099 (not precomposed U+30B0), fullwidth tildes, blanks. */
  const char *goju =
      "\xe8\xaa\x9e\xe5\xbd\x99\xe5\x8a\x9b\xe3\x82\x92\xe9\x8d\x9b\xe3\x81\x88\xe3\x82\x8b\xef\xbd\x9e\xe9\x87\x8f\xe3"
      "\x81\xa8\xe8\xb3\xaa\xe3\x82\x92\xe9\xab\x98\xe3\x82\x81\xe3\x82\x8b\xe3\x83\x88\xe3\x83\xac\xe3\x83\xbc\xe3\x83"
      "\x8b\xe3\x83\xb3\xe3\x82\xaf\xe3\x82\x99\xef\xbd\x9e\x20\x28\xe5\x85\x89\xe6\x96\x87\xe7\xa4\xbe\xe6\x96\xb0\xe6"
      "\x9b\xb8\x29\x20\x28\xe7\x9f\xb3\xe9\xbb\x92\x20\xe5\x9c\xad\x29\x20\x28\x7a\x2d\x6c\x69\x62\x72\x61\x72\x79\x2e"
      "\x73\x6b\x2c\x20\x31\x6c\x69\x62\x2e\x73\x6b\x2c\x20\x7a\x2d\x6c\x69\x62\x2e\x73\x6b\x29\x2e\x65\x70\x75\x62";
  const char *goju_nfc =
      "\xe8\xaa\x9e\xe5\xbd\x99\xe5\x8a\x9b\xe3\x82\x92\xe9\x8d\x9b\xe3\x81\x88\xe3\x82\x8b\xef\xbd\x9e\xe9\x87\x8f\xe3"
      "\x81\xa8\xe8\xb3\xaa\xe3\x82\x92\xe9\xab\x98\xe3\x82\x81\xe3\x82\x8b\xe3\x83\x88\xe3\x83\xac\xe3\x83\xbc\xe3\x83"
      "\x8b\xe3\x83\xb3\xe3\x82\xb0\xef\xbd\x9e\x20\x28\xe5\x85\x89\xe6\x96\x87\xe7\xa4\xbe\xe6\x96\xb0\xe6\x9b\xb8\x29"
      "\x20\x28\xe7\x9f\xb3\xe9\xbb\x92\x20\xe5\x9c\xad\x29\x20\x28\x7a\x2d\x6c\x69\x62\x72\x61\x72\x79\x2e\x73\x6b\x2c"
      "\x20\x31\x6c\x69\x62\x2e\x73\x6b\x2c\x20\x7a\x2d\x6c\x69\x62\x2e\x73\x6b\x29\x2e\x65\x70\x75\x62";
  const char *bible =
      "\xe8\x81\x96\xe6\x9b\xb8\x20\xe6\x96\xb0\xe5\x85\xb1\xe5\x90\x8c\xe8\xa8\xb3\x20\xe6\x96\xb0\xe7\xb4\x84\xe8\x81"
      "\x96\xe6\x9b\xb8\x20\x28\xe6\x97\xa5\xe6\x9c\xac\xe8\x81\x96\xe6\x9b\xb8\xe5\x8d\x94\xe4\xbc\x9a\x2c\x20\xe5\x85"
      "\xb1\xe5\x90\x8c\xe8\xa8\xb3\xe8\x81\x96\xe6\x9b\xb8\xe5\xae\x9f\xe8\xa1\x8c\xe5\xa7\x94\xe5\x93\xa1\xe4\xbc\x9a"
      "\x29\x20\x28\x7a\x2d\x6c\x69\x62\x72\x61\x72\x79\x2e\x73\x6b\x2c\x20\x31\x6c\x69\x62\x2e\x73\x6b\x2c\x20\x7a\x2d"
      "\x6c\x69\x62\x2e\x73\x6b\x29\x2e\x65\x70\x75\x62";
  const char *bible_cut =
      "\xe8\x81\x96\xe6\x9b\xb8\x20\xe6\x96\xb0\xe5\x85\xb1\xe5\x90\x8c\xe8\xa8\xb3\x20\xe6\x96\xb0\xe7\xb4\x84\xe8\x81"
      "\x96\xe6\x9b\xb8\x20\x28\xe6\x97\xa5\xe6\x9c\xac\xe8\x81\x96\xe6\x9b\xb8\xe5\x8d\x94\xe4\xbc\x9a\x2c\x20\xe5\x85"
      "\xb1\xe5\x90\x8c\xe8\xa8\xb3\xe8\x81\x96\xe6\x9b\xb8\xe5\xae\x9f\xe8\xa1\x8c\xe5\xa7\x94\xe5\x93\xa1\xe4\xbc\x9a"
      "\x29\x20\x28";
  expect(strlen(goju) == 139 && strlen(goju_nfc) == 136 && strlen(bible) == 124, "long name byte length");
  /* APFS treats NFD and NFC as one file, so the precomposed name is checked on its own. */
  const char *long_names[] = {goju_nfc, goju, bible};
  const char *long_payloads[] = {"nfc", "nfd", "bib"};
  char enc[768];
  char hdr[896];
  char long_url[1400];
  char form[1600];
  for (int i = 0; i < 3; i++) {
    expect(pct_encode(long_names[i], enc, sizeof enc) == 0, "long name encodes");
    expect(strlen(enc) > 255, "encoded long name exceeds the old cap");
    expect(snprintf(hdr, sizeof hdr, "X-File-Path: %%2F\r\nX-File-Name: %s\r\n", enc) < (int)sizeof hdr,
           "long name header");
    url_of(url, sizeof url, port, "/upload");
    expect(call("POST", url, "application/octet-stream", hdr, long_payloads[i], &status, &body) == 0, "long upload rc");
    expect(status == 200 && body && strstr(body, long_names[i]) != NULL, "long upload keeps the name");
    free(body);
    body = NULL;
    expect(snprintf(path, sizeof path, "%s/%s", dir, long_names[i]) < (int)sizeof path, "long disk path");
    expect(read_all(path, got, sizeof got) == (int)strlen(long_payloads[i]) && strcmp(got, long_payloads[i]) == 0,
           "long upload bytes");
    if (i == 0) {
      expect(snprintf(form, sizeof form, "path=%%2F%s", enc) < (int)sizeof form, "nfc delete form");
      url_of(url, sizeof url, port, "/delete");
      expect(call("POST", url, "application/x-www-form-urlencoded", NULL, form, &status, &body) == 0, "nfc delete rc");
      expect(status == 200, "nfc delete");
      free(body);
      body = NULL;
      expect(read_all(path, got, sizeof got) < 0, "nfc removed");
    }
  }
  url_of(url, sizeof url, port, "/api/files?path=%2F");
  expect(call("GET", url, NULL, NULL, NULL, &status, &body) == 0, "long list rc");
  expect(status == 200 && body && strstr(body, goju) && strstr(body, bible), "long list names");
  free(body);
  body = NULL;
  expect(snprintf(path, sizeof path, "%s/%s", dir, bible_cut) < (int)sizeof path, "cut bible path");
  expect(read_all(path, got, sizeof got) < 0, "bible name was not cut");

  expect(pct_encode(goju, enc, sizeof enc) == 0, "goju encodes");
  expect(snprintf(long_url, sizeof long_url, "/download?path=%%2F%s", enc) < (int)sizeof long_url, "goju download url");
  expect(snprintf(form, sizeof form,
                  "GET %s HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n", long_url) < (int)sizeof form,
         "goju download request");
  {
    char resp[4096];
    const char *rbody = NULL;
    expect(raw_exchange(port, form, resp, sizeof resp, &status, &rbody) == 0, "goju download rc");
    expect(status == 200 && rbody && strcmp(rbody, "nfd") == 0, "goju download body");
    expect(strstr(resp, "filename=\"") != NULL && strstr(resp, goju) != NULL, "goju disposition quotes the name");
    expect(strstr(resp, "%EF%BD%9E") != NULL && strstr(resp, "%E3%82%99") != NULL, "goju disposition keeps tilde and mark");
  }

  {
    char folder[160];
    size_t glen = strlen(goju);
    expect(glen > 5, "goju has an extension");
    memcpy(folder, goju, glen - 5);
    folder[glen - 5] = '\0';
    expect(pct_encode(folder, enc, sizeof enc) == 0, "folder encodes");
    expect(strlen(enc) > 255, "encoded folder exceeds the old cap");
    expect(snprintf(form, sizeof form, "name=%s&path=%%2F", enc) < (int)sizeof form, "folder form");
    url_of(url, sizeof url, port, "/mkdir");
    expect(call("POST", url, "application/x-www-form-urlencoded", NULL, form, &status, &body) == 0, "long mkdir rc");
    expect(status == 200, "long mkdir");
    free(body);
    body = NULL;
    expect(snprintf(path, sizeof path, "%s/%s", dir, folder) < (int)sizeof path, "long folder path");
    expect(exists_dir(path), "long folder kept");
  }

  memset(enc, 'a', 256);
  enc[256] = '\0';
  expect(snprintf(hdr, sizeof hdr, "X-File-Path: %%2F\r\nX-File-Name: %s\r\n", enc) < (int)sizeof hdr, "overlong header");
  url_of(url, sizeof url, port, "/upload");
  expect(call("POST", url, "application/octet-stream", hdr, "no", &status, &body) == 0, "overlong rc");
  expect(status == 400, "overlong name rejected");
  free(body);
  body = NULL;
  enc[255] = '\0';
  expect(snprintf(path, sizeof path, "%s/%s", dir, enc) < (int)sizeof path, "truncated path");
  expect(read_all(path, got, sizeof got) < 0, "overlong name was not truncated");

  url_of(url, sizeof url, port, "/download");
  expect(call("GET", url, NULL, NULL, NULL, &status, &body) == 0, "missing path rc");
  expect(status == 400, "missing path");
  free(body);
  body = NULL;

  url_of(url, sizeof url, port, "/nope");
  expect(call("GET", url, NULL, NULL, NULL, &status, &body) == 0, "unknown rc");
  expect(status == 404, "unknown route");
  free(body);

  expect(fileserver_stop(fs) == 1, "stop reports a change");
  rm_tree(dir);

  char ui[] = "/tmp/yomu-xfer-XXXXXX";
  expect(mkdtemp(ui) != NULL, "xfer ui dir");
  if (!strstr(ui, "XXXXXX")) {
    test_xfer_screen(ui);
    rmdir(ui);
  }
}
