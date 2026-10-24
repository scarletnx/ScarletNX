/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* gpu.c's queue with channels whose streams wait (gpu_park), through the real puller, 3D engine and macros
   (engines.c, maxwell3d.c, mme.c): another channel's submission queued after a waiting one runs and releases what it
   waits for; the waiting channel's later jobs, and the calls that name it, stay behind it while a call naming another
   channel doesn't; a release the CPU writes and a syncpoint incremented off the GPU thread end waits; a stream
   resumes in the middle of a method batch, of a job's entries, and of a macro; and a wait nothing ends times out. A
   new channel's render enable override starts at USE_RENDER_ENABLE, as Eden's does.
   Each mark is a ClearBuffers value the stand-in renderer records; the platform's locks are pthreads */
#include "nx/gpu/gpu.h"
#include "nx/gpu/maxwell/engines_int.h"
#include "nx/gpu/renderer.h"
#include "support/dma_ref.h"
#include "nx/hle/services/nvdrv.h"
#include "nx/memory/mem.h"
#include "nx/runtime/clock.h"
#include "nx/runtime/platform/log.h"
#include "nx/runtime/platform/plat.h"
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ------------------------------------------------ what the files under test reach outside themselves */
/* guest memory, aligned as the console's (the semaphores are written as u32s, the pushbuffers read as u32s) */
static u64 guest[0x10000 / 8];
u8* g_mem = (u8*)guest;
_Atomic int g_cs_any, g_gfx_any, g_gfx_img_any, g_uma_any;
int g_renderer_up = 1;
u64 g_draws;
/* GPU addresses are guest ones, 0 unmapped, but for two GPU pages mapped apart (0xA000 at 0x3000, 0xB000 at 0x5000):
   a pushbuffer across them isn't contiguous, and is copied */
u64 gmmu_translate(u64 gva) {
    if (gva >= 0xA000 && gva < 0xC000) return (gva < 0xB000 ? 0x3000 : 0x5000) + (gva & 0xFFF);
    return gva < sizeof guest ? gva : 0;
}
/* the run of gva's guest memory, as nvdrv's walks its table */
u64 gmmu_run(u64 gva, u64 max, u64* cpu) {
    u64 n, want;
    *cpu = gmmu_translate(gva);
    if (!max || !*cpu) return 0;
    n = 0x1000 - (gva & 0xFFF);
    for (want = *cpu + n; n < max && gmmu_translate(gva + n) == want; want += 0x1000) n += 0x1000;
    return n < max ? n : max;
}
/* nvdrv.c's index of mappings by guest address, by brute force over the translation above (from the middle of each
   page, as page 0 translates to 0) */
void gmmu_aliases(u64 cpu, u64 size, void (*fn)(void* ctx, u64 gva, u64 n), void* ctx) {
    for (u64 p = 0; p < (sizeof guest + 0xFFF) >> 12; p++) {
        u64 c = gmmu_translate((p << 12) + 0x800) & ~0xFFFull, a = c > cpu ? c : cpu;
        u64 b = c + 0x1000 < cpu + size ? c + 0x1000 : cpu + size;
        if (gmmu_translate((p << 12) + 0x800) && a < b) fn(ctx, (p << 12) + (a - c), b - a);
    }
}
void mem_query(u64 addr, MemBlock* out) {
    memset(out, 0, sizeof *out);
    out->size = sizeof guest;
    (void)addr;
}
u64 guest_ns(void) { return 0; }
/* save states' function names, which nothing here takes */
const char* state_fn_name(StateFnPtr fn) {
    (void)fn;
    return NULL;
}
StateFnPtr state_fn_find(const char* name) {
    (void)name;
    return NULL;
}
/* Keep compute reports and inline uploads identical to compute.c, using the
   real shared report and upload handlers. */
void query_get(GpuChannel* ch, u32* r, int base, u32 v);
void compute_method(GpuChannel* ch, u32 m, u32 v, int last) {
    (void)last;
    m &= 0xFFF;
    ch->rcomp[m] = v;
    if (m == 0x6C3) query_get(ch, ch->rcomp, 0x6C0, v);
    if (m == 0x6C) i2m_launch(ch, ch->rcomp, 0x60);
    if (m == 0x6D) i2m_data(ch, v);
}
void kepler_i2m_method(GpuChannel* ch, u32 m, u32 v) { (void)ch, (void)m, (void)v; }
void fermi2d_method(GpuChannel* ch, u32 m, u32 v) { (void)ch, (void)m, (void)v; }
/* the macros run natively or interpreted here: mme_jit.c's compiled forms are tested in mme_jit.c */
int mme_jit_run(GpuChannel* ch, MmeRec* rec, u32 idx, u32 np) {
    (void)ch, (void)rec, (void)idx, (void)np;
    return 0;
}

