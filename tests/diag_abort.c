/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* nn::diag's abort hook (src/nx/hle/hook/diag.c) on a fake abort: the generated adapter called as the SDK's own
   AbortImpl calls VAbortImpl, with its strings and Result in guest memory. The log line holds each value as passed,
   the error code the Result makes, "none" for a null string and "no result" for a null Result; the function's own
   code then runs once, with the registers the call left */
#include "nx/hle/hook/hook.h"
#include "nx/runtime/platform/log.h"
#include "hle/hook/hooks.hook.h"
#include "hle/hook/hooks.hook.inc"
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#define VABORT "_ZN2nn4diag6detail10VAbortImplEPKcS3_S3_iPKNS_6ResultEPKNS_2os17UserExceptionInfoES3_RSt9__va_list"
#define AT     0x8100040u /* where the sdk module exports it */

/* the log's lines, what the function's own code was called with, and the guest's memory */
static struct {
    char log[4096];
    unsigned own_runs;
    u64 own_entry, own_x[8];
    u8 mem[0x10000];
} g_diag_abort;

void logf_(int level, const char* format, ...) {
    size_t n = strlen(g_diag_abort.log);
    va_list args;
    assert(level == 0); /* a warning: the game is about to stop */
    va_start(args, format);
    vsnprintf(g_diag_abort.log + n, sizeof g_diag_abort.log - n, format, args);
    va_end(args);
}
void fatal(const char* format, ...) {
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    exit(1);
}
u64 kernel_find_symbol_exact(const char* name) { return strcmp(name, VABORT) ? 0 : AT; }
/* the function's own code: what it was called with */
static void own(CPU* RESTRICT c, u64 entry) {
    g_diag_abort.own_runs++;
    g_diag_abort.own_entry = entry;
    memcpy(g_diag_abort.own_x, c->x, sizeof g_diag_abort.own_x);
    c->pc = c->x[30];
}
GuestFn hook_original(u64 addr) { return addr == AT ? own : NULL; }
/* hooks.hook's other handlers, which the table names */
u64 hook_memmove(CPU* c, u64 dst, u64 src, u64 size) {
    (void)c, (void)src, (void)size;
    return dst;
}
u64 hook_memset(CPU* c, u64 dst, i32 value, u64 size) {
    (void)c, (void)value, (void)size;
    return dst;
}
/* codec.c's and audio_stream.c's, none reached here */
u32 audio_open_stream_event(CPU* c, u64 stream, u64 event, u64 work, u64 size, u64 device, u64 format, i32 capacity,
                            i32 threshold) {
    (void)c, (void)stream, (void)event, (void)work, (void)size, (void)device, (void)format, (void)capacity,
        (void)threshold;
    return 0;
}
u32 audio_open_stream(CPU* c, u64 stream, u64 work, u64 size, u64 device, u64 format, i32 capacity, u32 flags) {
    (void)c, (void)stream, (void)work, (void)size, (void)device, (void)format, (void)capacity, (void)flags;
    return 0;
}
u32 audio_stop_stream(CPU* c, u64 stream) {
    (void)c, (void)stream;
    return 0;
}
void audio_close_stream(CPU* c, u64 stream) { (void)c, (void)stream; }
u32 audio_purge_stream(CPU* c, u64 stream) {
    (void)c, (void)stream;
    return 0;
}
u32 audio_start_stream(CPU* c, u64 stream) {
    (void)c, (void)stream;
    return 0;
}
i32 audio_write_stream(CPU* c, u64 stream, u64 data, i32 frames) {
    (void)c, (void)stream, (void)data, (void)frames;
    return 0;
}
void audio_set_stream_gain(CPU* c, u64 stream, f32 gain, i64 fade) { (void)c, (void)stream, (void)gain, (void)fade; }
u32 audio_stream_state(CPU* c, u64 stream) {
    (void)c, (void)stream;
    return 0;
}
void audio_stream_statistics(CPU* c, u64 stream, int reset) { (void)c, (void)stream, (void)reset; }
u32 audio_default_virtual_output(CPU* c, u64 name) {
    (void)c, (void)name;
    return 0;
}
i32 audio_stream_free_frames(CPU* c, u64 stream) {
    (void)c, (void)stream;
    return 0;
}
i32 audio_stream_filled_frames(CPU* c, u64 stream) {
    (void)c, (void)stream;
    return 0;
}
u32 audio_virtual_output_properties(CPU* c, u64 props, u64 name) {
    (void)c, (void)props, (void)name;
    return 0;
}
i32 audio_list_virtual_inputs(CPU* c, u64 names, i32 count) {
    (void)c, (void)names, (void)count;
    return 0;
}
u32 audio_open_input(CPU* c, u64 stream, u64 work, u64 size, u64 device, u64 format, i32 capacity) {
    (void)c, (void)stream, (void)work, (void)size, (void)device, (void)format, (void)capacity;
    return 0;
}
u32 audio_open_input_event(CPU* c, u64 stream, u64 event, u64 work, u64 size, u64 device, u64 format, i32 capacity,
                           i32 threshold) {
    (void)c, (void)stream, (void)event, (void)work, (void)size, (void)device, (void)format, (void)capacity,
        (void)threshold;
    return 0;
}
u32 audio_start_input(CPU* c, u64 stream) {
    (void)c, (void)stream;
    return 0;
}
u32 audio_stop_input(CPU* c, u64 stream) {
    (void)c, (void)stream;
    return 0;
}
u32 audio_purge_input(CPU* c, u64 stream) {
    (void)c, (void)stream;
    return 0;
}
void audio_close_input(CPU* c, u64 stream) { (void)c, (void)stream; }
u32 audio_input_state(CPU* c, u64 stream) {
    (void)c, (void)stream;
    return 0;
}
i32 audio_input_filled_frames(CPU* c, u64 stream) {
    (void)c, (void)stream;
    return 0;
}
i32 audio_input_free_frames(CPU* c, u64 stream) {
    (void)c, (void)stream;
    return 0;
}
i32 audio_input_capacity_frames(CPU* c, u64 stream) {
    (void)c, (void)stream;
    return 0;
}
i32 audio_read_input(CPU* c, u64 stream, u64 data, i32 frames) {
    (void)c, (void)stream, (void)data, (void)frames;
    return 0;
}
void codec_decoder_reset(CPU* c, u64 decoder) { (void)c, (void)decoder; }
void codec_decoder_finalize(CPU* c, u64 decoder) { (void)c, (void)decoder; }
u32 codec_decoder_init(CPU* c, u64 decoder, i32 rate, i32 channels, i64 frame, u64 manager) {
    (void)c, (void)decoder, (void)rate, (void)channels, (void)frame, (void)manager;
    return 0;
}
u32 codec_manager_execute(CPU* c, u64 manager) {
    (void)c, (void)manager;
    return 0;
}
void codec_manager_finalize(CPU* c, u64 manager) { (void)c, (void)manager; }
u32 codec_decoder_enqueue(CPU* c, u64 decoder, u64 consumed, u64 data, u64 size) {
    (void)c, (void)decoder, (void)consumed, (void)data, (void)size;
    return 0;
}
u32 codec_manager_init(CPU* c, u64 manager, u64 event, i32 rate, i32 channels, i64 frame, i32 decoders, i32 buffers,
                       u64 work) {
    (void)c, (void)manager, (void)event, (void)rate, (void)channels, (void)frame, (void)decoders, (void)buffers,
        (void)work;
    return 0;
}
u64 codec_manager_work_size(CPU* c, i32 rate, i32 channels, i64 frame, i32 decoders, i32 buffers) {
    (void)c, (void)rate, (void)channels, (void)frame, (void)decoders, (void)buffers;
    return 0;
}
i32 codec_manager_free_buffers(CPU* c, u64 manager) {
    (void)c, (void)manager;
    return 0;
}
i32 codec_decoder_waiting(CPU* c, u64 decoder) {
    (void)c, (void)decoder;
    return 0;
}
u32 codec_decoder_dequeue(CPU* c, u64 decoder, u64 samples, u64 out, u64 size) {
    (void)c, (void)decoder, (void)samples, (void)out, (void)size;
    return 0;
}
i32 codec_decoder_completed(CPU* c, u64 decoder) {
    (void)c, (void)decoder;
    return 0;
}
i32 codec_decoder_executing(CPU* c, u64 decoder) {
    (void)c, (void)decoder;
    return 0;
}

