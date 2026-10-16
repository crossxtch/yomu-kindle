#ifndef YOMU_HTTP_H
#define YOMU_HTTP_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef struct HttpRequest {
  const char *method;
  const char *url;
  const char *user;
  const char *password;
  const char *content_type;
  const char *extra_headers; /* complete lines, each ending in CRLF */
  const void *body;
  size_t body_len;
  int max_redirects; /* followed for GET only */
  size_t max_body;   /* 0 means no cap */
  FILE *dest;
  unsigned char **mem;
  size_t *mem_len;
  void (*progress)(void *ud, int64_t got);
  void *progress_ud;
} HttpRequest;

/* 0 when a final HTTP status was read. -1 on a transport failure.
 * The body is kept only for a 2xx response.
 */
int http_request(const HttpRequest *req, int *status, char *err, size_t errcap);

#endif
