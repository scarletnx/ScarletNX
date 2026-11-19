/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "common/online/nasc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789.-";

size_t snx_online_b64n_encode(char* out, size_t cap, const void* data, size_t length) {
    const uint8_t* in = (const uint8_t*)data;
    size_t need = (length + 2) / 3 * 4, o = 0;
    if (cap < need + 1) return 0;
    for (size_t i = 0; i < length; i += 3) {
        uint32_t v =
            (uint32_t)in[i] << 16 | (i + 1 < length ? (uint32_t)in[i + 1] << 8 : 0) | (i + 2 < length ? in[i + 2] : 0);
        out[o++] = alphabet[v >> 18 & 63];
        out[o++] = alphabet[v >> 12 & 63];
        out[o++] = i + 1 < length ? alphabet[v >> 6 & 63] : '*';
        out[o++] = i + 2 < length ? alphabet[v & 63] : '*';
    }
    out[o] = 0;
    return o;
}

static int sextet(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '.' || c == '+') return 62;
    if (c == '-' || c == '/') return 63;
    return -1;
}

long snx_online_b64n_decode(uint8_t* out, size_t cap, const char* text, size_t length) {
    size_t o = 0;
    while (length && (text[length - 1] == '*' || text[length - 1] == '=')) --length;
    if (length % 4 == 1) return -1;
    uint32_t v = 0;
    int bits = 0;
    for (size_t i = 0; i < length; ++i) {
        int s = sextet(text[i]);
        if (s < 0) return -1;
        v = v << 6 | (uint32_t)s;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (o == cap) return -1;
            out[o++] = (uint8_t)(v >> bits);
        }
    }
    return (long)o;
}

void snx_online_form_start(SnxOnlineForm* f, char* buffer, size_t cap) {
    f->text = buffer;
    f->cap = cap;
    f->length = 0;
    f->full = cap == 0;
    if (cap) buffer[0] = 0;
}

void snx_online_form_put(SnxOnlineForm* f, const char* key, const void* value, size_t length) {
    size_t k = strlen(key), need = (f->length ? 1 : 0) + k + 1 + (length + 2) / 3 * 4 + 1;
    if (f->full || f->length + need > f->cap) {
        f->full = 1;
        return;
    }
    char* at = f->text + f->length;
    if (f->length) *at++ = '&';
    memcpy(at, key, k);
    at += k;
    *at++ = '=';
    at += snx_online_b64n_encode(at, f->cap - (size_t)(at - f->text), value, length);
    f->length = (size_t)(at - f->text);
}

void snx_online_form_text(SnxOnlineForm* f, const char* key, const char* value) {
    snx_online_form_put(f, key, value, strlen(value));
}

/* a form's value with %XX and + undone into out (a NUL after it): its length, or -1 when it does not fit */
static long unescape(const char* in, size_t length, char* out, size_t cap) {
    size_t o = 0;
    for (size_t i = 0; i < length; ++i) {
        char c = in[i];
        if (c == '+')
            c = ' ';
        else if (c == '%' && i + 2 < length) {
            char hex[3] = {in[i + 1], in[i + 2], 0}, *end;
            long v = strtol(hex, &end, 16);
            if (end == hex + 2) {
                c = (char)v;
                i += 2;
            }
        }
        if (o + 1 >= cap) return -1;
        out[o++] = c;
    }
    out[o] = 0;
    return (long)o;
}

/* field's value decoded from the console's base64 into out as text: 0, or -1 */
static int text_field(const char* value, char* out, size_t cap) {
    long n = snx_online_b64n_decode((uint8_t*)out, cap - 1, value, strlen(value));
    if (n < 0) return -1;
    out[n] = 0;
    return (long)strlen(out) == n ? 0 : -1; /* no NUL inside a text */
}

