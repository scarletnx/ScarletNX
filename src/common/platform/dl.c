/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The host's shared libraries (dl.h) */
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE /* glibc's <link.h> has dl_iterate_phdr's struct dl_phdr_info only for it */
#endif
#include "common/platform/dl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include "common/platform/file.h" /* snx_host_wpath */
#else
#include <dlfcn.h>
#endif
#if !defined(_WIN32) && !defined(__APPLE__)
#include <errno.h>
#include <link.h>
#include <stdint.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#if defined(__linux__) && !defined(MADV_POPULATE_READ)
#define MADV_POPULATE_READ 22 /* Linux 5.14's, which an older C library's headers lack */
#endif
#endif

static struct {
    void* (*_Atomic fallback)(const char* path); /* plat_dl_fallback's */
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
    void* (*fallback)(const char*) = atomic_load_explicit(&g_dl.fallback, memory_order_acquire);
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
void plat_dl_fallback(void* (*open)(const char* path)) {
    atomic_store_explicit(&g_dl.fallback, open, memory_order_release);
}

#if !defined(_WIN32) && !defined(__APPLE__)
#define DL_PARTS 16
typedef struct {
    uintptr_t at;                         /* the address asked about */
    uintptr_t lo[DL_PARTS], hi[DL_PARTS]; /* the parts (PT_LOAD) of the object that holds it, page-aligned */
    int n;
    const char* name;
} DlFind;
static int dl_find(struct dl_phdr_info* info, size_t size, void* data) {
    DlFind* f = data;
    uintptr_t page = (uintptr_t)sysconf(_SC_PAGESIZE);
    int i, held = 0;
    (void)size;
    for (i = 0; i < info->dlpi_phnum; i++) { /* its program headers by index: ElfW is glibc's and bionic's alone */
        uintptr_t a = (uintptr_t)info->dlpi_addr + info->dlpi_phdr[i].p_vaddr;
        if (info->dlpi_phdr[i].p_type == PT_LOAD && f->at >= a && f->at < a + info->dlpi_phdr[i].p_memsz) held = 1;
    }
    if (!held) return 0;
    for (i = 0; i < info->dlpi_phnum && f->n < DL_PARTS; i++) {
        uintptr_t a = (uintptr_t)info->dlpi_addr + info->dlpi_phdr[i].p_vaddr, n = info->dlpi_phdr[i].p_memsz;
        if (info->dlpi_phdr[i].p_type != PT_LOAD || !n) continue;
        f->lo[f->n] = a & ~(page - 1);
        f->hi[f->n] = (a + n + page - 1) & ~(page - 1);
        f->n++;
    }
    f->name = info->dlpi_name;
    return 1;
}
size_t plat_dl_resident(const void* inside, char* said, size_t size) {
    DlFind f = {.at = (uintptr_t)inside};
    struct timespec t0, t1;
    size_t bytes = 0;
    int i, err = 0, how = 0; /* 0 populated, 1 read ahead (MADV_WILLNEED) */
    const char* advice = "MADV_POPULATE_READ";
    if (!dl_iterate_phdr(dl_find, &f) || !f.n) {
        snprintf(said, size, "no loaded object holds the address %p: nothing read ahead", inside);
        return 0;
    }
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (i = 0; i < f.n; i++) {
        size_t len = f.hi[i] - f.lo[i];
#if defined(MADV_POPULATE_READ)
        if (!how && madvise((void*)f.lo[i], len, MADV_POPULATE_READ) == 0) {
            bytes += len;
            continue;
        }
        if (!how && errno != EINVAL) { /* EINVAL: a kernel before 5.14, which reads ahead instead */
            err = errno;
            break;
        }
#endif
        how = 1;
        if (madvise((void*)f.lo[i], len, MADV_WILLNEED)) {
            err = errno;
            break;
        }
        bytes += len;
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    if (how) advice = "MADV_WILLNEED";
    long ms = (long)((t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000);
    const char* name = f.name && f.name[0] ? f.name : "the program";
    if (err)
        snprintf(said, size, "%s: %d of its %d parts read ahead (%zu MB) when madvise %s refused part %d: %s", name, i,
                 f.n, bytes >> 20, advice, i + 1, strerror(err));
    else if (how)
        snprintf(said, size,
                 "%s: %zu MB in %d parts asked to be read ahead in %ld ms (MADV_WILLNEED, which does not wait: %s)",
                 name, bytes >> 20, f.n, ms,
#if defined(MADV_POPULATE_READ)
                 "this kernel has no MADV_POPULATE_READ, which came with Linux 5.14"
#else
                 "this host has no MADV_POPULATE_READ"
#endif
        );
    else
        snprintf(said, size, "%s: %zu MB in %d parts read into memory in %ld ms (%s)", name, bytes >> 20, f.n, ms,
                 advice);
    return bytes;
}
#else
size_t plat_dl_resident(const void* inside, char* said, size_t size) {
    snprintf(said, size, "the code at %p is not read ahead: this host's loader gives no list of its parts", inside);
    return 0;
}
#endif
