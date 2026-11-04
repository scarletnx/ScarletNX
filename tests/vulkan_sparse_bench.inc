/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* --bench-sparse-volume: what a sparse volume costs the renderer, made the way a game makes one through NVN
   (nvn_tex_layout, nvn_tic_encode) at the size of one game's light volumes, a few of its 64 KiB tiles mapped: the
   texture made, a frame with nothing changed, tiles mapped (the game's MapVirtual), the CPU writing a tile. Each step
   is timed on the calling thread's CPU clock and on the wall clock to the end of the GPU's work for it (vk_finish),
   with the bytes the texture loaded (vk_tex_stats). The checks of the ordinary run say the image is right */
#include <time.h>
static u64 sb_cpu_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return (u64)ts.tv_sec * 1000000000ull + (u64)ts.tv_nsec;
}
typedef struct {
    u64 cpu0, wall0, bytes0;
} SbStep;
static void sb_begin(SbStep* s) {
    u64 uploads;
    vk_tex_stats(&uploads, &s->bytes0);
    s->cpu0 = sb_cpu_ns();
    s->wall0 = plat_ns();
}
/* the step's times and bytes, said after what */
static void sb_end(const SbStep* s, const char* name, const char* what) {
    u64 uploads, bytes, cpu = sb_cpu_ns() - s->cpu0;
    vk_lock();
    vk_finish();
    vk_unlock();
    vk_tex_stats(&uploads, &bytes);
    printf("bench sparse volume %s, %s: %.3f ms CPU, %.3f ms to the GPU's end, %llu KB loaded\n", name, what, cpu / 1e6,
           (plat_ns() - s->wall0) / 1e6, (unsigned long long)((bytes - s->bytes0) >> 10));
}
static Surf* sb_frame(const u32* tic) {
    g_renderer->mem_may_have_changed();
    g_vk_present++;
    vk_lock();
    Surf* t = vk_tex_from_tic(tic);
    vk_unlock();
    return t;
}
static int bench_sparse_volume(int format, const char* name, u64 gva, u64 cpu) {
    NvnTexture tex = {0};
    NvnTexLayout lay;
    u32 tile[3] = {0}, tic[8];
    SbStep s;
    tex.d.target = NVN_TEXTURE_TARGET_3D;
    tex.d.format = (u16)format;
    tex.d.levels = 1;
    tex.d.flags = NVN_TEXTURE_FLAGS_SPARSE;
    tex.d.width = 1664, tex.d.height = 448, tex.d.depth = 1344;
    tex.gpu = gva;
    CHECK(nvn_tex_sparse_tile(&tex.d, tile));
    nvn_tex_layout(&tex.d, &lay);
    CHECK(nvn_tic_encode(&tex, NULL, 0, 0, tic));
    if (vk_tic_format_3d(tic[0], 0).vk == VK_FORMAT_UNDEFINED) {
        printf("bench sparse volume %s: no 3D images of its format here, skipped\n", name);
        return 0;
    }
    const u32 across = 1664 / (tile[0] * 4), down = 448 / (tile[1] * 4), deep = 1344 / tile[2];
    const u64 tiles = (u64)across * down * deep;
    CHECK(lay.size == tiles * 0x10000);
    /* 6 tiles mapped when it is made, 6 more later, 1 a frame after that: each a page of its own */
    CHECK(mem_map(cpu, 13 * 0x10000, MS_NORMAL, PERM_RW) == 0);
    for (u32 i = 0; i < 13 * 0x10000; i++) GP(cpu)[i] = (u8)(i * 13 + (i >> 16) * 7 + 1);
    u64 at[13];
    for (u32 k = 0; k < 13; k++) at[k] = (((u64)(k * 6 % deep) * down + k % down) * across + k * 5 % across) * 0x10000;
    for (u32 k = 0; k < 6; k++) gmmu_map(gva + at[k], cpu + k * 0x10000, 0x10000);
    printf("bench sparse volume %s: %ux%ux%u, %llu MB, %llu tiles of %ux%ux%u texels, 6 mapped\n", name, tex.d.width,
           tex.d.height, tex.d.depth, (unsigned long long)(lay.size >> 20), (unsigned long long)tiles, tile[0] * 4,
           tile[1] * 4, tile[2]);
    sb_begin(&s);
    vk_lock();
    Surf* t = vk_tex_from_tic(tic);
    vk_unlock();
    sb_end(&s, name, "made");
    CHECK(t);
    for (int i = 0; i < 60; i++) CHECK(sb_frame(tic) == t); /* its checks settle */
    sb_begin(&s);
    for (int i = 0; i < 60; i++) CHECK(sb_frame(tic) == t);
    u64 cpu60 = sb_cpu_ns() - s.cpu0;
    sb_end(&s, name, "60 frames unchanged");
    printf("bench sparse volume %s, a frame unchanged: %.1f us CPU\n", name, cpu60 / 60e3);
    sb_begin(&s);
    for (u32 k = 6; k < 12; k++) gmmu_map(gva + at[k], cpu + k * 0x10000, 0x10000);
    CHECK(sb_frame(tic) == t);
    sb_end(&s, name, "6 tiles mapped");
    for (int i = 0; i < 60; i++) CHECK(sb_frame(tic) == t);
    sb_begin(&s);
    memset(GP(cpu + 3 * 0x10000 + 0x2340), 0x5A, 64);
    CHECK(sb_frame(tic) == t);
    sb_end(&s, name, "the CPU writing 64 bytes of a tile");
    sb_begin(&s);
    gmmu_map(gva + at[12], cpu + 12 * 0x10000, 0x10000);
    CHECK(sb_frame(tic) == t);
    sb_end(&s, name, "1 tile mapped");
    return 0;
}
static int bench_sparse_volumes(void) {
    CHECK(!bench_sparse_volume(NVN_FORMAT_RGBA_DXT1, "BC1", 0x600000000ull, HEAP_BASE + 0x1000000));
    CHECK(!bench_sparse_volume(NVN_FORMAT_BPTC_UFLOAT, "BC6H", 0x680000000ull, HEAP_BASE + 0x1100000));
    return 0;
}