static pthread_mutex_t seen_lk = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t seen_cv = PTHREAD_COND_INITIALIZER;
static u32 seen[32]; /* the marks, in the order the renderer got them */
static u32 nseen;
static u32* rewrite_on_clear;
static u32 rewrite_value;
static void see(u32 v) {
    pthread_mutex_lock(&seen_lk);
    if (nseen < 32) seen[nseen++] = v;
    pthread_cond_broadcast(&seen_cv);
    pthread_mutex_unlock(&seen_lk);
}
static void record_clear(GpuChannel* ch, u32 v) {
    (void)ch;
    if (rewrite_on_clear) {
        *rewrite_on_clear = snx_le32(rewrite_value);
        rewrite_on_clear = NULL;
    }
    see(v);
}
static void no_draw(GpuChannel* ch, int indexed, u32 topology, u32 first, u32 count, u32 base_vertex, u32 base_instance,
                    u32 instances) {
    (void)ch, (void)indexed, (void)topology, (void)first, (void)count, (void)base_vertex, (void)base_instance;
    (void)instances;
}
static int epochs; /* mem_may_have_changed calls */
static void new_epoch(void) { __atomic_add_fetch(&epochs, 1, __ATOMIC_SEQ_CST); }
static void nothing(void) {}
static void no_surface(u64 addr) { (void)addr; }

static u32 syncpt[SYNCPT_IDS];
u32 syncpt_read(u32 id) { return id < SYNCPT_IDS ? __atomic_load_n(&syncpt[id], __ATOMIC_SEQ_CST) : 0; }
void syncpt_add(u32 id, u32 n) {
    if (id >= SYNCPT_IDS) return;
    __atomic_add_fetch(&syncpt[id], n, __ATOMIC_SEQ_CST);
    gpu_kick();
}
void syncpt_incr(u32 id) { syncpt_add(id, 1); }
static int dma_on_cpu(const GpuDmaOp* o) { /* the renderer's copy as its CPU reference does it */
    dma_copy_ref(o);
    return 1;
}
/* A compute result whose host mirror still has the old command payload. Safe
   reads publish it before the inline engine consumes it. */
static struct {
    u64 addr;
    u32 words[4], reads;
} pending_inline;
static void compute_read(u64 addr, u64 size) {
    if (!pending_inline.addr || addr >= pending_inline.addr + 16 || addr + size <= pending_inline.addr) return;
    u8 bytes[16];
    for (u32 i = 0; i < 4; i++) snx_st_le32(bytes + 4 * i, pending_inline.words[i]);
    gmem_write_raw(pending_inline.addr, bytes, sizeof bytes);
    pending_inline.addr = 0;
    pending_inline.reads++;
    g_cs_any = 0;
}
static const Renderer stand_in = {.name = "stand-in",
                                  .dma_copy = dma_on_cpu,
                                  .cs_mem_need = compute_read,
                                  .mem_may_have_changed = new_epoch,
                                  .clear = record_clear,
                                  .draw = no_draw,
                                  .before_syncwait = nothing,
                                  .rt_flush = no_surface,
                                  .rt_reload = no_surface,
                                  .stream_incr = syncpt_incr};
const Renderer* g_renderer = &stand_in;

static int warned;
void logf_(int lvl, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    if (!lvl) {
        __atomic_add_fetch(&warned, 1, __ATOMIC_SEQ_CST);
        vfprintf(stderr, fmt, ap);
    }
    va_end(ap);
}
void fatal(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    exit(1);
}

