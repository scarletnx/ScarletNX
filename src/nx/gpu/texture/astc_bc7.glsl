/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* ASTC blocks straight into BC7 blocks: the body of astc_bc7.comp, after astc_decode.glsl (DecodeBlock, one block
   into the invocation's slot, and SlotTexel, a texel out of it) and bc7_encode.glsl (Encode, px into a block). An
   invocation makes one 4x4 block, 64 a workgroup along a row of them.

   The block's 16 texels lie in one ASTC block where the footprint is 4x4, or 8 or 12 a side; else in up to two each
   way. Each of those is decoded here, and gives the texels it holds. An ASTC block under several 4x4 blocks is so
   decoded by each of them, up to nine times (12x12), where a decoder shared by the workgroup would decode it once:
   decoding a block is a hundredth of what encoding one costs, and sharing it took the workgroup's shared memory
   (astc_bc7.comp).

   A block's texels are the ones astc.comp writes for them, and a block past the strip's edge repeats its last texels
   (which adds no colour to the line), so the blocks are those bc7_encode.glsl makes of the decoded strip: the tests
   hold them to that, byte for byte (tests/glsl/astc_bc7_glsl.cpp, tests/astc_vulkan.c). */

void main() {
    const uint bx = gl_GlobalInvocationID.x, by = gl_GlobalInvocationID.y, z = gl_WorkGroupID.z;
    const uint cx = (width + 3u) >> 2u, cy = (height + 3u) >> 2u;
    if (bx >= cx || by >= cy) return;
    /* the block's texels, its last ones the strip's where it passes the edge, and the ASTC blocks they lie in */
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
                /* the texel's place in this block: past its sides, a number no block is wide or high */
                const uint i = min(x0 + (k & 3u), x1) - left, j = min(y0 + (k >> 2u), y1) - top;
                if (i < block_dims.x && j < block_dims.y) {
                    px[k] = SlotTexel(0u, i, j, infill_ds, infill_dt);
                }
            }
        }
    }
    bc7_blocks[(z * cy + by) * cx + bx] = Encode();
}
