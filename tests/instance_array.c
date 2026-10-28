/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* VERTEX_ARRAY_INSTANCE_FIRST and _SUBSEQUENT (3D 0x485, 0x486): the 3D engine's methods run against a renderer that
   records the draws reaching it. FIRST draws its range as instance 0; each SUBSEQUENT of the same range is one more
   instance of that draw. */
#include "nx/gpu/gpu.h"
#include "nx/gpu/maxwell/engines_int.h"
#include "nx/gpu/renderer.h"
#include "nx/hle/services/nvdrv.h"
#include "nx/memory/mem.h"
#include "nx/runtime/clock.h"
#include "nx/runtime/platform/log.h"
#include "nx/runtime/platform/plat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

u8* g_mem;
u64 g_draws;
int g_renderer_up = 1;
typedef struct {
    int indexed;
    u32 topology, first, count, base_vertex, base_instance, instances;
} Draw;
static Draw draws[8];
static int ndraws;
static int barriers[8], nbarriers, signals;
static void record_draw(const DsState* ds, const DrawCall* dc, DrawOwner* o) {
    (void)ds, (void)o;
    if (ndraws < 8)
        draws[ndraws] = (Draw){(int)dc->indexed, dc->topology,      dc->first,    dc->count,
                               dc->base_vertex,  dc->base_instance, dc->instances};
    ndraws++;
}
static void no_clear(const DsState* ds, const DrawClear* c, const DrawOwner* o) { (void)ds, (void)c, (void)o; }
static void record_barrier(int kind) {
    if (nbarriers < 8) barriers[nbarriers++] = kind;
}
static const Renderer recording = {
    .name = "recording", .clear = no_clear, .draw = record_draw, .method_barrier = record_barrier};
const Renderer* g_renderer = &recording;
int gmem_read(u64 gva, void* dst, u64 size) {
    (void)gva;
    memset(dst, 0, size);
    return 0;
}
int gmem_write(u64 gva, const void* src, u64 size) { return (void)gva, (void)src, (void)size, 0; }
u32 gmem_r32(u64 gva) { return (void)gva, 0; }
void gmem_w32(u64 gva, u32 v) { (void)gva, (void)v; }
u64 gmmu_translate(u64 gva) { return gva; }
/* maxwell3d.c's runs of contiguous pages, from the translation above */
u64 gmmu_run(u64 gva, u64 max, u64* cpu) {
    u64 n, first = gmmu_translate(gva);
    if (!max || !first) return 0;
    *cpu = first;
    n = 0x1000 - (gva & 0xFFF);
    while (n < max && gmmu_translate(gva + n) == first + n) n += 0x1000;
    return n < max ? n : max;
}
/* nvdrv.c's index of mappings by guest address, for the translation above */
void gmmu_aliases(u64 cpu, u64 size, void (*fn)(void* ctx, u64 gva, u64 n), void* ctx) { fn(ctx, cpu, size); }
u32 gpu_color_bpp(u32 format) { return (void)format, 4; }
u64 bl_offset(u32 x_bytes, u32 y, u32 z, u32 width_bytes, u32 height, u32 bh_log2, u32 bd_log2) {
    (void)x_bytes, (void)y, (void)z, (void)width_bytes, (void)height, (void)bh_log2, (void)bd_log2;
    return 0;
}
void gpu_signal_point(void) { signals++; }
void gpu_release_write(u64 gva, const void* data, u32 len) { (void)gva, (void)data, (void)len; }
u64 guest_ns(void) { return 0; }
void gpu_park(GpuChannel* ch, u32 kind, u64 addr, u32 val, int geq) {
    (void)ch, (void)kind, (void)addr, (void)val, (void)geq;
}
void pm_lock(PMutex* m) { (void)m; }
void pm_unlock(PMutex* m) { (void)m; }
void i2m_launch(GpuChannel* ch, u32* regs, int base) { (void)ch, (void)regs, (void)base; }
void i2m_data(GpuChannel* ch, u32 v) { (void)ch, (void)v; }
void mme_run(GpuChannel* ch, u32 idx, u32 np) { (void)ch, (void)idx, (void)np; }
void mme_upload_ptr(GpuChannel* ch, u32 v) { (void)ch, (void)v; }
void mme_upload(GpuChannel* ch, u32 v) { (void)ch, (void)v; }
void mme_bind(GpuChannel* ch, u32 v) { (void)ch, (void)v; }
void logf_(int lvl, const char* fmt, ...) { (void)lvl, (void)fmt; }

