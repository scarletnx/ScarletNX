/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* gpu/gpu_queue.c with no device and no gpu.c: the front end's guest writes, releases and block linear uploads go
   through the ring in their order, each with its bytes as the front end gave them, also when the ring wraps and when
   a write is too big for it. Only the front end's thread queues, and never while it runs a record. A queued write
   marks each page it covers with its fence (written[]).

   With the argument split, gpu_split's renderer thread runs the records: the front end goes on while it is behind, a
   read of the front end waits only for a queued write over its bytes, and a read that queued writes cover takes their
   bytes over memory's with no wait */
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nx/gpu/gpu_queue.c"

/* ------------------------------------------------ what gpu_queue.c reaches outside itself */
void logf_(int lvl, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    if (!lvl) vfprintf(stderr, fmt, ap);
    va_end(ap);
}
void fatal(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    exit(1);
}
static int on_thread = 1;
int gpu_on_thread(void) { return on_thread; }

/* what ran, in its order: the kind (W write, R release, I block linear upload), the address and a hash of the bytes */
typedef struct {
    char kind;
    u64 gva, size, hash;
} Ran;
#define RAN_MAX 40000
static Ran ran[RAN_MAX];
static u32 nran;
static int queued_in_run; /* a record's own work found gq_queues() set */
static pthread_t front;  /* the front end's thread */
static int off_front;    /* records that ran on another thread */
/* guest memory, as the records leave it */
static u8 mem[1u << 16];
void gmem_read_quiet(u64 gva, void* dst, u64 size) { memcpy(dst, mem + gva, (size_t)size); }
static u64 fnv(const void* p, u64 n) {
    u64 h = 0xcbf29ce484222325ull;
    for (u64 i = 0; i < n; i++) h = (h ^ ((const u8*)p)[i]) * 0x100000001b3ull;
    return h;
}
static void note(char kind, u64 gva, const void* p, u64 n) {
    if (t_gq_run && gq_queues()) queued_in_run++; /* a record runs, so its writes go to memory */
    if (nran < RAN_MAX) ran[nran++] = (Ran){kind, gva, n, fnv(p, n)};
    if (!pthread_equal(pthread_self(), front)) off_front++;
}
int gmem_write_now(u64 gva, const void* src, u64 size) {
    note('W', gva, src, size);
    if (gva + size <= sizeof mem) memcpy(mem + gva, src, (size_t)size);
    return 0;
}
void gpu_release_write_now(u64 gva, const void* data, u32 len) { note('R', gva, data, len); }
void gpu_i2m_block_now(const GpuI2m* t) {
    note('I', t->regs.dest.address, t->inner_buffer, t->copy_size);
    if (t->regs.line_count != 7 || t->regs.dest.block_height != 3) queued_in_run += 1000; /* the registers came along */
}

static int failed, checks;
#define CHECK(cond, ...)                                                                                               \
    do {                                                                                                               \
        checks++;                                                                                                      \
        if (!(cond)) {                                                                                                 \
            failed++;                                                                                                  \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);                                                       \
            fprintf(stderr, __VA_ARGS__);                                                                              \
            fputc('\n', stderr);                                                                                       \
        }                                                                                                              \
    } while (0)

