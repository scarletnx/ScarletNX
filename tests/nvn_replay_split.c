/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* nvn's replay (gpu/nvn/nvn_replay.c) with gpu_split's renderer thread (gpu_queue.c) and the queued calls of
   gpu/renderer.c: while the renderer thread is behind the replay, a submission's fence and a window's present reach
   the renderer after the submission's draw, in the queue's order, as a GPFIFO job's increments do (gpu.c's job_end).
   The present is a record, so the front end goes on without a wait, and the increment that frees the window's
   texture comes after the present on the renderer thread. The GPU thread is this one, which pumps the replay's work;
   the renderer's side is the queue's thread */
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
#include <stdatomic.h>
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
   thread a pthread (winpthreads on Windows) */
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
void plat_wait_word(_Atomic uint32_t* w, uint32_t v, int64_t timeout_ns) {
    if (atomic_load_explicit(w, memory_order_seq_cst) != v || !timeout_ns) return;
    struct timespec ts = {0, 50000};
    nanosleep(&ts, NULL);
}
void plat_wake_word(_Atomic uint32_t* w) { (void)w; }
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
void gpu_poll_why(u32 why) { (void)why; }
u64 gpu_thread_busy_ns(void) { return 0; } /* gpu_split=2's measure (gpu_queue.c gq_auto): this test sets gpu_split=1 */
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
int gmem_write_cb_now(u64 gva, const void* src, u64 size) { return gmem_write_now(gva, src, size); }
void gmem_read_quiet(u64 gva, void* dst, u64 size) { memcpy(dst, GP(gva), (size_t)size); }
void gpu_release_write_now(u64 gva, const void* data, u32 len) { memcpy(GP(gva), data, len); }
void gpu_i2m_block_now(const GpuI2m* t) { (void)t; }
void gpu_release_write(u64 gva, const void* data, u32 n) { memcpy(GP(gva), data, n); }
/* gpu.c's, with the clock this test has (guest_gpu_ticks: 0) */
void gpu_release_ticks_now(u64 gva) { memset(GP(gva), 0, 8); }
void gpu_release_write_stamped(u64 gva, const void* data) {
    memcpy(GP(gva), data, 8);
    gpu_release_ticks_now(gva + 8);
}
void gpu_release_stamp_ticked(u64 gva, const void* data, int top) {
    (void)top;
    gpu_release_write_stamped(gva, data);
}
void gpu_release_stamp(u64 gva, const void* data, int top) { gpu_release_stamp_ticked(gva, data, top); }
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
static _Atomic u32 g_current[32];
static u32 g_promised[32], g_used[32];
u32 syncpt_alloc(void) {
    for (u32 i = 1; i < 32; i++)
        if (!g_used[i]) {
            g_used[i] = 1;
            g_promised[i] = atomic_load_explicit(&g_current[i], memory_order_relaxed);
            return i;
        }
    fatal("no syncpoint left\n");
}
void syncpt_free(u32 id) { g_used[id] = 0; }
u32 syncpt_promise(u32 id, u32 n) { return g_promised[id] += n; }
void syncpt_incr(u32 id) { atomic_fetch_add_explicit(&g_current[id], 1, memory_order_seq_cst); }
u32 syncpt_read(u32 id) { return atomic_load_explicit(&g_current[id], memory_order_seq_cst); }
int syncpt_wait(u32 id, u32 value, i64 timeout) {
    (void)timeout;
    while ((i32)(syncpt_read(id) - value) < 0) pump();
    return 0;
}

/* ------------------------------------------------ the renderer: what it has drawn when the first increment comes, on
   the queue's thread */
static _Atomic u32 g_drawn, g_drawn_at_incr = ~0u, g_drawn_at_present = ~0u;
static _Atomic int g_incr_on_front = -1;           /* the last increment ran on this thread (1) or the queue's (0) */
static _Atomic u32 g_presents, g_presents_at_incr; /* the presents, and how many came before the last increment */
/* the shading rate ids the draws gave the renderer, in their order, and the renderer's calls of shading_rate: the
   first gives the id 7, the second none (0), as an image the renderer can't take */
