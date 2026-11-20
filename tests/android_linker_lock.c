/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The bridge's linker (src/android/bridge/linker.c) with two threads: a lookup in the libraries waits for a load
   another thread is in, which holds the group from its first look at it to its last constructor, as bionic's dlopen
   does. The load is held where it reads its library's header (libc's pread, wrapped at link time: under ThinLTO
   plat_pread_fd is bitcode the link can inline into the load, past the wrap, and the load never waited; glibc links
   the call as pread64, since the platform library takes 64-bit offsets, so both names are wrapped there) while a second
   thread asks for a symbol: the lookup has not answered a third of a second later, and answers once the load is let
   go. Without the group's lock on the lookup it walked the libraries while the load changed them */
#include "android/bridge/linker.h"
#include "common/platform/file.h"
#include "common/utilities/common.h"
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

static struct {
    pthread_mutex_t lock;
    pthread_cond_t changed;
    int hold, held, looked_up; /* under lock: the load is to wait in its read, it waits there, the lookup answered */
} g_test = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, 0, 0, 0};

static void hold_here(void) {
    pthread_mutex_lock(&g_test.lock);
    if (g_test.hold) {
        g_test.held = 1;
        pthread_cond_broadcast(&g_test.changed);
        while (g_test.hold) pthread_cond_wait(&g_test.changed, &g_test.lock);
    }
    pthread_mutex_unlock(&g_test.lock);
}
ssize_t __real_pread(int fd, void* buffer, size_t size, off_t offset);
ssize_t __wrap_pread(int fd, void* buffer, size_t size, off_t offset) {
    hold_here();
    return __real_pread(fd, buffer, size, offset);
}
#ifdef __GLIBC__
ssize_t __real_pread64(int fd, void* buffer, size_t size, int64_t offset);
ssize_t __wrap_pread64(int fd, void* buffer, size_t size, int64_t offset) {
    hold_here();
    return __real_pread64(fd, buffer, size, offset);
}
#endif

static void* load(void* path) {
    char error[256];
    /* no library: its header is read, held there, and refused */
    assert(!linker_load(path, error, sizeof error));
    return NULL;
}
static void* look_up(void* unused) {
    (void)unused;
    (void)linker_symbol(NULL, "a symbol no library has");
    pthread_mutex_lock(&g_test.lock);
    g_test.looked_up = 1;
    pthread_cond_broadcast(&g_test.changed);
    pthread_mutex_unlock(&g_test.lock);
    return NULL;
}

int main(void) {
    char* folder = snx_temp_dir(NULL);
    char* path = folder ? snx_path_join(folder, "libnone.so") : NULL;
    FILE* f = path ? snx_fopen(path, "wb") : NULL;
    pthread_t loader, reader;
    struct timespec until;
    assert(f && fputs("not a library", f) >= 0 && !fclose(f));
    g_test.hold = 1;
    assert(!pthread_create(&loader, NULL, load, path));
    pthread_mutex_lock(&g_test.lock);
    while (!g_test.held) pthread_cond_wait(&g_test.changed, &g_test.lock);
    pthread_mutex_unlock(&g_test.lock);
    assert(!pthread_create(&reader, NULL, look_up, NULL));
    /* the lookup waits for the load: it has not answered when the time is up */
    assert(!clock_gettime(CLOCK_REALTIME, &until));
    until.tv_nsec += 300000000;
    until.tv_sec += until.tv_nsec / 1000000000, until.tv_nsec %= 1000000000;
    pthread_mutex_lock(&g_test.lock);
    while (!g_test.looked_up && !pthread_cond_timedwait(&g_test.changed, &g_test.lock, &until)) {}
    assert(!g_test.looked_up);
    g_test.hold = 0;
    pthread_cond_broadcast(&g_test.changed);
    while (!g_test.looked_up) pthread_cond_wait(&g_test.changed, &g_test.lock);
    pthread_mutex_unlock(&g_test.lock);
    assert(!pthread_join(loader, NULL) && !pthread_join(reader, NULL));
    assert(!remove(path) && !snx_remove_tree(folder));
    free(path);
    free(folder);
    return 0;
}
