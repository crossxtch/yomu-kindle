#include "mdns.h"

#include "log.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define MDNS_PORT 5353
#define MDNS_GROUP 0xE00000FBu
#define MDNS_TTL 120u

enum {
  REC_A = 1,
  REC_PTR = 2,
  REC_SRV = 4,
  REC_TXT = 8,
  REC_ENUM = 16
};

static const uint8_t NAME_HOST[] = {4, 'y', 'o', 'm', 'u', 5, 'l', 'o', 'c', 'a', 'l', 0};
static const uint8_t NAME_HTTP[] = {5, '_', 'h', 't', 't', 'p', 4, '_', 't', 'c', 'p', 5, 'l', 'o', 'c', 'a', 'l', 0};
static const uint8_t NAME_INST[] = {4,   'y', 'o', 'm', 'u', 5,   '_', 'h', 't', 't', 'p', 4,   '_', 't',
                                    'c', 'p', 5,   'l', 'o', 'c', 'a', 'l', 0};
static const uint8_t NAME_SERVICES[] = {9,   '_', 's', 'e', 'r', 'v', 'i', 'c', 'e', 's', 7,   '_', 'd', 'n', 's',
                                        '-', 's', 'd', 4,   '_', 'u', 'd', 'p', 5,   'l', 'o', 'c', 'a', 'l', 0};

struct Mdns {
  int fd;
  int http_port;
  int firewall;
  int announces_left;
  uint32_t addr;
  int64_t next_announce_ms;
};

static uint16_t ru16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }

static int name_eq(const uint8_t *a, size_t an, const uint8_t *b, size_t bn) {
  size_t i = 0;
  while (i < an && i < bn) {
    uint8_t n = a[i];
    if (n != b[i]) {
      return 0;
    }
    if (n == 0) {
      return 1;
    }
    if ((n & 0xC0) != 0 || i + 1u + n >= an || i + 1u + n >= bn) {
      return 0;
    }
    for (uint8_t k = 0; k < n; k++) {
      unsigned char ca = a[i + 1u + k];
      unsigned char cb = b[i + 1u + k];
      if (tolower(ca) != tolower(cb)) {
        return 0;
      }
    }
    i += 1u + n;
  }
  return 0;
}

static int read_name(const uint8_t *pkt, size_t len, size_t off, size_t *used, uint8_t *dst, size_t dstcap,
                     size_t *dstlen) {
  size_t pos = off;
  int jumped = 0;
  int jumps = 0;
  size_t out = 0;
  for (;;) {
    if (pos >= len) {
      return -1;
    }
    uint8_t lab = pkt[pos];
    if (lab == 0) {
      if (out + 1 > dstcap) {
        return -1;
      }
      dst[out++] = 0;
      if (!jumped) {
        *used = pos + 1 - off;
      }
      *dstlen = out;
      return 0;
    }
    if ((lab & 0xC0) == 0xC0) {
      if (pos + 1 >= len || ++jumps > 16) {
        return -1;
      }
      if (!jumped) {
        *used = pos + 2 - off;
        jumped = 1;
      }
      pos = ((size_t)(lab & 0x3F) << 8) | pkt[pos + 1];
      continue;
    }
    if ((lab & 0xC0) != 0 || pos + 1u + lab > len || out + 1u + lab > dstcap) {
      return -1;
    }
    dst[out++] = lab;
    memcpy(dst + out, pkt + pos + 1, lab);
    out += lab;
    pos += 1u + lab;
  }
}

static int match_bits(const uint8_t *name, size_t namelen, uint16_t qtype) {
  int any = qtype == 255;
  int bits = 0;
  if (name_eq(name, namelen, NAME_HOST, sizeof NAME_HOST) && (qtype == 1 || any)) {
    bits |= REC_A;
  }
  if (name_eq(name, namelen, NAME_HTTP, sizeof NAME_HTTP) && (qtype == 12 || any)) {
    bits |= REC_PTR;
  }
  if (name_eq(name, namelen, NAME_INST, sizeof NAME_INST) && (qtype == 33 || any)) {
    bits |= REC_SRV;
  }
  if (name_eq(name, namelen, NAME_INST, sizeof NAME_INST) && (qtype == 16 || any)) {
    bits |= REC_TXT;
  }
  if (name_eq(name, namelen, NAME_SERVICES, sizeof NAME_SERVICES) && (qtype == 12 || any)) {
    bits |= REC_ENUM;
  }
  return bits;
}

