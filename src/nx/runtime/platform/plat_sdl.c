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
/* Shared storage (/sdcard) is mounted noexec: a module there loads from a copy in the app's cache folder, which
   plat_dl_open tries when the system refuses the module's own path */
static void* dl_open_from_cache(const char* path) {
    const char* cache = SDL_GetAndroidCachePath();
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
    return dlopen(copy, RTLD_NOW | RTLD_LOCAL);
}
__attribute__((constructor)) static void dl_cache_register(void) { plat_dl_fallback(dl_open_from_cache); }
#endif
