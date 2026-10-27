#pragma once

/* Bionic gained getifaddrs, if_nametoindex, and if_indextoname at API 24.
 * The DPT-RP1 is Android 5.1, API 22. This header is the same struct and
 * the same three calls, implemented with SIOCGIFCONF for that system.
 * Include it instead of <ifaddrs.h> when __ANDROID_API__ is below 24.
 */

#include <net/if.h>
#include <sys/socket.h>

#if defined(__ANDROID__) && __ANDROID_API__ < 24

struct ifaddrs {
  struct ifaddrs *ifa_next;
  char *ifa_name;
  unsigned int ifa_flags;
  struct sockaddr *ifa_addr;
  struct sockaddr *ifa_netmask;
  struct sockaddr *ifa_broadaddr;
  void *ifa_data;
};

int getifaddrs(struct ifaddrs **list);
void freeifaddrs(struct ifaddrs *list);
unsigned if_nametoindex(const char *name);
char *if_indextoname(unsigned index, char *name);

#endif