static void put(CPU* c, u64 at, const char* s) { memcpy(c->mem + at, s, strlen(s) + 1); }

/* the adapter of the diag hook, from the generated table */
static GuestFn adapter(void) {
    u32 i;
    for (i = 0; i < hook_table.count; i++)
        if (!strcmp(hook_table.symbol[i], VABORT)) return hook_fn[i];
    return NULL;
}

int main(void) {
    CPU c = {0};
    GuestFn fn = adapter();
    assert(fn != NULL);
    c.mem = g_diag_abort.mem;
    put(&c, 0x1000, "result.IsSuccess()");
    put(&c, 0x1100, "ReadFile");
    put(&c, 0x1200, "fs_File.cpp");
    put(&c, 0x1300, "Failed: %s");
    snx_st_le32(g_diag_abort.mem + 0x1400, 0x2EF02); /* module 0x102 (258, its 9th bit set), description 0x177 (375) */
    c.x[0] = 0x1000, c.x[1] = 0x1100, c.x[2] = 0x1200, c.x[3] = 117, c.x[4] = 0x1400, c.x[5] = 0x1500;
    c.x[6] = 0x1300, c.x[7] = 0x1600, c.x[30] = 0x8005123;
    fn(&c, AT);
    printf("%s", g_diag_abort.log);
    assert(strstr(g_diag_abort.log,
                  "nn::diag abort: \"result.IsSuccess()\" failed in ReadFile (fs_File.cpp, line 117) with "
                  "result 0x2ef02 (error 2258-0375), message format \"Failed: %s\""));
    assert(g_diag_abort.own_runs == 1 && g_diag_abort.own_entry == AT && g_diag_abort.own_x[0] == 0x1000 &&
           g_diag_abort.own_x[3] == 117 && g_diag_abort.own_x[4] == 0x1400 && g_diag_abort.own_x[7] == 0x1600);
    assert(c.pc == 0x8005123);
    puts("PASS a fake abort logged with its values, then its own code run with the call's registers");

    g_diag_abort.log[0] = 0;
    c.x[0] = 0, c.x[2] = 0, c.x[3] = 0, c.x[4] = 0, c.x[6] = 0;
    fn(&c, AT);
    printf("%s", g_diag_abort.log);
    assert(strstr(g_diag_abort.log,
                  "nn::diag abort: \"none\" failed in ReadFile (none, line 0) with no result, message format "
                  "\"none\""));
    assert(g_diag_abort.own_runs == 2);
    puts("PASS null strings and no Result said so");
    return 0;
}

