/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* ASTC blocks straight into BC7 blocks: the body of astc_bc7.comp, after astc_decode.glsl (DecodeBlock, one block
   into the invocation's slot, and SlotTexel, a texel out of it) and bc7_encode.glsl (Encode, px into a block). An
   invocation makes one 4x4 block.

   The block's 16 texels lie in one ASTC block where the footprint is 4x4, or 8 or 12 a side; else in up to two each
   way. Each of those is decoded here, and gives the texels it holds. An ASTC block under several 4x4 blocks is so
   decoded by each of them, up to nine times (12x12), where a decoder shared by the workgroup would decode it once:
   sharing it took the workgroup's shared memory (astc_bc7.comp).

   Two passes (SNX_BC7_PASS). A GPU runs invocations 32 or 64 abreast, each group at the pace of its slowest: a block
   mode 6 holds is done in a hundredth of the time of one that takes the whole search, and with one such block in six
   nearly every group has one and waits for it, so a level of a game's blocks took 24 times one mode 6 holds
   throughout where the operations counted are 5 times. So the first pass (1), an invocation a block along the rows
   of the strip, gives every block mode 6 alone and writes the blocks that the search would have stopped at there
   (Encode's enc_more says which). The others it lists: their numbers into work_list, counted by an atomic add, with
   the workgroups the second pass needs kept beside the count as an indirect dispatch reads them. The second pass (2)
   is that many invocations, each of which takes a listed block and gives it the whole search: every group is at the
   same work. The blocks are the ones a single pass writes (0, which the builds with no search past mode 6 are).

   A block's texels are the ones astc.comp writes for them, and a block past the strip's edge repeats its last texels
   (which adds no colour to the line), so without hints (below) the blocks are those bc7_encode.glsl makes of the
   decoded strip: the tests hold them to that, byte for byte (tests/glsl/astc_bc7_glsl.cpp, tests/astc_vulkan.c). */

/* SNX_BC7_HINTS 1: Encode is told what the ASTC blocks say of the block (bc7_encode.glsl's hint_), and tries only
   what that leaves. An ASTC partition is one line through RGBA, so texels of one partition of one block are a set
   mode 6 was made for, and texels of two such sets (two partitions of a block, or two blocks' texels) want the
   partition of two subsets that parts them: the one of BC7's 64 with the fewest texels on the wrong side, found by
   counting bits, where Encode otherwise sums each partition's spread over the texels, 128 passes of them. A block
   with a second plane names the channel that has weights of its own, which is the rotation of modes 4 and 5. More
   than two sets, two planes of different channels, or a partition more than SNX_BC7_HINT_MISS texels from every one
   of BC7's leave the block to Encode's whole search */
#ifndef SNX_BC7_HINTS
#define SNX_BC7_HINTS 0
#endif
#ifndef SNX_BC7_HINT_MISS
#define SNX_BC7_HINT_MISS 2u
#endif

#ifndef SNX_BC7_PASS
#define SNX_BC7_PASS 0
#endif

void main() {
    const uint cx = (width + 3u) >> 2u, cy = (height + 3u) >> 2u;
#if SNX_BC7_PASS == 2
    /* a listed block: its number is (z * cy + by) * cx + bx */
    if (gl_GlobalInvocationID.x >= work_count) return;
    const uint at = work_list[gl_GlobalInvocationID.x];
    const uint row = at / cx, bx = at - row * cx, z = row / cy, by = row - z * cy;
#else
    const uint bx = gl_GlobalInvocationID.x, by = gl_GlobalInvocationID.y, z = gl_WorkGroupID.z;
    if (bx >= cx || by >= cy) return;
#endif
    /* the block's texels, its last ones the strip's where it passes the edge, and the ASTC blocks they lie in */
    const uint x0 = bx * 4u, y0 = by * 4u;
    const uint x1 = min(x0 + 3u, width - 1u), y1 = min(y0 + 3u, height - 1u);
    const uint ax0 = (x0 * recip_w) >> 18u, ax1 = (x1 * recip_w) >> 18u;
    const uint ay0 = (y0 * recip_h) >> 18u, ay1 = (y1 * recip_h) >> 18u;
    /* each texel's set, a nibble a texel: the block it is in (0 to 3) and its partition there; and the rotation
       of the blocks with a second plane (5: none had one), mixed when two differ */
    uint set_lo = 0u, set_hi = 0u, nth = 0u, rot = 5u;
    bool mixed = false;
    for (uint ay = ay0; ay <= ay1; ay++) {
        for (uint ax = ax0; ax <= ax1; ax++) {
            blk = astc_data[in_base + (z * blocks_y + ay) * blocks_x + ax];
            DecodeBlock();
            const uint info = TILE(F_INFO, 0u);
            if (Bits(info, 8u, 1u) != 0u) {
                /* the plane's channel, red first, to the rotation that sets it apart: alpha is rotation 0 */
                const uint r = (Bits(info, 9u, 2u) + 1u) & 3u;
                mixed = mixed || (rot != 5u && rot != r);
                rot = r;
            }
            const uint left = ax * block_dims.x, top = ay * block_dims.y;
            for (uint k = 0u; k < 16u; k++) {
                /* the texel's place in this block: past its sides, a number no block is wide or high */
                const uint i = min(x0 + (k & 3u), x1) - left, j = min(y0 + (k >> 2u), y1) - top;
                if (i < block_dims.x && j < block_dims.y) {
                    px[k] = SlotTexel(0u, i, j, infill_ds, infill_dt);
                    const uint set = nth * 4u + SlotPartition(0u, i, j);
                    if (k < 8u)
                        set_lo |= set << (k * 4u);
                    else
                        set_hi |= set << ((k - 8u) * 4u);
                }
            }
            nth++;
        }
    }
    hint_on = false;
    if (SNX_BC7_HINTS != 0 && !mixed) {
        /* the texels not of the first texel's set, a bit each, and whether they are all of one other set */
        const uint s0 = set_lo & 15u;
        uint other = 0u, s1 = 16u;
        bool more = false;
        for (uint k = 1u; k < 16u; k++) {
            const uint set = Index(set_lo, set_hi, k);
            if (set == s0) continue;
            if (s1 == 16u) s1 = set;
            more = more || set != s1;
            other |= 1u << k;
        }
        uint part = 64u, miss = 0u;
        if (other != 0u && !more) {
            /* P2's bit k is texel k's subset, and texel 0 is of subset 0 in all 64, as it is here */
            miss = 17u;
            for (uint p = 0u; p < 64u; p++) {
                const uint d = uint(bitCount(other ^ P2[p]));
                if (d < miss) {
                    miss = d;
                    part = p;
                }
            }
        }
        if (!more && miss <= SNX_BC7_HINT_MISS) {
            hint_on = true;
            hint_part = part;
            hint_rot = rot;
        }
    }
    const uvec4 block = Encode();
#if SNX_BC7_PASS == 1
    if (enc_more) {
        const uint n = atomicAdd(work_count, 1u);
        work_list[n] = (z * cy + by) * cx + bx;
        atomicMax(work_groups, (n >> 6u) + 1u);
        return;
    }
#endif
    bc7_blocks[(z * cy + by) * cx + bx] = block;
}
