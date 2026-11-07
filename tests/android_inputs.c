/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* What an APK brings, read by the Android driver before any of its code runs: its resources.arsc
   (src/android/res/arsc.c), its binary XML (src/android/res/axml.c) and the names its manifest gives
   (src/android/runtime/install.c). A broken file is refused with its values said, and never read or written past */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L /* setenv */
#endif
#include "android/res/arsc.h"
#include "android/res/axml.h"
#include "android/runtime/install.h"
#include "common/utilities/common.h"
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

/* the manifest's package and split names, which become folders and files of the data folder */
static void names(void) {
    static const struct {
        const char* name;
        bool package, valid;
    } cases[] = {
        {"com.example.app", true, true},
        {"com.engine3d.player_x", true, true},
        {"a.b", true, true},
        {"single", true, false},
        {"single", false, true},
        {"config.arm64_v8a", false, true},
        {"../../../../tmp/evil", true, false},
        {"com.example/../x", true, false},
        {"com..example", true, false},
        {".com.example", true, false},
        {"com.example.", true, false},
        {"1com.example", true, false},
        {"com.1example", true, false},
        {"", true, false},
        {"", false, false},
        {"x/y", false, false},
    };
    for (size_t i = 0; i < sizeof cases / sizeof *cases; ++i) {
        bool valid = android_install_valid_name(cases[i].name, cases[i].package);
        char what[96];
        snprintf(what, sizeof what, "\"%s\" as a %s name is %s", cases[i].name, cases[i].package ? "package" : "split",
                 cases[i].valid ? "taken" : "refused");
        check(valid == cases[i].valid, what, valid ? "taken" : "refused");
    }
}

/* zip's CRC-32 */
static unsigned long crc32_of(const unsigned char* p, size_t n) {
    unsigned long crc = 0xffffffffu;
    for (size_t i = 0; i < n; ++i) {
        crc ^= p[i];
        for (int k = 0; k < 8; ++k) crc = crc & 1 ? (crc >> 1) ^ 0xedb88320u : crc >> 1;
    }
    return crc ^ 0xffffffffu;
}

/* an APK of one file, AndroidManifest.xml, stored: a binary XML manifest tag whose package attribute is package */
static unsigned char* apk(const char* package, size_t* size) {
    /* the string pool: "manifest", "package" and the package, UTF-8 */
    const char* strings[3] = {"manifest", "package", package};
    unsigned char pool[256] = {0};
    size_t at = 28 + 3 * 4;
    for (int i = 0; i < 3; ++i) {
        size_t n = strlen(strings[i]);
        if (n > 100) exit(2);
        put32(pool + 28 + 4 * i, at - 40);
        pool[at] = (unsigned char)n;     /* its length in UTF-16 units */
        pool[at + 1] = (unsigned char)n; /* and in bytes */
        memcpy(pool + at + 2, strings[i], n + 1);
        at += n + 3;
    }
    size_t pool_size = (at + 3) & ~(size_t)3;
    put16(pool, 0x0001);
    put16(pool + 2, 28);
    put32(pool + 4, pool_size);
    put32(pool + 8, 3);
    put32(pool + 16, 0x100);
    put32(pool + 20, 40);
    /* the document: its header, the pool, then the manifest tag with the attribute package (string 1) = string 2 */
    size_t xml_size = 8 + pool_size + 56;
    unsigned char xml[512] = {0};
    put16(xml, 0x0003);
    put16(xml + 2, 8);
    put32(xml + 4, xml_size);
    memcpy(xml + 8, pool, pool_size);
    unsigned char* e = xml + 8 + pool_size;
    put16(e, 0x0102);
    put16(e + 2, 16);
    put32(e + 4, 56);
    put32(e + 16, 0xffffffffu);
    put32(e + 20, 0);
    put16(e + 24, 20);
    put16(e + 26, 20);
    put16(e + 28, 1);
    put32(e + 36, 0xffffffffu);
    put32(e + 40, 1);
    put32(e + 44, 2);
    put16(e + 48, 8);
    e[51] = 0x03; /* a string */
    put32(e + 52, 2);
    /* the zip: a local header and the file, the central directory's entry, its end */
    static const char name[] = "AndroidManifest.xml";
    const size_t name_size = sizeof name - 1;
    unsigned long crc = crc32_of(xml, xml_size);
    *size = 30 + name_size + xml_size + 46 + name_size + 22;
    unsigned char* z = calloc(1, *size);
    if (!z) exit(2);
    put32(z, 0x04034b50u);
    put16(z + 4, 20);
    put32(z + 14, crc);
    put32(z + 18, xml_size);
    put32(z + 22, xml_size);
    put16(z + 26, (unsigned)name_size);
    memcpy(z + 30, name, name_size);
    memcpy(z + 30 + name_size, xml, xml_size);
    unsigned char* c = z + 30 + name_size + xml_size;
    put32(c, 0x02014b50u);
    put16(c + 4, 20);
    put16(c + 6, 20);
    put32(c + 16, crc);
    put32(c + 20, xml_size);
    put32(c + 24, xml_size);
    put16(c + 28, (unsigned)name_size);
    memcpy(c + 46, name, name_size);
    unsigned char* end = c + 46 + name_size;
    put32(end, 0x06054b50u);
    put16(end + 8, 1);
    put16(end + 10, 1);
    put32(end + 12, 46 + name_size);
    put32(end + 16, (unsigned long)(c - z));
    return z;
}

/* an APK whose manifest names the package ../escaped: refused before anything is made of it, where its folders were
   android/escaped of the data folder, out of android/app */
static void install_escape(void) {
    char* tmp = snx_temp_dir(NULL);
    char* data = tmp ? snx_path_join(tmp, "data") : NULL;
    char* path = tmp ? snx_path_join(tmp, "escape.apk") : NULL;
    if (!path || !data || snx_mkdirs(data)) exit(2);
    /* the data folder in the temporary one, on every host */
#ifdef _WIN32
    char* variable = snx_format("APPDATA=%s", data);
    if (!variable || _putenv(variable)) exit(2);
#else
    if (setenv("XDG_DATA_HOME", data, 1) || setenv("HOME", data, 1)) exit(2);
#endif
    char* folder = snx_user_data_dir();
    char* escaped = folder ? snx_path_join(folder, "android/escaped") : NULL;
    free(folder);
    if (!escaped) exit(2);
    size_t size;
    unsigned char* file = apk("../escaped", &size);
    if (snx_write_file(path, file, size)) exit(2);
    AndroidInstall app;
    char error[512] = "";
    bool installed = android_install(path, &app, error, sizeof error);
    if (installed) android_install_free(&app);
    const char* want = "the manifest's package \"../escaped\" is not a package name";
    size_t n = strlen(error), w = strlen(want);
    char got[600];
    snprintf(got, sizeof got, "%s, \"%s\"", installed ? "installed" : "refused", error);
    check(!installed && n >= w && !strcmp(error + n - w, want), "an APK of the package ../escaped is refused", got);
    check(!snx_is_dir(escaped), "no folder is made for the package ../escaped", escaped);
    snx_remove_tree(tmp);
    free(file);
    free(escaped);
    free(path);
    free(data);
    free(tmp);
#ifdef _WIN32
    free(variable);
#endif
}

int main(void) {
    tables();
    documents();
    names();
    install_escape();
    if (failures) {
        fprintf(stderr, "android_inputs: %d checks failed\n", failures);
        return 1;
    }
    puts("android_inputs: ok");
    return 0;
}
