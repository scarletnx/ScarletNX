/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The JDKs Settings > Launch > Java home lists (src/gui/jdk.c): a folder with a JVM to load and a release file of 21 or
   later, in a root, under a Contents/Home, or the root itself; none older, none without its JVM, a link's once; the
   roots, the home's after the system's */
#include "common/utilities/common.h"
#include "gui/jdk.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define JVM "bin/server/jvm.dll"
#elif defined(__APPLE__)
#define JVM "lib/server/libjvm.dylib"
#else
#define JVM "lib/server/libjvm.so"
#endif
/* dir/name as a JDK of version (its release file's line; NULL for no such file), with its JVM or without */
static char* jdk(const char* dir, const char* name, const char* version, int jvm) {
    char *home = snx_path_join(dir, name), *file = snx_path_join(home, JVM), *release = snx_path_join(home, "release"),
         *folder = snx_dirname(file), *text = snx_format("IMPLEMENTOR=\"x\"\nJAVA_VERSION=\"%s\"\n", version ? version : "");
    assert(!snx_mkdirs(folder));
    if (jvm) assert(!snx_write_file(file, "jvm", 3));
    if (version) assert(!snx_write_file(release, text, strlen(text)));
    free(text);
    free(folder);
    free(release);
    free(file);
    return home;
}
int main(void) {
    char *dir = snx_temp_dir(NULL), *a, *b, *c, *d, *e, *f, *root2 = snx_path_join(dir, "second"),
         *none = snx_path_join(dir, "none"), *jdks, **found, **roots;
    const char* list[4];
    size_t n, i, home_at = 0;
    assert(dir && root2 && none);
    a = jdk(dir, "java-21", "21.0.4", 1);
    b = jdk(dir, "java-17", "17.0.12", 1); /* too old */
    c = jdk(dir, "java-8", "1.8.0_402", 1);
    d = jdk(dir, "no-jvm", "22", 0);
    e = jdk(dir, "mac.jdk/Contents/Home", "24.0.1", 1); /* macOS's: in its Contents/Home */
    f = jdk(root2, "itself", "23", 1);
    assert(gui_jdk_version(a) == 21 && gui_jdk_version(b) == 17 && gui_jdk_version(c) == 8 && !gui_jdk_version(d) &&
           gui_jdk_version(e) == 24 && gui_jdk_version(f) == 23 && !gui_jdk_version(none) && !gui_jdk_version(dir));
    {
        char *release = snx_path_join(b, "release"), *text = snx_strdup("OLD_JAVA_VERSION=\"99\"\nJAVA_VERSION=\"17\"\n");
        assert(!snx_write_file(release, text, strlen(text)));
        assert(gui_jdk_version(b) == 17); /* the line's own name, not one that ends with it */
        free(text);
        free(release);
    }
    list[0] = dir;
    list[1] = none;
    list[2] = root2;
    list[3] = NULL;
    found = gui_jdks(list, &n);
    assert(n == 3 && !strcmp(found[0], a) && strstr(found[1], "mac.jdk") && !strcmp(found[2], f));
    gui_jdks_free(found, n);
    list[0] = root2; /* a root a JDK is in, twice: once */
    list[1] = root2;
    list[2] = NULL;
    found = gui_jdks(list, &n);
    assert(n == 1 && !strcmp(found[0], f));
    gui_jdks_free(found, n);
    list[0] = f; /* the JDK's own folder as a root: itself */
    list[1] = NULL;
    found = gui_jdks(list, &n);
    assert(n == 1 && !strcmp(found[0], f));
    gui_jdks_free(found, n);
    list[0] = none;
    found = gui_jdks(list, &n);
    assert(!n && !found);
    assert(!snx_mkdirs(none)); /* a root there and empty, as a system's /opt can be */
    found = gui_jdks(list, &n);
    assert(!n && !found);
#ifndef _WIN32 /* a link to a JDK listed already: not twice (Debian's java-21-openjdk-amd64 beside its linked name) */
    {
        char* link = snx_path_join(root2, "link");
        assert(link && !snx_symlink(f, link));
        list[0] = root2;
        list[1] = NULL;
        found = gui_jdks(list, &n);
        assert(n == 1);
        gui_jdks_free(found, n);
        free(link);
    }
#endif
    roots = gui_jdk_roots("/home/someone", &n);
    assert(roots && n > 4);
    jdks = snx_path_join("/home/someone", ".jdks"); /* with the system's separator */
    for (i = 0; i < n; ++i)
        if (!strcmp(roots[i], jdks)) home_at = i;
    assert(home_at > 0 && home_at < n);
    free(jdks);
    gui_jdks_free(roots, n);
    roots = gui_jdk_roots(NULL, &n);
    assert(roots && n);
    gui_jdks_free(roots, n);
    assert(!snx_remove_tree(dir));
    free(a);
    free(b);
    free(c);
    free(d);
    free(e);
    free(f);
    free(root2);
    free(none);
    free(dir);
    return 0;
}
