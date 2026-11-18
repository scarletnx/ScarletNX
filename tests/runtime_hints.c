/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A save paused at publication must not keep new hints behind its file I/O, and must not mark those hints saved. */
#include "nx/runtime/rtapi.h"
#include "nx/runtime/main.h"
#include "nx/runtime/platform/plat.h"
#include "nx/runtime/platform/log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#ifndef _WIN32
#include <pthread.h>
#include <time.h>
#endif

char g_exedir[1024], g_exename[256] = "test-runtime-hints";
static _Atomic int paused, release_write, added;
static int save_fail;
static _Atomic int block_write;

void logf_(int level, const char* format, ...) {
    (void)level;
    (void)format;
}
static void tick(void) {
#ifdef _WIN32
    Sleep(1);
#else
    struct timespec delay = {0, 1000000};
    nanosleep(&delay, NULL);
#endif
}
int plat_replace_file(const char* from, const char* to) {
    if (atomic_load_explicit(&block_write, memory_order_acquire)) {
        atomic_store_explicit(&paused, 1, memory_order_release);
        while (!atomic_load_explicit(&release_write, memory_order_acquire)) tick();
    }
    if (save_fail) return -1;
#ifdef _WIN32
    return MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING) ? 0 : -1;
#else
    return rename(from, to);
#endif
}
static void save_job(void) { save_hints(); }
static void add_job(void) {
    hints_add(0x2000);
    atomic_store_explicit(&added, 1, memory_order_release);
}
#ifdef _WIN32
static DWORD WINAPI save_thread(void* unused) { (void)unused; save_job(); return 0; }
static DWORD WINAPI add_thread(void* unused) { (void)unused; add_job(); return 0; }
typedef HANDLE Thread;
static Thread start(int save) { return CreateThread(NULL, 0, save ? save_thread : add_thread, NULL, 0, NULL); }
static void join(Thread t) { WaitForSingleObject(t, INFINITE); CloseHandle(t); }
#else
static void* save_thread(void* unused) { (void)unused; save_job(); return NULL; }
static void* add_thread(void* unused) { (void)unused; add_job(); return NULL; }
typedef pthread_t Thread;
static Thread start(int save) {
    Thread t;
    if (pthread_create(&t, NULL, save ? save_thread : add_thread, NULL)) exit(2);
    return t;
}
static void join(Thread t) { pthread_join(t, NULL); }
#endif
static int wait_for(_Atomic int* value) {
    for (unsigned i = 0; i < 2000; i++) {
        if (atomic_load_explicit(value, memory_order_acquire)) return 1;
        tick();
    }
    return 0;
}
static int file_is(const char* expected) {
    FILE* f = fopen("test-runtime-hints.hints", "r");
    if (!f) return 0;
    char buffer[256] = {0};
    size_t size = fread(buffer, 1, sizeof buffer - 1, f);
    fclose(f);
    return size == strlen(expected) && !strcmp(buffer, expected);
}
int main(void) {
    remove("test-runtime-hints.hints");
    hints_init();
    if (hints_add(0) || !hints_add(0x1000) || hints_add(0x1000)) return 1;
    atomic_store_explicit(&block_write, 1, memory_order_release);
    Thread saver = start(1);
    int at_write = wait_for(&paused);
    Thread writer = start(0);
    int responsive = wait_for(&added);
    atomic_store_explicit(&release_write, 1, memory_order_release);
    join(writer);
    join(saver);
    atomic_store_explicit(&block_write, 0, memory_order_release);
    if (!at_write || !responsive || !file_is("1000\n")) return 1;
    save_hints();
    if (!file_is("1000\n2000\n")) return 1;
    hints_add(0x3000);
    save_fail = 1;
    save_hints();
    if (!file_is("1000\n2000\n")) return 1;
    save_fail = 0;
    save_hints();
    if (!file_is("1000\n2000\n3000\n")) return 1;
    load_hints();
    size_t count;
    u64* copy = hints_copy(&count);
    int exact = count == 3 && copy && copy[0] == 0x1000 && copy[1] == 0x2000 && copy[2] == 0x3000;
    free(copy);
    remove("test-runtime-hints.hints");
    puts("Hints remain responsive during save, preserve concurrent additions, and retry failed publication");
    return exact ? 0 : 1;
}
