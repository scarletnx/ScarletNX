/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The platform layer's parts SDL already does the same way on every host: the path queries and the power state,
   and Android's cache folder for a module shared storage cannot run. plat_posix.c and plat_win32.c keep what SDL has no call for (condition variables with nanosecond
   deadlines, threads with their alternate stacks, the guest's address space, the crash handler) */
#include "nx/runtime/platform/plat.h"
#include <SDL3/SDL.h>
#include <stdio.h>
#include <string.h>

/* ================================================================ power */
/* SDL_GetPowerInfo reads the host's battery and charger on every platform SDL supports. Only a host on its battery
   is not on its charger; a state it doesn't know (or cannot read) is -1, which TV mode Auto takes as On */
int plat_power_on_charger(void) {
    switch (SDL_GetPowerInfo(NULL, NULL)) {
    case SDL_POWERSTATE_ON_BATTERY: return 0;
    case SDL_POWERSTATE_NO_BATTERY:
    case SDL_POWERSTATE_CHARGING:
    case SDL_POWERSTATE_CHARGED: return 1;
    default: return -1;
    }
}

/* ================================================================ libraries */
#if defined(__ANDROID__)
#include <dlfcn.h>
#include <errno.h>
#include <stdlib.h>
#include <sys/syscall.h>
#include <unistd.h>
#include "nx/runtime/main.h" /* g_datadir, the folder snx_user_path's cache below comes from */
/* Shared storage (/sdcard) is mounted noexec: a module there loads from a copy in the app's cache folder, which
   plat_dl_open tries when the system refuses the module's own path */
/* The cache folder a module is loaded from. Android runs no module of the app's files folder: a module is loaded
   from the app's own cache folder, and only an activity's process can ask SDL for it (SDL_GetAndroidCachePath reads
   the activity, which a driver the menu runs as its child has none of, and faults there). HOME is the app's files
   folder (GameActivity sets it), and the cache is beside it, as getCacheDir gives it */
static const char* android_cache(char* out, size_t size) {
    const char* home = getenv("HOME");
    char* own = NULL;
    /* HOME is the app's own folder (getCacheDir is its cache/), or the files folder a game activity sets, whose
       cache is beside it */
    static const char* const beside[] = {"%s/cache", "%s/../cache"};
    for (size_t i = 0; home && *home && i < sizeof beside / sizeof *beside; ++i)
        if ((size_t)snprintf(out, size, beside[i], home) < size && snx_is_dir(out) && !access(out, W_OK)) return out;
    /* a data folder the app owns: its cache/ is the cache of a run that has no activity */
    if (g_datadir[0]) own = snx_user_path(g_datadir, SNX_USER_CACHE);
    if (own) {
        if (snx_mkdirs(own) || access(own, W_OK) || (size_t)snprintf(out, size, "%s", own) >= size) out[0] = 0;
        free(own);
    } else
        out[0] = 0;
    return *out ? out : NULL;
}
/* A module of the app's own storage, in an anonymous file: an app that maps a module's text segment out of its own
   storage fails there with EACCES (GrapheneOS's exploit protection, in the app's processes only), and a memfd is no
   such file. The fd needs no name: /proc/self/fd/<fd> is the module's path. NULL where the kernel keeps no memfd */
static void* dl_open_from_memory(const char* path) {
#ifndef __NR_memfd_create
    (void)path;
    return NULL;
#else
    size_t size = 0;
    unsigned char* bytes = snx_read_file(path, &size);
    if (!bytes) {
        fprintf(stderr, "scarletnx: cannot read %s into memory: %s\n", path, strerror(errno));
        return NULL;
    }
    int fd = (int)syscall(__NR_memfd_create, "scarletnx-module", 1 /* MFD_CLOEXEC */);
    if (fd < 0) {
        fprintf(stderr, "scarletnx: no memfd: %s\n", strerror(errno));
        free(bytes);
        return NULL;
    }
    for (size_t at = 0; at < size;) {
        ssize_t written = write(fd, bytes + at, size - at);
        if (written <= 0) {
            fprintf(stderr, "scarletnx: cannot write the module into memory: %s\n", strerror(errno));
            close(fd);
            free(bytes);
            return NULL;
        }
        at += (size_t)written;
    }
    free(bytes);
    char name[64];
    snprintf(name, sizeof name, "/proc/self/fd/%d", fd);
    void* library = dlopen(name, RTLD_NOW | RTLD_LOCAL);
    if (!library) fprintf(stderr, "scarletnx: the module in memory does not load either: %s\n", dlerror());
    close(fd); /* the mapping keeps the file */
    return library;
#endif
}
static void* dl_open_from_cache(const char* path) {
    /* a copy of the module in the cache folder, which is where Android loads one from */
    char cache_buf[1024];
    void* in_memory = dl_open_from_memory(path); /* the RAM, before 60 MB of copy on the storage */
    if (in_memory) return in_memory;
    const char* cache = android_cache(cache_buf, sizeof cache_buf);
    const char* base = strrchr(path, '/');
    char copy[1400], buf[65536];
    FILE *in, *out;
    size_t n;
    int ok = 1;
    if (!cache) return NULL;
    base = base ? base + 1 : path;
    if ((size_t)snprintf(copy, sizeof copy, "%s/%s", cache, base) >= sizeof copy) return NULL;
    in = fopen(path, "rb");
    if (!in) return NULL;
    out = fopen(copy, "wb");
    if (!out) {
        fclose(in);
        return NULL;
    }
    while (ok && (n = fread(buf, 1, sizeof buf, in)) > 0) ok = fwrite(buf, 1, n, out) == n;
    ok = ok && !ferror(in);
    fclose(in);
    if (fclose(out) || !ok) return NULL;
    /* Android maps a module only where its file is executable */
    if (snx_make_executable(copy)) return NULL;
    void* library = dlopen(copy, RTLD_NOW | RTLD_LOCAL);
    /* the module's own path's error is the one the caller reports: this one is the copy's, said here */
    if (!library) fprintf(stderr, "scarletnx: the cache's copy at %s does not load either: %s\n", copy, dlerror());
    return library;
}
__attribute__((constructor)) static void dl_cache_register(void) { plat_dl_fallback(dl_open_from_cache); }
#endif
