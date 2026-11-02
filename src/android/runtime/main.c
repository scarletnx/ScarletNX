/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "android/runtime/main.h"
#include "android/dex/translate.h"
#include "common/utilities/common.h"
#include "common/utilities/zip.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char usage[] =
    "Usage: scarletnx-android [options] APK\n"
    "Runs an Android app: its DEX code translated to JVM classes, with ScarletNX's own Android framework.\n"
    "\n"
    "  --translate=DIR   Translate every class of the APK ahead of time into DIR, a .class file for each, and stop\n";

typedef struct {
    unsigned warnings;
} Translation;
static void warn(void* context, const char* message) {
    Translation* t = context;
    if (t->warnings++ < 50) fprintf(stderr, "scarletnx-android: %s\n", message);
}

/* every class of every DEX file of the APK (classes.dex, classes2.dex and on) into folder */
static int translate_apk(const char* apk, const char* folder) {
    char error[512];
    SnxZip* zip = snx_zip_open(apk, error, sizeof error);
    if (!zip) {
        fprintf(stderr, "scarletnx-android: %s\n", error);
        return 1;
    }
    /* the DEX files, each read whole */
    enum { MAX_DEX = 100 };
    Dex dexes[MAX_DEX];
    unsigned char* bytes[MAX_DEX];
    uint32_t count = 0;
    int status = 0;
    for (unsigned n = 1; n <= MAX_DEX; ++n) {
        char name[32];
        if (n == 1)
            snprintf(name, sizeof name, "classes.dex");
        else
            snprintf(name, sizeof name, "classes%u.dex", n);
        uint32_t index;
        if (!snx_zip_find(zip, name, &index)) break;
        size_t size;
        bytes[count] = snx_zip_read(zip, index, &size, error, sizeof error);
        if (!bytes[count] || !dex_open(&dexes[count], bytes[count], size, error, sizeof error)) {
            fprintf(stderr, "scarletnx-android: %s: %s\n", name, error);
            free(bytes[count]);
            status = 1;
            break;
        }
        ++count;
    }
    snx_zip_close(zip);
    Translation t = {0};
    DexTranslateOptions options = {NULL, NULL, warn};
    options.context = &t;
    DexProgram* program = status ? NULL : dex_program_open(dexes, count, &options, error, sizeof error);
    if (!status && !program) {
        fprintf(stderr, "scarletnx-android: %s\n", error);
        status = 1;
    }
    unsigned classes = 0, failures = 0;
    for (uint32_t d = 0; program && d < count; ++d)
        for (uint32_t i = 0; i < dexes[d].class_count; ++i) {
            char class_name[1024];
            size_t class_size;
            unsigned char* file = NULL;
            if (dex_class_name(&dexes[d], i, class_name, sizeof class_name))
                file = dex_translate_class(program, d, i, &class_size, error, sizeof error);
            else
                snprintf(error, sizeof error, "class def %u has a bad name", (unsigned)i);
            if (!file) {
                fprintf(stderr, "scarletnx-android: %s\n", error);
                ++failures;
                continue;
            }
            size_t length = strlen(class_name);
            char* relative = malloc(length + 7);
            char* path = NULL;
            if (relative) {
                memcpy(relative, class_name, length);
                memcpy(relative + length, ".class", 7);
                path = snx_path_join(folder, relative);
            }
            char* parent = path ? snx_dirname(path) : NULL;
            if (!parent || snx_mkdirs(parent) || snx_write_file(path, file, class_size)) {
                fprintf(stderr, "scarletnx-android: cannot write %s\n", path ? path : class_name);
                ++failures;
            } else {
                ++classes;
            }
            free(parent);
            free(path);
            free(relative);
            free(file);
        }
    dex_program_close(program);
    for (uint32_t d = 0; d < count; ++d) {
        dex_close(&dexes[d]);
        free(bytes[d]);
    }
    if (status) return status;
    printf("%u classes translated, %u failed, %u methods that throw VerifyError\n", classes, failures, t.warnings);
    return failures ? 1 : 0;
}

int snx_android_main(int argc, char** argv) {
    const char *apk = NULL, *translate = NULL;
    for (int i = 1; i < argc; ++i) {
        if (!strncmp(argv[i], "--translate=", 12))
            translate = argv[i] + 12;
        else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            fputs(usage, stdout);
            return 0;
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "scarletnx-android: unknown option %s\n%s", argv[i], usage);
            return 2;
        } else {
            apk = argv[i];
        }
    }
    if (!apk) {
        fputs(usage, stderr);
        return 2;
    }
    if (translate) return translate_apk(apk, translate);
    fprintf(stderr, "scarletnx-android: running an app is not implemented yet; --translate=DIR translates it\n");
    return 1;
}
