/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* nvn's replay (gpu/nvn/nvn_replay.c) with gpu_split's renderer thread (gpu_queue.c) and the queued calls of
   gpu/renderer.c: while the renderer thread is behind the replay, a submission's fence and a window's present reach
   the renderer after the submission's draw, in the queue's order, as a GPFIFO job's increments do (gpu.c's job_end).
   The GPU thread is this one, which pumps the replay's work; the renderer's side is the queue's thread */
#include "nx/gpu/nvn/nvn_replay.h"
#include "nx/gpu/nvn/nvn_object.h"
#include "nx/gpu/nvn/nvn_draw.h"
#include "nx/gpu/nvn/nvn_copy.h"
#include "nx/gpu/gpu.h"
#include "nx/gpu/gpu_queue.h"
#include "nx/gpu/renderer.h"
#include "nx/hle/services/nvdrv.h"
#include "nx/runtime/options.h"
#include "nx/runtime/clock.h"
#include "nx/runtime/platform/plat.h"
#include "nx/runtime/platform/log.h"
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static u8 memory[65536];
u8* g_mem = memory;
Settings g_settings;

static int failures;
static void expect(int ok, const char* what) {
    if (!ok) {
        fprintf(stderr, "FAIL: %s\n", what);
        failures++;
    }
}

/* ------------------------------------------------ the platform: the replay's lock is this thread's alone, the queue's
   thread a pthread */
void pm_init(PMutex* m) { (void)m; }
void pm_lock(PMutex* m) { (void)m; }
void pm_unlock(PMutex* m) { (void)m; }
typedef struct {
    PThreadFn fn;
    void* arg;
} ThreadStart;
static void* thread_main(void* p) {
    ThreadStart start = *(ThreadStart*)p;
    free(p);
    start.fn(start.arg);
    return NULL;
}
int plat_thread(PThreadFn fn, void* arg, size_t stack) {
    pthread_t t;
    ThreadStart* start = malloc(sizeof *start);
    (void)stack;
    if (!start) return 1;
    *start = (ThreadStart){fn, arg};
    if (pthread_create(&t, NULL, thread_main, start)) {
        free(start);
        return 1;
    }
    return 0;
}
/* a wait on a word that may end early, as thread.h allows: a short sleep while the word holds v */
void plat_wait_word(uint32_t* w, uint32_t v, int64_t timeout_ns) {
    if (__atomic_load_n(w, __ATOMIC_SEQ_CST) != v || !timeout_ns) return;
    struct timespec ts = {0, 50000};
    nanosleep(&ts, NULL);
}
void plat_wake_word(uint32_t* w) { (void)w; }
void snx_thread_name(const char* name) { (void)name; }
char* snx_os_error(char* out, size_t size) {
    snprintf(out, size, "a thread of the test");
    return out;
}
void logf_(int level, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    if (!level) vfprintf(stderr, fmt, args);
    va_end(args);
}
void fatal(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    exit(1);
}

