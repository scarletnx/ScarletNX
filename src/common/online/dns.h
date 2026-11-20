/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* DNS for IPv4 names (RFC 1035): the messages a resolver sends and reads, the answer a small server gives, and a
   resolver that asks one server, as a console asks the DNS server its connection settings name */
#ifndef SNX_ONLINE_DNS_H
#define SNX_ONLINE_DNS_H
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SNX_ONLINE_DNS_PORT    53u
#define SNX_ONLINE_DNS_MESSAGE 512u /* the most a reply over UDP carries without EDNS, which these never ask for */
#define SNX_ONLINE_DNS_NAME    254u /* a name's text, its NUL included */

/* A query for name's A records, recursion desired, no EDNS (some replacement servers answer EDNS wrongly): its
   length, or 0 when name is not a host name (empty, a label over 63 bytes or empty, over 253 bytes) or cap is short */
size_t snx_online_dns_query(uint8_t* out, size_t cap, uint16_t id, const char* name);

/* The first question of a message: its name as text (lower case kept as sent) and its type. 0, or -1 */
int snx_online_dns_question(const uint8_t* message, size_t length, char name[SNX_ONLINE_DNS_NAME], uint16_t* type);

/* A reply to query id: its A records' addresses, up to max of them, in addr. The count (0 for a name the server has
   no address for), or -1 with why saying what in the message was wrong (another id, cut short, an error code) */
int snx_online_dns_reply_read(const uint8_t* message, size_t length, uint16_t id, uint8_t (*addr)[4], int max,
                              char* why, size_t size);

/* The reply to a query (a message snx_online_dns_question reads) with count A records, ttl seconds each: no such
   name when count is 0. Its length, or 0 when the query is not one or cap is short */
size_t snx_online_dns_reply(uint8_t* out, size_t cap, const uint8_t* query, size_t length, const uint8_t (*addr)[4],
                            int count, uint32_t ttl);

/* Asks the server at server:port (port 0: 53) for name's IPv4 addresses over UDP, waiting up to timeout_ms for the
   reply to this query: the count put in addr (up to max; 0 when the server knows no address), or -1 with why saying
   what failed, with the server, the name and the time */
int snx_online_dns_resolve(const uint8_t server[4], uint16_t port, const char* name, uint8_t (*addr)[4], int max,
                           int timeout_ms, char* why, size_t size);

#ifdef __cplusplus
}
#endif
#endif