u64 plat_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (u64)ts.tv_sec * 1000000000ull + (u64)ts.tv_nsec;
}
void pm_init(PMutex* m) { pthread_mutex_init(&m->l, NULL); }
void pm_lock(PMutex* m) { pthread_mutex_lock(&m->l); }
void pm_unlock(PMutex* m) { pthread_mutex_unlock(&m->l); }
void pm_lock_announce(PMutex* m, const char* name) { (void)m, (void)name; }
void pc_init(PCond* c) {
    pthread_condattr_t a;
    pthread_condattr_init(&a);
#ifndef __APPLE__ /* Darwin has no clock to choose: pc_wait waits a relative time there, as plat_posix.c's does */
    pthread_condattr_setclock(&a, CLOCK_MONOTONIC);
#endif
    pthread_cond_init(&c->c, &a);
}
void pc_broadcast(PCond* c) { pthread_cond_broadcast(&c->c); }
int pc_wait(PCond* c, PMutex* m, i64 ns) {
    if (ns < 0) return pthread_cond_wait(&c->c, &m->l), 0;
#ifdef __APPLE__
    struct timespec rel = {(time_t)(ns / 1000000000), (long)(ns % 1000000000)};
    return pthread_cond_timedwait_relative_np(&c->c, &m->l, &rel) != 0;
#endif
    u64 t = plat_ns() + (u64)ns;
    struct timespec ts = {(time_t)(t / 1000000000ull), (long)(t % 1000000000ull)};
    return pthread_cond_timedwait(&c->c, &m->l, &ts) != 0;
}
static PThreadFn thread_fn;
static void* thread_main(void* arg) {
    thread_fn(arg);
    return NULL;
}
int plat_thread(PThreadFn fn, void* arg, size_t stack) {
    pthread_t t;
    (void)stack;
    thread_fn = fn;
    return pthread_create(&t, NULL, thread_main, arg) != 0;
}

/* ------------------------------------------------ pushbuffers */
#define SEM  0x8000 /* the semaphores */
#define PUSH 0x1000 /* the pushbuffers, 64 words apart */
static u32 npush;
/* a GPFIFO entry for these words */
static u64 push(const u32* w, u32 n) {
    u64 va = PUSH + (u64)npush++ * 256;
    for (u32 i = 0; i < n; i++) snx_st_le32(g_mem + va + 4 * i, w[i]); /* guest memory is little-endian */
    return va | ((u64)n << 42);
}
static u32 inc(u32 m, u32 n) { return (1u << 29) | (n << 16) | m; } /* subchannel 0, incrementing */
static u32 imm(u32 m, u32 v) { return (4u << 29) | (v << 16) | m; } /* subchannel 0, immediate */
#define CLEAR 0x674                                                 /* 3D ClearBuffers: a mark */
/* the puller's SemaphoreOperation op on the u32 at addr with payload v, then a mark */
static u64 sem_then(u64 addr, u32 v, u32 op, u32 mark) {
    u32 w[6] = {inc(4, 4), (u32)(addr >> 32), (u32)addr, v, op, imm(CLEAR, mark)};
    return push(w, 6);
}
static u64 mark(u32 m) {
    u32 w[1] = {imm(CLEAR, m)};
    return push(w, 1);
}
/* the entries as the guest's ring holds them, little-endian */
static void submit_wait(const u64* e, u32 n, u32 channel, u32 wait_id, u32 wait_val) {
    u8 ring[8 * 8];
    for (u32 i = 0; i < n && i < 8; i++) snx_st_le64(ring + 8 * i, e[i]);
    gpu_submit(ring, n, channel, 0, wait_id, wait_val, 0);
}
static void submit(const u64* e, u32 n, u32 channel) { submit_wait(e, n, channel, ~0u, 0); }
static void submit1(u64 e, u32 channel) { submit(&e, 1, channel); }
static u32 marks(void) {
    pthread_mutex_lock(&seen_lk);
    u32 n = nseen;
    pthread_mutex_unlock(&seen_lk);
    return n;
}
/* the marks so far, if they are exactly these */
static int marks_are(const u32* want, u32 n) {
    pthread_mutex_lock(&seen_lk);
    int ok = nseen == n && !memcmp(seen, want, n * sizeof *want);
    pthread_mutex_unlock(&seen_lk);
    return ok;
}
static void forget_marks(void) {
    pthread_mutex_lock(&seen_lk);
    nseen = 0;
    pthread_mutex_unlock(&seen_lk);
}
/* waits up to 3 s for n marks, and says how long it took */
static u64 wait_marks(u32 n) {
    u64 t0 = plat_ns();
    pthread_mutex_lock(&seen_lk);
    while (nseen < n && plat_ns() - t0 < 3000000000ull) {
        u64 t = plat_ns() + 10000000ull;
        struct timespec ts = {(time_t)(t / 1000000000ull), (long)(t % 1000000000ull)};
        pthread_cond_timedwait(&seen_cv, &seen_lk, &ts);
    }
    pthread_mutex_unlock(&seen_lk);
    return plat_ns() - t0;
}
static void called(void* arg) { see((u32)(uintptr_t)arg); }
static pthread_mutex_t gate_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gate_cv = PTHREAD_COND_INITIALIZER;
static int gate_entered, gate_open;
static void held_call(void* arg) {
    (void)arg;
    pthread_mutex_lock(&gate_lock);
    gate_entered = 1;
    pthread_cond_broadcast(&gate_cv);
    while (!gate_open) pthread_cond_wait(&gate_cv, &gate_lock);
    pthread_mutex_unlock(&gate_lock);
}
/* a release the game's CPU code writes: it stands in for the guest's plain store, which the GPU thread reads with a
   plain read, racing as it does on the console */
