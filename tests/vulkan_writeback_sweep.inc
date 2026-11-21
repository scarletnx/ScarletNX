/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A color target's write-back in the guest's layout is the GPU's: copied into a scratch buffer and moved by
   blocklinear.comp into block linear order, or copied as rows of its pitch (surface.c's flush_record), then landed by
   bl_land, which keeps the bytes of a GOB the surface does not cover. Swept against the CPU's reference of the layout
   (gpu.c's bl_offset): R8, RG8, RGBA8, RGBA16F and RGBA32F; block linear in blocks 1 to 32 GOBs high and pitch linear;
   one layer and three; block linear 100 pixels across, so the last GOB of a row holds part of it, and two rows of
   blocks, the second cut short. Each is loaded from guest memory holding a pattern, its red cleared to 0.5 alone, and written
   back: the red bytes of every pixel hold 0.5 in the format, and every other byte, padding too, keeps the pattern */
#define WS_CPU  (HEAP_BASE + 0x40000000) /* past every other check's host bytes: the process is one address space */
#define WS_GVA  0xD00000000ull
#define WS_SPAN 0x1400000ull /* 20 MiB */

/* below 0x3C in every byte: no half or float of it is a NaN or an infinity, which a load may not keep as it is */
static u8 ws_pattern(u64 i) { return (u8)((i * 7 + (i >> 10) * 3 + 1) & 0x3B); }

typedef struct {
    u32 format, bpp, red_bytes; /* RT_FORMAT, the bytes of a pixel, the bytes of its red */
    u8 red[4];                  /* 0.5 in those bytes */
    int unorm;                  /* red is an 8-bit UNORM: 127 or 128 */
} WsFormat;

/* one target at gva (its guest bytes at cpu): 1 when a byte differs from the reference, said */
static int ws_case(u64 gva, u64 cpu, const WsFormat* f, int pitch_linear, u32 bh, u32 layers, u64* used) {
    /* a pitch linear target is as wide as its pitch (its width register holds the pitch): 100 pixels' rows padded
       to 64 bytes, every pixel of them its own */
    const u32 pitch = (100 * f->bpp + 63) & ~63u, w = pitch_linear ? pitch / f->bpp : 100, row = w * f->bpp;
    const u32 h = pitch_linear ? 37 : 2 * (8u << bh) - 3;
    const u64 gobs_x = (row + 63) / 64, rows_bl = (u64)2 * (8u << bh);
    const u64 layer_bytes = pitch_linear ? (u64)pitch * h : gobs_x * 64 * rows_bl;
    const u64 size = layer_bytes * layers;
    int bad = 0;
    *used = (size + 0xFFFF) & ~0xFFFFull;
    GpuChannel* ch = calloc(1, sizeof *ch);
    u8* got = malloc(size);
    if (!ch || !got) {
        free(ch);
        free(got);
        fprintf(stderr, "write-back sweep: no memory for a case of %llu bytes\n", (unsigned long long)size);
        return 1;
    }
    for (u64 i = 0; i < size; i++) GP(cpu)[i] = ws_pattern(i);
    u32* r = ch->r3d;
    r[0x487] = 1;
    r[0x200] = (u32)(gva >> 32);
    r[0x201] = (u32)gva;
    r[0x202] = pitch_linear ? pitch : w;
    r[0x203] = h;
    r[0x204] = f->format;
    r[0x205] = pitch_linear ? 0x1000 : bh << 4;
    r[0x206] = layers;
    r[0x207] = (u32)(layer_bytes >> 2);
    float red[4] = {0.5f, 0, 0, 0};
    memcpy(&r[0x360], red, sizeof red);
    for (u32 l = 0; l < layers; l++) engine_clear(ch, (1 << 2) | (l << 10)); /* red alone, layer l */
    vk_render_end();
    vk_rt_flush(gva);
    CHECK(!gmem_read(gva, got, size));
    u8* want = malloc(size);
    CHECK(want);
    for (u64 i = 0; i < size; i++) want[i] = ws_pattern(i);
    for (u32 l = 0; l < layers; l++)
        for (u32 y = 0; y < h; y++)
            for (u32 x = 0; x < w; x++)
                for (u32 k = 0; k < f->red_bytes; k++) {
                    const u32 xb = x * f->bpp + k;
                    const u64 at =
                        l * layer_bytes + (pitch_linear ? (u64)y * pitch + xb : bl_offset(xb, y, 0, row, h, bh, 0));
                    want[at] = f->red[k];
                }
    for (u64 i = 0; i < size && !bad; i++) {
        const int near = f->unorm && want[i] == 128 && (got[i] == 127 || got[i] == 128);
        if (got[i] != want[i] && !near) {
            fprintf(stderr,
                    "write-back sweep: format %#x, %s%s%u, %u layers: byte %llu (layer %llu) is %#x, the CPU's "
                    "reference has %#x\n",
                    f->format, pitch_linear ? "pitch linear" : "block linear", pitch_linear ? "" : " bh ",
                    pitch_linear ? pitch : bh, layers, (unsigned long long)i, (unsigned long long)(i / layer_bytes),
                    got[i], want[i]);
            bad = 1;
        }
    }
    free(want);
    free(got);
    free(ch);
    return bad;
}
static int check_writeback_sweep(void) {
    static const WsFormat formats[] = {
        {0xF3, 1, 1, {128}, 1},                    /* R8_UNORM */
        {0xEA, 2, 1, {128}, 1},                    /* R8G8_UNORM */
        {0xD5, 4, 1, {128}, 1},                    /* A8B8G8R8_UNORM */
        {0xCA, 8, 2, {0x00, 0x38}, 0},             /* R16G16B16A16_FLOAT: 0x3800 */
        {0xC0, 16, 4, {0x00, 0x00, 0x00, 0x3F}, 0} /* R32G32B32A32_FLOAT: 0x3F000000 */
    };
    u64 at = 0, used = 0;
    u32 cases = 0, failed = 0;
    CHECK(mem_map(WS_CPU, WS_SPAN, MS_NORMAL, PERM_RW) == 0);
    gmmu_map(WS_GVA, WS_CPU, WS_SPAN);
    for (u32 fi = 0; fi < sizeof formats / sizeof formats[0]; fi++)
        for (u32 lay = 0; lay < 7; lay++) { /* block linear in blocks of 2^0 to 2^5 GOBs, then pitch linear */
            const int pitch_linear = lay == 6;
            for (u32 layers = 1; layers <= 3; layers += 2) {
                if (at + (3u << 20) > WS_SPAN) { /* the next one's room: the largest takes 2.4 MiB, 15.3 MiB all */
                    fprintf(stderr, "write-back sweep: case %u starts at %llu of the %llu bytes mapped\n", cases,
                            (unsigned long long)at, (unsigned long long)WS_SPAN);
                    failed++;
                    break;
                }
                failed += (u32)ws_case(WS_GVA + at, WS_CPU + at, &formats[fi], pitch_linear, pitch_linear ? 0 : lay,
                                       layers, &used);
                at += used;
                cases++;
            }
        }
    CHECK(mem_unmap(WS_CPU, WS_SPAN) == 0);
    printf("write-back sweep: %u targets written back by the GPU, %u differ from the CPU's reference of the layout\n",
           cases, failed);
    CHECK(!failed);
    return 0;
}