static GpuChannel ch;
static int failures;
static u32 word(u32 topology, u32 first, u32 count) { return first | count << 16 | topology << 28; }
static void expect(int n, const char* what) {
    r3d_pending_flush(); /* what something else in the stream would do */
    if (ndraws != n) {
        fprintf(stderr, "FAIL: %s: %d draws, expected %d\n", what, ndraws, n);
        failures++;
    }
}
static void expect_draw(int i, u32 topology, u32 first, u32 count, u32 instances, const char* what) {
    const Draw* d = &draws[i];
    if (i >= ndraws || d->indexed || d->topology != topology || d->first != first || d->count != count ||
        d->instances != instances) {
        fprintf(stderr, "FAIL: %s: draw %d is topology %u first %u count %u x%u, expected %u %u %u x%u\n", what, i,
                d->topology, d->first, d->count, d->instances, topology, first, count, instances);
        failures++;
    }
}

int main(void) {
    maxwell3d_method(&ch, 0x485, word(4, 0, 6), 1);
    expect(1, "FIRST draws");
    expect_draw(0, 4, 0, 6, 1, "FIRST is one instance of its range");

    ndraws = 0;
    maxwell3d_method(&ch, 0x485, word(4, 6, 6), 1);
    for (int i = 0; i < 3; i++) maxwell3d_method(&ch, 0x486, word(4, 6, 6), 1);
    expect(1, "FIRST and three SUBSEQUENT are one draw");
    expect_draw(0, 4, 6, 6, 4, "FIRST and three SUBSEQUENT are four instances");

    ndraws = 0;
    maxwell3d_method(&ch, 0x485, word(4, 0, 6), 1);
    maxwell3d_method(&ch, 0x486, word(5, 0, 6), 1);
    expect(2, "a SUBSEQUENT of another topology is a draw of its own");
    expect_draw(0, 4, 0, 6, 1, "the FIRST before it");
    expect_draw(1, 5, 0, 6, 1, "only instance 1 of the SUBSEQUENT");
    if (draws[1].base_instance != 1) {
        fprintf(stderr, "FAIL: nonmerging subsequent must start at instance 1\n");
        failures++;
    }

    memset(&ch, 0, sizeof ch);
    ndraws = 0;
    ch.r3d[0x50D] = 7;
    ch.r3d[0x50E] = 2;
    maxwell3d_method(&ch, 0x5F9, word(4, 3, 6), 1);
    maxwell3d_method(&ch, 0x5FC, word(4, 3, 6), 1);
    expect(1, "packed indexed FIRST and SUBSEQUENT merge");
    if (draws[0].indexed != 1 || draws[0].topology != 4 || draws[0].first != 3 || draws[0].count != 6 ||
        draws[0].base_vertex != 7 || draws[0].base_instance != 2 || draws[0].instances != 2) {
        fprintf(stderr, "FAIL: packed indexed draw parameters\n");
        failures++;
    }
    for (u32 method = 0x5F9; method <= 0x5FE; method++) {
        memset(&ch, 0, sizeof ch);
        ndraws = 0;
        maxwell3d_method(&ch, method, word(4, 2, 3), 1);
        expect(1, "each packed indexed opcode draws");
        if (draws[0].indexed != 1 || draws[0].first != 2 || draws[0].count != 3 ||
            draws[0].instances != 1 || draws[0].base_instance != (method >= 0x5FC ? 1u : 0u)) {
            fprintf(stderr, "FAIL: packed indexed opcode %x\n", method);
            failures++;
        }
    }
    memset(&ch, 0, sizeof ch);
    ndraws = 0;
    maxwell3d_method(&ch, 0x5F9, word(4, 2, 3), 1);
    maxwell3d_method(&ch, 0x5F7, 2, 1);
    maxwell3d_method(&ch, 0x5F8, 3, 1);
    maxwell3d_method(&ch, 0x586, 4 | (1u << 26), 1);
    maxwell3d_method(&ch, 0x585, 0, 1);
    expect(2, "packed indexed draw and BEGIN/END draw remain separate");

    memset(&ch, 0, sizeof ch);
    ndraws = 0;
    ch.r3d[0x652] = 1;
    ch.r3d[0x65C] = 5;
    maxwell3d_method(&ch, 0x485, word(4, 0, 3), 1);
    expect(1, "topology override draw");
    expect_draw(0, 5, 0, 3, 1, "separate topology overrides BEGIN topology");
    for (u32 override = 1; override <= 2; ++override) {
        memset(&ch, 0, sizeof ch);
        ndraws = 0;
        ch.r3d[0x652] = 1;
        ch.r3d[0x65C] = override;
        maxwell3d_method(&ch, 0x485, word(4, 0, 3), 1);
        expect(1, "point or line topology override draw");
        expect_draw(0, override - 1, 0, 3, 1, "override enum maps to renderer enum");
    }

    memset(&ch, 0, sizeof ch);
    ndraws = 0;
    maxwell3d_method(&ch, 0x586, 4, 1);
    maxwell3d_method(&ch, 0x4C1, 0x03020100, 1);
    maxwell3d_method(&ch, 0x57C, 0x00050004, 1);
    maxwell3d_method(&ch, 0x57A, 6, 1);
    if (ch.inline_n != 7 || memcmp(ch.inline_indices, (u32[]){0, 1, 2, 3, 4, 5, 6}, 7 * sizeof(u32))) {
        fprintf(stderr, "FAIL: inline indices were decoded in the wrong order\n");
        failures++;
    }
    maxwell3d_method(&ch, 0x585, 0, 1);
    expect(1, "packed inline indices draw");
    if (draws[0].indexed != 3 || draws[0].count != 7 || ch.inline_n != 0) {
        fprintf(stderr, "FAIL: inline index draw count or reset\n");
        failures++;
    }
    free(ch.inline_indices);

    memset(&ch, 0, sizeof ch);
    maxwell3d_method(&ch, 0x35D, 7, 1); /* default TRACK */
    maxwell3d_method(&ch, 0x49, 3, 1);  /* REPLAY */
    maxwell3d_method(&ch, 0x35D, 19, 1);
    if (ch.r3d[0x35D] != 7) {
        fprintf(stderr, "FAIL: shadow RAM replay did not restore tracked value\n");
        failures++;
    }
    maxwell3d_method(&ch, 0x49, 2, 1); /* PASSTHROUGH uses the incoming control word */
    maxwell3d_method(&ch, 0x35D, 19, 1);
    if (ch.r3d[0x35D] != 19 || ch.r3d_shadow[0x35D] != 7) {
        fprintf(stderr, "FAIL: shadow RAM passthrough changed the tracked value\n");
        failures++;
    }
    maxwell3d_method(&ch, 0x49, 1, 1); /* TRACK_WITH_FILTER tracks like TRACK */
    maxwell3d_method(&ch, 0x35D, 23, 1);
    if (ch.r3d_shadow[0x35D] != 23) {
        fprintf(stderr, "FAIL: shadow RAM track-with-filter did not track\n");
        failures++;
    }

    memset(&ch, 0, sizeof ch);
    ndraws = nbarriers = signals = 0;
    maxwell3d_method(&ch, 0x485, word(4, 0, 3), 1);
    maxwell3d_method(&ch, 0x378, 0, 1);
    maxwell3d_method(&ch, 0x3DF, 0, 1);
    maxwell3d_method(&ch, 0x3DD, 0, 1);
    maxwell3d_method(&ch, 0x509, 0, 1);
    maxwell3d_method(&ch, 0x50A, 0, 1);
    maxwell3d_method(&ch, 0x44, 0, 1);
    /* neither the texture cache invalidation nor WAIT_FOR_IDLE lands shader results: the game observes neither */
    if (ndraws != 1 || nbarriers != 6 || memcmp(barriers, (int[]){0, 1, 2, 3, 3, 4}, sizeof(int) * 6) || signals) {
        fprintf(stderr, "FAIL: Maxwell barriers or idle were not dispatched in stream order\n");
        failures++;
    }

    if (failures) return 1;
    puts("VERTEX_ARRAY_INSTANCE_FIRST draws its range; each SUBSEQUENT of it is one more instance");
    return 0;
}
