#include "android_ifaddrs.h"

#if defined(__ANDROID__) && __ANDROID_API__ < 24

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

void freeifaddrs(struct ifaddrs *list) {
  while (list) {
    struct ifaddrs *next = list->ifa_next;
    free(list->ifa_name);
    free(list->ifa_addr);
    free(list->ifa_netmask);
    free(list);
    list = next;
  }
}

int getifaddrs(struct ifaddrs **out) {
  struct ifconf ifc;
  struct ifreq *ifr;
  struct ifreq *end;
  char buf[8192];
  int fd;
  if (!out) {
    errno = EINVAL;
    return -1;
  }
  *out = NULL;
  fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) {
    return -1;
  }
  memset(&ifc, 0, sizeof ifc);
  memset(buf, 0, sizeof buf);
  ifc.ifc_len = (int)sizeof buf;
  ifc.ifc_buf = buf;
  if (ioctl(fd, SIOCGIFCONF, &ifc) != 0) {
    close(fd);
    return -1;
  }
  ifr = ifc.ifc_req;
  end = (struct ifreq *)((char *)ifc.ifc_buf + ifc.ifc_len);
  struct ifaddrs *head = NULL;
  struct ifaddrs **tail = &head;
  while (ifr < end) {
    struct ifreq flags;
    struct ifaddrs *ifa;
    memset(&flags, 0, sizeof flags);
    snprintf(flags.ifr_name, sizeof flags.ifr_name, "%s", ifr->ifr_name);
    if (ioctl(fd, SIOCGIFFLAGS, &flags) != 0) {
      ifr++;
      continue;
    }
    ifa = calloc(1, sizeof *ifa);
    if (!ifa) {
      freeifaddrs(head);
      close(fd);
      return -1;
    }
    ifa->ifa_name = strdup(ifr->ifr_name);
    ifa->ifa_addr = malloc(sizeof(struct sockaddr));
    if (!ifa->ifa_name || !ifa->ifa_addr) {
      free(ifa->ifa_name);
      free(ifa->ifa_addr);
      free(ifa);
      freeifaddrs(head);
      close(fd);
      return -1;
    }
    memcpy(ifa->ifa_addr, &ifr->ifr_addr, sizeof(struct sockaddr));
    ifa->ifa_flags = flags.ifr_flags;
    *tail = ifa;
    tail = &ifa->ifa_next;
    ifr++;
  }
  close(fd);
  *out = head;
  return 0;
}

unsigned if_nametoindex(const char *name) {
  struct ifreq ifr;
  int fd;
  unsigned index;
  if (!name || !name[0]) {
    return 0;
  }
  fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) {
    return 0;
  }
  memset(&ifr, 0, sizeof ifr);
  snprintf(ifr.ifr_name, sizeof ifr.ifr_name, "%s", name);
  if (ioctl(fd, SIOCGIFINDEX, &ifr) != 0) {
    close(fd);
    return 0;
  }
  index = (unsigned)ifr.ifr_ifindex;
  close(fd);
  return index;
}

char *if_indextoname(unsigned index, char *name) {
  struct ifreq ifr;
  int fd;
  if (!name || index == 0) {
    return NULL;
  }
  fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) {
    return NULL;
  }
  memset(&ifr, 0, sizeof ifr);
  ifr.ifr_ifindex = (int)index;
  if (ioctl(fd, SIOCGIFNAME, &ifr) != 0) {
    close(fd);
    return NULL;
  }
  snprintf(name, IF_NAMESIZE, "%s", ifr.ifr_name);
  close(fd);
  return name;
}

#endif