typedef struct Buf {
  uint8_t *p;
  size_t n;
  size_t cap;
} Buf;

static int put_bytes(Buf *b, const void *s, size_t n) {
  if (b->n + n > b->cap) {
    return -1;
  }
  memcpy(b->p + b->n, s, n);
  b->n += n;
  return 0;
}

static int put_u16(Buf *b, uint16_t v) {
  uint8_t d[2];
  d[0] = (uint8_t)(v >> 8);
  d[1] = (uint8_t)v;
  return put_bytes(b, d, 2);
}

static int put_u32(Buf *b, uint32_t v) {
  uint8_t d[4];
  d[0] = (uint8_t)(v >> 24);
  d[1] = (uint8_t)(v >> 16);
  d[2] = (uint8_t)(v >> 8);
  d[3] = (uint8_t)v;
  return put_bytes(b, d, 4);
}

static int put_rr(Buf *b, const uint8_t *name, size_t namelen, uint16_t type, uint16_t class, uint32_t ttl,
                  const void *rdata, size_t rdlen) {
  if (rdlen > 65535) {
    return -1;
  }
  if (put_bytes(b, name, namelen) != 0 || put_u16(b, type) != 0 || put_u16(b, class) != 0 || put_u32(b, ttl) != 0 ||
      put_u16(b, (uint16_t)rdlen) != 0) {
    return -1;
  }
  return put_bytes(b, rdata, rdlen);
}

static int emit(Buf *b, int bits, uint32_t addr, int http_port, uint32_t ttl) {
  int n = 0;
  if (bits & REC_PTR) {
    if (put_rr(b, NAME_HTTP, sizeof NAME_HTTP, 12, 1, ttl, NAME_INST, sizeof NAME_INST) != 0) {
      return -1;
    }
    n++;
  }
  if (bits & REC_ENUM) {
    if (put_rr(b, NAME_SERVICES, sizeof NAME_SERVICES, 12, 1, ttl, NAME_HTTP, sizeof NAME_HTTP) != 0) {
      return -1;
    }
    n++;
  }
  if (bits & REC_SRV) {
    uint8_t rdata[8 + sizeof NAME_HOST];
    rdata[0] = 0;
    rdata[1] = 0;
    rdata[2] = 0;
    rdata[3] = 0;
    rdata[4] = (uint8_t)(http_port >> 8);
    rdata[5] = (uint8_t)http_port;
    memcpy(rdata + 6, NAME_HOST, sizeof NAME_HOST);
    if (put_rr(b, NAME_INST, sizeof NAME_INST, 33, 0x8001, ttl, rdata, 6 + sizeof NAME_HOST) != 0) {
      return -1;
    }
    n++;
  }
  if (bits & REC_TXT) {
    uint8_t empty = 0;
    if (put_rr(b, NAME_INST, sizeof NAME_INST, 16, 0x8001, ttl, &empty, 1) != 0) {
      return -1;
    }
    n++;
  }
  if (bits & REC_A) {
    uint8_t ip[4];
    memcpy(ip, &addr, 4);
    if (put_rr(b, NAME_HOST, sizeof NAME_HOST, 1, 0x8001, ttl, ip, 4) != 0) {
      return -1;
    }
    n++;
  }
  return n;
}

static int write_header(uint8_t *out, size_t cap, uint16_t id, uint16_t qd, uint16_t an, uint16_t ar, const uint8_t *q,
                        size_t qlen, const uint8_t *rec, size_t reclen) {
  if (cap < 12 + qlen + reclen) {
    return 0;
  }
  out[0] = (uint8_t)(id >> 8);
  out[1] = (uint8_t)id;
  out[2] = 0x84;
  out[3] = 0x00;
  out[4] = (uint8_t)(qd >> 8);
  out[5] = (uint8_t)qd;
  out[6] = (uint8_t)(an >> 8);
  out[7] = (uint8_t)an;
  out[8] = 0;
  out[9] = 0;
  out[10] = (uint8_t)(ar >> 8);
  out[11] = (uint8_t)ar;
  if (qlen) {
    memcpy(out + 12, q, qlen);
  }
  memcpy(out + 12 + qlen, rec, reclen);
  return (int)(12 + qlen + reclen);
}

