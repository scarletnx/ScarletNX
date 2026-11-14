/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* gpu/gpu_queue.c with no device and no gpu.c: the front end's guest writes, releases and block linear uploads go
   through the ring in their order, each with its bytes as the front end gave them, also when the ring wraps and when
   a write is too big for it. Only the front end's thread queues, and never while it runs a record. A queued write
   marks each page it covers with its fence (written[]) */
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
static u64 fnv(const void* p, u64 n) {
    u64 h = 0xcbf29ce484222325ull;
    for (u64 i = 0; i < n; i++) h = (h ^ ((const u8*)p)[i]) * 0x100000001b3ull;
    return h;
}
static void note(char kind, u64 gva, const void* p, u64 n) {
    if (t_gq_run && gq_queues()) queued_in_run++; /* a record runs, so its writes go to memory */
    if (nran < RAN_MAX) ran[nran++] = (Ran){kind, gva, n, fnv(p, n)};
}
int gmem_write_now(u64 gva, const void* src, u64 size) {
    note('W', gva, src, size);
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

int main(void) {
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
