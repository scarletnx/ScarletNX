/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "gui/jdk.h"
#include "common/utilities/common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
static const char jvm_path[] = "bin/server/jvm.dll";
#elif defined(__APPLE__)
static const char jvm_path[] = "lib/server/libjvm.dylib";
#else
static const char jvm_path[] = "lib/server/libjvm.so";
#endif

int gui_jdk_version(const char* home) {
    char *jvm = snx_path_join(home, jvm_path), *release = snx_path_join(home, "release"), *text = NULL, *at;
    size_t size;
    int major = 0;
    if (jvm && release && snx_is_file(jvm)) text = (char*)snx_read_file(release, &size);
    for (at = text; at && (at = strstr(at, "JAVA_VERSION=\"")) != NULL && !major; ++at) {
        if (at > text && at[-1] != '\n') continue; /* another key that ends with it */
        at += strlen("JAVA_VERSION=\"");
        major = atoi(at);
        if (major == 1 && at[1] == '.') major = atoi(at + 2); /* 1.8.0_402 is 8 */
    }
    free(text);
    free(release);
    free(jvm);
    return major;
}
typedef struct {
    char** at;
    size_t count, size;
} Jdks;
/* path kept where it is a JDK of GUI_JDK_LEAST or later and is none of those kept already */
static void keep(Jdks* j, const char* path) {
    if (gui_jdk_version(path) < GUI_JDK_LEAST) return;
    for (size_t i = 0; i < j->count; ++i)
        if (snx_same_file(j->at[i], path)) return;
    if (j->count == j->size) {
        size_t size = j->size ? 2 * j->size : 8;
        char** grown = (char**)realloc(j->at, size * sizeof *grown);
        if (!grown) return;
        j->at = grown;
        j->size = size;
    }
    j->at[j->count] = snx_strdup(path);
    if (j->at[j->count]) ++j->count;
}
static int by_name(const void* a, const void* b) {
    return strcmp(*(char* const*)a, *(char* const*)b);
}
char** gui_jdks(const char* const* roots, size_t* count) {
    Jdks j = {0};
    *count = 0;
    for (; *roots; ++roots) {
        char** names = NULL;
        size_t n = 0;
        keep(&j, *roots);
        if (snx_list_dir(*roots, &names, &n)) continue; /* a root this machine does not have */
        if (n) qsort(names, n, sizeof *names, by_name); /* an empty folder has no list at all */
        for (size_t i = 0; i < n; ++i) {
            char *path = snx_path_join(*roots, names[i]), *home = path ? snx_path_join(path, "Contents/Home") : NULL;
            if (path) keep(&j, path);
            if (home) keep(&j, home);
            free(home);
            free(path);
        }
        snx_free_strings(names, n);
    }
    if (j.count) qsort(j.at, j.count, sizeof *j.at, by_name);
    *count = j.count;
    return j.at;
}
char** gui_jdk_roots(const char* home, size_t* count) {
    static const char* const system_roots[] = {
#if defined(_WIN32)
        "C:/Program Files/Java",
        "C:/Program Files/Eclipse Adoptium",
        "C:/Program Files/Microsoft",
        "C:/Program Files/Zulu",
        "C:/Program Files/Amazon Corretto",
        "C:/Program Files/BellSoft",
#elif defined(__APPLE__)
        "/Library/Java/JavaVirtualMachines",
#else
        "/usr/lib/jvm",   "/usr/lib64/jvm", "/usr/java", "/usr/local",     "/opt",
        "/usr/lib/jdks",  "/usr/share/java-jdk", "/opt/java",
#endif
        NULL};
    static const char* const home_roots[] = {".sdkman/candidates/java", ".jdks", ".asdf/installs/java",
                                             ".local/share/mise/installs/java",
#if defined(__APPLE__)
                                             "Library/Java/JavaVirtualMachines",
#endif
                                             NULL};
    char** roots = (char**)calloc(sizeof system_roots / sizeof *system_roots + sizeof home_roots / sizeof *home_roots,
                                  sizeof *roots);
    size_t n = 0;
    *count = 0;
    if (!roots) return NULL;
    for (int i = 0; system_roots[i]; ++i)
        if ((roots[n] = snx_strdup(system_roots[i])) != NULL) ++n;
    for (int i = 0; home && *home && home_roots[i]; ++i)
        if ((roots[n] = snx_path_join(home, home_roots[i])) != NULL) ++n;
    *count = n;
    return roots;
}
void gui_jdks_free(char** jdks, size_t count) { snx_free_strings(jdks, count); }