int mdns_announce(uint32_t addr_be, int http_port, uint32_t ttl, uint8_t *out, size_t cap) {
  if (!out || http_port <= 0 || http_port > 65535) {
    return 0;
  }
  uint8_t rec[512];
  Buf b;
  b.p = rec;
  b.n = 0;
  b.cap = sizeof rec;
  int an = emit(&b, REC_PTR | REC_SRV | REC_TXT | REC_A, addr_be, http_port, ttl);
  if (an <= 0) {
    return 0;
  }
  return write_header(out, cap, 0, 0, (uint16_t)an, 0, NULL, 0, rec, b.n);
}

int mdns_reply(const uint8_t *query, size_t query_len, uint32_t addr_be, int http_port, int from_port, uint8_t *out,
               size_t cap, int *unicast) {
  if (unicast) {
    *unicast = 0;
  }
  if (!query || query_len < 12 || !out || http_port <= 0 || http_port > 65535) {
    return 0;
  }
  uint16_t flags = ru16(query + 2);
  if ((flags & 0x8000) || (flags & 0x7800)) {
    return 0;
  }
  uint16_t qd = ru16(query + 4);
  if (qd == 0 || qd > 8) {
    return 0;
  }

  typedef struct Hit {
    uint8_t name[256];
    size_t namelen;
    uint16_t qtype;
    uint16_t qclass;
  } Hit;
  Hit hits[8];
  int nhits = 0;
  int bits = 0;
  int qu_yes = 0;
  int qu_no = 0;
  size_t off = 12;
  for (uint16_t i = 0; i < qd; i++) {
    size_t used = 0;
    uint8_t name[256];
    size_t namelen = 0;
    if (read_name(query, query_len, off, &used, name, sizeof name, &namelen) != 0) {
      return 0;
    }
    off += used;
    if (off + 4 > query_len) {
      return 0;
    }
    uint16_t qtype = ru16(query + off);
    uint16_t qclass = ru16(query + off + 2);
    off += 4;
    uint16_t cls = (uint16_t)(qclass & 0x7FFF);
    if (cls != 1 && cls != 255) {
      continue;
    }
    int b = match_bits(name, namelen, qtype);
    if (!b) {
      continue;
    }
    bits |= b;
    if (qclass & 0x8000) {
      qu_yes = 1;
    } else {
      qu_no = 1;
    }
    if (nhits < 8) {
      memcpy(hits[nhits].name, name, namelen);
      hits[nhits].namelen = namelen;
      hits[nhits].qtype = qtype;
      hits[nhits].qclass = cls;
      nhits++;
    }
  }
  if (!bits || nhits == 0) {
    return 0;
  }

  int uni = from_port != MDNS_PORT || (qu_yes && !qu_no);
  uint8_t qsec[1024];
  Buf qb;
  qb.p = qsec;
  qb.n = 0;
  qb.cap = sizeof qsec;
  if (uni) {
    for (int i = 0; i < nhits; i++) {
      if (put_bytes(&qb, hits[i].name, hits[i].namelen) != 0 || put_u16(&qb, hits[i].qtype) != 0 ||
          put_u16(&qb, hits[i].qclass) != 0) {
        return 0;
      }
    }
  }

  int extra = 0;
  if ((bits & (REC_PTR | REC_ENUM | REC_SRV)) && (bits & REC_A) == 0) {
    extra |= REC_A;
  }
  if ((bits & (REC_PTR | REC_ENUM)) && (bits & REC_SRV) == 0) {
    extra |= REC_SRV;
  }
  if ((bits & (REC_PTR | REC_ENUM)) && (bits & REC_TXT) == 0) {
    extra |= REC_TXT;
  }
  if ((bits & REC_ENUM) && (bits & REC_PTR) == 0) {
    extra |= REC_PTR;
  }

  uint8_t rec[512];
  Buf b;
  b.p = rec;
  b.n = 0;
  b.cap = sizeof rec;
  int an = emit(&b, bits, addr_be, http_port, MDNS_TTL);
  size_t answer_len = b.n;
  int ar = 0;
  if (extra) {
    ar = emit(&b, extra, addr_be, http_port, MDNS_TTL);
  }
  if (an <= 0 || ar < 0 || b.n < answer_len) {
    return 0;
  }
  if (unicast) {
    *unicast = uni;
  }
  return write_header(out, cap, uni ? ru16(query) : 0, uni ? (uint16_t)nhits : 0, (uint16_t)an, (uint16_t)ar,
                      uni ? qsec : NULL, uni ? qb.n : 0, rec, b.n);
}

