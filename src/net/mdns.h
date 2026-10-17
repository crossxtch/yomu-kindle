#ifndef YOMU_MDNS_H
#define YOMU_MDNS_H

#include <stddef.h>
#include <stdint.h>

/* Answers yomu.local and the _http._tcp service on UDP 5353.
 * addr_be is the IPv4 address in network byte order. ifindex selects
 * the interface. NULL when the socket cannot be opened.
 */
typedef struct Mdns Mdns;

Mdns *mdns_start(uint32_t addr_be, int http_port, unsigned ifindex);
void mdns_stop(Mdns *m);
int mdns_fd(const Mdns *m);

/* Milliseconds until the next gratuitous announcement, or -1 when none is due. */
int mdns_announce_wait(const Mdns *m);
void mdns_announce_due(Mdns *m);
void mdns_on_readable(Mdns *m);

/* Response length, or 0 when the packet needs no answer.
 * *unicast is 1 when the response must go to the querier instead of the group.
 */
int mdns_reply(const uint8_t *query, size_t query_len, uint32_t addr_be, int http_port, int from_port,
               uint8_t *out, size_t cap, int *unicast);

/* Unsolicited announcement. ttl 0 is the goodbye. Returns the packet length, or 0. */
int mdns_announce(uint32_t addr_be, int http_port, uint32_t ttl, uint8_t *out, size_t cap);

#endif