/* ------------------------------------------------ the GPU thread (gpu.c): this one, pumping the replay's work */
static pthread_t g_front;
int gpu_on_thread(void) { return pthread_equal(pthread_self(), g_front); }
static struct {
    int (*poll)(void*);
    void* arg;
} g_work[16];
static u32 g_nwork;
void gpu_call_poll(int (*fn)(void*), void* arg, u32 lane) {
    (void)lane;
    if (g_nwork == 16) fatal("more than 16 pieces of work queued\n");
    g_work[g_nwork].poll = fn;
    g_work[g_nwork++].arg = arg;
}
void gpu_call(void (*fn)(void*), void* arg, const u32* chans, u32 n) {
    (void)fn, (void)arg, (void)chans, (void)n;
    fatal("gpu_call: nothing here makes one\n");
}
void gpu_kick(void) {}
/* each piece of work in its order, every one done */
static void pump(void) {
    for (u32 i = 0; i < g_nwork; i++)
        if (!g_work[i].poll(g_work[i].arg)) fatal("work %u waits\n", i);
    g_nwork = 0;
}
/* what the queue's records do that this test doesn't make */
int gmem_write_now(u64 gva, const void* src, u64 size) {
    memcpy(GP(gva), src, (size_t)size);
    return 0;
}
void gmem_read_quiet(u64 gva, void* dst, u64 size) { memcpy(dst, GP(gva), (size_t)size); }
void gpu_release_write_now(u64 gva, const void* data, u32 len) { memcpy(GP(gva), data, len); }
void gpu_i2m_block_now(const GpuI2m* t) { (void)t; }
void gpu_release_write(u64 gva, const void* data, u32 n) { memcpy(GP(gva), data, n); }
u32 gmem_r32(u64 gva) { return gr32(gva); }
u64 gmmu_translate(u64 gva) { return gva; }
/* maxwell/engines.c's render enable and maxwell3d.c's held draw: none here */
int gpu_render_state(u32 owner, u32* pred) {
    (void)owner, (void)pred;
    return -1;
}
void gpu_render_states_reset(void) {}
void r3d_pending_flush(void) {}

/* ------------------------------------------------ syncpoints */
static u32 g_current[32], g_promised[32], g_used[32];
u32 syncpt_alloc(void) {
    for (u32 i = 1; i < 32; i++)
        if (!g_used[i]) {
            g_used[i] = 1;
            g_promised[i] = g_current[i];
            return i;
        }
    fatal("no syncpoint left\n");
}
void syncpt_free(u32 id) { g_used[id] = 0; }
u32 syncpt_promise(u32 id, u32 n) { return g_promised[id] += n; }
void syncpt_incr(u32 id) { __atomic_add_fetch(&g_current[id], 1, __ATOMIC_SEQ_CST); }
u32 syncpt_read(u32 id) { return __atomic_load_n(&g_current[id], __ATOMIC_SEQ_CST); }
int syncpt_wait(u32 id, u32 value, i64 timeout) {
    (void)timeout;
    while ((i32)(syncpt_read(id) - value) < 0) pump();
    return 0;
}

/* ------------------------------------------------ the renderer: what it has drawn when the first increment and the
   present come, on the queue's thread */
