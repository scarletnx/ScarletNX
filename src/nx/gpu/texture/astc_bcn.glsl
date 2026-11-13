/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* ASTC blocks straight into BC1, BC4 or BC5 blocks (SNX_BCN_KIND 1, 4 or 5): the body of astc_bcn.comp, after
   astc_decode.glsl (DecodeBlock, SlotTexel) and bcn_encode.glsl. An invocation makes one 4x4 block from the ASTC
   blocks its 16 texels lie in, one to four of them, as astc_bc7.glsl does, and a block past the strip's edge repeats
   its last texels. The texture's blocks hold no more than the format (gpu/texture/astc_kind.h): BC4 takes the texels'
   red, which is their green and blue too; BC5 red and alpha, the view giving them back as R, R, R, G
   (vk/render.c's tex_map); BC1 red, green and blue, alpha being 255 */
#ifndef SNX_BCN_KIND
#define SNX_BCN_KIND 4
#endif

void main() {
    const uint cx = (width + 3u) >> 2u, cy = (height + 3u) >> 2u;
    const uint bx = gl_GlobalInvocationID.x, by = gl_GlobalInvocationID.y, z = gl_WorkGroupID.z;
    if (bx >= cx || by >= cy) return;
    const uint x0 = bx * 4u, y0 = by * 4u;
    const uint x1 = min(x0 + 3u, width - 1u), y1 = min(y0 + 3u, height - 1u);
    const uint ax0 = (x0 * recip_w) >> 18u, ax1 = (x1 * recip_w) >> 18u;
    const uint ay0 = (y0 * recip_h) >> 18u, ay1 = (y1 * recip_h) >> 18u;
    for (uint ay = ay0; ay <= ay1; ay++) {
        for (uint ax = ax0; ax <= ax1; ax++) {
            blk = astc_data[in_base + (z * blocks_y + ay) * blocks_x + ax];
            DecodeBlock();
            const uint left = ax * block_dims.x, top = ay * block_dims.y;
            for (uint k = 0u; k < 16u; k++) {
                const uint i = min(x0 + (k & 3u), x1) - left, j = min(y0 + (k >> 2u), y1) - top;
                if (i < block_dims.x && j < block_dims.y) px[k] = SlotTexel(0u, i, j, infill_ds, infill_dt);
            }
        }
    }
    const uint at = (z * cy + by) * cx + bx;
#if SNX_BCN_KIND == 5
    const uvec2 r = Bc4Block(0u), a = Bc4Block(24u);
    bcn_words[at * 4u] = r.x;
    bcn_words[at * 4u + 1u] = r.y;
    bcn_words[at * 4u + 2u] = a.x;
    bcn_words[at * 4u + 3u] = a.y;
#else
#if SNX_BCN_KIND == 1
    const uvec2 b = Bc1Block();
#else
    const uvec2 b = Bc4Block(0u);
#endif
    bcn_words[at * 2u] = b.x;
    bcn_words[at * 2u + 1u] = b.y;
#endif
}
