/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The HLE libraries' symbols (hle.h), found by name through one sorted index of every table */
#include "android/bridge/hle.h"
#include "android/bridge/gles2.h"
#include "android/bridge/hle_vulkan.h"
#include "common/utilities/common.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* the system libraries an app links to, which the HLE answers (Android's NDK stable libraries) */
static const char* const system_libraries[] = {
    "libc.so",     "libm.so",       "libdl.so",      "liblog.so",       "libandroid.so",    "libz.so",
    "libEGL.so",   "libGLESv1_CM.so", "libGLESv2.so", "libGLESv3.so",    "libvulkan.so",     "libOpenSLES.so",
    "libaaudio.so", "libmediandk.so", "libjnigraphics.so", "libnativewindow.so", "libstdc++.so", "libc++.so",
    "libsync.so",  "libbinder_ndk.so", "libcamera2ndk.so", "libOpenMAXAL.so", "libneuralnetworks.so",
};

bool hle_is_system_library(const char* name) {
    for (size_t i = 0; i < sizeof system_libraries / sizeof *system_libraries; ++i)
        if (!strcmp(system_libraries[i], name)) return true;
    return false;
}

static const HleSymbol* const tables[] = {hle_libc,   hle_posix,  hle_thread,       hle_sync,          hle_dl,
                                          hle_sles,   hle_media,  hle_android,      hle_egl,           hle_gles,
                                          hle_gles2,  hle_gles2_object, hle_gles2_program, hle_gles2_draw, hle_zlib,
                                          hle_signal};

/* a name that GLES 1.1 (hle_gles) and GLES 2 (gles2*.c) both have: one export, which calls the current context's */
typedef struct {
    HleSymbol symbol;        /* the export's: gles_either with this pair */
    const HleSymbol* gles1;
    const HleSymbol* gles2;
} HleEither;

static struct {
    const HleSymbol** sorted;
    size_t count;
    HleEither* either;
    pthread_once_t once;
} h = {.once = PTHREAD_ONCE_INIT};

static void gles_either(CPU* c, void* data) {
    const HleEither* e = data;
    const HleSymbol* s = gles2_current() ? e->gles2 : e->gles1;
    s->handler(c, s->data);
}
static bool in_table(const HleSymbol* s, const HleSymbol* table) {
    for (const HleSymbol* t = table; t->name[0]; ++t)
        if (t == s) return true;
    return false;
}

static int by_name(const void* a, const void* b) {
    return strcmp((*(const HleSymbol* const*)a)->name, (*(const HleSymbol* const*)b)->name);
}

static void index_tables(void) {
    hle_posix_init();
    for (size_t t = 0; t < sizeof tables / sizeof *tables; ++t)
        for (const HleSymbol* s = tables[t]; s->name[0]; ++s) ++h.count;
    h.sorted = snx_alloc((h.count ? h.count : 1) * sizeof *h.sorted);
    size_t n = 0;
    for (size_t t = 0; t < sizeof tables / sizeof *tables; ++t)
        for (const HleSymbol* s = tables[t]; s->name[0]; ++s) h.sorted[n++] = s;
    qsort(h.sorted, h.count, sizeof *h.sorted, by_name);
    /* the names of both GLES libraries: the pair as one entry */
    size_t pairs = 0;
    for (size_t i = 1; i < h.count; ++i) pairs += !strcmp(h.sorted[i - 1]->name, h.sorted[i]->name);
    h.either = snx_alloc((pairs ? pairs : 1) * sizeof *h.either);
    size_t kept = 0, made = 0;
    for (size_t i = 0; i < h.count; ++i) {
        const HleSymbol* a = h.sorted[i];
        const HleSymbol* b = i + 1 < h.count ? h.sorted[i + 1] : NULL;
        bool first_gles1 = b && in_table(a, hle_gles);
        if (b && !strcmp(a->name, b->name) && (first_gles1 || in_table(b, hle_gles))) {
            HleEither* e = &h.either[made++];
            e->gles1 = first_gles1 ? a : b;
            e->gles2 = first_gles1 ? b : a;
            e->symbol = (HleSymbol){a->name, gles_either, e};
            h.sorted[kept++] = &e->symbol;
            ++i;
            continue;
        }
        h.sorted[kept++] = h.sorted[i];
    }
    h.count = kept;
}

/* the functions that can wait as long as another thread or the system makes them: a guest signal runs its handler
   during the wait, as Linux interrupts it (a garbage collector stops each thread with one) */
static const char* const blocking[] = {
    "pthread_cond_wait", "pthread_cond_timedwait", "pthread_cond_clockwait", "pthread_cond_timedwait_monotonic_np",
    "pthread_mutex_lock", "pthread_mutex_timedlock", "pthread_mutex_lock_timeout_np", "pthread_rwlock_rdlock",
    "pthread_rwlock_wrlock", "pthread_rwlock_timedrdlock", "pthread_rwlock_timedwrlock", "pthread_join",
    "pthread_barrier_wait", "sem_wait", "sem_timedwait", "sem_clockwait", "nanosleep", "clock_nanosleep", "usleep",
    "sleep", "poll", "ppoll", "select", "pselect", "epoll_wait", "epoll_pwait", "read", "readv", "pread", "pread64",
    "write", "writev", "recv", "recvfrom", "recvmsg", "send", "sendto", "sendmsg", "accept", "accept4", "connect",
    "waitpid", "wait", "wait4", "flock", "syscall", "ALooper_pollOnce", "ALooper_pollAll",
};
static bool blocks(const char* name) {
    for (size_t i = 0; i < sizeof blocking / sizeof *blocking; ++i)
        if (!strcmp(blocking[i], name)) return true;
    return false;
}

const HleSymbol* hle_find(const char* name) {
    pthread_once(&h.once, index_tables);
    HleSymbol key = {name, NULL, NULL};
    const HleSymbol* k = &key;
    const HleSymbol** found = bsearch(&k, h.sorted, h.count, sizeof *h.sorted, by_name);
    return found ? *found : NULL;
}

uint64_t hle_symbol(const char* name) {
    uint64_t code = hle_code_symbol(name);
    if (code) return code;
    const HleSymbol* s = hle_find(name);
    if (!s) return hle_vulkan_symbol(name);
    if (!s->handler) return (uint64_t)(uintptr_t)s->data;
    uint64_t address = bridge_export(s->name, s->handler, s->data);
    if (blocks(s->name)) bridge_export_blocks(address);
    return address;
}

/* an import without an HLE: said once, then 0 (or NULL) for every call, as a stub of the NDK that is not there */
static void missing(CPU* c, void* data) {
    const char* name = data;
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    static char** said;
    static size_t said_count;
    pthread_mutex_lock(&lock);
    bool first = true;
    for (size_t i = 0; i < said_count && first; ++i) first = strcmp(said[i], name) != 0;
    if (first) {
        said = snx_realloc(said, (said_count + 1) * sizeof *said);
        said[said_count++] = (char*)name;
        fprintf(stderr, "scarletnx-android: %s (no HLE yet) called from 0x%llx: returns 0\n", name,
                (unsigned long long)c->x[30]);
    }
    pthread_mutex_unlock(&lock);
    c->x[0] = 0;
}

uint64_t hle_missing(const char* name) {
    size_t n = strlen(name) + 1;
    char* copy = snx_alloc(n);
    memcpy(copy, name, n);
    uint64_t a = bridge_export(name, missing, copy);
    return a;
}
