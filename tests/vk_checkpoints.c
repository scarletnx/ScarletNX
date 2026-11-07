/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Exercise the real ring with independent recorder/query callbacks, without a Vulkan loader or GPU. */
#include "nx/gpu/vk/vk.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if !defined(_WIN32)
#include <sched.h>
#endif

VkState g_vk;
PFN_vkGetQueueCheckpointDataNV vkGetQueueCheckpointDataNV;
PFN_vkGetQueueCheckpointData2NV vkGetQueueCheckpointData2NV;
#include "../src/nx/gpu/vk/checkpoints.c"

enum { PRODUCERS = 4, RECORDS = 512 };
static struct {
    PMutex log_lock;
    unsigned checks, failures, calls, submit_reads, reports, ready, release, finished, concurrent;
    u64 last_marker, marker[4];
    char log[65536];
    size_t log_size;
} g_test = {.log_lock = PMUTEX_INIT};
static _Thread_local struct { int checkpoint_locked; } t_test;

static void check(int pass, const char* what) {
    __atomic_fetch_add(&g_test.checks, 1, __ATOMIC_RELAXED);
    if (!pass && __atomic_fetch_add(&g_test.failures, 1, __ATOMIC_RELAXED) < 12)
        fprintf(stderr, "FAIL: %s\n", what);
}
static void yield_thread(void) {
#if defined(_WIN32)
    SwitchToThread();
#else
    sched_yield();
#endif
}
void pm_lock(PMutex* lock) {
#if defined(_WIN32)
    AcquireSRWLockExclusive(&lock->l);
#else
    pthread_mutex_lock(&lock->l);
#endif
    if (lock == &g_ck.lock) t_test.checkpoint_locked++;
}
void pm_unlock(PMutex* lock) {
    if (lock == &g_ck.lock) t_test.checkpoint_locked--;
#if defined(_WIN32)
    ReleaseSRWLockExclusive(&lock->l);
#else
    pthread_mutex_unlock(&lock->l);
#endif
}
static void outside_lock(void) {
    check(!t_test.checkpoint_locked, "no external callback runs with the checkpoint lock held");
}
void logf_(int level, const char* format, ...) {
    (void)level;
    outside_lock();
    va_list args;
    va_start(args, format);
    pm_lock(&g_test.log_lock);
    size_t left = sizeof g_test.log - g_test.log_size;
    int length = vsnprintf(g_test.log + g_test.log_size, left, format, args);
    if (length >= 0 && (size_t)length < left) g_test.log_size += (size_t)length;
    else check(0, "mock report log fits");
    pm_unlock(&g_test.log_lock);
    va_end(args);
}
u64 vk_submit_number(void) {
    outside_lock();
    __atomic_fetch_add(&g_test.submit_reads, 1, __ATOMIC_RELAXED);
    return 77;
}
static void emit(u32 value) {
    vk_ck((VkCommandBuffer)(uintptr_t)1, value % VK_CK_KINDS, value, value * 3u + 1u,
          value ^ 0xa51ce399u, ~value);
}
void rec_CmdSetCheckpointNV(VkCommandBuffer cb, const void* pointer) {
    (void)cb;
    outside_lock();
    u64 marker = (u64)(uintptr_t)pointer;
    VkCk value;
    pm_lock(&g_ck.lock);
    int current = marker && marker <= g_ck.newest && g_ck.newest - marker < CK_RING;
    value = g_ck.ring[(marker - 1) % CK_RING];
    pm_unlock(&g_ck.lock);
    /* Another producer can overwrite a marker before its recorder callback. That is permitted; a reporter must
       reject it. Every still-current record must contain all fields from one complete publication. */
    if (current) {
        check(value.submit == 77 && value.kind == value.a % VK_CK_KINDS && value.b == value.a * 3u + 1u &&
              value.c == (value.a ^ 0xa51ce399u) && value.d == ~value.a, "record publication has one complete payload");
    }
    __atomic_store_n(&g_test.last_marker, marker, __ATOMIC_RELAXED);
    __atomic_fetch_add(&g_test.calls, 1, __ATOMIC_RELAXED);
}
void vk_submit_progress(u64* submitted, u64* done) {
    outside_lock();
    __atomic_fetch_add(&g_test.reports, 1, __ATOMIC_RELAXED);
    if (g_test.concurrent) {
        /* Deterministically release all producers only after the report has taken its snapshot. Their attempts
           must finish without changing that snapshot, even while this external callback is still running. */
        __atomic_store_n(&g_test.release, 1, __ATOMIC_RELEASE);
        while (__atomic_load_n(&g_test.finished, __ATOMIC_ACQUIRE) != PRODUCERS) yield_thread();
    }
    emit(0xeffe0001u); /* reentrant recorder attempt must be suppressed after the freeze */
    vk_ck_report(); /* a second reporter must return without querying Vulkan again */
    *submitted = 77;
    *done = 76;
}
static VKAPI_ATTR void VKAPI_CALL query(VkQueue queue, u32* count, VkCheckpointDataNV* data) {
    (void)queue;
    outside_lock();
    vk_ck_report();
    if (!data) { *count = 4; return; }
    u32 amount = *count < 4 ? *count : 4;
    for (u32 i = 0; i < amount; i++) {
        check(data[i].sType == VK_STRUCTURE_TYPE_CHECKPOINT_DATA_NV, "legacy query initializes every returned record");
        data[i].stage = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
        data[i].pCheckpointMarker = (void*)(uintptr_t)g_test.marker[i];
    }
    *count = amount;
}
static VKAPI_ATTR void VKAPI_CALL query2(VkQueue queue, u32* count, VkCheckpointData2NV* data) {
    (void)queue;
    outside_lock();
    if (!data) { *count = 4; return; }
    u32 amount = *count < 4 ? *count : 4;
    for (u32 i = 0; i < amount; i++) {
        check(data[i].sType == VK_STRUCTURE_TYPE_CHECKPOINT_DATA_2_NV, "synchronization2 query initializes every record");
        data[i].stage = VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT;
        data[i].pCheckpointMarker = (void*)(uintptr_t)g_test.marker[i];
    }
    *count = amount;
}
static void reset(void) {
    /* Every previous worker is joined. Runtime init remains startup-only; there is no test hook in production. */
    free(g_ck.ring);
    g_ck.ring = NULL;
    g_ck.newest = 0;
    g_ck.reported = 0;
    g_test.calls = g_test.submit_reads = g_test.reports = g_test.ready = g_test.release = g_test.finished = 0;
    g_test.concurrent = 0;
    g_test.last_marker = 0;
    g_test.log_size = 0;
    g_test.log[0] = 0;
    memset(g_test.marker, 0, sizeof g_test.marker);
    g_vk.has_checkpoints = 1;
    g_vk.gfx = g_vk.compute = (VkQueue)(uintptr_t)1;
    vk_ck_init();
    check(g_ck.ring != NULL, "checkpoint ring allocation");
}
static void wrap_case(void) {
    reset();
    for (u32 i = 0; i < CK_RING + 5; i++) emit(i);
    g_test.marker[0] = 1;
    g_test.marker[1] = CK_RING + 1;
    g_test.marker[2] = CK_RING + 6; /* unknown future marker */
    vk_ck_report();
    VkCkView view = {g_ck.ring, g_ck.newest};
    check(!ck_find(&view, 1) && ck_find(&view, CK_RING + 1), "overwritten generation cannot name its replacement");
    check(!ck_find(&view, CK_RING + 6) && !ck_find(&view, 0), "future and zero markers never resolve");
    check(strstr(g_test.log, "reached marker 1 of 262149: no longer in the diagnostic ring") != NULL,
          "driver's stale marker is reported as unavailable rather than a newer payload");
    check(strstr(g_test.log, "submission 77 holds 262144 markers") != NULL, "unfinished summary counts only the retained ring");
    unsigned calls = g_test.calls;
    emit(123);
    vk_ck_report();
    check(g_test.calls == calls && g_test.reports == 1 && g_ck.newest == CK_RING + 5,
          "the first report freezes publication and duplicate reports do no Vulkan work");
}
static void wide_marker_case(void) {
    reset();
    if (UINTPTR_MAX > UINT32_MAX) {
        g_ck.newest = UINT32_MAX - 1ull;
        emit(11); emit(12);
        g_test.marker[0] = UINT32_MAX;
        g_test.marker[1] = UINT32_MAX + 1ull;
        vk_ck_report();
        VkCkView view = {g_ck.ring, g_ck.newest};
        check(g_ck.newest == UINT32_MAX + 1ull && ck_find(&view, UINT32_MAX + 1ull),
              "marker identity retains its generation across the old32-bit counter boundary");
        check(strstr(g_test.log, "reached marker 4294967296") != NULL, "both driver query paths retain the full pointer marker");
    }
    reset();
    g_ck.newest = (u64)UINTPTR_MAX - 1;
    emit(21);
    emit(22);
    check(g_test.calls == 1 && g_test.last_marker == UINTPTR_MAX && g_ck.newest == UINTPTR_MAX,
          "exhausted pointer identities stop publication without repeating an old marker or producing zero");
    g_test.marker[0] = UINTPTR_MAX;
    vk_ck_report();
    VkCkView view = {g_ck.ring, g_ck.newest};
    check(ck_find(&view, UINTPTR_MAX) != NULL, "the final representable generation remains reportable");
}
static void produce(void* argument) {
    u32 worker = (u32)(uintptr_t)argument;
    for (u32 i = 0; i < RECORDS; i++) emit(worker * RECORDS + i);
    __atomic_fetch_add(&g_test.ready, 1, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&g_test.release, __ATOMIC_ACQUIRE)) yield_thread();
    for (u32 i = 0; i < RECORDS; i++) emit(0x10000000u + worker * RECORDS + i);
    __atomic_fetch_add(&g_test.finished, 1, __ATOMIC_RELEASE);
}
#if defined(_WIN32)
static DWORD WINAPI produce_entry(LPVOID argument) { produce(argument); return 0; }
static DWORD WINAPI report_entry(LPVOID argument) { (void)argument; vk_ck_report(); return 0; }
#else
static void* produce_entry(void* argument) { produce(argument); return NULL; }
static void* report_entry(void* argument) { (void)argument; vk_ck_report(); return NULL; }
#endif
static void concurrent_case(void) {
    reset();
#if defined(_WIN32)
    HANDLE worker[PRODUCERS];
    for (u32 i = 0; i < PRODUCERS; i++) {
        worker[i] = CreateThread(NULL, 0, produce_entry, (void*)(uintptr_t)i, 0, NULL);
        if (!worker[i]) { check(0, "producer thread creation"); exit(1); }
    }
#else
    pthread_t worker[PRODUCERS];
    for (u32 i = 0; i < PRODUCERS; i++)
        if (pthread_create(&worker[i], NULL, produce_entry, (void*)(uintptr_t)i)) {
            check(0, "producer thread creation"); exit(1);
        }
#endif
    while (__atomic_load_n(&g_test.ready, __ATOMIC_ACQUIRE) != PRODUCERS) yield_thread();
    g_test.marker[0] = 1;
    g_test.marker[1] = PRODUCERS * RECORDS;
    g_test.concurrent = 1;
#if defined(_WIN32)
    HANDLE reporter[2];
    for (u32 i = 0; i < 2; i++) {
        reporter[i] = CreateThread(NULL, 0, report_entry, NULL, 0, NULL);
        if (!reporter[i]) { check(0, "reporter thread creation"); exit(1); }
    }
    for (u32 i = 0; i < PRODUCERS; i++) { WaitForSingleObject(worker[i], INFINITE); CloseHandle(worker[i]); }
    for (u32 i = 0; i < 2; i++) { WaitForSingleObject(reporter[i], INFINITE); CloseHandle(reporter[i]); }
#else
    pthread_t reporter[2];
    for (u32 i = 0; i < 2; i++)
        if (pthread_create(&reporter[i], NULL, report_entry, NULL)) {
            check(0, "reporter thread creation"); exit(1);
        }
    for (u32 i = 0; i < PRODUCERS; i++) pthread_join(worker[i], NULL);
    for (u32 i = 0; i < 2; i++) pthread_join(reporter[i], NULL);
#endif
    check(g_test.reports == 1 && g_test.calls == PRODUCERS * RECORDS && g_ck.newest == PRODUCERS * RECORDS,
          "concurrent reporters freeze once and reject all later producer writes");
    VkCkView view = {g_ck.ring, g_ck.newest};
    u8 seen[PRODUCERS * RECORDS] = {0};
    for (u32 marker = 1; marker <= PRODUCERS * RECORDS; marker++) {
        const VkCk* entry = ck_find(&view, marker);
        check(entry && entry->a < sizeof seen && !seen[entry->a], "all concurrent publications have unique complete identities");
        if (entry && entry->a < sizeof seen) seen[entry->a] = 1;
    }
    check(strstr(g_test.log, "submission 77 holds 2048 markers") != NULL, "report sees one stable complete concurrent snapshot");
}
int main(void) {
    vkGetQueueCheckpointDataNV = query;
    vkGetQueueCheckpointData2NV = query2;
    g_vk.has_checkpoints = 0;
    emit(1);
    vk_ck_report();
    check(!g_test.calls && !g_test.submit_reads && !g_test.reports, "disabled diagnostics perform no publication or callbacks");
    wrap_case();
    wide_marker_case();
    concurrent_case();
    free(g_ck.ring);
    g_ck.ring = NULL;
    printf("checkpoint ring: %u checks, %u failures\n", g_test.checks, g_test.failures);
    return g_test.failures ? 1 : 0;
}
