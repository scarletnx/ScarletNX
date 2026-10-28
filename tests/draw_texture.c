/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* DRAW_TEXTURE (3D 0x420-0x42B) reaches the renderer once, when its last word is written, with the words in place. */
#include "nx/gpu/gpu.h"
#include "nx/gpu/maxwell/engines_int.h"
#include "nx/gpu/renderer.h"
#include "nx/hle/services/nvdrv.h"
#include "nx/memory/mem.h"
#include "nx/runtime/clock.h"
#include "nx/runtime/platform/log.h"
#include "nx/runtime/platform/plat.h"
#include <stdio.h>
#include <string.h>

u8* g_mem;
u64 g_draws;
int g_renderer_up = 1;
static int calls;
static DrawTexture seen;
static void record(const DsState* ds, const DrawTexture* t) {
    (void)ds;
    calls++;
    seen = *t;
}
static const Renderer recording = {.name = "recording", .draw_texture = record};
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
void gpu_signal_point(void) {}
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

int main(void) {
    /* destination (40, 24) 32 x 16 in 20.12, one source texel per pixel in 32.32, sampler 3, texture 5, source (0, 0) */
    const u32 words[12] = {40u << 12, 24u << 12, 32u << 12, 16u << 12, 0, 1, 0, 1, 3, 5, 0, 0};
    int failures = 0;
    for (u32 i = 0; i < 11; i++) maxwell3d_method(&ch, 0x420 + i, words[i], 0);
    if (calls) {
        fputs("FAIL: drawn before its last word\n", stderr);
        failures++;
    }
    maxwell3d_method(&ch, 0x42B, words[11], 1);
    /* the words decoded: the rectangle in pixels, a texel per pixel, texture 5 with sampler 3 */
    if (calls != 1 || seen.x0 != 40 || seen.y0 != 24 || seen.w != 32 || seen.h != 16 || seen.du != 1 || seen.dv != 1 ||
        seen.handle != (3u << 20 | 5) || seen.sx0 != 0 || seen.sy0 != 0) {
        fprintf(stderr, "FAIL: %d calls after the last word, or its words weren't in place\n", calls);
        failures++;
    }
    if (failures) return 1;
    puts("DRAW_TEXTURE reaches the renderer once, on its last word, with all its words");
    return 0;
}