/* the expected log, as the test put the work in */
static Ran want[RAN_MAX];
static u32 nwant;
static u8 buf[3u << 20];
static void fill(u8* p, u64 n, u32 seed) {
    for (u64 i = 0; i < n; i++) p[i] = (u8)(seed * 131 + i * 7 + (i >> 8));
}
static void put_write(u64 gva, u64 n, u32 seed) {
    fill(buf, n, seed);
    want[nwant++] = (Ran){'W', gva, n, fnv(buf, n)};
    gq_write(gva, buf, n);
    memset(buf, 0xEE, (size_t)n); /* the queue keeps its own copy */
}
static void put_release(u64 gva, u32 v) {
    u8 b[4] = {(u8)v, (u8)(v >> 8), (u8)(v >> 16), (u8)(v >> 24)};
    want[nwant++] = (Ran){'R', gva, 4, fnv(b, 4)};
    gq_release(gva, b, 4);
}
static void put_i2m(u64 gva, u32 n, u32 seed, u64 at, u64 size) {
    GpuI2m t = {0};
    fill(buf, n, seed);
    t.regs.dest.address = gva;
    t.regs.line_count = 7;
    t.regs.dest.block_height = 3;
    t.inner_buffer = buf;
    t.copy_size = n;
    want[nwant++] = (Ran){'I', gva, n, fnv(buf, n)};
    gq_i2m_block(&t, at, size);
    memset(buf, 0xEE, n);
}
static void check_log(const char* what) {
    gq_drain();
    int same = nran == nwant;
    for (u32 i = 0; same && i < nran; i++)
        same = ran[i].kind == want[i].kind && ran[i].gva == want[i].gva && ran[i].size == want[i].size &&
               ran[i].hash == want[i].hash;
    CHECK(same, "%s: %u records ran, %u queued, or not in their order with their bytes", what, nran, nwant);
    CHECK(!queued_in_run, "%s: a record's own work was queued again (%d)", what, queued_in_run);
    CHECK(__atomic_load_n(&g_gq.done, __ATOMIC_ACQUIRE) == g_gq.queued, "%s: the last fence is not done", what);
    nran = nwant = 0;
    queued_in_run = 0;
}

/* ------------------------------------------------ gpu_split=1: the renderer thread runs the records */
static pthread_mutex_t gate_lk = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gate_cv = PTHREAD_COND_INITIALIZER;
static int gate_open, gate_held;
/* a record that holds the renderer thread until the gate opens */
static void held(const void* arg, u64 bytes) {
    (void)arg, (void)bytes;
    pthread_mutex_lock(&gate_lk);
    gate_held = 1;
    pthread_cond_broadcast(&gate_cv);
    while (!gate_open) pthread_cond_wait(&gate_cv, &gate_lk);
    gate_open = gate_held = 0;
    pthread_mutex_unlock(&gate_lk);
}
static void hold(void) {
    gq_call(held, NULL, 0);
    pthread_mutex_lock(&gate_lk);
    while (!gate_held) pthread_cond_wait(&gate_cv, &gate_lk);
    pthread_mutex_unlock(&gate_lk);
}
static void open_gate(void) {
    pthread_mutex_lock(&gate_lk);
    gate_open = 1;
    pthread_cond_broadcast(&gate_cv);
    pthread_mutex_unlock(&gate_lk);
}
static void* open_later(void* arg) {
    (void)arg;
    struct timespec ts = {0, 300000000};
    nanosleep(&ts, NULL);
    open_gate();
    return NULL;
}
static u64 now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (u64)ts.tv_sec * 1000000000ull + (u64)ts.tv_nsec;
}
static int split_main(void) {
    g_settings.gpu_split = 1;
    front = pthread_self();
    hold(); /* the ring and the renderer thread start here */
    u8 a8[16], b4[4], got[16];
    memset(a8, 0xAA, sizeof a8);
    memset(b4, 0xBB, sizeof b4);
    memset(mem + 0x1000, 0x11, 32);
    want[nwant++] = (Ran){'W', 0x1000, 16, fnv(a8, 16)};
    gq_write(0x1000, a8, 16);
    want[nwant++] = (Ran){'W', 0x1008, 4, fnv(b4, 4)};
    gq_write(0x1008, b4, 4);
    CHECK(nran == 0 && g_gq.done < g_gq.queued, "the front end waited for the renderer thread to run its writes");
    /* a read over both writes and memory: memory's bytes, then each write's over them in their order */
    memset(got, 0, sizeof got);
    CHECK(gq_forward(0x1000, got, 16) == 1, "a read that queued writes cover was not served from them");
    static const u8 want16[16] = {0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA,
                                  0xBB, 0xBB, 0xBB, 0xBB, 0xAA, 0xAA, 0xAA, 0xAA};
    CHECK(!memcmp(got, want16, 16), "a read over two queued writes did not lay the newer over the older");
    memset(got, 0, sizeof got);
    CHECK(gq_forward(0x100C, got, 8) == 1 && got[0] == 0xAA && got[3] == 0xAA && got[4] == 0x11 && got[7] == 0x11,
          "a read past a queued write did not take memory's bytes for the rest");
    /* a read of bytes that no queued write covers neither waits nor forwards */
    CHECK(gq_forward(0x3000, got, 4) == 0, "a read of bytes no write covers was forwarded");
    gq_read_wait(0x3000, 4);
    CHECK(nran == 0, "a read of bytes no write covers waited for the renderer thread");
    /* a read of a queued write's bytes waits for that write */
    pthread_t opener;
    pthread_create(&opener, NULL, open_later, NULL);
    u64 t0 = now_ns();
    gq_read_wait(0x1008, 4);
    u64 waited = now_ns() - t0;
    CHECK(waited >= 200000000ull, "a read of a queued write's bytes did not wait for it (%llu ns)",
          (unsigned long long)waited);
    CHECK(mem[0x1008] == 0xBB && mem[0x1000] == 0xAA, "after the wait, memory lacks the queued writes");
    pthread_join(opener, NULL);
    check_log("split reads");

    /* many records of many sizes, with the renderer thread held at times: the ring wraps several times */
    off_front = 0;
    for (u32 i = 0; i < 30000; i++) {
        if (i % 5000 == 0) {
            hold();
            open_gate();
        }
        if (i % 7 == 3)
            put_release(0x8000 + 4 * (i % 64), i);
        else
            put_write(0x100000 + (u64)i * 64, 1 + (i * 97) % 2000, i);
    }
    put_write(0x300000, sizeof buf, 7); /* too big for the ring: after the queued ones */
    put_write(0x200040, 64, 8);
    check_log("split wraps");
    CHECK(off_front == 30001, "%d of 30001 records ran on the renderer thread", off_front);
    if (failed) {
        fprintf(stderr, "gpu_queue split: %d of %d checks failed\n", failed, checks);
        return 1;
    }
    printf("gpu_queue split: %d checks passed\n", checks);
    return 0;
}

