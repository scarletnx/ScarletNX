/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* nvn's texture storage answers (src/nx/gpu/nvn/nvn_tex.c nvn_tex_storage) against the rules of the game's
   NVN driver (docs/NVN_HLE.md, "Texture storage answers"), over targets, formats, sizes, levels, samples and flags:
   the kind, the alignment, the size and the ZCull area, and that each texture's extent (its layout) stays inside the
   storage the game allocates for it. A storage sweep compared the same answers with the driver's own code in a game
   run */
#include "nx/gpu/nvn/nvn_tex.h"
#include <stdio.h>
#include <string.h>

static int failures, checks;
#define CHECK(cond, ...)                                                                                               \
    do {                                                                                                               \
        checks++;                                                                                                      \
        if (!(cond)) {                                                                                                 \
            failures++;                                                                                                \
            if (failures <= 20) {                                                                                      \
                printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                            \
                printf(__VA_ARGS__);                                                                                   \
                printf("\n");                                                                                          \
            }                                                                                                          \
        }                                                                                                              \
    } while (0)

static NvnTexDesc desc(int target, int format, u32 w, u32 h, u32 d, int levels, int samples, u32 flags) {
    NvnTexDesc t;
    memset(&t, 0, sizeof t);
    t.target = (u8)target;
    t.format = (u16)format;
    t.width = w, t.height = h, t.depth = d;
    t.levels = (u8)levels;
    t.samples = (u8)samples;
    t.flags = flags;
    return t;
}
static int is_depth(int f) { return f >= NVN_FORMAT_STENCIL8 && f <= NVN_FORMAT_DEPTH32F_STENCIL8; }

/* the 64 KiB page of byte xb of block row y, slice z, of a block-linear level gobs_x GOBs across and rows rows of
   blocks down, its blocks 2^th GOBs high and 2^td deep: the block's place, then the GOB's in it (a GOB's 512 bytes
   are in one page whatever their order) */
static u64 bl_page(u32 xb, u32 y, u32 z, u32 gobs_x, u32 rows, unsigned th, unsigned td) {
    u64 block = ((u64)(z >> td) * rows + ((y >> 3) >> th)) * gobs_x + (xb >> 6);
    u64 gob = ((u64)(z & ((1u << td) - 1)) << th) + ((y >> 3) & ((1u << th) - 1));
    return ((block << (th + td)) + gob) * 512 >> 16;
}
/* A sparse texture of nx by ny by nz tiles of format (a block of bpb bytes, bw texels across and down; 3D when nz,
   else 2D), whose tile has the standard shape tb (blocks across, down and deep): the tile nvn answers is that one, the
   storage is the tiles' pages, and the corners of every tile are in its own page, the tiles across, then down, then
   deep, where the game maps it */
static void sparse_tiles(int format, u32 bw, u32 bpb, const u32 tb[3], u32 nx, u32 ny, u32 nz) {
    int is3d = nz != 0;
    u32 tile[3] = {0, 0, 0}, tz = is3d ? tb[2] : 1, gobs_x = nx * tb[0] * bpb / 64, rows, x, y, z, c;
    NvnTexDesc d = desc(is3d ? NVN_TEXTURE_TARGET_3D : NVN_TEXTURE_TARGET_2D, format, nx * tb[0] * bw, ny * tb[1] * bw,
                        is3d ? nz * tb[2] : 1, 1, 0, NVN_TEXTURE_FLAGS_SPARSE);
    NvnTexLayout lay;
    if (!is3d) nz = 1;
    CHECK(nvn_tex_sparse_tile(&d, tile) && tile[0] == tb[0] && tile[1] == tb[1] && tile[2] == tz,
          "format %#x %dD: tile %ux%ux%u, the standard %ux%ux%u", format, is3d ? 3 : 2, tile[0], tile[1], tile[2],
          tb[0], tb[1], tz);
    nvn_tex_layout(&d, &lay);
    CHECK(lay.size == (u64)nx * ny * nz << 16, "format %#x %dD %ux%ux%u: storage %llx, %u tiles", format, is3d ? 3 : 2,
          d.width, d.height, d.depth, (unsigned long long)lay.size, nx * ny * nz);
    rows = ((ny * tb[1] + 7) / 8 + (1u << lay.tile_h) - 1) >> lay.tile_h;
    for (z = 0; z < nz; z++)
        for (y = 0; y < ny; y++)
            for (x = 0; x < nx; x++)
                for (c = 0; c < 8; c++) { /* the tile's corner blocks: first or last across, down and deep */
                    u32 bx = x * tb[0] + (c & 1 ? tb[0] - 1 : 0), by = y * tb[1] + (c & 2 ? tb[1] - 1 : 0);
                    u32 bz = z * tz + (c & 4 ? tz - 1 : 0);
                    u64 page = bl_page(bx * bpb, by, bz, gobs_x, rows, lay.tile_h, lay.tile_d);
                    u64 want = ((u64)z * ny + y) * nx + x;
                    CHECK(
                        page == want,
                        "format %#x %dD %ux%ux%u, blocks %u high and %u deep: block (%u, %u, %u) of tile (%u, %u, %u) "
                        "in page %llu, not %llu",
                        format, is3d ? 3 : 2, d.width, d.height, d.depth, 1u << lay.tile_h, 1u << lay.tile_d, bx, by,
                        bz, x, y, z, (unsigned long long)page, (unsigned long long)want);
                }
}