static int mdns_send(const Mdns *m, const uint8_t *pkt, size_t n, const struct sockaddr_in *to) {
  struct sockaddr_in dest;
  if (!to) {
    memset(&dest, 0, sizeof dest);
    dest.sin_family = AF_INET;
    dest.sin_port = htons(MDNS_PORT);
    dest.sin_addr.s_addr = htonl(MDNS_GROUP);
    to = &dest;
  }
  ssize_t w = sendto(m->fd, pkt, n, 0, (const struct sockaddr *)to, sizeof *to);
  return w == (ssize_t)n ? 0 : -1;
}

static void firewall_open(Mdns *m) {
  if (system("command -v iptables >/dev/null 2>&1") != 0) {
    return;
  }
  if (system("iptables -C INPUT -p udp --dport 5353 -j ACCEPT >/dev/null 2>&1") == 0) {
    return;
  }
  if (system("iptables -I INPUT 1 -p udp --dport 5353 -j ACCEPT >/dev/null 2>&1") == 0) {
    m->firewall = 1;
  }
}

static void firewall_close(Mdns *m) {
  if (!m->firewall) {
    return;
  }
  system("iptables -D INPUT -p udp --dport 5353 -j ACCEPT >/dev/null 2>&1");
  m->firewall = 0;
}

static void send_announce(Mdns *m, uint32_t ttl) {
  uint8_t pkt[512];
  int n = mdns_announce(m->addr, m->http_port, ttl, pkt, sizeof pkt);
  if (n > 0) {
    mdns_send(m, pkt, (size_t)n, NULL);
  }
}

Mdns *mdns_start(uint32_t addr_be, int http_port, unsigned ifindex) {
  if (!addr_be || http_port <= 0 || http_port > 65535 || ifindex == 0) {
    return NULL;
  }
  int fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) {
    yomu_log("mdns socket failed");
    return NULL;
  }
  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
#ifdef SO_REUSEPORT
  setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof one);
#endif
  /* Receivers discard multicast DNS whose IP TTL is not 255. */
  int hop = 255;
  setsockopt(fd, IPPROTO_IP, IP_TTL, &hop, sizeof hop);
  setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &hop, sizeof hop);
  struct in_addr iface;
  iface.s_addr = addr_be;
  setsockopt(fd, IPPROTO_IP, IP_MULTICAST_IF, &iface, sizeof iface);
#ifdef SO_BINDTODEVICE
  char ifname[IF_NAMESIZE];
  if (if_indextoname(ifindex, ifname)) {
    setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, ifname, strlen(ifname) + 1);
  }
#endif
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof addr);
  addr.sin_family = AF_INET;
  addr.sin_port = htons(MDNS_PORT);
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  if (bind(fd, (struct sockaddr *)&addr, sizeof addr) != 0) {
    yomu_log("mdns bind failed");
    close(fd);
    return NULL;
  }
  struct ip_mreq mreq;
  memset(&mreq, 0, sizeof mreq);
  mreq.imr_multiaddr.s_addr = htonl(MDNS_GROUP);
  mreq.imr_interface.s_addr = addr_be;
  if (setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof mreq) != 0) {
    yomu_log("mdns join failed");
    close(fd);
    return NULL;
  }
#ifdef IP_RECVTTL
  setsockopt(fd, IPPROTO_IP, IP_RECVTTL, &one, sizeof one);
