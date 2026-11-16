/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* What an ASTC texture's blocks can hold, read from each block's header and color endpoint modes (CEMs) without
   decoding a texel, and the BCn format that holds it on a GPU that samples BCn but not ASTC (astc_mode's Auto,
   texture.c): luminance alone as BC4 (8 bytes a 4x4 block, an eighth of RGBA8), luminance and alpha as BC5 (16), opaque
   colour on one line a block as BC1 (8, half of BC7). The rest stays as astc_mode holds it. The GPU reads the blocks
   (astc_kind.glsl, vk/astc.c's vk_astc_kind_read); this header has the kind's bits, the format each kind takes, and
   how the CPU makes a texture's kind of what the GPU leaves. The CPU's own read of a block and of a level, which the
   GLSL is held to, is the tests' (tests/support/astc_kind_ref.h).

   The kind is what the decoded texels may be, by the ASTC specification's decode (tests/support/astc.c, the GPU's
   astc_decode.glsl, which the tests hold to it byte for byte), never less: a texture said to be luminance decodes to
   R, G and B within SNX_ASTC_GREY_SPREAD of each other and A = 255 in every texel. A partition of an RGB or RGBA mode
   counts as grey where its two endpoints, as the decoders make them from its colour values, are grey within that
   (snx_astc_rgb_spread): an encoder writes grey in those modes too, as astcenc does a normal map's X in R, G and B */
#ifndef SNX_TEXTURE_ASTC_KIND_H
#define SNX_TEXTURE_ASTC_KIND_H
#include "nx/gpu/texture/blocklinear.h"

/* the kind's bits, or'd over a texture's blocks */
#define SNX_ASTC_CHROMA 1u /* R, G and B may differ in a texel */
#define SNX_ASTC_ALPHA  2u /* alpha may be under 255 */
/* R, G and B may lie off one line in a 4x4 block, BC1's one line: two partitions or more of colour (each is a line of
   its own), a second plane on R, G or B (that channel's weights apart from the others'), or a footprint whose sides
   are not multiples of 4, where a 4x4 block takes the texels of two to four ASTC blocks, each a line of its own. On
   random blocks of one line each, BC1 erred by up to 141 levels in 256 at those footprints and 36 at 4x4, 8x8 and
   12x12 (tests/glsl/astc_bcn_glsl.cpp) */
#define SNX_ASTC_LINES  4u
/* the error colour (an illegal block, a reserved bit, too few bits for its values) or HDR, which the LDR profile
   decodes as the error colour too: nothing BCn is chosen for */
#define SNX_ASTC_OTHER  8u

/* The most an RGB or RGBA partition's endpoints may have R, G and B apart, in levels of 256, and still count as grey.
   astcenc's -normal encodings of two normal maps wrote the RGBA mode (12) in 17% and 0.1% of their blocks, X in R, G
   and B; decoded, 8 and 16 texels of a million and four were a level apart, the endpoints' rounding, which
   counted as colour kept the maps from BC5 (tests/glsl/astc_bcn_glsl.cpp --measure). A texel is then within 2 levels
   of grey, the BC5 view giving R for G and B: the tests hold every decoded texel of such blocks to that */
#define SNX_ASTC_GREY_SPREAD 1u

/* the format snx_astc_kind_as picks: none (as astc_mode holds them), BC4, BC5, BC1 */
enum { SNX_ASTC_AS_USUAL, SNX_ASTC_AS_BC4, SNX_ASTC_AS_BC5, SNX_ASTC_AS_BC1 };

/* the format a texture of kind is held in (SNX_ASTC_AS_*), decoded as sRGB or not: BC4 and BC5 have no sRGB twin, so
   an sRGB texture of luminance goes to BC1, which holds the grey line in sRGB, and one with alpha stays as it is */
static inline int snx_astc_kind_as(u32 kind, int srgb) {
    if (kind & SNX_ASTC_OTHER) return SNX_ASTC_AS_USUAL;
    if (!(kind & SNX_ASTC_CHROMA))
        return kind & SNX_ASTC_ALPHA ? (srgb ? SNX_ASTC_AS_USUAL : SNX_ASTC_AS_BC5)
               : srgb                ? SNX_ASTC_AS_BC1
                                     : SNX_ASTC_AS_BC4;
    return kind & (SNX_ASTC_ALPHA | SNX_ASTC_LINES) ? SNX_ASTC_AS_USUAL : SNX_ASTC_AS_BC1;
}
/* a kind past which no format but the usual is left: the walk over a level stops there */
static inline int snx_astc_kind_done(u32 kind, int srgb) { return snx_astc_kind_as(kind, srgb) == SNX_ASTC_AS_USUAL; }