int main(void) {
    static const int formats[] = {NVN_FORMAT_R8, NVN_FORMAT_RG8, NVN_FORMAT_RG16F, NVN_FORMAT_RGBA8,
                                  NVN_FORMAT_RGBA8_SRGB, NVN_FORMAT_R32F, NVN_FORMAT_RGBA16F, NVN_FORMAT_RGBA32F,
                                  NVN_FORMAT_RGB10A2, NVN_FORMAT_R11G11B10F, NVN_FORMAT_STENCIL8, NVN_FORMAT_DEPTH16,
                                  NVN_FORMAT_DEPTH24, NVN_FORMAT_DEPTH32F, NVN_FORMAT_DEPTH24_STENCIL8,
                                  NVN_FORMAT_DEPTH32F_STENCIL8, NVN_FORMAT_RGBA_DXT1, NVN_FORMAT_RGBA_DXT5,
                                  NVN_FORMAT_BPTC_UNORM, NVN_FORMAT_RGBA_ASTC_4x4, NVN_FORMAT_RGBA_ASTC_8x8};
    static const struct {
        int target, depth, samples;
    } targets[] = {{NVN_TEXTURE_TARGET_1D, 1, 0},          {NVN_TEXTURE_TARGET_2D, 1, 0},
                   {NVN_TEXTURE_TARGET_3D, 16, 0},         {NVN_TEXTURE_TARGET_1D_ARRAY, 1, 0},
                   {NVN_TEXTURE_TARGET_2D_ARRAY, 6, 0},    {NVN_TEXTURE_TARGET_2D_MULTISAMPLE, 1, 4},
                   {NVN_TEXTURE_TARGET_2D_MULTISAMPLE, 1, 8}, {NVN_TEXTURE_TARGET_RECTANGLE, 1, 0},
                   {NVN_TEXTURE_TARGET_CUBEMAP, 6, 0},     {NVN_TEXTURE_TARGET_CUBEMAP_ARRAY, 12, 0}};
    static const u32 sizes[][2] = {{1, 1}, {7, 5}, {16, 16}, {33, 17}, {100, 60}, {320, 192}, {1000, 3},
                                   {1280, 720}, {1600, 896}, {4096, 2048}};
    static const u32 flags[] = {0, NVN_TEXTURE_FLAGS_COMPRESSIBLE, NVN_TEXTURE_FLAGS_IMAGE,
                                NVN_TEXTURE_FLAGS_MINIMAL_LAYOUT, NVN_TEXTURE_FLAGS_LINEAR,
                                NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET,
                                NVN_TEXTURE_FLAGS_COMPRESSIBLE | NVN_TEXTURE_FLAGS_ZCULL_SUPPORT_STENCIL};
    size_t t, f, z, g;
    int l;
    for (t = 0; t < sizeof targets / sizeof targets[0]; t++)
        for (f = 0; f < sizeof formats / sizeof formats[0]; f++)
            for (z = 0; z < sizeof sizes / sizeof sizes[0]; z++)
                for (g = 0; g < sizeof flags / sizeof flags[0]; g++)
                    for (l = 0; l < 2; l++) {
                        int tg = targets[t].target, fm = formats[f];
                        u32 w = sizes[z][0], h = sizes[z][1], fl = flags[g];
                        NvnTexDesc d;
                        NvnTexLayout lay;
                        NvnTexStorage st;
                        int linear = (fl & (NVN_TEXTURE_FLAGS_LINEAR | NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET)) != 0;
                        int compression;
                        if (tg == NVN_TEXTURE_TARGET_1D || tg == NVN_TEXTURE_TARGET_1D_ARRAY) h = 1;
                        if (tg == NVN_TEXTURE_TARGET_CUBEMAP || tg == NVN_TEXTURE_TARGET_CUBEMAP_ARRAY) h = w;
                        d = desc(tg, fm, w, h, (u32)targets[t].depth, l ? 6 : 1, targets[t].samples, fl);
                        if (linear) d.stride = (w * 16 + 127) & ~127u;
                        nvn_tex_layout(&d, &lay);
                        nvn_tex_storage(&d, &st);
                        if (!lay.bpb) continue;
                        /* the extent the runtime lays the texture out in stays inside what the game allocates */
                        CHECK(lay.size <= st.size, "target %d format %#x %ux%u flags %#x: extent %llx past size %llx",
                              tg, fm, w, h, fl, (unsigned long long)lay.size, (unsigned long long)st.size);
                        CHECK(st.align && !(st.align & (st.align - 1)), "alignment %x isn't a power of two",
                              st.align);
                        if (linear) {
                            CHECK(st.kind == 0, "linear: kind %x", st.kind);
                            CHECK(st.align == ((fl & NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET) ? 0x80u : 0x20u),
                                  "linear: alignment %x", st.align);
                            continue;
                        }
                        compression = is_depth(fm) ||
                                      ((fl & NVN_TEXTURE_FLAGS_COMPRESSIBLE) && lay.bpb >= 4 &&
                                       fm != NVN_FORMAT_RGBA_DXT5 && fm != NVN_FORMAT_BPTC_UNORM &&
                                       fm != NVN_FORMAT_RGBA_ASTC_4x4 && fm != NVN_FORMAT_RGBA_ASTC_8x8 &&
                                       fm != NVN_FORMAT_RGBA_DXT1);
                        CHECK(compression ? st.kind != 0xfe && st.kind != 0 : st.kind == 0xfe,
                              "target %d format %#x flags %#x: kind %x", tg, fm, fl, st.kind);
                        CHECK(st.align == (compression ? 0x10000u : 0x200u), "format %#x flags %#x: alignment %x", fm,
                              fl, st.align);
                        CHECK(!compression || !(st.size & 0xFFFF), "format %#x flags %#x: size %llx not a big page", fm,
                              fl, (unsigned long long)st.size);
                        CHECK(compression || st.size == lay.size, "format %#x: size %llx, layout %llx", fm,
                              (unsigned long long)st.size, (unsigned long long)lay.size);
                        CHECK(!!st.zcull == ((fm != NVN_FORMAT_STENCIL8 && is_depth(fm)) ||
                                             (fm == NVN_FORMAT_STENCIL8 &&
                                              (fl & NVN_TEXTURE_FLAGS_ZCULL_SUPPORT_STENCIL) != 0)),
                              "format %#x flags %#x: zcull %llx", fm, fl, (unsigned long long)st.zcull);
                    }
    { /* the kinds and areas of a few common textures */
        NvnTexDesc d = desc(NVN_TEXTURE_TARGET_2D, NVN_FORMAT_RGBA8, 1280, 720, 1, 1, 0, NVN_TEXTURE_FLAGS_COMPRESSIBLE);
        NvnTexStorage st;
        nvn_tex_storage(&d, &st);
        CHECK(st.kind == 0xdb, "compressible RGBA8: kind %x", st.kind);
        d.format = NVN_FORMAT_RGBA16F;
        nvn_tex_storage(&d, &st);
        CHECK(st.kind == 0xe9, "compressible RGBA16F: kind %x", st.kind);
        d.format = NVN_FORMAT_RGBA32F;
        nvn_tex_storage(&d, &st);
        CHECK(st.kind == 0xf5, "compressible RGBA32F: kind %x", st.kind);
        { /* the kinds the driver answered in the verify sweep: color by element size, depth by samples */
            static const struct {
                int format;
                u32 samples, kind;
            } k[] = {{NVN_FORMAT_RGBA8, 2, 0xcc},
                     {NVN_FORMAT_RGBA8, 4, 0xe0},
                     {NVN_FORMAT_RG16F, 0, 0xdb},
                     {NVN_FORMAT_R11G11B10F, 0, 0xdb},
                     {NVN_FORMAT_RGBA16F, 2, 0xcd},
                     {NVN_FORMAT_RGBA16F, 4, 0xee},
                     {NVN_FORMAT_RGBA32F, 2, 0xf7},
                     {NVN_FORMAT_RGBA32F, 4, 0xf9},
                     {NVN_FORMAT_DEPTH16, 0, 0x07},
                     {NVN_FORMAT_DEPTH16, 2, 0x08},
                     {NVN_FORMAT_DEPTH16, 4, 0x09},
                     {NVN_FORMAT_DEPTH24_STENCIL8, 0, 0x51},
                     {NVN_FORMAT_DEPTH24_STENCIL8, 2, 0x52},
                     {NVN_FORMAT_DEPTH24_STENCIL8, 4, 0x53},
                     {NVN_FORMAT_DEPTH32F, 0, 0x86},
                     {NVN_FORMAT_DEPTH32F, 2, 0x87},
                     {NVN_FORMAT_DEPTH32F, 4, 0x88},
                     {NVN_FORMAT_DEPTH32F_STENCIL8, 0, 0xce},
                     {NVN_FORMAT_DEPTH32F_STENCIL8, 4, 0xd0}};
            size_t i;
            for (i = 0; i < sizeof k / sizeof k[0]; i++) {
                d = desc(k[i].samples ? NVN_TEXTURE_TARGET_2D_MULTISAMPLE : NVN_TEXTURE_TARGET_2D, k[i].format, 64, 64,
                         1, 1, (int)k[i].samples, NVN_TEXTURE_FLAGS_COMPRESSIBLE);
                nvn_tex_storage(&d, &st);
                CHECK(st.kind == k[i].kind, "compressible format %#x, %u samples: kind %x, the driver's %x",
                      k[i].format, k[i].samples, st.kind, k[i].kind);
            }
        }
        d = desc(NVN_TEXTURE_TARGET_2D, NVN_FORMAT_DEPTH24_STENCIL8, 320, 192, 1, 1, 0, 0);
        nvn_tex_storage(&d, &st);
        CHECK(st.kind == 0x46 && st.zcull == 0x860, "D24S8 320x192: kind %x zcull %llx", st.kind,
              (unsigned long long)st.zcull);
        d.width = 1280, d.height = 720;
        nvn_tex_storage(&d, &st);
        CHECK(st.zcull == 0x73e0, "D24S8 1280x720: zcull %llx", (unsigned long long)st.zcull);
        d = desc(NVN_TEXTURE_TARGET_2D_MULTISAMPLE, NVN_FORMAT_RGBA8, 64, 64, 1, 5, 4, 0);
        {
            NvnTexLayout a, b;
            nvn_tex_layout(&d, &a);
            d.levels = 1;
            nvn_tex_layout(&d, &b);
            CHECK(a.size == b.size, "a multisampled texture's levels count: %llx, one level %llx",
                  (unsigned long long)a.size, (unsigned long long)b.size);
        }
    }
    { /* sparse textures: every block size's standard tile, 2D and 3D, then one game's 1664x448x1344 BC1 volume */
        static const struct {
            int format;
            u32 bw, bpb, t2[3], t3[3];
        } sp[] = {{NVN_FORMAT_R8, 1, 1, {256, 256, 1}, {64, 32, 32}},
                  {NVN_FORMAT_RG8, 1, 2, {256, 128, 1}, {32, 32, 32}},
                  {NVN_FORMAT_RGBA8, 1, 4, {128, 128, 1}, {32, 32, 16}},
                  {NVN_FORMAT_RGBA16F, 1, 8, {128, 64, 1}, {32, 16, 16}},
                  {NVN_FORMAT_RGBA32F, 1, 16, {64, 64, 1}, {16, 16, 16}},
                  {NVN_FORMAT_RGBA_DXT1, 4, 8, {128, 64, 1}, {32, 16, 16}},
                  {NVN_FORMAT_BPTC_UFLOAT, 4, 16, {64, 64, 1}, {16, 16, 16}}};
        size_t i;
        for (i = 0; i < sizeof sp / sizeof sp[0]; i++) {
            sparse_tiles(sp[i].format, sp[i].bw, sp[i].bpb, sp[i].t2, 3, 2, 0);
            sparse_tiles(sp[i].format, sp[i].bw, sp[i].bpb, sp[i].t3, 3, 2, 2);
        }
        sparse_tiles(NVN_FORMAT_RGBA_DXT1, 4, 8, sp[5].t3, 13, 7, 84);
    }
    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
