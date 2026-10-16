#include "app.h"
#include "fileserver.h"
#include "http.h"

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
  expect(app_run(&host, NULL) == 0, "xfer screen run");
  expect(g_xfer_saw == 1, "transfer row serves the page");
  expect(!port_open(port), "back stops the server");
  unsetenv("YOMU_PORT");
  free(gray);
}

void test_fileserver(void) {
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