#endif
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags >= 0) {
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  }
  Mdns *m = calloc(1, sizeof *m);
  if (!m) {
    close(fd);
    return NULL;
  }
  m->fd = fd;
  m->http_port = http_port;
  m->addr = addr_be;
  firewall_open(m);
  send_announce(m, MDNS_TTL);
  int64_t now = yomu_ms();
  if (now > 0) {
    m->announces_left = 1;
    m->next_announce_ms = now + 1000;
  }
  char ip[16];
  if (!inet_ntop(AF_INET, &iface, ip, sizeof ip)) {
    ip[0] = '\0';
  }
  yomu_log("mdns yomu.local via %s port %d", ip, http_port);
  return m;
}

void mdns_stop(Mdns *m) {
  if (!m) {
    return;
  }
  send_announce(m, 0);
  firewall_close(m);
  close(m->fd);
  free(m);
}

int mdns_fd(const Mdns *m) { return m ? m->fd : -1; }

int mdns_announce_wait(const Mdns *m) {
  if (!m || m->announces_left <= 0) {
    return -1;
  }
  int64_t now = yomu_ms();
  if (now <= 0) {
    return -1;
  }
  int64_t d = m->next_announce_ms - now;
  if (d <= 0) {
    return 0;
  }
  if (d > 60000) {
    return 60000;
  }
  return (int)d;
}

void mdns_announce_due(Mdns *m) {
  if (!m || m->announces_left <= 0) {
    return;
  }
  int64_t now = yomu_ms();
  if (now <= 0 || now < m->next_announce_ms) {
    return;
  }
  send_announce(m, MDNS_TTL);
  m->announces_left--;
  m->next_announce_ms = now + 1000;
}

static int packet_ttl(const struct msghdr *msg) {
  if (!msg->msg_control || msg->msg_controllen < sizeof(struct cmsghdr)) {
    return -1;
  }
  unsigned char *base = msg->msg_control;
  size_t len = msg->msg_controllen;
  size_t off = 0;
  while (off + sizeof(struct cmsghdr) <= len) {
    struct cmsghdr *c = (struct cmsghdr *)(base + off);
    size_t clen = (size_t)c->cmsg_len;
    if (clen < sizeof(struct cmsghdr) || clen > len - off) {
      break;
    }
    if (c->cmsg_level == IPPROTO_IP && c->cmsg_type == IP_TTL && clen >= sizeof(struct cmsghdr) + sizeof(int)) {
      int v = 0;
      memcpy(&v, (unsigned char *)(c + 1), sizeof v);
      return v;
    }
    size_t align = sizeof(long);
    size_t step = (clen + align - 1) & ~(align - 1);
    if (step < sizeof(struct cmsghdr) || off + step > len) {
      break;
    }
    off += step;
  }
  return -1;
}

void mdns_on_readable(Mdns *m) {
  if (!m) {
    return;
  }
  for (;;) {
    uint8_t buf[2048];
    uint8_t cbuf[64];
    struct sockaddr_in from;
    struct iovec iov;
    iov.iov_base = buf;
    iov.iov_len = sizeof buf;
    struct msghdr msg;
    memset(&msg, 0, sizeof msg);
    msg.msg_name = &from;
    msg.msg_namelen = sizeof from;
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = cbuf;
    msg.msg_controllen = sizeof cbuf;
    ssize_t n = recvmsg(m->fd, &msg, 0);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      return;
    }
    if (n < 12) {
      continue;
    }
    int ttl = packet_ttl(&msg);
    if (ttl != -1 && ttl != 255) {
      static int logged = 0;
      if (!logged) {
        yomu_log("mdns dropped ttl %d", ttl);
        logged = 1;
      }
      continue;
    }
    uint8_t out[768];
    int unicast = 0;
    int from_port = ntohs(from.sin_port);
    int rn = mdns_reply(buf, (size_t)n, m->addr, m->http_port, from_port, out, sizeof out, &unicast);
    if (rn <= 0) {
      continue;
    }
    if (unicast) {
      mdns_send(m, out, (size_t)rn, &from);
    } else {
      mdns_send(m, out, (size_t)rn, NULL);
    }
  }
}