static u32 g_drawn, g_drawn_at_incr = ~0u, g_drawn_at_present = ~0u;
static void draw(const DsState* ds, const DrawCall* dc, DrawOwner* o) {
    (void)ds, (void)dc, (void)o;
    __atomic_add_fetch(&g_drawn, 1, __ATOMIC_SEQ_CST);
}
static void stream_incr(u32 id) {
    u32 none = ~0u;
    __atomic_compare_exchange_n(&g_drawn_at_incr, &none, __atomic_load_n(&g_drawn, __ATOMIC_SEQ_CST), 0,
                                __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    syncpt_incr(id);
}
static void stream_end(void) {}
static u32 release_value(u32 id) { return syncpt_read(id); }
static void present_buffer(u64 cpu, u32 w, u32 h, u32 pitch, u32 layout, u32 bhl, u32 fmt, const i32 crop[4],
                           u32 transform, u64 released, u64 period, u32 shown) {
    (void)cpu, (void)w, (void)h, (void)pitch, (void)layout, (void)bhl, (void)fmt, (void)crop, (void)transform;
    (void)released, (void)period;
    __atomic_store_n(&g_drawn_at_present, __atomic_load_n(&g_drawn, __ATOMIC_SEQ_CST), __ATOMIC_SEQ_CST);
    syncpt_incr(shown);
}
static const Renderer renderer = {.draw = draw,
                                  .stream_incr = stream_incr,
                                  .stream_end = stream_end,
                                  .release_value = release_value,
                                  .present_buffer = present_buffer};
const Renderer* g_renderer = &renderer;

/* ------------------------------------------------ nvn's records: a draw goes to the renderer as nvn_draw.c's do */
void nvn_draw_init(NvnDraw* d, u32 owner) { d->ds.owner = owner; }
void nvn_draw_free(NvnDraw* d) { (void)d; }
int nvn_draw_record(NvnDraw* d, u32 op, u64 payload, u32 size) {
    (void)payload, (void)size;
    if (op != OP_DRAW) return 0;
    DrawCall dc = {.count = 3, .instances = 1};
    gpu_draw(&d->ds, &dc, &d->own);
    return 1;
}
int nvn_copy_record(u32 op, u64 payload, u32 size, i64 stride[2]) {
    (void)op, (void)payload, (void)size, (void)stride;
    return 0;
}
int nvn_draw_samples(NvnDraw* d, u64 at, u64 ticks) {
    (void)d, (void)at, (void)ticks;
    return 0;
}
int nvn_cmd_unfinished(u64 p) {
    (void)p;
    return 0;
}
void nvn_cmd_describe(u64 p, char* out, size_t n) {
    (void)p;
    if (n) out[0] = 0;
}
u64 guest_ticks(void) { return 0; }
u64 guest_gpu_ticks(void) { return 0; }

/* a record of the queue's that holds the renderer thread until the gate opens */
static pthread_mutex_t gate_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gate_cv = PTHREAD_COND_INITIALIZER;
static int gate_open;
static void held(const void* arg, u64 bytes) {
    (void)arg, (void)bytes;
    pthread_mutex_lock(&gate_lock);
    while (!gate_open) pthread_cond_wait(&gate_cv, &gate_lock);
    pthread_mutex_unlock(&gate_lock);
}

int main(void) {
    g_front = pthread_self();
    g_settings.gpu_split = 1;
    g_renderer_up = 1;
    u32 queue = nvn_replay_queue(), id = nvn_replay_syncpt(queue), shown = 0;
    u64 ready = 0;
    /* a recording of a draw and a fence of the sync at 0x400 */
    *(NvnRecHdr*)(void*)GP(0x300) = (NvnRecHdr){OP_DRAW, 8};
    *(NvnRecHdr*)(void*)GP(0x308) = (NvnRecHdr){OP_FENCE_SYNC | NVN_REC_LAST, 16};
    gw64(0x310, 0x400);
    ((NvnSync*)(void*)GP(0x400))->magic = NVN_MAGIC('S', 'Y', 'N', 'C');
    gw64(0x100, 0x300);
    gq_call(held, NULL, 0); /* the renderer thread stops here, behind everything the replay queues next */
    nvn_replay_submit(queue, 1, 0x100, &ready, &shown);
    NvnReplayPresent present = {.gva = 0x2000, .w = 1, .h = 1};
    u32 fenced = nvn_replay_present(queue, &present, &shown);
    pump();
    pthread_mutex_lock(&gate_lock);
    gate_open = 1;
    pthread_cond_broadcast(&gate_cv);
    pthread_mutex_unlock(&gate_lock);
    gq_drain();
    u32 at_incr = __atomic_load_n(&g_drawn_at_incr, __ATOMIC_SEQ_CST);
    u32 at_present = __atomic_load_n(&g_drawn_at_present, __ATOMIC_SEQ_CST);
    if (at_incr != 1 || at_present != 1)
        fprintf(stderr, "the renderer had drawn %d draws at the fence's increment and %d at the present (1 each)\n",
                (int)at_incr, (int)at_present);
    expect(g_drawn == 1, "the submission's draw reaches the renderer once");
    expect(at_incr == 1, "the fence's increment comes after the submission's draw");
    expect(at_present == 1, "the present comes after the draw before it");
    expect(syncpt_read(id) == fenced && syncpt_read(nvn_replay_shown_syncpt()) == shown,
           "the queue's syncpoint and the presents' reach what the replay promised");
    if (!failures) puts("nvn_replay_split: ok");
    return failures != 0;
}
