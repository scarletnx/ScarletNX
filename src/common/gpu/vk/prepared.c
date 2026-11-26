/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "common/gpu/vk/prepared.h"
#include <stdlib.h>
#include <string.h>
#include <zstd.h>
#include "common/platform/file.h"
#include "common/utilities/common.h"
#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

uint64_t snx_prep_mix(uint64_t h, const void* p, size_t n) {
    const uint8_t* b = p;
    for (size_t i = 0; i + 4 <= n; i += 4) {
        uint32_t w;
        memcpy(&w, b + i, 4);
        h = (h ^ w) * 0x100000001B3ull;
    }
    return h;
}
uint64_t snx_prep_start(uint32_t type, uint32_t size) {
    uint64_t h = 0xCBF29CE484222325ull;
    h = (h ^ type) * 0x100000001B3ull;
    return (h ^ size) * 0x100000001B3ull;
}
uint64_t snx_prep_end(uint64_t h) {
    h ^= h >> 31;
    h *= 0x9E3779B97F4A7C15ull;
    return h ^ (h >> 29);
}
uint64_t snx_prep_check(uint32_t type, const void* p, uint32_t len) {
    return snx_prep_end(snx_prep_mix(snx_prep_start(type, len), p, len));
}

uint64_t snx_prep_head(uint8_t head[SNX_PREP_REC], uint32_t type, const void* const* piece, const size_t* len,
                       int pieces) {
    static const uint8_t zero[8] = {0};
    size_t payload = 0;
    for (int i = 0; i < pieces; ++i) payload += len[i];
    const size_t pad = (8 - payload % 8) % 8;
    const uint32_t size = (uint32_t)(payload + pad);
    uint64_t h = snx_prep_start(type, size);
    for (int i = 0; i < pieces; ++i) h = snx_prep_mix(h, piece[i], len[i]);
    h = snx_prep_end(snx_prep_mix(h, zero, pad));
    memcpy(head, &type, 4);
    memcpy(head + 4, &size, 4);
    memcpy(head + 8, &h, 8);
    return h;
}

bool snx_prep_record(const uint8_t* file, size_t at, size_t size, uint32_t* type, uint32_t* len) {
    uint64_t check;
    if (at > size || size - at < SNX_PREP_REC) return false;
    memcpy(type, file + at, 4);
    memcpy(len, file + at + 4, 4);
    memcpy(&check, file + at + 8, 8);
    return *len % 8 == 0 && *len <= size - at - SNX_PREP_REC &&
           snx_prep_check(*type, file + at + SNX_PREP_REC, *len) == check;
}

uint8_t* snx_prep_header(const char* magic, uint32_t version, const char* key, size_t* size) {
    const size_t n = strlen(key), total = (16 + n + 7) & ~(size_t)7;
    uint8_t* h = calloc(1, total);
    const uint32_t len = (uint32_t)n;
    if (!h) return NULL;
    memcpy(h, magic, 8);
    memcpy(h + 8, &version, 4);
    memcpy(h + 12, &len, 4);
    memcpy(h + 16, key, n);
    *size = total;
    return h;
}

