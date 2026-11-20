/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* DNS messages: the query's bytes, a reply read back, and the replies a resolver must refuse */
#include "common/online/dns.h"
#include "online_check.h"
#include <stdint.h>

static void query(void) {
    static const uint8_t want[] = {0x12, 0x34, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 3,
                                   'n',  'a',  's',  12,   'n',  'i',  'n',  't',  'e',  'n',  'd',  'o',  'w',
                                   'i',  'f',  'i',  3,    'n',  'e',  't',  0,    0x00, 0x01, 0x00, 0x01};
    uint8_t out[SNX_ONLINE_DNS_MESSAGE];
    CHECK_INT(snx_online_dns_query(out, sizeof out, 0x1234, "nas.nintendowifi.net"), sizeof want);
    CHECK(!memcmp(out, want, sizeof want));
    CHECK_INT(snx_online_dns_query(out, sizeof out, 0x1234, "nas.nintendowifi.net."), sizeof want); /* the root's dot */
    CHECK_INT(snx_online_dns_query(out, sizeof out, 1, ""), 0);
    CHECK_INT(snx_online_dns_query(out, sizeof out, 1, "a..b"), 0);
    CHECK_INT(snx_online_dns_query(out, sizeof out, 1, ".a"), 0);
    char long_label[70], long_name[300];
    memset(long_label, 'a', 64);
    long_label[64] = 0;
    CHECK_INT(snx_online_dns_query(out, sizeof out, 1, long_label), 0);
    for (int i = 0; i < 254; ++i) long_name[i] = i % 10 == 9 ? '.' : 'b';
    long_name[254] = 0;
    CHECK_INT(snx_online_dns_query(out, sizeof out, 1, long_name), 0);
    CHECK_INT(snx_online_dns_query(out, 20, 1, "nas.nintendowifi.net"), 0); /* a buffer too short */
}

static void reply(void) {
    uint8_t q[SNX_ONLINE_DNS_MESSAGE], r[SNX_ONLINE_DNS_MESSAGE], addr[4][4];
    const uint8_t two[2][4] = {{178, 62, 43, 212}, {10, 0, 0, 1}};
    char name[SNX_ONLINE_DNS_NAME], why[256] = "";
    uint16_t type = 0;
    size_t ql = snx_online_dns_query(q, sizeof q, 0xBEEF, "conntest.nintendowifi.net");
    CHECK_INT(snx_online_dns_question(q, ql, name, &type), 0);
    CHECK_STR(name, "conntest.nintendowifi.net");
    CHECK_INT(type, 1);
    size_t rl = snx_online_dns_reply(r, sizeof r, q, ql, two, 2, 300);
    CHECK_INT(rl, ql + 32);
    CHECK_INT(snx_online_dns_reply_read(r, rl, 0xBEEF, addr, 4, why, sizeof why), 2);
    CHECK(!memcmp(addr[0], two[0], 4) && !memcmp(addr[1], two[1], 4));
    CHECK_INT(snx_online_dns_reply_read(r, rl, 0xBEEF, addr, 1, why, sizeof why), 1); /* max respected */
    CHECK_INT(snx_online_dns_reply_read(r, rl, 0xBEEE, addr, 4, why, sizeof why), -1);
    CHECK_HAS(why, "query 0xBEEE (id 0xBEEF");
    CHECK_INT(snx_online_dns_reply_read(q, ql, 0xBEEF, addr, 4, why, sizeof why), -1); /* a query is no reply */
    CHECK_HAS(why, "a query");
    CHECK_INT(snx_online_dns_reply_read(r, rl - 3, 0xBEEF, addr, 4, why, sizeof why), -1);
    CHECK_HAS(why, "answer 2");
    CHECK_INT(snx_online_dns_reply_read(r, 5, 0xBEEF, addr, 4, why, sizeof why), -1);
    CHECK_HAS(why, "has 5 bytes");
    /* no address: NXDOMAIN, no error */
    rl = snx_online_dns_reply(r, sizeof r, q, ql, NULL, 0, 300);
    CHECK_INT(r[3] & 15, 3);
    CHECK_INT(snx_online_dns_reply_read(r, rl, 0xBEEF, addr, 4, why, sizeof why), 0);
    /* a server error says its code */
    r[3] = 0x82;
    CHECK_INT(snx_online_dns_reply_read(r, rl, 0xBEEF, addr, 4, why, sizeof why), -1);
    CHECK_HAS(why, "error 2 (SERVFAIL)");
    r[3] = 0x80;
    r[2] |= 0x02;
    CHECK_INT(snx_online_dns_reply_read(r, rl, 0xBEEF, addr, 4, why, sizeof why), -1);
    CHECK_HAS(why, "TC set");
}

/* a reply as a recursive server sends one: a CNAME first, then the A of its target, the names compressed */
static void cname(void) {
    static const uint8_t m[] = {
        0x00, 0x07, 0x81, 0x80, 0x00, 0x01, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 3, 'w', 'w', 'w', 7, 'e', 'x', 'a', 'm',
        'p', 'l', 'e', 3, 'c', 'o', 'm', 0, 0x00, 0x01, 0x00, 0x01,
        /* www.example.com CNAME a.example.com */
        0xC0, 0x0C, 0x00, 0x05, 0x00, 0x01, 0x00, 0x00, 0x00, 0x3C, 0x00, 0x04, 1, 'a', 0xC0, 0x10,
        /* a.example.com A 93.184.216.34 */
        0xC0, 0x2D, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x3C, 0x00, 0x04, 93, 184, 216, 34};
    uint8_t addr[2][4];
    char why[256] = "", name[SNX_ONLINE_DNS_NAME];
    CHECK_INT(snx_online_dns_reply_read(m, sizeof m, 7, addr, 2, why, sizeof why), 1);
    CHECK(addr[0][0] == 93 && addr[0][1] == 184 && addr[0][2] == 216 && addr[0][3] == 34);
    CHECK_INT(snx_online_dns_question(m, sizeof m, name, NULL), 0);
    CHECK_STR(name, "www.example.com");
    /* a pointer to itself: refused, not followed for ever */
    uint8_t loop[sizeof m];
    memcpy(loop, m, sizeof m);
    loop[12] = 0xC0, loop[13] = 0x0C;
    CHECK_INT(snx_online_dns_question(loop, sizeof loop, name, NULL), -1);
    CHECK_INT(snx_online_dns_reply_read(loop, sizeof loop, 7, addr, 2, why, sizeof why), -1);
}

int main(void) {
    query();
    reply();
    cname();
    CHECK_DONE();
}