int snx_online_nasc_reply_read(const char* body, size_t length, SnxOnlineNascReply* out, char* why, size_t size) {
    char key[32], value[1024], text[640];
    memset(out, 0, sizeof *out);
    for (size_t at = 0; at < length;) {
        size_t end = at, eq;
        while (end < length && body[end] != '&') ++end;
        for (eq = at; eq < end && body[eq] != '='; ++eq) {}
        size_t key_length = eq - at;
        while (end > at && (body[end - 1] == '\r' || body[end - 1] == '\n')) --end;
        if (key_length && key_length < sizeof key) {
            memcpy(key, body + at, key_length);
            key[key_length] = 0;
            if (eq == end)
                value[0] = 0;
            else if (unescape(body + eq + 1, end - eq - 1, value, sizeof value) < 0) {
                snprintf(why, size, "the reply's %s has %u bytes, more than %u", key, (unsigned)(end - eq - 1),
                         (unsigned)sizeof value - 1);
                return -1;
            }
            int bad = 0;
            if (!strcmp(key, "returncd")) {
                if (!strcmp(value, "null"))
                    strcpy(out->returncd, "null");
                else
                    bad = text_field(value, out->returncd, sizeof out->returncd);
            } else if (!strcmp(key, "retry")) {
                bad = text_field(value, text, sizeof text);
                out->retry = !bad && text[0] == '1';
            } else if (!strcmp(key, "locator")) {
                char* colon;
                bad = text_field(value, text, sizeof text);
                colon = bad ? NULL : strrchr(text, ':');
                long port = colon ? strtol(colon + 1, NULL, 10) : -1;
                if (!colon || colon == text || (size_t)(colon - text) >= sizeof out->host || port < 0 || port > 65535)
                    bad = 1;
                else {
                    memcpy(out->host, text, (size_t)(colon - text));
                    out->host[colon - text] = 0;
                    out->port = (uint16_t)port;
                }
            } else if (!strcmp(key, "token")) {
                if (strlen(value) >= sizeof out->token)
                    bad = 1;
                else
                    strcpy(out->token, value);
            } else if (!strcmp(key, "datetime")) {
                bad = text_field(value, out->datetime, sizeof out->datetime);
            } else if (!strcmp(key, "servicetoken")) {
                if (strlen(value) >= sizeof out->service_token)
                    bad = 1;
                else
                    strcpy(out->service_token, value);
            } else if (!strcmp(key, "svchost")) {
                bad = text_field(value, out->service_host, sizeof out->service_host);
            } else if (!strcmp(key, "statusdata")) {
                bad = text_field(value, text, sizeof text);
                out->status_data = bad ? 0 : (uint8_t)text[0];
            }
            if (bad) {
                snprintf(why, size, "the reply's %s=%.64s is not what NASC sends there", key, value);
                return -1;
            }
        }
        at = end < length ? end + 1 : length;
        while (at < length && (body[at] == '\r' || body[at] == '\n')) ++at;
    }
    if (!out->returncd[0]) {
        snprintf(why, size, "the reply has no returncd: %.80s", length ? body : "(empty)");
        return -1;
    }
    return 0;
}

int snx_online_nasc_result(const SnxOnlineNascReply* reply) {
    char* end;
    long v = strtol(reply->returncd, &end, 10);
    return end == reply->returncd || *end || v < 0 ? -1 : (int)v;
}

uint64_t snx_online_nex_datetime(const char* t) {
    int v[6];
    static const int width[6] = {4, 2, 2, 2, 2, 2}, high[6] = {9999, 12, 31, 23, 59, 59}, low[6] = {0, 1, 1, 0, 0, 0};
    if (!t || strlen(t) != 14) return 0;
    for (int f = 0, at = 0; f < 6; at += width[f++]) {
        v[f] = 0;
        for (int i = 0; i < width[f]; ++i) {
            char c = t[at + i];
            if (c < '0' || c > '9') return 0;
            v[f] = v[f] * 10 + (c - '0');
        }
        if (v[f] < low[f] || v[f] > high[f]) return 0;
    }
    return (uint64_t)v[0] << 26 | (uint64_t)v[1] << 22 | (uint64_t)v[2] << 17 | (uint64_t)v[3] << 12 |
           (uint64_t)v[4] << 6 | (uint64_t)v[5];
}
