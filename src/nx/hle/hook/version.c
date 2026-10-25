/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Hooks' versions (hook.h): the SDK version a module's middleware label gives, and the entry of a generated table
   whose versions hold a module's. Pure functions of their arguments, so tests/hook_versions.c runs them on labels and
   tables of its own */
#include "nx/hle/hook/hook.h"
#include <stdio.h>

static int letter(u8 ch) { return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z'); }

/* the number at p of at most 255, before end: its digits' count, 0 for none or too large */
static size_t part(const u8* p, const u8* end, u32* v) {
    size_t n = 0;
    *v = 0;
    while (p + n < end && p[n] >= '0' && p[n] <= '9' && n < 3) *v = *v * 10 + (u32)(p[n++] - '0');
    return *v > 255 || (p + n < end && p[n] >= '0' && p[n] <= '9') ? 0 : n;
}

/* "SDK MW+<vendor>+<vendor>SDK_<lib>-x_y_z" at p: its version, 0 when the text at p isn't one */
static u32 label(const u8* p, const u8* end, const char* lib) {
    static const char head[] = "SDK MW+";
    size_t libn = strlen(lib), n;
    const u8* q = p + sizeof head - 1;
    u32 v[3], i;
    if ((size_t)(end - p) < sizeof head - 1 || memcmp(p, head, sizeof head - 1)) return 0;
    for (n = 0; q + n < end && letter(q[n]); n++) {
    }
    if (!n || q + n >= end || q[n] != '+') return 0;
    q += n + 1;
    for (n = 0; q + n < end && letter(q[n]); n++) {
    }
    /* the second vendor part ends in SDK or Sdk, and an underscore follows it */
    if (n < 4 || q + n >= end || q[n] != '_' || q[n - 3] != 'S' || (q[n - 2] != 'D' && q[n - 2] != 'd') ||
        (q[n - 1] != 'K' && q[n - 1] != 'k'))
        return 0;
    q += n + 1;
    if ((size_t)(end - q) < libn + 1 || memcmp(q, lib, libn) || q[libn] != '-') return 0;
    q += libn + 1;
    for (i = 0; i < 3; i++) {
        size_t digits = part(q, end, &v[i]);
        if (!digits) return 0;
        q += digits;
        if (i < 2 && (q >= end || *q++ != '_')) return 0;
    }
    return v[0] << 16 | v[1] << 8 | v[2];
}

u32 hook_label_version(const u8* bytes, size_t size, const char* lib) {
    const u8* end = bytes + size;
    const u8* p = bytes;
    while ((p = (const u8*)memchr(p, 'S', (size_t)(end - p))) != NULL) {
        u32 v = label(p, end, lib);
        if (v) return v;
        p++;
    }
    return 0;
}

int hook_select(const HookTable* t, const char* symbol, const u32 versions[HOOK_KEYS], int* first) {
    u32 lo = 0, hi = t->count, i;
    while (lo < hi) { /* the first entry at or after symbol */
        u32 middle = lo + (hi - lo) / 2;
        if (strcmp(t->symbol[middle], symbol) < 0)
            lo = middle + 1;
        else
            hi = middle;
    }
    *first = lo < t->count && !strcmp(t->symbol[lo], symbol) ? (int)lo : -1;
    if (*first < 0) return -1;
    for (i = lo; i < t->count && !strcmp(t->symbol[i], symbol); i++) {
        u32 v = t->key[i] < HOOK_KEYS ? versions[t->key[i]] : 0;
        if (t->key[i] == HOOK_ANY || (v && v >= t->lo[i] && v < t->hi[i])) return (int)i;
    }
    return -2;
}

void hook_version_text(char* out, size_t n, int key, u32 version) {
    if (!version)
        snprintf(out, n, "unknown");
    else if (key == HOOK_NVN)
        snprintf(out, n, "%u.%u", (unsigned)(version >> 16), (unsigned)(version & 0xFFFF));
    else
        snprintf(out, n, "%u.%u.%u", (unsigned)(version >> 16), (unsigned)(version >> 8 & 255),
                 (unsigned)(version & 255));
}

void hook_range_text(char* out, size_t n, int key, u32 lo, u32 hi) {
    static const char* const names[HOOK_KEYS] = {"any", "sdk", "nvn"};
    char a[32], b[32];
    if (key <= HOOK_ANY || key >= HOOK_KEYS) {
        snprintf(out, n, "any version");
        return;
    }
    hook_version_text(a, sizeof a, key, lo);
    hook_version_text(b, sizeof b, key, hi);
    if (hi == lo + 1)
        snprintf(out, n, "%s == %s", names[key], a);
    else if (hi == 0xFFFFFFFFu)
        snprintf(out, n, "%s >= %s", names[key], lo ? a : "0");
    else if (!lo)
        snprintf(out, n, "%s < %s", names[key], b);
    else
        snprintf(out, n, "%s >= %s, < %s", names[key], a, b);
}