static u32 g_draw_rates[8];
static _Atomic u32 g_rate_calls;
static void draw(const DsState* ds, const DrawCall* dc, DrawOwner* o) {
    (void)ds, (void)o;
    u32 n = atomic_fetch_add_explicit(&g_drawn, 1, memory_order_seq_cst);
    if (n < 8) g_draw_rates[n] = dc->shading_rate;
}
static u32 shading_rate(const GpuShadingRate* r) {
    (void)r;
    return atomic_fetch_add_explicit(&g_rate_calls, 1, memory_order_seq_cst) ? 0 : 7;
}
static void stream_incr(u32 id) {
    u32 none = ~0u;
    atomic_compare_exchange_strong_explicit(&g_drawn_at_incr, &none,
                                            atomic_load_explicit(&g_drawn, memory_order_seq_cst), memory_order_seq_cst,
                                            memory_order_seq_cst);
    atomic_store_explicit(&g_incr_on_front, pthread_equal(pthread_self(), g_front) ? 1 : 0, memory_order_seq_cst);
    atomic_store_explicit(&g_presents_at_incr, atomic_load_explicit(&g_presents, memory_order_seq_cst),
                          memory_order_seq_cst);
    syncpt_incr(id);
}
static void stream_end(void) {}
static u32 release_value(u32 id) { return syncpt_read(id); }
static void present_buffer(u64 cpu, u32 w, u32 h, u32 pitch, u32 layout, u32 bhl, u32 fmt, const i32 crop[4],
                           u32 transform, u64 released, u64 period, u32 shown) {
    (void)cpu, (void)w, (void)h, (void)pitch, (void)layout, (void)bhl, (void)fmt, (void)crop, (void)transform;
    (void)released, (void)period;
    atomic_store_explicit(&g_drawn_at_present, atomic_load_explicit(&g_drawn, memory_order_seq_cst),
                          memory_order_seq_cst);
    atomic_fetch_add_explicit(&g_presents, 1, memory_order_seq_cst);
    syncpt_incr(shown);
}
static const Renderer renderer = {.draw = draw,
                                  .stream_incr = stream_incr,
                                  .stream_end = stream_end,
                                  .release_value = release_value,
                                  .present_buffer = present_buffer,
                                  .shading_rate = shading_rate};
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
int nvn_draw_samples(NvnDraw* d, u64 at) {
    (void)d, (void)at;
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
    pump(); /* the present does not wait for the held renderer thread */
    expect(!atomic_load_explicit(&g_presents, memory_order_seq_cst),
           "the present ran before the work queued before it");
    pthread_mutex_lock(&gate_lock);
    gate_open = 1;
    pthread_cond_broadcast(&gate_cv);
    pthread_mutex_unlock(&gate_lock);
    gq_drain();
    u32 at_incr = atomic_load_explicit(&g_drawn_at_incr, memory_order_seq_cst);
    u32 at_present = atomic_load_explicit(&g_drawn_at_present, memory_order_seq_cst);
    if (at_incr != 1 || at_present != 1)
        fprintf(stderr, "the renderer had drawn %d draws at the fence's increment and %d at the present (1 each)\n",
                (int)at_incr, (int)at_present);
    expect(atomic_load_explicit(&g_drawn, memory_order_relaxed) == 1,
           "the submission's draw reaches the renderer once");
    expect(at_incr == 1, "the fence's increment comes after the submission's draw");
    expect(at_present == 1, "the present comes after the draw before it");
    expect(atomic_load_explicit(&g_incr_on_front, memory_order_relaxed) == 0 &&
               atomic_load_explicit(&g_presents, memory_order_relaxed) == 1 &&
               atomic_load_explicit(&g_presents_at_incr, memory_order_relaxed) == 1,
           "the increment that frees the window's texture did not come after the present on the renderer thread");
    expect(syncpt_read(id) == fenced && syncpt_read(nvn_replay_shown_syncpt()) == shown,
           "the queue's syncpoint and the presents' reach what the replay promised");
    /* A shading rate image's id for the draws after it (gpu_shading_rate) is a record: the front end goes on while the
       renderer thread is held, and each draw takes the id of the call before it, when it runs */
    pthread_mutex_lock(&gate_lock);
    gate_open = 0;
    pthread_mutex_unlock(&gate_lock);
    gq_call(held, NULL, 0);
    NvnDraw d = {0};
    nvn_draw_init(&d, 9);
    GpuShadingRate sr = {.addr = 0x8000, .new_image = 1};
    DrawCall dc = {.count = 3, .instances = 1};
    dc.shading_rate = gpu_shading_rate(&sr);
    gpu_draw(&d.ds, &dc, &d.own);
    dc.shading_rate = gpu_shading_rate(&sr);
    gpu_draw(&d.ds, &dc, &d.own);
    expect(!atomic_load_explicit(&g_rate_calls, memory_order_seq_cst),
           "the front end asked the held renderer for a shading rate id");
    pthread_mutex_lock(&gate_lock);
    gate_open = 1;
    pthread_cond_broadcast(&gate_cv);
    pthread_mutex_unlock(&gate_lock);
    gq_drain();
    expect(atomic_load_explicit(&g_drawn, memory_order_seq_cst) == 3 && g_draw_rates[1] == 7 && g_draw_rates[2] == 0,
           "each draw takes the id of the shading rate call before it (7, then none)");
    if (!failures) puts("nvn_replay_split: ok");
    return failures != 0;
}
