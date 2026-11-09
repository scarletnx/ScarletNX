/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The host's shared libraries (dl.h) */
#include "common/platform/dl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include "common/platform/file.h" /* snx_host_wpath */
#else
#include <dlfcn.h>
#endif

static struct {
    void* (*fallback)(const char* path); /* plat_dl_fallback's */
} g_dl;
static _Thread_local struct {
    char error[512]; /* plat_dl_error's */
} t_dl;

static void* failed(const char* what, const char* path) {
#if defined(_WIN32)
    DWORD code = GetLastError();
    char reason[256];
    DWORD n = FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, code, 0, reason,
                             sizeof reason, NULL);
    while (n && (reason[n - 1] == '\n' || reason[n - 1] == '\r' || reason[n - 1] == ' ')) reason[--n] = 0;
    if (!n) snprintf(reason, sizeof reason, "error %lu", (unsigned long)code);
    snprintf(t_dl.error, sizeof t_dl.error, "%s", reason);
    (void)what;
    (void)path;
#else
    const char* reason = dlerror();
    snprintf(t_dl.error, sizeof t_dl.error, "%s", reason ? reason : what);
    (void)path;
#endif
    return NULL;
}

void* plat_dl_open(const char* path) {
#if defined(_WIN32)
    wchar_t* w = snx_host_wpath(path);
    void* h = w ? (void*)LoadLibraryW(w) : NULL;
    free(w);
#else
    void* h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
#endif
    if (!h) failed("cannot load", path);
    void* (*fallback)(const char*) = __atomic_load_n(&g_dl.fallback, __ATOMIC_ACQUIRE);
    if (!h && fallback) h = fallback(path); /* the first reason stays */
    return h;
}
void* plat_dl_sym(void* library, const char* name) {
#if defined(_WIN32)
    FARPROC f = GetProcAddress((HMODULE)library, name);
    void* p;
    memcpy(&p, &f, sizeof p);
#else
    void* p = dlsym(library, name);
#endif
    return p ? p : failed("no symbol", name);
}
void plat_dl_close(void* library) {
#if defined(_WIN32)
    FreeLibrary((HMODULE)library);
#else
    dlclose(library);
#endif
}
const char* plat_dl_error(void) { return t_dl.error[0] ? t_dl.error : "no error reported"; }
void* plat_dl_loaded(const char* name) {
#if defined(_WIN32)
    return GetModuleHandleA(name);
#elif defined(RTLD_NOLOAD)
    return dlopen(name, RTLD_NOW | RTLD_NOLOAD);
#else
    (void)name;
    return NULL;
#endif
}
void plat_dl_fallback(void* (*open)(const char* path)) { __atomic_store_n(&g_dl.fallback, open, __ATOMIC_RELEASE); }
