/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "common/online/dns.h"
#include "common/online/wfc.h"
#include "common/online/socket.h"
#include <stdio.h>
#include <string.h>

/* a query id no reply forged without seeing the query is likely to carry: the clock's and a counter's bits */
static uint16_t next_id(void) {
    static uint32_t counter;
    uint64_t t = snx_online_clock_ms();
    uint32_t x = (uint32_t)(t ^ t >> 17) * 2654435761u + ++counter * 40503u;
    return (uint16_t)(x >> 16 ^ x);
}

int snx_online_dns_resolve(const uint8_t server[4], uint16_t port, const char* name, uint8_t (*addr)[4], int max,
                           int timeout_ms, char* why, size_t size) {
    uint8_t query[SNX_ONLINE_DNS_MESSAGE], reply[SNX_ONLINE_DNS_MESSAGE];
    char what[192];
    uint16_t id = next_id();
    size_t length = snx_online_dns_query(query, sizeof query, id, name);
    if (!port) port = SNX_ONLINE_DNS_PORT;
    if (!length) {
        snprintf(why, size, "\"%s\" is not a host name (labels of 1 to 63 bytes, 253 in all)", name ? name : "");
        return -1;
    }
    SnxOnlineSocket s;
    if (snx_online_udp_connect(&s, server, port, what, sizeof what) ||
        snx_online_udp_send(&s, query, length, what, sizeof what)) {
        snprintf(why, size, "asking %u.%u.%u.%u:%u for %s: %s", server[0], server[1], server[2], server[3], port, name,
                 what);
        return -1;
    }
    uint64_t start = snx_online_clock_ms(), end = start + (uint64_t)(timeout_ms > 0 ? timeout_ms : 0);
    int result = -1;
    snprintf(what, sizeof what, "no reply in %d ms", timeout_ms);
    for (uint64_t now = start; now <= end; now = snx_online_clock_ms()) {
        int got = snx_online_udp_receive(&s, reply, sizeof reply, (int)(end - now), what, sizeof what);
        if (got <= 0) {
            if (!got) snprintf(what, sizeof what, "no reply in %d ms", timeout_ms);
            break;
        }
        /* a datagram that is not this query's reply (a late one of an earlier query) is passed over */
        if ((size_t)got >= 2 && (reply[0] << 8 | reply[1]) != id) continue;
        result = snx_online_dns_reply_read(reply, (size_t)got, id, addr, max, what, sizeof what);
        break;
    }
    snx_online_socket_close(&s);
    if (result < 0)
        snprintf(why, size, "asking %u.%u.%u.%u:%u for %s: %s", server[0], server[1], server[2], server[3], port, name,
                 what);
    return result;
}

int snx_online_wfc_fastest(const uint8_t (*servers)[4], const uint16_t* ports, size_t count, int timeout_ms, int* ms,
                           char* why, size_t size) {
    enum { MOST = 32 };
    SnxOnlineSocket s[MOST];
    uint16_t ids[MOST];
    size_t open = 0, waiting = 0;
    char what[192] = "";
    if (count > MOST) count = MOST;
    uint64_t start = snx_online_clock_ms(), end = start + (uint64_t)(timeout_ms > 0 ? timeout_ms : 0);
    for (size_t i = 0; i < count; ++i) {
        uint8_t query[SNX_ONLINE_DNS_MESSAGE];
        uint16_t port = ports && ports[i] ? ports[i] : SNX_ONLINE_DNS_PORT;
        ms[i] = -1;
        ids[i] = next_id();
        size_t length = snx_online_dns_query(query, sizeof query, ids[i], "nas.nintendowifi.net");
        if (snx_online_udp_connect(&s[open], servers[i], port, what, sizeof what)) {
            ms[i] = -2; /* no socket: never asked */
            continue;
        }
        if (snx_online_udp_send(&s[open], query, length, what, sizeof what)) {
            snx_online_socket_close(&s[open]);
            ms[i] = -2;
            continue;
        }
        ++open;
        ++waiting;
    }
    /* s[k] is the k-th server that was asked: its index among all */
    size_t index[MOST];
    for (size_t i = 0, k = 0; i < count; ++i)
        if (ms[i] != -2) index[k++] = i;
    for (uint64_t now = start; waiting && now < end; now = snx_online_clock_ms()) {
        int k = snx_online_udp_wait_any(s, open, (int)(end - now), what, sizeof what);
        if (k < 0) break;
        uint8_t reply[SNX_ONLINE_DNS_MESSAGE], addr[1][4];
        int got = snx_online_udp_receive(&s[k], reply, sizeof reply, 0, what, sizeof what);
        size_t i = index[k];
        if (ms[i] != -1) continue; /* answered, or its port closed, already */
        if (got < 0) {             /* its port closed: no answer will come */
            ms[i] = -3;
            --waiting;
            continue;
        }
        if (!got) continue;
        char ignored[64];
        if (snx_online_dns_reply_read(reply, (size_t)got, ids[i], addr, 1, ignored, sizeof ignored) > 0) {
            ms[i] = (int)(snx_online_clock_ms() - start);
            --waiting;
        }
    }
    for (size_t k = 0; k < open; ++k) snx_online_socket_close(&s[k]);
    int best = -1;
    for (size_t i = 0; i < count; ++i) {
        if (ms[i] < -1) ms[i] = -1;
        if (ms[i] >= 0 && (best < 0 || ms[i] < ms[best])) best = (int)i;
    }
    if (best < 0)
        snprintf(why, size, "none of the %u servers gave an address for nas.nintendowifi.net within %d ms%s%s",
                 (unsigned)count, timeout_ms, what[0] ? ": " : "", what);
    return best;
}
