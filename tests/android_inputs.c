/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* What an APK brings, read by the Android driver before any of its code runs: a resources.arsc (src/android/res/arsc.c).
   A broken file is refused with its values said, and never read or written past */
#include "android/res/arsc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

static void check(int ok, const char* what, const char* got) {
    if (ok) return;
    fprintf(stderr, "android_inputs: %s (got: %s)\n", what, got);
    ++failures;
}

static void put16(unsigned char* p, unsigned v) {
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
}
static void put32(unsigned char* p, unsigned long v) {
    put16(p, (unsigned)(v & 0xffff));
    put16(p + 2, (unsigned)(v >> 16));
}

/* a resource table's header: its chunk's size and its count of packages, then size - 12 zero bytes; in a buffer of
   exactly size bytes, so a sanitizer sees any access past it */
static unsigned char* table(size_t size, unsigned long chunk, unsigned long packages) {
    unsigned char* t = calloc(1, size);
    if (!t) exit(2);
    put16(t, 0x0002);
    put16(t + 2, 12);
    put32(t + 4, chunk);
    put32(t + 8, packages);
    return t;
}

static void tables(void) {
    char error[128];
    char got[160];

    /* 2^32 - 1 packages: the count's + 1 is 0 in 32 bits, and the package chunk was read into no room */
    enum { PACKAGE = 288, SIZE = 12 + PACKAGE };
    unsigned char* t = table(SIZE, SIZE, 0xffffffffu);
    put16(t + 12, 0x0200);
    put16(t + 14, 284);
    put32(t + 16, PACKAGE);
    ArscAssets* a = arsc_create();
    error[0] = 0;
    int32_t cookie = arsc_add(a, t, SIZE, error, sizeof error);
    snprintf(got, sizeof got, "%d, \"%s\"", (int)cookie, error);
    check(cookie == 0 && !strcmp(error, "a resource table of 300 bytes that names 4294967295 packages"),
          "a table of 300 bytes naming 4294967295 packages is refused with both numbers", got);
    arsc_destroy(a);
    free(t);

    /* a chunk that says it is 2 bytes, inside a buffer of 12: its header is read from those 2 */
    t = table(12, 2, 0);
    a = arsc_create();
    error[0] = 0;
    cookie = arsc_add(a, t, 12, error, sizeof error);
    snprintf(got, sizeof got, "%d, \"%s\"", (int)cookie, error);
    check(cookie == 0 && !strcmp(error, "not a resource table"), "a table chunk of 2 bytes is refused", got);
    arsc_destroy(a);
    free(t);

    /* the smallest table, its header alone and no package, is still one */
    t = table(12, 12, 0);
    a = arsc_create();
    error[0] = 0;
    cookie = arsc_add(a, t, 12, error, sizeof error);
    snprintf(got, sizeof got, "%d, \"%s\"", (int)cookie, error);
    check(cookie == 1, "a table of 12 bytes with no package is read", got);
    arsc_destroy(a);
    free(t);
}

int main(void) {
    tables();
    if (failures) {
        fprintf(stderr, "android_inputs: %d checks failed\n", failures);
        return 1;
    }
    puts("android_inputs: ok");
    return 0;
}
