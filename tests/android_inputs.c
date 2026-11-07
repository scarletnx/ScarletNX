/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* What an APK brings, read by the Android driver before any of its code runs: its resources.arsc
   (src/android/res/arsc.c) and binary XML (src/android/res/axml.c). A broken file is refused with its values said, and
   never read or written past */
#include "android/res/arsc.h"
#include "android/res/axml.h"
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

/* a binary XML document: a resource map chunk of map_size bytes whose header says map_header, with the id 0x01010003
   past that header when it fits, then a start tag with one attribute whose name is string name. In a buffer of its
   exact size */
static unsigned char* document(unsigned map_header, unsigned map_size, unsigned long name, size_t* size) {
    *size = 8 + map_size + 56;
    unsigned char* d = calloc(1, *size);
    if (!d) exit(2);
    put16(d, 0x0003);
    put16(d + 2, 8);
    put32(d + 4, *size);
    unsigned char* m = d + 8;
    put16(m, 0x0180);
    put16(m + 2, map_header);
    put32(m + 4, map_size);
    if (map_header + 4 <= map_size) put32(m + map_header, 0x01010003);
    /* the start tag: its 16-byte header, 20 bytes of its own (no namespace, name 0, one attribute of 20 bytes at 20),
       then the attribute (no namespace, its name) */
    unsigned char* e = m + map_size;
    put16(e, 0x0102);
    put16(e + 2, 16);
    put32(e + 4, 56);
    put32(e + 16, 0xffffffffu);
    put16(e + 24, 20);
    put16(e + 26, 20);
    put16(e + 28, 1);
    put32(e + 36, 0xffffffffu);
    put32(e + 40, name);
    return d;
}

static void documents(void) {
    char error[128];
    char got[160];
    size_t size;
    AxmlDocument doc;

    /* a resource map whose header is past its chunk: the count of its ids wrapped, and an attribute's resource was read
       from past the document */
    unsigned char* d = document(64, 8, 1000, &size);
    error[0] = 0;
    bool ok = axml_open(&doc, d, size, error, sizeof error);
    snprintf(got, sizeof got, "%s, \"%s\"", ok ? "read" : "refused", error);
    check(!ok && !strcmp(error, "a resource map whose header of 64 bytes is past its chunk of 8"),
          "a resource map with a header of 64 bytes in a chunk of 8 is refused with both sizes", got);
    free(d);

    /* a map of one id still gives the attribute its resource */
    d = document(8, 12, 0, &size);
    error[0] = 0;
    ok = axml_open(&doc, d, size, error, sizeof error);
    uint32_t id = 0;
    if (ok) {
        AxmlParser p;
        axml_parser(&p, &doc);
        int event;
        while ((event = axml_next(&p)) != AXML_START_TAG && event != AXML_END_DOCUMENT) {}
        if (event == AXML_START_TAG) id = axml_attribute_resource(&p, 0);
    }
    snprintf(got, sizeof got, "%s, \"%s\", 0x%lx", ok ? "read" : "refused", error, (unsigned long)id);
    check(ok && id == 0x01010003, "a resource map of one id gives the attribute resource 0x01010003", got);
    free(d);
}

int main(void) {
    tables();
    documents();
    if (failures) {
        fprintf(stderr, "android_inputs: %d checks failed\n", failures);
        return 1;
    }
    puts("android_inputs: ok");
    return 0;
}