/* a key's fields: name=value words, and the words with no = (the build's name) as one field, "build" */
typedef struct {
    char name[24], value[160];
} KeyField;
static int key_fields(const char* key, size_t len, KeyField* f, int max) {
    int count = 0, build = -1;
    size_t at = 0;
    while (at < len) {
        size_t end = at, eq = 0;
        while (end < len && key[end] != ' ') {
            if (key[end] == '=' && !eq) eq = end;
            end++;
        }
        size_t wlen = end - at;
        if (wlen && key[end - 1] == ';') wlen--; /* the guest part's end */
        if (wlen) {
            if (eq && eq < at + wlen && count < max) {
                size_t nl = eq - at, vl = at + wlen - eq - 1;
                snprintf(f[count].name, sizeof f[count].name, "%.*s", (int)nl, key + at);
                snprintf(f[count].value, sizeof f[count].value, "%.*s", (int)vl, key + eq + 1);
                count++;
            } else if (!eq) {
                if (build < 0 && count < max) {
                    build = count++;
                    snprintf(f[build].name, sizeof f[build].name, "build");
                    f[build].value[0] = 0;
                }
                if (build >= 0) {
                    size_t have = strlen(f[build].value);
                    snprintf(f[build].value + have, sizeof f[build].value - have, "%s%.*s", have ? " " : "", (int)wlen,
                             key + at);
                }
            }
        }
        at = end + 1;
    }
    return count;
}
/* the field of f (count of them) that is the nth (from 0) of that name, NULL for none: a key names "sizes" twice */
static const KeyField* key_field(const KeyField* f, int count, const char* name, int nth) {
    for (int k = 0; k < count; k++)
        if (!strcmp(f[k].name, name) && nth-- == 0) return &f[k];
    return NULL;
}
static int key_nth(const KeyField* f, int i) {
    int nth = 0;
    for (int k = 0; k < i; k++) nth += !strcmp(f[k].name, f[i].name);
    return nth;
}
int snx_prep_key_diff(char* out, size_t n, const char* key, size_t len, const char* want) {
    KeyField was[32], now[32];
    int nw = key_fields(key, len, was, 32), nn = key_fields(want, strlen(want), now, 32), count = 0;
    size_t at = 0;
    if (n) out[0] = 0;
    for (int i = 0; i < nw + nn; i++) {
        const KeyField* a = i < nw ? &was[i] : NULL;
        const KeyField* b = i < nw ? NULL : &now[i - nw];
        const char* name = a ? a->name : b->name;
        const char *in_file = a ? a->value : NULL, *in_now = b ? b->value : NULL;
        if (a) {
            const KeyField* other = key_field(now, nn, name, key_nth(was, i));
            in_now = other ? other->value : NULL;
        }
        if (b && key_field(was, nw, name, key_nth(now, i - nw))) continue; /* compared from the file's side */
        if (in_file && in_now && !strcmp(in_file, in_now)) continue;
        count++;
        if (at + 1 < n) {
            int w = snprintf(out + at, n - at, "%s%s %s in the file, %s now", at ? "; " : "", name,
                             in_file ? in_file : "none", in_now ? in_now : "none");
            at = w < 0 ? n : at + (size_t)w;
            if (at >= n) at = n - 1;
        }
    }
    return count;
}

uint8_t* snx_prep_pack(const void* raw, size_t n, int level, size_t over, size_t* packed, const char** why) {
    const size_t bound = ZSTD_compressBound(n);
    uint8_t* out = malloc(bound + 8);
    *why = NULL;
    if (!out) {
        *why = "no memory";
        return NULL;
    }
    const size_t got = ZSTD_compress(out, bound, raw, n, level);
    if (ZSTD_isError(got)) {
        *why = ZSTD_getErrorName(got);
        free(out);
        return NULL;
    }
    const size_t padded = (got + 7) & ~(size_t)7;
    if (over + padded >= n) { /* a small one: the frame's own bytes outweigh it */
        free(out);
        return NULL;
    }
    memset(out + got, 0, padded - got);
    *packed = got;
    return out;
}

bool snx_prep_unpack(void* raw, size_t n, const void* frame, size_t packed, char* why, size_t why_n) {
    const size_t got = ZSTD_decompress(raw, n, frame, packed);
    if (!ZSTD_isError(got) && got == n) return true;
    if (why_n) {
        if (ZSTD_isError(got))
            snprintf(why, why_n, "zstd: %s", ZSTD_getErrorName(got));
        else
            snprintf(why, why_n, "it unpacks to %zu bytes", got);
    }
    return false;
}

unsigned snx_prep_pid(void) {
#if defined(_WIN32)
    return (unsigned)GetCurrentProcessId();
#else
    return (unsigned)getpid();
#endif
}

int snx_prep_replace(const char* path, bool (*write)(FILE* f, void* ctx), void (*before)(void* ctx), void* ctx,
                     char* why, size_t why_n) {
    char* tmp = snx_format("%s.%u.tmp", path, snx_prep_pid());
    FILE* f = tmp ? fopen(tmp, "wb") : NULL;
    bool ok = f && write(f, ctx);
    if (!ok) snx_os_error(why, why_n); /* each failure's reason where it happened, before the cleanup's calls */
    if (f && fclose(f) && ok) {
        snx_os_error(why, why_n);
        ok = false;
    }
    if (before) before(ctx);
    if (ok && plat_replace_file(tmp, path)) {
        snx_os_error(why, why_n);
        ok = false;
    }
    if (!ok && tmp) remove(tmp);
    free(tmp);
    return ok ? 0 : -1;
}