/* where GOB (gx, gy) of slice z of a block linear level lies, rows block rows of GOBs a slice (snx_astc_level_kind) */
static inline u64 snx_astc_gob_at(u32 z, u32 gy, u32 gx, u64 rows, u32 gobs_x, u32 lbh, u32 lbd) {
    return (((u64)(z >> lbd) * rows + (gy >> lbh)) * gobs_x + gx) * (512ull << (lbh + lbd)) +
           ((((u64)z & ((1u << lbd) - 1)) << lbh) + (gy & ((1u << lbh) - 1))) * 512;
}
/* A texture's kind is that of a walk over the blocks of every level of every layer in guest memory, through the block
   linear layout (or pitch rows), its padding left out, which stops once no BCn format is left (snx_astc_kind_done) and
   returns OTHER where the layout reaches past the memory (tests/support/astc_kind_ref.h's snx_astc_level_kind). The
   GPU reads every block at once (gpu/texture/astc_kind.glsl, vk/astc.c's vk_astc_kind_read): a block's number in the
   walk's order is y * nx + x in rows pitch apart, and in block linear GOBs ((z * GOB rows + GOB row) * GOBs across +
   GOB column) * 32 + its row in the GOB * 4 + its column, the GOB's blocks being 4 across and 8 down. The GPU gives,
   for each kind bit (1 << b), the first block in that order that has it; these two give the kind the walk makes of
   them.

   snx_astc_level_stop: where the walk returns OTHER for its layout, before it reads a block: twice the number of the
   first block it would not read (0: none of them; ~0: it reads every block). Block linear GOBs lie further in memory
   the later the walk reaches them (a GOB row's step is a whole block of GOBs, more than the GOB rows inside a block,
   and a slice's more than its rows), so when the last one fits every one does */
static inline u64 snx_astc_level_stop(u64 size, u32 nx, u32 ny, u32 nz, u32 pitch, u32 gobs_x, u32 lbh, u32 lbd) {
    if (pitch) return nz != 1 || (u64)pitch * (ny - 1) + (u64)nx * 16 > size ? 0 : ~0ull;
    const u64 rows = ((((u64)ny + 7) >> 3) + (1u << lbh) - 1) >> lbh;
    const u32 gyn = (ny + 7) / 8, gxn = (nx + 3) / 4;
    if ((u64)gxn > gobs_x) return 0;
    if (snx_astc_gob_at(nz - 1, gyn - 1, gxn - 1, rows, gobs_x, lbh, lbd) + 512 <= size) return ~0ull;
    for (u32 z = 0; z < nz; z++) /* the last GOB does not fit: the first that does not */
        for (u32 gy = 0; gy < gyn; gy++)
            for (u32 gx = 0; gx < gxn; gx++)
                if (snx_astc_gob_at(z, gy, gx, rows, gobs_x, lbh, lbd) + 512 > size)
                    return 2 * ((((u64)z * gyn + gy) * gxn + gx) * 32);
    return ~0ull;
}
/* The kind snx_astc_level_kind gives a level, from the number of the first block with each kind bit (first[b], ~0u
   for none) and where the walk stops (stop): the bits in the walk's order, a block's together, up to where they are
   done (snx_astc_kind_done); OTHER alone where the walk stops before that */
static inline u32 snx_astc_level_kind_firsts(const u32 first[4], u64 stop, int srgb) {
    u32 kind = 0;
    for (;;) {
        u64 next = ~0ull;
        for (u32 b = 0; b < 4; b++)
            if (first[b] != ~0u && !(kind >> b & 1) && 2ull * first[b] + 1 < next) next = 2ull * first[b] + 1;
        if (stop < next) return SNX_ASTC_OTHER;
        if (next == ~0ull) return kind;
        for (u32 b = 0; b < 4; b++)
            if (first[b] != ~0u && 2ull * first[b] + 1 == next) kind |= 1u << b;
        if (snx_astc_kind_done(kind, srgb)) return kind;
    }
}

#endif
