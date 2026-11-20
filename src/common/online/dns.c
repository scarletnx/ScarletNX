/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "common/online/dns.h"
#include <stdio.h>
#include <string.h>

enum { HEADER = 12, TYPE_A = 1, CLASS_IN = 1 };

static uint16_t be16(const uint8_t* p) { return (uint16_t)(p[0] << 8 | p[1]); }
static void put16(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

size_t snx_online_dns_query(uint8_t* out, size_t cap, uint16_t id, const char* name) {
    size_t length = name ? strlen(name) : 0, at = HEADER;
    if (length && name[length - 1] == '.') --length; /* a fully qualified name's root */
    if (!length || length > 253 || cap < HEADER + length + 2 + 4) return 0;
    memset(out, 0, HEADER);
    put16(out, id);
    out[2] = 0x01; /* RD */
    put16(out + 4, 1);
    for (size_t start = 0; start <= length;) {
        size_t end = start;
        while (end < length && name[end] != '.') ++end;
        size_t label = end - start;
        if (!label || label > 63) return 0;
        out[at++] = (uint8_t)label;
        memcpy(out + at, name + start, label);
        at += label;
        start = end + 1;
    }
    out[at++] = 0;
    put16(out + at, TYPE_A);
    put16(out + at + 2, CLASS_IN);
    return at + 4;
}

/* the name at *at, its labels as text into name when it is not NULL (pointers followed), *at past it in the message:
   0, or -1 for a name that runs out of the message, loops or overflows */
static int name_read(const uint8_t* m, size_t length, size_t* at, char* name) {
    size_t p = *at, out = 0, end = 0;
    int jumps = 0;
    for (;;) {
        if (p >= length) return -1;
        uint8_t label = m[p];
        if ((label & 0xC0) == 0xC0) {
            if (p + 1 >= length || ++jumps > 16) return -1;
            if (!end) end = p + 2;
            p = (size_t)(label & 0x3F) << 8 | m[p + 1];
            continue;
        }
        if (label & 0xC0) return -1; /* the reserved 01 and 10 kinds */
        if (!label) {
            if (!end) end = p + 1;
            break;
        }
        if (p + 1 + label > length || out + label + 1 > SNX_ONLINE_DNS_NAME - 1) return -1;
        if (name) {
            if (out) name[out++] = '.';
            memcpy(name + out, m + p + 1, label);
        } else if (out)
            ++out;
        out += label;
        p += 1 + label;
    }
    if (name) name[out] = 0;
    *at = end;
    return 0;
}

int snx_online_dns_question(const uint8_t* message, size_t length, char name[SNX_ONLINE_DNS_NAME], uint16_t* type) {
    size_t at = HEADER;
    if (length < HEADER || be16(message + 4) < 1) return -1;
    if (name_read(message, length, &at, name) || at + 4 > length) return -1;
    if (type) *type = be16(message + at);
    return 0;
}

int snx_online_dns_reply_read(const uint8_t* message, size_t length, uint16_t id, uint8_t (*addr)[4], int max,
                              char* why, size_t size) {
    if (length < HEADER) {
        snprintf(why, size, "the reply has %u bytes, fewer than a header's 12", (unsigned)length);
        return -1;
    }
    if (be16(message) != id || !(message[2] & 0x80)) {
        snprintf(why, size, "the message is not the reply to query 0x%04X (id 0x%04X, %s)", id, be16(message),
                 message[2] & 0x80 ? "a reply" : "a query");
        return -1;
    }
    if (message[2] & 0x02) {
        snprintf(why, size, "the reply was cut to fit (TC set), which this resolver does not ask again over TCP");
        return -1;
    }
    unsigned rcode = message[3] & 0x0F;
    if (rcode == 3) return 0; /* NXDOMAIN: no such name */
    if (rcode) {
        static const char* const codes[] = {"", "FORMERR", "SERVFAIL", "NXDOMAIN", "NOTIMP", "REFUSED"};
        snprintf(why, size, "the server answered error %u (%s)", rcode, rcode < 6 ? codes[rcode] : "unassigned");
        return -1;
    }
    unsigned questions = be16(message + 4), answers = be16(message + 6);
    size_t at = HEADER;
    for (unsigned q = 0; q < questions; ++q)
        if (name_read(message, length, &at, NULL) || (at += 4) > length) {
            snprintf(why, size, "the reply's question %u runs past its %u bytes", q + 1, (unsigned)length);
            return -1;
        }
    int count = 0;
    for (unsigned a = 0; a < answers; ++a) {
        if (name_read(message, length, &at, NULL) || at + 10 > length) {
            snprintf(why, size, "the reply's answer %u runs past its %u bytes", a + 1, (unsigned)length);
            return -1;
        }
        uint16_t type = be16(message + at), class_ = be16(message + at + 2), data = be16(message + at + 8);
        at += 10;
        if (at + data > length) {
            snprintf(why, size, "the reply's answer %u holds %u bytes past its %u", a + 1, data, (unsigned)length);
            return -1;
        }
        if (type == TYPE_A && class_ == CLASS_IN && data == 4 && count < max) memcpy(addr[count++], message + at, 4);
        at += data;
    }
    return count;
}

size_t snx_online_dns_reply(uint8_t* out, size_t cap, const uint8_t* query, size_t length, const uint8_t (*addr)[4],
                            int count, uint32_t ttl) {
    size_t at = HEADER;
    if (length < HEADER || be16(query + 4) < 1 || name_read(query, length, &at, NULL) || at + 4 > length) return 0;
    at += 4; /* the question, copied as it came */
    if (count < 0 || cap < at + (size_t)count * 16) return 0;
    memcpy(out, query, at);
    out[2] = (uint8_t)(0x80 | 0x04 | (query[2] & 0x79)); /* QR, AA, the query's opcode and RD */
    out[3] = (uint8_t)(0x80 | (count ? 0 : 3));          /* RA; NXDOMAIN with no address */
    put16(out + 4, 1);
    put16(out + 6, (uint16_t)count);
    put16(out + 8, 0);
    put16(out + 10, 0);
    for (int i = 0; i < count; ++i) {
        uint8_t* r = out + at;
        put16(r, 0xC000 | HEADER); /* the question's name */
        put16(r + 2, TYPE_A);
        put16(r + 4, CLASS_IN);
        put16(r + 6, (uint16_t)(ttl >> 16));
        put16(r + 8, (uint16_t)ttl);
        put16(r + 10, 4);
        memcpy(r + 12, addr[i], 4);
        at += 16;
    }
    return at;
}