int main(int argc, char** argv) {
    front = pthread_self();
    if (argc > 1 && !strcmp(argv[1], "split")) return split_main();
    g_settings.gpu_split = 0; /* the records run on this thread, as they are queued */
    /* only the front end's thread queues */
    on_thread = 0;
    CHECK(!gq_queues(), "a thread other than the front end's queues");
    on_thread = 1;
    CHECK(gq_queues(), "the front end's thread does not queue");

    /* the three kinds in their order */
    put_write(0x10000, 64, 1);
    put_release(0x20000, 0x12345678);
    put_i2m(0x30000, 300, 2, 0x30000, 0x4000);
    put_write(0x10010, 8, 3);
    check_log("kinds");

    /* each page a write covers has its fence, and only those */
    put_write(0x41FF0, 0x2020, 4); /* pages 0x41 to 0x44 */
    u64 f = g_gq.queued;
    for (u64 p = 0x41; p <= 0x44; p++)
        CHECK(g_gq.written[gq_slot(p)] == f, "page %#llx of a write has fence %llu, not %llu", (unsigned long long)p,
              (unsigned long long)g_gq.written[gq_slot(p)], (unsigned long long)f);
    CHECK(g_gq.written[gq_slot(0x40)] != f && g_gq.written[gq_slot(0x45)] != f, "a page next to a write has its fence");
    /* a block linear upload marks the span it was given, not its source's size */
    put_i2m(0x50000, 16, 5, 0x60000, 0x3000);
    f = g_gq.queued;
    CHECK(g_gq.written[gq_slot(0x60)] == f && g_gq.written[gq_slot(0x62)] == f, "an upload's span lacks its fence");
    check_log("marks");

    /* many records of many sizes: the ring wraps several times */
    for (u32 i = 0; i < 30000; i++) {
        if (i % 7 == 3)
            put_release(0x8000 + 4 * (i % 64), i);
        else
            put_write(0x100000 + (u64)i * 64, 1 + (i * 97) % 2000, i);
    }
    check_log("wraps");

    /* a write too big for the ring goes after the queued ones */
    put_write(0x200000, 64, 6);
    put_write(0x300000, sizeof buf, 7);
    put_write(0x200040, 64, 8);
    check_log("big");

    if (failed) {
        fprintf(stderr, "gpu_queue: %d of %d checks failed\n", failed, checks);
        return 1;
    }
    printf("gpu_queue: %d checks passed\n", checks);
    return 0;
}
