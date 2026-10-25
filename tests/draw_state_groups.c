/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The 3D engine's register groups (g_m3d_group, gpu.h R3D_*) against what the draw state reads (maxwell/draw_state.c):
   every register a group of the draw state reads, written through maxwell3d_method, leaves the state synced from the
   groups equal to the registers read again, and gives the groups that changed a new generation. A register the state
   reads but its group misses would keep a draw's state stale. The registers each group reads are found here, by
   changing one register at a time, not listed by hand. */
#include "nx/gpu/gpu.h"
#include "nx/gpu/maxwell/draw_state.h"
#include "nx/gpu/maxwell/engines_int.h"
#include "nx/gpu/maxwell/maxwell_3d_regs.h"
#include "nx/gpu/renderer.h"
#include "nx/hle/services/nvdrv.h"
#include "nx/memory/mem.h"
#include "nx/runtime/clock.h"
#include "nx/runtime/platform/log.h"
#include "nx/runtime/platform/plat.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
/* ------------------------------------------------ what engines.c and maxwell3d.c reach outside themselves */
u8* g_mem;
u64 g_draws;
int g_renderer_up = 1;
static const Renderer nothing = {.name = "nothing"};
const Renderer* g_renderer = &nothing;
#define VA 0x100000ull
static u8 vram[64];
int gmem_read(u64 gva, void* dst, u64 size) {
    if (gva < VA || gva - VA + size > sizeof vram) {
        memset(dst, 0, size);
        return -1;
    }
    memcpy(dst, vram + (gva - VA), size);
    return 0;
}
int gmem_write(u64 gva, const void* src, u64 size) {
    if (gva < VA || gva - VA + size > sizeof vram) return -1;
    memcpy(vram + (gva - VA), src, size);
    return 0;
}
u32 gmem_r32(u64 gva) {
    u8 b[4] = {0};
    gmem_read(gva, b, 4);
    return snx_ld_le32(b);
}
void gmem_w32(u64 gva, u32 v) {
    u8 b[4];
    snx_st_le32(b, v);
    gmem_write(gva, b, 4);
}
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
u32 gpu_color_bpp(u32 format) {
    (void)format;
    return 4;
}
u64 bl_offset(u32 x_bytes, u32 y, u32 z, u32 width_bytes, u32 height, u32 bh_log2, u32 bd_log2) {
    (void)x_bytes, (void)y, (void)z, (void)width_bytes, (void)height, (void)bh_log2, (void)bd_log2;
    return 0;
}
void gpu_signal_point(void) {}
void gpu_release_write(u64 gva, const void* data, u32 len) { gmem_write(gva, data, len); }
u64 guest_ns(void) { return 0; }
static struct { /* the last wait the 3D engine asked for */
    u32 kind, val;
    u64 addr;
    int geq, n;
} parked;
void gpu_park(GpuChannel* ch, u32 kind, u64 addr, u32 val, int geq) {
    (void)ch;
    parked.kind = kind, parked.addr = addr, parked.val = val, parked.geq = geq;
    parked.n++;
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

/* ------------------------------------------------ the check */
static GpuChannel ch;
static int failures;
static u64 rng = 0x2545F4914F6CDD1Dull;
static u32 next(void) {
    rng ^= rng << 13, rng ^= rng >> 7, rng ^= rng << 17;
    return (u32)(rng >> 16);
}
/* the bits tried in each register */
static const u32 flips[] = {1, 2, 4, 0x10, 0x100, 0x1000, 0x8000, 0x10000, 0x1000000, 0x80000000u, 0xFFFFFFFFu};
/* the state read from the registers as they are, every group counted as changed */
static void sync_all(void) {
    ch.r3d_dirty = ~0u;
    mx_draw_state_sync(&ch);
}

int main(void) {
    static u32 reads[MAXWELL3D_MACRO_REGISTERS_START]; /* the groups (DS_ bits) each register is read by */
    u32 checked = 0;
    /* register values: all zero, random, and random with half the words zero, so that a register a group reads only
       under another's value (the back stencil under two-sided stencil, a color target under the count) is found */
    for (int base = 0; base < 3; base++) {
        for (u32 m = 0; m < MAXWELL3D_MACRO_REGISTERS_START; m++)
            ch.r3d[m] = base == 0 ? 0 : base == 1 || next() % 2 ? next() : 0;
        sync_all();
        for (u32 m = 0; m < MAXWELL3D_MACRO_REGISTERS_START; m++)
            for (u32 f = 0; f < sizeof flips / sizeof flips[0]; f++) {
                u32 old = ch.r3d[m];
                ch.r3d[m] = old ^ flips[f];
                reads[m] |= mx_draw_state_stale(&ch);
                ch.r3d[m] = old;
            }
        /* each register found, written through the 3D engine as a stream would */
        for (u32 m = 0; m < MAXWELL3D_MACRO_REGISTERS_START; m++) {
            if (!reads[m]) continue;
            for (u32 f = 0; f < sizeof flips / sizeof flips[0]; f++) {
                u32 old = ch.r3d[m], stale, changed = 0;
                u64 gen[DS_GENS];
                memcpy(gen, ch.ds.gen, sizeof gen);
                maxwell3d_method(&ch, m, old ^ flips[f], 1);
                mx_draw_state_sync(&ch);
                stale = mx_draw_state_stale(&ch);
                for (u32 g = 0; g < DS_GENS; g++) changed |= (u32)(gen[g] != ch.ds.gen[g]) << g;
                if (stale) {
                    fprintf(stderr,
                            "FAIL: register %#x changed groups %#x of the draw state, which still hold the "
                            "old value: its register group misses them\n",
                            m, stale);
                    failures++;
                }
                if (changed & ~(reads[m] | 1u << DS_TARGET_FORMATS)) {
                    fprintf(stderr, "FAIL: register %#x gave groups %#x a new generation, of which only %#x read it\n",
                            m, changed, reads[m]);
                    failures++;
                }
                maxwell3d_method(&ch, m, old, 1);
                mx_draw_state_sync(&ch);
                checked++;
            }
        }
    }
    u32 found = 0;
    for (u32 m = 0; m < MAXWELL3D_MACRO_REGISTERS_START; m++) found += reads[m] != 0;
    if (found < 200) {
        fprintf(stderr, "FAIL: only %u registers found read by the draw state\n", found);
        failures++;
    }
    if (failures) return 1;
    printf("draw_state_groups: %u registers the draw state reads, %u writes checked\n", found, checked);
    return 0;
}
