/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* nvn_direct's texture storage answers (src/nx/gpu/nvn/nvn_tex.c nvn_tex_storage) against the rules of the game's
   NVN driver (docs/NVN_HLE.md, "Texture storage answers"), over targets, formats, sizes, levels, samples and flags:
   the kind, the alignment, the size and the ZCull area, and that each texture's extent (its layout) stays inside the
   storage the game allocates for it. nvn_verify's storage sweep compares the same answers with the driver's own code
   in a game run */
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
        CHECK(st.kind == 0xcc, "compressible RGBA8: kind %x", st.kind);
        d.format = NVN_FORMAT_RGBA16F;
        nvn_tex_storage(&d, &st);
        CHECK(st.kind == 0xcd, "compressible RGBA16F: kind %x", st.kind);
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
    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