static void cpu_store(u64 addr, u32 v) { __atomic_store_n((u32*)guest + addr / 4, snx_le32(v), __ATOMIC_SEQ_CST); }

static int failures;
static void expect(int ok, const char* what) {
    if (!ok) {
        fprintf(stderr, "FAIL: %s\n", what);
        failures++;
    }
}
#define FAST 500000000ull /* a wait an event ends, well short of the 1 and 2 s timeouts */

int main(void) {
    struct timespec ms20 = {0, 20000000};
    gpu_init();

    /* the record of GPU-side writes (maxwell3d.c): a write through one GPU address dirties every GPU page showing the
       same guest memory, 0x3000 and 0xA000 here, and nothing else */
    {
        u64 g = gpu_mem_generation();
        gpu_mem_written(0x3010, 4);
        expect(gpu_mem_dirty_since(g, 0xA000, 0x1000) && gpu_mem_dirty_since(g, 0x3000, 0x20),
               "a write dirties its page and another GPU address of it");
        expect(!gpu_mem_dirty_since(g, 0xB000, 0x1000) && !gpu_mem_dirty_since(g, 0x4000, 0x1000),
               "and not the pages beside them");
        g = gpu_mem_generation();
        gpu_mem_written(0xB800, 4);
        expect(gpu_mem_dirty_since(g, 0x5000, 0x1000) && !gpu_mem_dirty_since(g, 0xA000, 0x1000),
               "a write through the other address dirties the guest address's page");
        g = gpu_mem_generation();
        gpu_mem_written_cpu(0x3FFC, 8); /* guest memory across two pages */
        expect(gpu_mem_dirty_since(g, 0xA000, 0x1000) && gpu_mem_dirty_since(g, 0x4000, 4) &&
                   !gpu_mem_dirty_since(g, 0xB000, 0x1000),
               "a write of guest memory dirties the GPU pages showing it");
    }

    /* NVN temporarily forces rendering in passthrough mode, then replays the default override without ever
       tracking a write to it. Conditional clears must then initialize targets only when their metadata differs. */
    {
        u32 channel = 32;
        u32 commands[] = {imm(0x49, 2),     imm(0x651, 1),
                          imm(0x49, 3),     imm(0x651, 1),
                          imm(0x49, 2),     imm(CLEAR, 0x80),
                          inc(0x554, 3),    0,
                          SEM + 96,         4,
                          imm(CLEAR, 0x81), imm(0x556, 1),
                          imm(CLEAR, 0x82)};
        cpu_store(SEM + 96, 7);
        cpu_store(SEM + 112, 7);
        submit1(push(commands, sizeof commands / sizeof commands[0]), channel);
        gpu_call(called, (void*)(uintptr_t)0x83, &channel, 1);
        wait_marks(3);
        const u32 equal[] = {0x80, 0x82, 0x83};
        expect(marks_are(equal, 3), "restored defaults render until a false condition suppresses a clear");
        forget_marks();
        cpu_store(SEM + 112, 8);
        submit1(push(commands, sizeof commands / sizeof commands[0]), channel);
        gpu_call(called, (void*)(uintptr_t)0x83, &channel, 1);
        wait_marks(4);
        const u32 different[] = {0x80, 0x81, 0x82, 0x83};
        expect(marks_are(different, 4), "restored defaults execute a conditional clear when the values differ");
        forget_marks();
    }

    /* Falcon firmware call 4 signals a waiting MME macro through shadow scratch 0. */
    GpuChannel* fw = calloc(1, sizeof *fw);
    expect(fw != NULL, "firmware test channel allocated");
    if (fw) {
        maxwell3d_method(fw, 0x8C4, 0x1234, 1);
        expect(fw->r3d[0xD00] == 1 && fw->r3d[0x8C4] == 0x1234,
               "firmware call 4 signals completion and keeps its argument");
        free(fw);
    }

    /* a new channel's render enable override is USE_RENDER_ENABLE (0), which deko3d never writes: the render enable
       decides, FALSE skipping a clear until TRUE */
    u32 cond[] = {inc(0x554, 3), 0, 0, 0, imm(CLEAR, 0x5A1), imm(0x556, 1), imm(CLEAR, 0x5A2)};
    submit1(push(cond, sizeof cond / sizeof cond[0]), 19);
    wait_marks(1);
    nanosleep(&ms20, NULL);
    expect(marks_are((u32[]){0x5A2}, 1), "a new channel's render enable FALSE skips, TRUE draws");
    forget_marks();

    /* Executing an entry can change its backing memory. The fetched commands must stay intact, including for a
       contiguous mapping that could otherwise be read through a live guest pointer. */
    u32 fetched[] = {imm(CLEAR, 41), imm(CLEAR, 42)};
    u64 fetched_entry = push(fetched, 2);
    u32* backing = (u32*)(g_mem + (fetched_entry & 0xFFFFFFFFFCull));
    rewrite_value = imm(CLEAR, 99);
    rewrite_on_clear = backing + 1;
    submit1(fetched_entry, 20);
    wait_marks(2);
    const u32 fetched_order[] = {41, 42};
    expect(marks_are(fetched_order, 2), "entry execution uses the fetched commands after backing memory changes");
    expect(snx_le32(backing[1]) == rewrite_value, "the first command changed the second command's backing memory");
    forget_marks();

    /* Commands before SYNC are fetched at submission; the SYNC entry and everything after it see writes
       made before execution. A parked stream holds both kinds safely until its semaphore is released. */
    for (u32 deferred = 0; deferred < 2; deferred++) {
        cpu_store(SEM, 0);
        u64 entries[3] = {sem_then(SEM, 1, 4, 50), mark(51), mark(52)};
        if (deferred) entries[1] |= 1ull << 63;
        submit(entries, 3, 20);
        cpu_store(entries[1] & 0xfffffffffcull, imm(CLEAR, 61));
        cpu_store(entries[2] & 0xfffffffffcull, imm(CLEAR, 62));
        cpu_store(SEM, 1);
        wait_marks(3);
        u32 expected[] = {50, deferred ? 61 : 51, deferred ? 62 : 52};
        expect(marks_are(expected, 3), "GPFIFO SYNC controls command prefetch through the rest of the submission");
        forget_marks();
    }
    cpu_store(SEM, 0);

    /* Reaching SYNC resumes prefetch up to the next SYNC. Executing the first command may release memory
       the CPU immediately reuses; a later fetched entry survives, while a second SYNC sees the new bytes. */
    for (u32 barrier = 0; barrier < 2; barrier++) {
        u64 entries[2] = {mark(71) | (1ull << 63), mark(72)};
        if (barrier) entries[1] |= 1ull << 63;
        rewrite_value = imm(CLEAR, 99);
        rewrite_on_clear = (u32*)(g_mem + (entries[1] & 0xfffffffffcull));
        submit(entries, 2, 20);
        wait_marks(2);
        const u32 expected[] = {71, barrier ? 99 : 72};
        expect(marks_are(expected, 2), "prefetch resumes at SYNC and stops at the next SYNC");
        forget_marks();
    }

    /* channel 21's job waits for SEM to reach 1 (a mark after it, a second entry with another), its next job and a
       call naming it come after; channel 22's job, queued last, releases it (op 2, bit 24: one word) */
    u64 a[2] = {sem_then(SEM, 1, 4, 0xA1), mark(0xA2)};
    u32 c21 = 21, c22 = 22;
    submit(a, 2, 21);
    submit1(mark(0xA3), 21);
    gpu_call(called, (void*)(uintptr_t)0xC0, &c21, 1);
    submit1(sem_then(SEM, 1, 2 | (1u << 24), 0xB1), 22);
    u64 took = wait_marks(5);
    const u32 order1[5] = {0xB1, 0xA1, 0xA2, 0xA3, 0xC0};
    expect(marks_are(order1, 5), "the releasing channel first, then the waiting one's entries and jobs, then the call");
    expect(took < FAST, "the release ended the wait without waiting out its timeout");

    /* a call naming another channel doesn't wait for the parked one, and calls keep their order: channel 21 waits for
       the CPU to write 7 at SEM + 16; a call naming 22 runs, a call naming 21 and a call naming none wait for it */
    forget_marks();
    submit1(sem_then(SEM + 16, 7, 1, 0xD1), 21);
    gpu_call(called, (void*)(uintptr_t)0xD2, &c22, 1);
    wait_marks(1);
    gpu_call(called, (void*)(uintptr_t)0xD3, &c21, 1);
    gpu_call(called, (void*)(uintptr_t)0xD4, NULL, 0);
    nanosleep(&ms20, NULL);
    const u32 order2a[1] = {0xD2};
    expect(marks_are(order2a, 1), "an equal acquire waits while the value differs, and a call naming another runs");
    int epochs_before = __atomic_load_n(&epochs, __ATOMIC_SEQ_CST);
    cpu_store(SEM + 16, 7);
    took = wait_marks(4);
    const u32 order2[4] = {0xD2, 0xD1, 0xD3, 0xD4};
    expect(marks_are(order2, 4), "a release the CPU wrote ended the wait, then the calls in order");
    expect(took < FAST, "the CPU's release was seen soon after");
    expect(__atomic_load_n(&epochs, __ATOMIC_SEQ_CST) > epochs_before,
           "the end of a wait tells the renderer memory may have changed");

    /* an acquire already met: the stream goes on, and the renderer is told memory may have changed (what the CPU
       wrote before releasing it is for the stream to see), on top of the submission's own */
    forget_marks();
    epochs_before = __atomic_load_n(&epochs, __ATOMIC_SEQ_CST);
    submit1(sem_then(SEM + 16, 7, 1, 0xDD), 22);
    took = wait_marks(1);
    expect(marks() == 1 && took < FAST, "an acquire already met doesn't wait");
    expect(__atomic_load_n(&epochs, __ATOMIC_SEQ_CST) >= epochs_before + 2,
           "a wait already met tells the renderer too, as the submission does");

    /* a submission waiting for a syncpoint's fence, incremented off the GPU thread */
    forget_marks();
    u64 e = mark(0xE1);
    submit_wait(&e, 1, 23, 30, 1);
    nanosleep(&ms20, NULL);
    expect(marks() == 0, "a submission waits for its fence");
    syncpt_incr(30);
    took = wait_marks(1);
    expect(marks() == 1 && took < FAST, "the syncpoint increment started the submission");

    /* the puller's syncpoint wait in the middle of an incrementing batch (0x1C payload, 0x1D wait for syncpoint 31,
       then 0x1E and 0x1F, puller registers whose words would read as marks if the stream resumed in the wrong
       place), and a mark in the job's next entry */
    forget_marks();
    u32 w[6] = {imm(CLEAR, 0xF1), inc(0x1C, 4), 1, 31u << 8, imm(CLEAR, 0x666), imm(CLEAR, 0x667)};
    u32 w2[1] = {imm(CLEAR, 0xF2)};
    u64 f[2] = {push(w, 6), push(w2, 1)};
    submit(f, 2, 24);
    wait_marks(1);
    nanosleep(&ms20, NULL);
    const u32 order3a[1] = {0xF1};
    expect(marks_are(order3a, 1), "the stream stops at the puller's syncpoint wait");
    syncpt_incr(31);
    took = wait_marks(2);
    const u32 order3[2] = {0xF1, 0xF2};
    expect(marks_are(order3, 2) && took < FAST, "it resumes after the wait, in the middle of the batch");

    /* a macro that acquires a report semaphore (its first parameter is the 3D 0x6C3 word, GE) and then clears with a
       mark: it stops after the acquire, and its clear comes only after channel 26's 3D report release */
    forget_marks();
    const u32 macro[5] = {
        1u | 2u << 4 | 0x6C3u << 14,           /* method = 0x6C3 */
        1u | 4u << 4 | 1u << 11,               /* send r1, the first parameter: the acquire */
        1u | 2u << 4 | (u32)CLEAR << 14,       /* method = ClearBuffers */
        1u | 4u << 4 | 1u << 7 | 0x6D1u << 14, /* send the mark, and exit */
        1u | 1u << 4,                          /* the exit's delay slot: r0 = r0 */
    };
    u32 m[32], n = 0;
    m[n++] = imm(0x45, 0); /* upload at word 0 */
    m[n++] = (3u << 29) | (5u << 16) | 0x46;
    for (int i = 0; i < 5; i++) m[n++] = macro[i];
    m[n++] = imm(0x47, 0); /* macro 0 starts at word 0 */
    m[n++] = imm(0x48, 0);
    m[n++] = inc(0x6C0, 3); /* the report semaphore: SEM + 32, 5 */
    m[n++] = 0;
    m[n++] = SEM + 32;
    m[n++] = 5;
    m[n++] = inc(0xE00, 1); /* macro 0, its one parameter */
    m[n++] = 1u | 1u << 16;
    m[n++] = imm(CLEAR, 0x6D2);
    submit1(push(m, n), 25);
    nanosleep(&ms20, NULL);
    expect(marks() == 0, "the macro stops at its acquire, before its clear");
    u32 r[6] = {inc(0x6C0, 4), 0, SEM + 32, 5, 1u << 28, imm(CLEAR, 0x6E1)}; /* release, one word */
    submit1(push(r, 6), 26);
    took = wait_marks(3);
    const u32 order4[3] = {0x6E1, 0x6D1, 0x6D2};
    expect(marks_are(order4, 3), "the macro goes on after the other channel's release, before the stream does");
    expect(took < FAST, "the report release ended the macro's wait at once");

    /* entries across the two pages mapped apart, one after another: the copy's buffer grown (2 words each side, then
       4), then kept for a smaller one (2 again) whose stale tail must not run */
    forget_marks();
    for (u32 k = 0; k < 3; k++) {
        u32 half = k == 1 ? 4 : 2; /* words before the page's end, the same after it */
        for (u32 i = 0; i < half; i++) {
            u32 wi = imm(CLEAR, 0x8A0 + k * 16 + i);
            snx_st_le32(g_mem + 0x4000 - 4 * half + 4 * i, wi);
            wi = imm(CLEAR, 0x8B0 + k * 16 + i);
            snx_st_le32(g_mem + 0x5000 + 4 * i, wi);
        }
        submit1((0xB000 - 4 * half) | ((u64)(2 * half) << 42), 28);
        took = wait_marks(2 * half);
        nanosleep(&ms20, NULL); /* a stale tail would show as more marks */
        expect(marks() == 2 * half && took < FAST, "a pushbuffer across pages mapped apart runs from its copy");
        pthread_mutex_lock(&seen_lk);
        int ok = seen[0] == 0x8A0 + k * 16 && seen[half] == 0x8B0 + k * 16 &&
                 seen[2 * half - 1] == 0x8B0 + k * 16 + half - 1;
        pthread_mutex_unlock(&seen_lk);
        expect(ok, "its words in order, from both pages");
        forget_marks();
    }

    /* A preceding semaphore wait lets the CPU produce the source after submission. The scanner must not
       snapshot either this job or later jobs on the same channel from the pre-wait state. */
    for (int gpu_write = 0; gpu_write < 2; gpu_write++) {
        cpu_store(SEM + 64, 0);
        cpu_store(0x9000, 0x11223344);
        submit1(sem_then(SEM + 64, 1, 1, 0x910), 30);
        u32 copy[] = {inc(0x100, 7) | (4u << 13), 0, 0x9000, 0, 0x9100, 4, 4, 4,
                      imm(0xC0, 0x180) | (4u << 13), imm(CLEAR, 0x911)};
        submit1(push(copy, sizeof copy / sizeof copy[0]), 30);
        if (gpu_write) gmem_w32(0x9000, 0x55667788);
        else cpu_store(0x9000, 0x55667788);
        cpu_store(SEM + 64, 1);
        wait_marks(2);
        expect(marks_are((u32[]){0x910, 0x911}, 2), "DMA follows the channel's earlier wait");
        expect(gmem_r32(0x9100) == 0x55667788, "DMA reads the source after an earlier channel wait");
        forget_marks();
    }
    for (int gpu_write = 0; gpu_write < 2; gpu_write++) {
        cpu_store(0x9000, 0x11223344);
        u32 copy[] = {inc(0x100, 7) | (4u << 13), 0, 0x9000, 0, 0x9100, 4, 4, 4,
                      imm(0xC0, 0x180) | (4u << 13), imm(CLEAR, 0x912)};
        pthread_mutex_lock(&gate_lock);
        gate_entered = gate_open = 0;
        pthread_mutex_unlock(&gate_lock);
        gpu_call(held_call, NULL, NULL, 0);
        pthread_mutex_lock(&gate_lock);
        while (!gate_entered) pthread_cond_wait(&gate_cv, &gate_lock);
        pthread_mutex_unlock(&gate_lock);
        submit1(push(copy, sizeof copy / sizeof copy[0]), 31);
        if (gpu_write)
            gmem_w32(0x9000, 0x55667788);
        else
            cpu_store(0x9000, 0x55667788);
        pthread_mutex_lock(&gate_lock);
        gate_open = 1;
        pthread_cond_broadcast(&gate_cv);
        pthread_mutex_unlock(&gate_lock);
        wait_marks(1);
        expect(gmem_r32(0x9100) == (gpu_write ? 0x55667788 : 0x11223344),
               "DMA keeps CPU staging bytes but discards a snapshot after a GPU write");
        forget_marks();
    }
    /* Both report engines can wait for CPU-produced DMA input. Use fresh channels so a previous wait cannot
       already have disabled their submission scanner. */
    for (u32 sub = 0; sub < 2; sub++) {
        cpu_store(SEM + 80, 0);
        cpu_store(0x9000, 0x11223344);
        u32 copy[] = {inc(0x6C0, 4) | (sub << 13), 0, SEM + 80, 1, 1,
                      inc(0x100, 7) | (4u << 13), 0, 0x9000, 0, 0x9100, 4, 4, 4,
                      imm(0xC0, 0x180) | (4u << 13), imm(CLEAR, 0x913)};
        submit1(push(copy, sizeof copy / sizeof copy[0]), 32 + sub);
        cpu_store(0x9000, 0x55667788);
        cpu_store(SEM + 80, 1);
        wait_marks(1);
        expect(gmem_r32(0x9100) == 0x55667788, "DMA reads CPU data produced after a 3D or compute report acquire");
        forget_marks();
    }
    /* Citron's Peach fix: compute-inline payloads need the earlier GPU
       result, including a continuation entry and discontiguous source pages. */
    for (u32 layout = 0; layout < 3; layout++) {
        const u32 output = 0x9400;
        u32 commands[] = {inc(0x60, 5) | (1u << 13), 16, 1, 0, output, 16,
                          imm(0x6C, 1) | (1u << 13),
                          (3u << 29) | (4u << 16) | (1u << 13) | 0x6D,
                          0, 0, 0, 0, imm(CLEAR, 0x920 + layout)};
        u64 entries[2];
        u32 count = layout ? 2 : 1;
        entries[0] = push(commands, layout ? 8 : 13);
        if (layout == 1) entries[1] = push(commands + 8, 5);
        if (layout == 2) {
            const u64 source = 0xAFF8;
            u8 bytes[20];
            for (u32 i = 0; i < 5; i++) snx_st_le32(bytes + i * 4, commands[8 + i]);
            gmem_write_raw(source, bytes, sizeof bytes);
            entries[1] = source | 5ull << 42;
        }
        pending_inline.addr = layout ? gpfifo_addr(entries[1]) : gpfifo_addr(entries[0]) + 32;
        pending_inline.reads = 0;
        for (u32 i = 0; i < 4; i++) {
            pending_inline.words[i] = 0x87654321 + i;
            cpu_store(output + i * 4, 0);
        }
        g_cs_any = 1;
        submit(entries, count, 34 + layout);
        wait_marks(1);
        expect(marks_are((u32[]){0x920 + layout}, 1), "compute-inline command finishes");
        for (u32 i = 0; i < 4; i++)
            expect(gmem_r32(output + i * 4) == pending_inline.words[i], "compute-inline consumes the GPU result");
        expect(pending_inline.reads == 1, "compute-inline resolves its pending input once per run");
        pending_inline.addr = 0;
        g_cs_any = 0;
        forget_marks();
    }
    expect(!warned, "no wait timed out");

    /* a report acquire nothing releases goes on after its 1 s, with a warning */
    forget_marks();
    u32 t[6] = {inc(0x6C0, 4), 0, SEM + 48, 9, 1, imm(CLEAR, 0x7A1)};
    submit1(push(t, 6), 27);
    took = wait_marks(1);
    expect(marks() == 1 && took >= 900000000ull && warned == 1, "a wait nothing ends times out after 1 s and warns");

    if (!failures) printf("gpu_park: ok\n");
    return failures != 0;
}