/* the alloc and sync families' handlers (alloc.c, sync.c), which the table names and this test never calls */
int alloc_ready(void) { return 0; }
u64 alloc_malloc(CPU* c, u64 size) { (void)c, (void)size; return 0; }
u64 alloc_calloc(CPU* c, u64 n, u64 size) { (void)c, (void)n, (void)size; return 0; }
u64 alloc_realloc(CPU* c, u64 p, u64 size) { (void)c, (void)p, (void)size; return 0; }
void alloc_free(CPU* c, u64 p) { (void)c, (void)p; }
u64 alloc_aligned(CPU* c, u64 align, u64 size) { (void)c, (void)align, (void)size; return 0; }
i32 alloc_posix_memalign(CPU* c, u64 out, u64 align, u64 size) { (void)c, (void)out, (void)align, (void)size; return 0; }
u64 alloc_strdup(CPU* c, u64 s) { (void)c, (void)s; return 0; }
u64 alloc_strndup(CPU* c, u64 s, u64 max) { (void)c, (void)s, (void)max; return 0; }
u64 alloc_new(CPU* c, u64 size) { (void)c, (void)size; return 0; }
u64 alloc_new_aligned(CPU* c, u64 size, u64 align) { (void)c, (void)size, (void)align; return 0; }
u64 alloc_new_nothrow(CPU* c, u64 size, u64 nothrow) { (void)c, (void)size, (void)nothrow; return 0; }
u64 alloc_new_aligned_nothrow(CPU* c, u64 size, u64 align, u64 nothrow) {
    (void)c, (void)size, (void)align, (void)nothrow;
    return 0;
}
void alloc_delete(CPU* c, u64 p) { (void)c, (void)p; }
void sync_mutex_lock(CPU* c, u64 m) { (void)c, (void)m; }
int sync_mutex_try_lock(CPU* c, u64 m) { (void)c, (void)m; return 0; }
void sync_mutex_unlock(CPU* c, u64 m) { (void)c, (void)m; }
void sync_drop(CPU* c, u64 o) { (void)c, (void)o; }
void sync_recursive_init(CPU* c, u64 m) { (void)c, (void)m; }
void sync_recursive_lock(CPU* c, u64 m) { (void)c, (void)m; }
int sync_recursive_try_lock(CPU* c, u64 m) { (void)c, (void)m; return 0; }
void sync_recursive_unlock(CPU* c, u64 m) { (void)c, (void)m; }
void sync_cond_notify_one(CPU* c, u64 cv) { (void)c, (void)cv; }
void sync_cond_notify_all(CPU* c, u64 cv) { (void)c, (void)cv; }
void sync_cond_wait(CPU* c, u64 cv, u64 lk) { (void)c, (void)cv, (void)lk; }
void sync_cond_timed_wait(CPU* c, u64 cv, u64 lk, i64 ns) { (void)c, (void)cv, (void)lk, (void)ns; }
i64 sync_system_now(CPU* c) { (void)c; return 0; }
i64 sync_steady_now(CPU* c) { (void)c; return 0; }
void sync_sleep_for(CPU* c, u64 d) { (void)c, (void)d; }
/* libc_std.c's qsort and errno call into the guest, which this test never does */
void hook_guest_call(CPU* c, u64 fn, u64 a0, u64 a1, u64 a2, u64 a3) { (void)c, (void)fn, (void)a0, (void)a1, (void)a2, (void)a3; }
