/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */

/* Synthetic SM70+ instructions, with fields encoded independently of the lowerer's readers. */
static void ampere_op(u8* code, u32* count, u32 opcode, u32 dst, u32 a, u32 b, u32 modifiers) {
    u8* out = code + (*count)++ * 16;
    snx_st_le32(out, opcode | 7u << 12 | dst << 16 | a << 24);
    snx_st_le32(out + 4, b);
    snx_st_le32(out + 8, modifiers);
    snx_st_le32(out + 12, 0x000fe200);
}

/* Two workgroups write distinct values through a uniform base plus a per-thread offset. The pointer crosses a
   32-bit carry boundary. Its size is deliberately absent: the next parameter is unrelated data, not a descriptor.
   A native QMD release must become visible only after the actual dispatch, and must preserve neighboring bytes. */
static int check_ampere_compute(void) {
    const u64 program = 0x7000000, parameters = program + 0x2000, semaphore = program + 0x3000;
    const u64 mapping = 0x1fffff000ull, base = mapping + 0xf80;
    const u64 cpu = HEAP_BASE + 0x600000, code_cpu = HEAP_BASE + 0x700000;
    gmmu_map(mapping, cpu, 0x2000);
    static const u32 floats[16] = {
        0, 0x80000000, 0x3fe00000, 0xbfe00000, 0x7f800000, 0xff800000, 0x7fc00000, 0x7fa00000,
        0x4f000000, 0xcf000000, 0x4effffff, 0xceffffff, 1, 0x80000001, 0x4f7fffff, 0xcf800000
    };
    static const u32 integers[16] = {
        0, 0, 1, 0xffffffff, 0x7fffffff, 0x80000000, 0, 0,
        0x7fffffff, 0x80000000, 0x7fffff80, 0x80000080, 0, 0, 0x7fffffff, 0x80000000
    };
    gmmu_map(program, code_cpu, 0x10000);
    for (u32 variant = 0; variant < 49; ++variant) {
        u32 transform = variant < 9 ? variant / 3 : variant - 6;
        u32 width = variant < 9 ? 4 + variant % 3
                    : transform == 9 || (transform >= 17 && transform != 25 && transform != 30 &&
                                          transform != 32 && transform != 33) ? 6 : 4;
        u32 components = 1u << (width - 4);
        if (transform == 32 && g_vk.subgroup_size != 32 &&
            !(g_vk.subgroup32_stages & VK_SHADER_STAGE_COMPUTE_BIT)) continue; /* admission is checked separately */
        if (transform == 37 && !g_vk.has_null_descriptor) continue;
        u8 code[4096] = {0};
        u32 count = 0, call = 0, link_setup = 0;
        ampere_op(code, &count, 0x919, 0, 0, 0, 37u << 8);                /* R0 = CTAID.X */
        ampere_op(code, &count, 0x919, 1, 0, 0, 33u << 8);                /* R1 = TID.X */
        ampere_op(code, &count, 0x824, 0, 0, 32, 0x078e0201);             /* R0 = R0 * 32 + R1 */
        ampere_op(code, &count, 0x824, 2, 0, components * 4, 0x078e02ff); /* R2 = byte offset */
        ampere_op(code, &count, 0x810, 2, 2, 8, 0x07ffe0ff);     /* compensated by the store's negative displacement */
        ampere_op(code, &count, 0xab9, 0, 0, 0xb0u << 6, 5u << 9); /* UR0:1 = c[0][0xb0:0xb8] */
        if (transform == 42) {
            /* Local spills are per invocation, preserve neighboring words, and obey the execution predicate. */
            ampere_op(code, &count, 0x810, 12, 0, 0x12340000, 0x07ffe0ff);
            ampere_op(code, &count, 0x387, 0, 255, 4u << 8 | 12, 0x00100800);
            ampere_op(code, &count, 0x810, 12, 0, 0x56780000, 0x07ffe0ff);
            ampere_op(code, &count, 0x387, 0, 255, 8u << 8 | 12, 0x00100800);
            ampere_op(code, &count, 0x802, 12, 0, 0xdeadbeef, 0xf00);
            for (u32 k = 0; k < 3; ++k)
                ampere_op(code, &count, 0x983, 4 + k, 255, (k == 1 ? 8u : 4u) << 8, 0x00300800);
            ampere_op(code, &count, 0x387, 0, 255, 4u << 8 | 12, 0x00100800);
            snx_st_le32(code + (count - 1) * 16, 0xff00f387); /* @!PT must not overwrite the first spill. */
            ampere_op(code, &count, 0x983, 7, 255, 4u << 8, 0x00300800);
        } else if (transform == 41) {
            /* Half MUFU promotes the selected half, applies ABS, then returns one half with zero high bits. */
            ampere_op(code, &count, 0x802, 12, 0, 0x3c00c000, 0xf00);
            ampere_op(code, &count, 0x802, 13, 0, 0x40004400, 0xf00);
            ampere_op(code, &count, 0x308, 4, 0, 0x4000000c, 0xa00);
            ampere_op(code, &count, 0x308, 5, 0, 0x1000000c, 0xa00);
            ampere_op(code, &count, 0x308, 6, 0, 13, 0xe00);
            ampere_op(code, &count, 0x308, 7, 0, 0x1000000d, 0xe00);
            ampere_op(code, &count, 0x434, 0, 12, 0x38003800, 0x03b44000); /* P2 false, P3 true */
            for (u32 p = 2; p <= 3; ++p) {
                ampere_op(code, &count, 0x810, 2 + p, 2 + p, 0x100, 0x07ffe0ff);
                snx_st_le32(code + (count - 1) * 16, (2 + p) << 24 | (2 + p) << 16 | p << 12 | 0x810);
            }
        } else if (transform == 40) {
            ampere_op(code, &count, 0x802, 12, 0, 0x3c00c000, 0xf00); /* (-2, 1) */
            ampere_op(code, &count, 0x802, 13, 0, 0xc4003800, 0xf00); /* (0.5, -4) */
            ampere_op(code, &count, 0x230, 4, 12, 13, 0);
            ampere_op(code, &count, 0x230, 5, 12, 13, 0x800); /* A.H0_H0 */
            ampere_op(code, &count, 0x832, 6, 12, 0x38004000, 0);
            ampere_op(code, &count, 0x431, 7, 12, 0x4000b400, 13);
            ampere_op(code, &count, 0x882, 2, 0, 0x55, 0);
            ampere_op(code, &count, 0x290, 2, 63, 63, 0x0fffe03f);
            snx_st_le32(code + (count - 1) * 16, 0x3f02f290); /* @!UPT must preserve UR2 */
            ampere_op(code, &count, 0x290, 63, 63, 63, 0x0fffe03f);
            snx_st_le32(code + (count - 1) * 16, 0x3f3ff290); /* the compiler's guarded URZ no-op */
            ampere_op(code, &count, 0xc10, 4, 4, 2, 0x0fffe0ff);
        } else if (transform == 39) {
            /* Constant IABS must preserve INT_MIN and select the signed magnitude, not clear the sign bit. */
            for (u32 k = 0; k < 4; ++k)
                ampere_op(code, &count, 0xa13, 4 + k, 0, (0x80 + k * 4) << 6, 0);
        } else if (transform == 38) {
            /* A nonzero uniform pair with signed byte displacements and contended ADD/MAX. RZ must add zero. */
            ampere_op(code, &count, 0xab9, 6, 0, 16u << 6, 5u << 9);
            ampere_op(code, &count, 0x890, 6, 6, 0x100, 0x0fffe03f);
            ampere_op(code, &count, 0x98e, 0, 255, 0xffff8000, 0x0c10e106);
            ampere_op(code, &count, 0x98e, 0, 255, 0x8400, 0x0d10e106);
            ampere_op(code, &count, 0x98e, 0, 255, 0xffff88ff, 0x0c10e106);
            for (u32 k = 0; k < 4; ++k)
                ampere_op(code, &count, 0x810, 4 + k, 0, 0x12340000 + k, 0x07ffe0ff);
        } else if (transform == 34) {
            ampere_op(code, &count, 0x824, 12, 0, 0x03456789, 0x078e02ff);
            ampere_op(code, &count, 0x810, 12, 12, 0x87654321, 0x07ffe0ff);
            ampere_op(code, &count, 0x812, 13, 0, 3, 0x078ec0ff);
            ampere_op(code, &count, 0x810, 4, 0, 0x12340000, 0x07ffe0ff);
            ampere_op(code, &count, 0x810, 5, 0, 0xfedc0000, 0x07ffe0ff);
            ampere_op(code, &count, 0x810, 14, 0, 0xdead0000, 0x07ffe0ff);
            ampere_op(code, &count, 0x202, 6, 0, 12, 0xf00);
            ampere_op(code, &count, 0x202, 7, 0, 12, 0xf00);
            static const u32 shifts[] = {0, 1, 30, 31};
            for (u32 k = 0; k < 4; ++k) {
                ampere_op(code, &count, 0x80c, 0, 13, k, 0x03f02070);
                for (u32 component = 0; component < 4; ++component) {
                    u32 dest = 4 + component, a = component < 2 ? 12 : dest;
                    u32 b = component < 2 ? dest : component == 2 ? 14 : 255;
                    ampere_op(code, &count, 0x211, dest, a, b,
                              0x078f00ff | (component != 0 ? 0x200 : 0) | shifts[k] << 11);
                    snx_st_le32(code + (count - 1) * 16, a << 24 | dest << 16 | 0x211);
                }
            }
        } else if (transform == 33) {
            /* Eight lanes contend on each of four texels in each row. Check both coordinate words, the descriptor
               offset and the operand independently of the ordinary global-output witness. */
            ampere_op(code, &count, 0x812, 4, 0, 3, 0x078ec0ff);
            ampere_op(code, &count, 0x919, 5, 0, 0, 37u << 8);
            ampere_op(code, &count, 0x810, 8, 0, 1, 0x07ffe0ff);
            ampere_op(code, &count, 0x59f, 0, 4, 0x60000c08, 0x0010e000);
            ampere_op(code, &count, 0x202, 4, 0, 8, 0xf00);
        } else if (transform == 32) {
            /* Two warps share one workgroup. The second barrier is reached by only the first warp, so it must not
               become a workgroup execution barrier. Each warp reads its opposite half's shared stores. */
            ampere_op(code, &count, 0x824, 12, 1, 4, 0x078e02ff);
            ampere_op(code, &count, 0x810, 13, 0, 0x12340000, 0x07ffe0ff);
            ampere_op(code, &count, 0x388, 0, 12, 0x1000d, 4u << 9);
            ampere_op(code, &count, 0x948, 0, 0, 0xffffffff, 0x03800000);
            ampere_op(code, &count, 0x812, 14, 1, 16, 0x078e3cff);
            ampere_op(code, &count, 0x824, 14, 14, 4, 0x078e02ff);
            ampere_op(code, &count, 0x984, 4, 14, 0x10000, 4u << 9);
            ampere_op(code, &count, 0x80c, 0, 1, 32, 0x03f01070);
            u32 scope = count;
            ampere_op(code, &count, 0x945, 0, 0, 0, 0x03800000);
            u32 skip = count;
            ampere_op(code, &count, 0x947, 0, 0, 0, 0x03800000);
            snx_st_le32(code + skip * 16, 0x8947);
            ampere_op(code, &count, 0x948, 0, 0, 0xffffffff, 0x03800000);
            ampere_op(code, &count, 0x810, 4, 4, 0x1000, 0x07ffe0ff);
            u32 sync = count;
            ampere_op(code, &count, 0x941, 0, 0, 0, 0x03800000);
            snx_st_le32(code + scope * 16 + 4, (count - scope - 1) * 16);
            snx_st_le32(code + skip * 16 + 4, (sync - skip - 1) * 16);
        } else if (transform == 31) {
            ampere_op(code, &count, 0x824, 12, 0, 0x01234567, 0x078e02ff);
            ampere_op(code, &count, 0x810, 12, 12, 0x87654321, 0x07ffe0ff);
            ampere_op(code, &count, 0x812, 13, 0, 31, 0x078ec0ff);
            for (u32 bits = 0; bits < 32; ++bits) {
                ampere_op(code, &count, 0x80c, 0, 13, bits, 0x03f02070);
                for (u32 sign = 0; sign < 2; ++sign) {
                    ampere_op(code, &count, 0x81a, 4 + sign, 12, bits, sign << 9);
                    snx_st_le32(code + (count - 1) * 16, 12u << 24 | (4 + sign) << 16 | 0x81a);
                }
            }
            ampere_op(code, &count, 0x81a, 6, 12, 32, 0);
            ampere_op(code, &count, 0x81a, 12, 12, 32, 0x200);
            ampere_op(code, &count, 0x202, 7, 0, 12, 0xf00);
        } else if (transform == 30) {
            ampere_op(code, &count, 0x802, 12, 0, 0x3c00c000, 0xf00);
            ampere_op(code, &count, 0x802, 13, 0, 0xc4003800, 0xf00);
            ampere_op(code, &count, 0x812, 14, 0, 15, 0x078ec0ff);
            static const u32 selectors[3] = {0, 2, 3};
            for (u32 choice = 0; choice < 16; ++choice) {
                ampere_op(code, &count, 0x80c, 0, 14, choice, 0x03f02070);
                u32 pair = choice % 9;
                ampere_op(code, &count, 0x232, 4, 12, 13 | selectors[pair % 3] << 28, selectors[pair / 3] << 10);
                snx_st_le32(code + (count - 1) * 16, 12u << 24 | 4u << 16 | 0x232);
            }
        } else if (transform == 29) {
            for (u32 mode = 0; mode < 4; ++mode) {
                ampere_op(code, &count, 0x802, 12, 0, 0xdeadbeef, 0xf00);
                ampere_op(code, &count, 0x802, 13, 0, 1, 0xf00);
                ampere_op(code, &count, 0x882, 12, 0, 0x87654321, 0);
                ampere_op(code, &count, 0x882, 13, 0, 32 + (mode & 1), 0);
                u32 target = mode & 1 ? 13 : 12, upper = mode == 1 || mode == 2;
                ampere_op(code, &count, 0x299, target, upper ? 63 : 12, 13,
                          0x08000600 | (upper ? 12 : 63) | upper << 16 | (mode & 1) << 12 | (mode >> 1) << 11);
                ampere_op(code, &count, 0xc02, 4 + mode, 0, target, 0x08000f00);
            }
        } else if (transform == 28) {
            for (u32 mode = 0; mode < 4; ++mode) {
                ampere_op(code, &count, 0x802, 12, 0, 0x87654321, 0xf00);
                ampere_op(code, &count, 0x202, 13, 0, 0, 0xf00);
                u32 target = mode < 2 ? 12 : 13;
                ampere_op(code, &count, 0x219, target, 12, 13, 0x6ff | (mode & 1 ? 0 : 0x800));
                ampere_op(code, &count, 0x202, 4 + mode, 0, target, 0xf00);
            }
        } else if (transform == 27) {
            ampere_op(code, &count, 0x802, 12, 0, 0x3c00c000, 0xf00);
            ampere_op(code, &count, 0x802, 13, 0, 0xc4003800, 0xf00);
            ampere_op(code, &count, 0x232, 4, 12, 13, 0);
            ampere_op(code, &count, 0x802, 12, 0, 0xdeadb400, 0xf00);
            ampere_op(code, &count, 0xa3e, 12, 255, 8u << 6, 0x400c);
            ampere_op(code, &count, 0x202, 5, 0, 12, 0xf00);
            ampere_op(code, &count, 0x802, 12, 0, 0x3fc00000, 0xf00);
            ampere_op(code, &count, 0xa3e, 6, 12, 0, 255);
            ampere_op(code, &count, 0x802, 12, 0, 0x3c004000, 0xf00);
            ampere_op(code, &count, 0x232, 12, 12, 12, 0);
            ampere_op(code, &count, 0x202, 7, 0, 12, 0xf00);
        } else if (transform == 26) {
            for (u32 mode = 0; mode < 4; ++mode) {
                ampere_op(code, &count, 0x810, 12, 0, 17, 0x07ffe0ff);
                ampere_op(code, &count, 0x802, 13, 0, 0x89abcdef, 0xf00);
                u32 target = mode & 1 ? 12 : 13;
                ampere_op(code, &count, 0xa10, target, 12, 4u << 6 | (mode == 2 ? 0x80000000u : 0),
                          0x07ffe00d | (mode == 1 ? 0x100 : 0) | (mode == 3 ? 0x800 : 0));
                ampere_op(code, &count, 0x202, 4 + mode, 0, target, 0xf00);
            }
        } else if (transform == 17 || transform == 18) {
            /* Every FLO sign/shift mode, with and without source inversion. Zero, negative and positive inputs
               occur across the invocations. Keep each result separate, including the not-found sentinel. */
            ampere_op(code, &count, 0x824, 12, 0, 0x01234567, 0x078e02ff);
            for (u32 mode = 0; mode < 4; ++mode)
                ampere_op(code, &count, 0x300, 4 + mode, 0, 12 | (transform == 18 ? 0x80000000u : 0),
                          0x000e0000 | (mode & 1) << 10 | (mode >> 1) << 9);
        } else if (transform == 19) {
            static const u32 shifts[4] = {0, 1, 29, 31};
            ampere_op(code, &count, 0x882, 6, 0, 0xffffffff, 0);
            for (u32 mode = 0; mode < 4; ++mode) {
                ampere_op(code, &count, 0x882, 4, 0, 0xe0000080, 0);
                ampere_op(code, &count, mode & 1 ? 0x291 : 0x891, 4, 4, mode & 1 ? 6 : 0xffffffff,
                          0x0f8f003f | shifts[mode] << 11);
                ampere_op(code, &count, 0xc02, 4 + mode, 0, 4, 0x08000f00);
            }
        } else if (transform == 23) {
            ampere_op(code, &count, 0x824, 12, 0, 0x9e3779b9, 0x078e02ff);
            for (u32 mode = 0; mode < 2; ++mode) {
                ampere_op(code, &count, 0x3c4, 4 + mode, 12, 0, mode ? 0x14000 : 0xc000);
                ampere_op(code, &count, 0xc02, 4 + mode, 0, 4 + mode, 0x08000f00);
                ampere_op(code, &count, 0x802, 6 + mode, 0, 0xabc0 + mode, 0xf00);
            }
            ampere_op(code, &count, 0x812, 255, 1, 1, 0x0780c0ff);
            u32 scope = count;
            ampere_op(code, &count, 0x945, 0, 0, 0, 0x03800000);
            u32 skip = count;
            ampere_op(code, &count, 0x947, 0, 0, 0, 0x03800000);
            snx_st_le32(code + skip * 16, 0x947); /* odd lanes skip both reductions */
            for (u32 mode = 0; mode < 2; ++mode) {
                ampere_op(code, &count, 0x3c4, 6 + mode, 12, 0, mode ? 0x14000 : 0xc000);
                ampere_op(code, &count, 0xc02, 6 + mode, 0, 6 + mode, 0x08000f00);
            }
            u32 sync = count;
            ampere_op(code, &count, 0x941, 0, 0, 0, 0x03800000);
            snx_st_le32(code + scope * 16 + 4, (count - scope - 1) * 16);
            snx_st_le32(code + skip * 16 + 4, (sync - skip - 1) * 16);
        } else if (transform == 24) {
            ampere_op(code, &count, 0x80c, 0, 1, 0, 0x03f02070);
            ampere_op(code, &count, 0x388, 0, 255, 0x40ff, 4u << 9);
            snx_st_le32(code + (count - 1) * 16, 0xff000388); /* only TID 0 initializes the counter */
            ampere_op(code, &count, 0x802, 8, 0, 0xdeadbeef, 0xf00);
            ampere_op(code, &count, 0x388, 0, 255, 0x4408, 4u << 9);
            snx_st_le32(code + (count - 1) * 16, 0xff000388);
            ampere_op(code, &count, 0xb1d, 0, 0, 0, 0);
            ampere_op(code, &count, 0x802, 8, 0, 1, 0xf00);
            ampere_op(code, &count, 0x802, 12, 0, 0x100, 0xf00);
            ampere_op(code, &count, 0x38c, 4, 12, 0xffff4008, 0); /* signed byte offset -0xc0 */
            static const u32 scopes[4] = {0, 2, 3, 5};
            for (u32 s = 0; s < 4; ++s) ampere_op(code, &count, 0x992, 0, 0, 0, scopes[s] << 12);
            ampere_op(code, &count, 0x9ab, 0, 0, 0, 0);
            ampere_op(code, &count, 0xb1d, 0, 0, 0, 0);
            ampere_op(code, &count, 0x38c, 6, 12, 0xffff40ff, 0); /* read the final value by adding RZ */
            ampere_op(code, &count, 0x984, 5, 255, 0x4000, 4u << 9);
            ampere_op(code, &count, 0x984, 7, 255, 0x4400, 4u << 9);
        } else if (transform >= 20 && transform <= 22) {
            /* Poison a per-lane shared vector, then conditionally clear 32, 64 or 128 bits with RZ. The scaled
               address must survive zero-vector staging, and the unselected lanes and words must stay unchanged. */
            for (u32 k = 0; k < 4; ++k)
                ampere_op(code, &count, 0x802, 8 + k, 0, 0x11223344u + k, 0xf00);
            ampere_op(code, &count, 0x824, 12, 1, 16, 0x078e02ff);
            ampere_op(code, &count, 0x824, 13, 1, 2, 0x078e02ff);
            ampere_op(code, &count, 0x388, 0, 12, 0x4008, 6u << 9);
            ampere_op(code, &count, 0x812, 255, 1, 1, 0x0780c0ff); /* P0 = odd lane */
            ampere_op(code, &count, 0x388, 0, 13, 0x40ff, (transform - 16) << 9 | 2u << 14);
            snx_st_le32(code + (count - 1) * 16, 13u << 24 | 8u << 12 | 0x388); /* @!P0 */
            ampere_op(code, &count, 0x984, 4, 12, 0x4000, 6u << 9);
        } else if (transform == 16) {
            /* The guard is the carry destination. Both sums clear P3, but their register destination must still be
               written. The register case aliases both addends with its destination. */
            ampere_op(code, &count, 0x802, 4, 0, 0, 0xf00);
            for (u32 constant = 0; constant < 2; ++constant) {
                ampere_op(code, &count, 0x802, 104, 0, 8, 0xf00);
                ampere_op(code, &count, 0x804, 0, 104, 8, 0); /* P3 = true */
                ampere_op(code, &count, 0x802, 112, 0, constant ? 0x7ffffffe : 0x40000080, 0xf00);
                ampere_op(code, &count, 0x802, 114, 0, 7, 0xf00);
                ampere_op(code, &count, constant ? 0xa10 : 0x210, 112, 112, constant ? 4u << 6 : 112, 0x07f7e0ff);
                snx_st_le32(code + (count - 1) * 16, 112u << 24 | 112u << 16 | 3u << 12 | (constant ? 0xa10 : 0x210));
                ampere_op(code, &count, 0x224, 114, 255, 255, 0x018e0472);
                ampere_op(code, &count, 0x210, 4, 4, 112, 0x07ffe0ff);
                ampere_op(code, &count, 0x210, 4, 4, 114, 0x07ffe0ff);
            }
        } else if (transform == 15) {
            /* Every accepted I2F source size, sign and byte/half selector. Each invocation selects one form and writes
               its float bits directly, so signedness or selector errors cannot cancel in an aggregate. */
            ampere_op(code, &count, 0x824, 6, 0, 0x01234567, 0x078e02ff);
            ampere_op(code, &count, 0x810, 6, 6, 0xc8b4a03c, 0x07ffe0ff);
            ampere_op(code, &count, 0x812, 7, 0, 15, 0x078ec0ff); /* R7 = invocation & 15 */
            u32 form = 0;
            for (u32 size = 0; size < 3; ++size)
                for (u32 sign = 0; sign < 2; ++sign)
                    for (u32 part = 0; part < (4u >> size); ++part, ++form) {
                        ampere_op(code, &count, 0x80c, 0, 7, form, 0x03f02070); /* P0 = R7 == form */
                        ampere_op(code, &count, 0x306, 4, 0, 6 | part << 28, 0x1000 | size << 20 | sign << 10);
                        snx_st_le32(code + (count - 1) * 16, 0x00040306); /* @P0 */
                    }
            for (u32 extra = 14; extra < 16; ++extra) {
                ampere_op(code, &count, 0x80c, 0, 7, extra, 0x03f02070);
                ampere_op(code, &count, 0x306, 4, 0, 6, 0x201000 | (extra & 1) << 10);
                snx_st_le32(code + (count - 1) * 16, 0x00040306);
            }
            /* Both STG address forms store all sub-word widths, with high source bits to discard and a halfword of
               untouched fill between them. The offset includes each invocation, past the low-word address carry. */
            ampere_op(code, &count, 0x824, 8, 0, 16, 0x078e02ff);
            ampere_op(code, &count, 0xc02, 10, 0, 0, 0x08000f00);
            ampere_op(code, &count, 0xc02, 11, 0, 1, 0x08000f00);
            for (u32 address_form = 0; address_form < 2; ++address_form)
                for (u32 size = 0; size < 4; ++size) {
                    static const u32 source[4] = {0xabcd985a, 0xabcd98a5, 0x89abfedc, 0x12348076};
                    static const u32 offset[4] = {0, 1, 2, 6};
                    ampere_op(code, &count, 0x802, 12, 0, source[size], 0xf00);
                    u32 displacement = 0x400 + address_form * 8 + offset[size];
                    if (address_form)
                        ampere_op(code, &count, 0x986, 0, 8, 12 | displacement << 8, 0x0810a100 | size << 9);
                    else {
                        ampere_op(code, &count, 0x210, 14, 10, 8, 0x07f7e0ff);
                        ampere_op(code, &count, 0x224, 15, 255, 255, 0x018e040b);
                        ampere_op(code, &count, 0x386, 0, 14, 12 | displacement << 8, 0x0010a100 | size << 9);
                    }
                }
        } else if (transform >= 12 && transform <= 14) {
            ampere_op(code, &count, 0x80c, 0, 1, 16, 0x03f01070); /* P0 = TID.X < 16 */
            u32 left_pc = count;
            ampere_op(code, &count, 0x802, 6, 0, 0, 0xf00);
            u32 right_pc = count;
            ampere_op(code, &count, 0x802, 7, 0, 0, 0xf00);
            ampere_op(code, &count, 0x207, 6, 6, 7, 0);
            u32 indirect = count;
            ampere_op(code, &count, 0x949, 0, 6, 0u - (indirect + 1) * 16, 0x0383ffff);
            snx_st_le32(code + left_pc * 16 + 4, count * 16);
            ampere_op(code, &count, 0x810, 4, 0, 0x1000, 0x07ffe0ff);
            u32 join = count;
            if (transform == 13) {
                ampere_op(code, &count, 0x810, 4, 4, 0x40, 0x07ffe0ff);
                ampere_op(code, &count, 0x986, 0, 2, 0xfffff804, 0x0810a900);
                ampere_op(code, &count, 0x94d, 0, 0, 0, 0x03800000);
            } else
                ampere_op(code, &count, 0x947, 0, 0, 0, 0x03800000);
            snx_st_le32(code + right_pc * 16 + 4, count * 16);
            ampere_op(code, &count, 0x824, 4, 0, 1, 0x078e02ff);
            ampere_op(code, &count, 0x810, 4, 4, 0x2000, 0x07ffe0ff);
            if (transform != 13) snx_st_le32(code + join * 16 + 4, (count - join - 1) * 16);
            u32 preserved = count;
            ampere_op(code, &count, 0x810, 4, 4, 0x40, 0x07ffe0ff);
            snx_st_le32(code + preserved * 16, 0x04040810); /* @P0 */
            if (transform == 14) {
                ampere_op(code, &count, 0x802, 8, 0, 0, 0xf00);
                u32 loop = count;
                ampere_op(code, &count, 0x810, 8, 8, 1, 0x07ffe0ff);
                ampere_op(code, &count, 0x810, 4, 4, 3, 0x07ffe0ff);
                ampere_op(code, &count, 0x80c, 0, 8, 3, 0x03f21070); /* P1 = R8 < 3 */
                ampere_op(code, &count, 0x802, 9, 0, loop * 16, 0xf00);
                u32 after = count;
                ampere_op(code, &count, 0x802, 10, 0, 0, 0xf00);
                ampere_op(code, &count, 0x207, 9, 9, 10, 1u << 23);
                u32 back = count;
                ampere_op(code, &count, 0x949, 0, 9, 0u - (back + 1) * 16, 0x0383ffff);
                snx_st_le32(code + after * 16 + 4, count * 16);
            }
        } else if (transform == 9) {
            ampere_op(code, &count, 0xa11, 12, 0, 0xb0u << 6, 0x078020ff); /* low address, P0 carry */
            ampere_op(code, &count, 0xa10, 13, 255, 0xb4u << 6, 0x007fe4ff); /* high address plus P0 */
            ampere_op(code, &count, 0x802, 4, 0, 0xfffffff0, 0xf00);
            ampere_op(code, &count, 0x211, 4, 0, 4, 0x078220ff); /* destination aliases the addend */
            ampere_op(code, &count, 0x802, 8, 0, 0x100, 0xf00);
            ampere_op(code, &count, 0x207, 8, 8, 255, 0x00800000);
            ampere_op(code, &count, 0x210, 4, 4, 8, 0x07ffe0ff);
            ampere_op(code, &count, 0x812, 5, 4, ~0u, 0x078e3cff);
        } else if (transform == 11) {
            /* A compute program's forms: UIMAD.WIDE, POPC and FLO of registers and uniform registers, LOP3 with a
               uniform operand, a BRA with a condition predicate besides its guard, a BREAK out of its barrier, and an
               ATOMG.ADD that every thread applies to one counter at a uniform base */
            ampere_op(code, &count, 0x882, 6, 0, 0x89abcdef, 0);
            ampere_op(code, &count, 0x882, 7, 0, 0x12345, 0);
            ampere_op(code, &count, 0x882, 8, 0, 0xfffffff0, 0);
            ampere_op(code, &count, 0x882, 9, 0, 1, 0);
            ampere_op(code, &count, 0x2a5, 4, 6, 7, 0x0f8e0008);   /* UR4:UR5 = UR6 * UR7 + UR8:UR9 */
            ampere_op(code, &count, 0xc02, 4, 0, 4, 0x08000f00);
            ampere_op(code, &count, 0xc02, 5, 0, 5, 0x08000f00);
            ampere_op(code, &count, 0x210, 4, 4, 5, 0x07ffe0ff);
            ampere_op(code, &count, 0x824, 11, 0, 0x9e3779b9, 0x078e02ff);
            ampere_op(code, &count, 0x309, 10, 0, 11, 0);           /* POPC R10 = popcount(R11) */
            ampere_op(code, &count, 0xd09, 12, 0, 6, 0x08000000);  /* POPC R12 = popcount(UR6) */
            ampere_op(code, &count, 0xd00, 13, 0, 7, 0x080e0000);  /* FLO R13 = 16 */
            ampere_op(code, &count, 0xd00, 14, 0, 7, 0x080e0400);  /* FLO.SH R14 = 15 */
            ampere_op(code, &count, 0xc12, 15, 0, 6, 0x0f8e3cff);  /* R15 = R0 ^ UR6 */
            static const u32 terms[] = {10, 12, 13, 14, 15};
            for (u32 t = 0; t < sizeof terms / sizeof *terms; ++t)
                ampere_op(code, &count, 0x210, 4, 4, terms[t], 0x07ffe0ff);
            for (u32 bit = 0; bit < 2; ++bit) /* P0 = TID.X & 1, P1 = TID.X & 2 */
                ampere_op(code, &count, 0x812, 255, 1, 1u << bit, 0x0780c0ff | bit << 17);
            /* MATCH.ALL.U64 inside the region below that only lanes with TID.X & 2 clear run: R18:R19 is equal over
               those lanes and differs on the others, so P2 holds only where the match ran over the active lanes */
            ampere_op(code, &count, 0x20b, 0, 255, 255, 0x03f41000); /* P2 = false */
            ampere_op(code, &count, 0x812, 18, 1, 2, 0x078ec0ff);    /* R18 = TID.X & 2 */
            ampere_op(code, &count, 0x802, 19, 0, 0, 0xf00);
            u32 scope = count;
            ampere_op(code, &count, 0x945, 0, 0, 0, 0x03800000);
            u32 choose = count;
            ampere_op(code, &count, 0x947, 0, 0, 0, 0x00800000);
            snx_st_le32(code + choose * 16, 0x0947); /* @P0 BRA P1: skips the add when both hold */
            ampere_op(code, &count, 0x810, 4, 4, 0x1000, 0x07ffe0ff);
            u32 sync = count;
            ampere_op(code, &count, 0x941, 0, 0, 0, 0x03800000);
            snx_st_le32(code + scope * 16 + 4, (count - scope - 1) * 16);
            snx_st_le32(code + choose * 16 + 4, (sync - choose - 1) * 16);
            /* Leaving the inner barrier must not make the outer BSYNC return to the skipped body. */
            u32 outer = count;
            ampere_op(code, &count, 0x945, 2, 0, 0, 0x03800000);
            u32 scope1 = count;
            ampere_op(code, &count, 0x945, 1, 0, 0, 0x03800000);
            u32 leave = count;
            ampere_op(code, &count, 0x942, 1, 0, 0, 0x03800000);
            snx_st_le32(code + leave * 16, 0x11942); /* @P1 BREAK B1 */
            u32 jump = count;
            ampere_op(code, &count, 0x947, 0, 0, 0, 0x03800000);
            snx_st_le32(code + jump * 16, 0x1947); /* @P1 BRA past the add */
            ampere_op(code, &count, 0x810, 4, 4, 0x2000, 0x07ffe0ff);
            ampere_op(code, &count, 0x3a1, 255, 18, 0, 0x00040200); /* MATCH.ALL.U64 P2, R18 */
            ampere_op(code, &count, 0x941, 1, 0, 0, 0x03800000);
            snx_st_le32(code + scope1 * 16 + 4, (count - scope1 - 1) * 16);
            ampere_op(code, &count, 0x810, 4, 4, 0x4000, 0x07ffe0ff);
            snx_st_le32(code + (count - 1) * 16, 0x04042810); /* @P2 */
            ampere_op(code, &count, 0x810, 4, 4, 0x8000, 0x07ffe0ff);
            u32 outer_sync = count;
            ampere_op(code, &count, 0x941, 2, 0, 0, 0x03800000);
            snx_st_le32(code + outer * 16 + 4, (count - outer - 1) * 16);
            snx_st_le32(code + jump * 16 + 4, (outer_sync - jump - 1) * 16);
            ampere_op(code, &count, 0x802, 17, 0, 1, 0xf00);
            ampere_op(code, &count, 0x9a8, 16, 255, 17 | 0x300u << 8, 0x080ee140); /* [UR0:UR1 + 0x300] += 1 */
            /* The same counter's neighbour through a register pair, past the 32-bit carry, from the even lanes */
            ampere_op(code, &count, 0xc02, 26, 0, 0, 0x08000f00);
            ampere_op(code, &count, 0xc02, 27, 0, 1, 0x08000f00);
            ampere_op(code, &count, 0x802, 28, 0, 3, 0xf00);
            ampere_op(code, &count, 0x3a8, 255, 26, 28 | 0x304u << 8, 0x001ee100);
            snx_st_le32(code + (count - 1) * 16, 0x1aff83a8); /* @!P0 [R26:R27 + 0x304] += 3 */
            /* Negated addends, first with both sources aliased by the destination, then with just the addend.
               A negative immediate and the doubled result wrap in 32 bits. */
            ampere_op(code, &count, 0x824, 4, 4, 0xffffffff, 0x078e0804);
            ampere_op(code, &count, 0x824, 4, 0, 1, 0x078e0a04);
            /* Mixed GPR/UR multiplication, with aliased inputs, URZ, both signedness bits and an ordinary guard.
               Poison R63 so that mistaking URZ for an ordinary register cannot pass. */
            ampere_op(code, &count, 0xc24, 4, 4, 6, 0x0f8e0200);
            ampere_op(code, &count, 0x802, 63, 0, 0x10203040, 0xf00);
            ampere_op(code, &count, 0xc24, 4, 0, 63, 0x0f8e0804);
            ampere_op(code, &count, 0xc24, 4, 0, 7, 0x0f8e0004);
            ampere_op(code, &count, 0xc24, 4, 4, 6, 0x0f8e0a04);
            snx_st_le32(code + (count - 1) * 16, 0x04040c24); /* @P0, destination aliases both GPR sources */
            /* IADD3 with a uniform second source: poisoned R6 to R9 catch a GPR read in place of the UR, and R63
               catches URZ read as a GPR. Aliased destination, negated UR, negated addend and an ordinary guard. */
            for (u32 r = 6; r <= 9; ++r) ampere_op(code, &count, 0x802, r, 0, 0x5a5a0000u + r, 0xf00);
            ampere_op(code, &count, 0xc10, 4, 4, 6, 0x0fffe000);              /* R4 = R4 + UR6 + R0 */
            ampere_op(code, &count, 0xc10, 4, 0, 7 | 0x80000000u, 0x0fffe004); /* R4 = R0 - UR7 + R4 */
            ampere_op(code, &count, 0xc10, 4, 4, 63, 0x0fffe0ff);             /* R4 = R4 + URZ + RZ */
            ampere_op(code, &count, 0xc10, 4, 4, 9, 0x0fffe800);              /* R4 = R4 + UR9 - R0 */
            ampere_op(code, &count, 0xc10, 4, 4, 8, 0x0fffe0ff);
            snx_st_le32(code + (count - 1) * 16, 0x04040c10); /* @P0 R4 = R4 + UR8 */
            /* LEA.HI of a zero high word: the destination aliases the addend, then the shifted source, then a guard */
            ampere_op(code, &count, 0x211, 4, 0, 4, 0x078fe8ff); /* R4 = (R0 >> 3) + R4 */
            ampere_op(code, &count, 0x211, 4, 4, 0, 0x078f08ff); /* R4 = (R4 >> 31) + R0 */
            ampere_op(code, &count, 0x211, 4, 4, 4, 0x078f20ff);
            snx_st_le32(code + (count - 1) * 16, 0x04040211); /* @P0 R4 = (R4 >> 28) + R4 */
            /* IMAD with a uniform addend: the move of a negated UR that the game uses, then an aliased product of two
               GPRs (R6 holds poison, so UR6 must be the addend) and a guard */
            ampere_op(code, &count, 0xe24, 5, 255, 0x80000006, 0x0f8e02ff); /* R5 = -UR6 */
            ampere_op(code, &count, 0xe24, 4, 4, 6, 0x0f8e0200);           /* R4 = R4 * R0 + UR6 */
            ampere_op(code, &count, 0x210, 4, 4, 5, 0x07ffe0ff);           /* R4 += R5 */
            ampere_op(code, &count, 0xe24, 4, 0, 7, 0x0f8e0004);
            snx_st_le32(code + (count - 1) * 16, 0x00040e24); /* @P0 R4 = R0 * R4 + UR7 */
            /* Converged matches: an equal pair, a pair whose high word is TID.X, and one whose low word is */
            ampere_op(code, &count, 0x802, 20, 0, 0x11111111, 0xf00);
            ampere_op(code, &count, 0x802, 21, 0, 0x22222222, 0xf00);
            ampere_op(code, &count, 0x802, 22, 0, 0x33333333, 0xf00);
            ampere_op(code, &count, 0x810, 23, 1, 0, 0x07ffe0ff);
            ampere_op(code, &count, 0x810, 24, 1, 0, 0x07ffe0ff);
            ampere_op(code, &count, 0x802, 25, 0, 0x44444444, 0xf00);
            for (u32 p = 3; p <= 5; ++p) ampere_op(code, &count, 0x3a1, 255, 20 + (p - 3) * 2, 0, 0x200 | p << 17);
            for (u32 p = 3; p <= 5; ++p) {
                ampere_op(code, &count, 0x810, 4, 4, 1u << (p - 3), 0x07ffe0ff);
                snx_st_le32(code + (count - 1) * 16, 0x04040810 | p << 12); /* @Pp */
            }
            /* IMAD.HI.U32 by a division magic number, in place, then again on the odd lanes */
            ampere_op(code, &count, 0x810, 30, 0, 0x89abcdef, 0x07ffe0ff);
            ampere_op(code, &count, 0x827, 30, 30, 0xf0f0f0f1, 0x078e00ff);
            ampere_op(code, &count, 0x827, 30, 30, 0xf0f0f0f1, 0x078e00ff);
            snx_st_le32(code + (count - 1) * 16, 0x1e1e0827); /* @P0 */
            ampere_op(code, &count, 0x210, 4, 4, 30, 0x07ffe0ff);
            /* IMNMX with a constant: an unsigned minimum with c[0][0x18], then a signed maximum with c[0][0x4] (3) */
            ampere_op(code, &count, 0x810, 31, 0, 0x12340008, 0x07ffe0ff);
            ampere_op(code, &count, 0xa17, 31, 31, 0x18u << 6, 0x03800000);
            ampere_op(code, &count, 0x810, 32, 0, 0xfffffffc, 0x07ffe0ff);
            ampere_op(code, &count, 0xa17, 32, 32, 0x4u << 6, 0x07800200);
            ampere_op(code, &count, 0x210, 4, 4, 31, 0x07ffe0ff);
            ampere_op(code, &count, 0x210, 4, 4, 32, 0x07ffe0ff);
            /* F2FP.PACK_AB: 1.0 in the high half, -2.0 in the low, the destination aliasing B */
            ampere_op(code, &count, 0x802, 33, 0, 0x3f800000, 0xf00);
            ampere_op(code, &count, 0x802, 34, 0, 0xc0000000, 0xf00);
            ampere_op(code, &count, 0x23e, 34, 33, 34, 0x000000ff);
            ampere_op(code, &count, 0x210, 4, 4, 34, 0x07ffe0ff);
            /* LDG of 8 and 16 bits, after a store of 0x80ff7f01 at [UR0 + R2 - 8 + 0x400], past the counters: byte 1
               unsigned, byte 3 signed, half 0 unsigned and half 1 signed */
            ampere_op(code, &count, 0x802, 45, 0, 0x80ff7f01, 0xf00);
            ampere_op(code, &count, 0x986, 0, 2, 45 | 0x3f8u << 8, 0x0810a900);
            ampere_op(code, &count, 0x981, 41, 2, 0x3f9u << 8, 0x081ea100);
            ampere_op(code, &count, 0x981, 42, 2, 0x3fbu << 8, 0x081ea300);
            ampere_op(code, &count, 0x981, 43, 2, 0x3f8u << 8, 0x081ea500);
            ampere_op(code, &count, 0x981, 44, 2, 0x3fau << 8, 0x081ea700);
            /* UIMAD by an immediate, SEL with a constant, IMAD with an immediate addend */
            ampere_op(code, &count, 0x8a4, 10, 7, 0x10, 0x0f8e0009);   /* UR10 = UR7 * 16 + UR9 */
            ampere_op(code, &count, 0xc02, 46, 0, 10, 0x08000f00);
            ampere_op(code, &count, 0xa07, 47, 0, 0x18u << 6, 0);     /* R47 = P0 ? R0 : c[0][0x18] */
            ampere_op(code, &count, 0x424, 48, 0, 0x1000, 0x078e0200); /* R48 = R0 * R0 + 0x1000 */
            /* FSWZADD 0x1b: the quad's lanes add, subtract B, subtract A, and move B */
            ampere_op(code, &count, 0x802, 51, 0, 0x3f800000, 0xf00);
            ampere_op(code, &count, 0x802, 52, 0, 0x40800000, 0xf00);
            ampere_op(code, &count, 0x822, 53, 51, 0x1b, 52);
            /* IABS of R0 - 32, and F2I.S32.TRUNC of a uniform -2.5 */
            ampere_op(code, &count, 0x810, 55, 0, 0xffffffe0, 0x07ffe0ff);
            ampere_op(code, &count, 0x213, 54, 0, 55, 0);
            ampere_op(code, &count, 0x882, 11, 0, 0xc0200000, 0);
            ampere_op(code, &count, 0xd05, 56, 0, 11, 0x0821f100);
            static const u32 batch[] = {41, 42, 43, 44, 46, 47, 48, 53, 54, 56};
            for (u32 t = 0; t < sizeof batch / sizeof *batch; ++t)
                ampere_op(code, &count, 0x210, 4, 4, batch[t], 0x07ffe0ff);
            /* Shared memory: STS.32 at [TID.X4 + 0x100] read back at [TID * 4 + 0x100] and at [TID * 4 + UR12 + 0xc0]
               (UR12 = 0x40), and STS.64 / LDS.64 at [TID.X8 + 0x200] */
            ampere_op(code, &count, 0x812, 58, 0, 0xabc, 0x078e3cff);              /* R58 = R0 ^ 0xabc */
            ampere_op(code, &count, 0x388, 0, 1, 58 | 0x100u << 8, 0x4800);
            ampere_op(code, &count, 0x824, 60, 1, 4, 0x078e02ff);
            ampere_op(code, &count, 0x984, 59, 60, 0x100u << 8, 0x800);
            ampere_op(code, &count, 0x810, 62, 0, 0x5000, 0x07ffe0ff);
            ampere_op(code, &count, 0x802, 63, 0, 0x77, 0xf00);
            ampere_op(code, &count, 0x388, 0, 1, 62 | 0x200u << 8, 0x8a00);
            ampere_op(code, &count, 0x984, 64, 1, 0x200u << 8, 0x8a00);
            ampere_op(code, &count, 0x882, 12, 0, 0x40, 0);
            ampere_op(code, &count, 0x984, 66, 60, 12 | 0xc0u << 8, 0x08000800);
            static const u32 shared[] = {59, 64, 65, 66};
            for (u32 t = 0; t < sizeof shared / sizeof *shared; ++t)
                ampere_op(code, &count, 0x210, 4, 4, shared[t], 0x07ffe0ff);
            /* BAR.SYNC 0, then each thread reads the shared word of TID ^ 16. Lavapipe runs this workgroup in lockstep,
               so the read passes without the barrier too: this checks that the barrier lowers and translates */
            ampere_op(code, &count, 0xb1d, 0, 0, 0, 0);
            ampere_op(code, &count, 0x812, 68, 1, 16, 0x078e3cff);
            ampere_op(code, &count, 0x824, 69, 68, 4, 0x078e02ff);
            ampere_op(code, &count, 0x984, 67, 69, 0x100u << 8, 0x800);
            /* FADD with a negated uniform operand: 1.0 + -UR13 (3.0); LDC.S8 and LDC.U16 of c[0][0] (0xbfe00000) */
            ampere_op(code, &count, 0x882, 13, 0, 0x40400000, 0);
            ampere_op(code, &count, 0xe21, 70, 51, 0x80000000u | 13, 0x08000000);
            ampere_op(code, &count, 0xb82, 71, 255, 3u << 6, 0x200);
            ampere_op(code, &count, 0xb82, 72, 255, 2u << 6, 0x400);
            static const u32 more[] = {67, 70, 71, 72};
            for (u32 t = 0; t < sizeof more / sizeof *more; ++t)
                ampere_op(code, &count, 0x210, 4, 4, more[t], 0x07ffe0ff);
            /* SHFL.IDX of lane 3 with a register mask (0x1f), SHFL.UP by 1 with RZ, and FFMA 1.0 * 4.0 + -UR13 */
            ampere_op(code, &count, 0x802, 74, 0, 0x1f, 0xf00);
            ampere_op(code, &count, 0x989, 75, 0, 3u << 21, 0x000e004a);
            ampere_op(code, &count, 0x989, 73, 0, 1u << 21 | 1u << 26, 0x000e00ff);
            ampere_op(code, &count, 0xe23, 76, 51, 0x80000000u | 13, 0x08000034);
            static const u32 last[] = {73, 75, 76};
            for (u32 t = 0; t < sizeof last / sizeof *last; ++t)
                ampere_op(code, &count, 0x210, 4, 4, last[t], 0x07ffe0ff);
            /* Packed halves: R77 = (1.0, -2.0), R79 = (2.0, 0.5), R81 = (0.5, 0.5), low half first. HMNMX2's maximum
               with an immediate pair, of A's H1_H1, and the minimum with a register; HSETP2.GT writing the low
               half's outcome to P2 and the high half's to P3; PRMT 0x7a31 of A's bytes 1 and 3, a sign of A's byte
               2 and C's byte 3 */
            ampere_op(code, &count, 0x802, 77, 0, 0xc0003c00, 0xf00);
            ampere_op(code, &count, 0x802, 79, 0, 0x38004000, 0xf00);
            ampere_op(code, &count, 0x802, 81, 0, 0x38003800, 0xf00);
            ampere_op(code, &count, 0x840, 78, 77, 0x38003800, 0x07800000);
            ampere_op(code, &count, 0x840, 80, 77, 0x38003800, 0x07800c00);
            ampere_op(code, &count, 0x240, 83, 79, 77, 0x03800000);
            ampere_op(code, &count, 0x234, 0, 77, 81, 0x03b44000);
            ampere_op(code, &count, 0x816, 82, 77, 0x7a31, 81);
            static const u32 halves[] = {78, 80, 82, 83};
            for (u32 t = 0; t < sizeof halves / sizeof *halves; ++t)
                ampere_op(code, &count, 0x210, 4, 4, halves[t], 0x07ffe0ff);
            for (u32 p = 2; p <= 3; ++p) {
                ampere_op(code, &count, 0x810, 4, 4, 0x80u << p, 0x07ffe0ff);
                snx_st_le32(code + (count - 1) * 16, 0x04040810 | p << 12); /* @Pp */
            }
            /* SHF with a register shift (R85 = 35, R89 = 4, R90 = 40): SHF.L.W.U32.HI of an immediate high word 1,
               SHF.R.W.U32.HI and SHF.R.S32.HI of R87 = 0x80000000 with RZ low, SHF.R.U32 of R77 with R85 high. The
               wrap takes 35 as 3, the clamp takes 40 as 32. */
            static const u32 shift_sources[][2] = {{85, 35}, {87, 0x80000000u}, {89, 4}, {90, 40}};
            for (u32 t = 0; t < 4; ++t) ampere_op(code, &count, 0x802, shift_sources[t][0], 0, shift_sources[t][1], 0xf00);
            ampere_op(code, &count, 0x419, 84, 255, 1, 0x10e55);
            ampere_op(code, &count, 0x219, 86, 255, 85, 0x11e57);
            ampere_op(code, &count, 0x219, 91, 255, 90, 0x11457);
            ampere_op(code, &count, 0x219, 88, 77, 89, 0x01655);
            static const u32 shifted[] = {84, 86, 88, 91};
            for (u32 t = 0; t < sizeof shifted / sizeof *shifted; ++t)
                ampere_op(code, &count, 0x210, 4, 4, shifted[t], 0x07ffe0ff);
            /* FMNMX and IMNMX of a uniform register: the maximum of 1.0 and UR14 = 2.0, the FTZ minimum of 3.0 and UR14,
               and the signed and unsigned minimum of the invocation ID and UR15 = 0xfffffffb */
            ampere_op(code, &count, 0x882, 14, 0, 0x40000000, 0);
            ampere_op(code, &count, 0x882, 15, 0, 0xfffffffb, 0);
            ampere_op(code, &count, 0x802, 92, 0, 0x3f800000, 0xf00);
            ampere_op(code, &count, 0x802, 97, 0, 0x40400000, 0xf00);
            ampere_op(code, &count, 0xc09, 93, 92, 14, 0x0f800000);
            ampere_op(code, &count, 0xc09, 94, 97, 14, 0x0b810000);
            ampere_op(code, &count, 0xc17, 95, 0, 15, 0x0b800200);
            ampere_op(code, &count, 0xc17, 96, 0, 15, 0x0b800000);
            for (u32 t = 93; t <= 96; ++t) ampere_op(code, &count, 0x210, 4, 4, t, 0x07ffe0ff);
            /* RED without a result at the register pair R26:R27 (base): MAX.S32 of the invocation ID at 0x308 over the
               0xcdcd... fill, which is negative, and ADD.U32 of 2 at 0x30c. STG.U8 at 0x310 and STG.U16 at 0x312 keep
               the byte between them */
            ampere_op(code, &count, 0x802, 99, 0, 2, 0xf00);
            ampere_op(code, &count, 0x98e, 0, 26, 0 | 0x308u << 8, 0x0110e300);
            ampere_op(code, &count, 0x98e, 0, 26, 99 | 0x30cu << 8, 0x0010e100);
            ampere_op(code, &count, 0x802, 102, 0, 0x5a, 0xf00);
            ampere_op(code, &count, 0x802, 103, 0, 0x1234, 0xf00);
            ampere_op(code, &count, 0x386, 0, 26, 102 | 0x310u << 8, 0x0010a100);
            ampere_op(code, &count, 0x386, 0, 26, 103 | 0x312u << 8, 0x0010a100 | 2u << 9);
            /* P2R after R2P of 0x55 (P0, P2, P4 and P6): of all seven into RZ, and of the low four into byte 1 of
               R106 = 0xffff0000 */
            ampere_op(code, &count, 0x802, 104, 0, 0x55, 0xf00);
            ampere_op(code, &count, 0x804, 0, 104, 0x7f, 0);
            ampere_op(code, &count, 0x803, 105, 255, 0x7f, 0);
            ampere_op(code, &count, 0x802, 106, 0, 0xffff0000u, 0xf00);
            ampere_op(code, &count, 0x803, 107, 106, 0x0f, 0x1000);
            /* I2F of R108 = 0xc8b4a03c: U8 of byte 3 (200), S16 of half 1 (-14156), U16 of half 0 (41020) */
            ampere_op(code, &count, 0x802, 108, 0, 0xc8b4a03cu, 0xf00);
            ampere_op(code, &count, 0x306, 109, 0, 108 | 3u << 28, 0x00001000);
            ampere_op(code, &count, 0x306, 110, 0, 108 | 1u << 28, 0x00101400);
            ampere_op(code, &count, 0x306, 111, 0, 108, 0x00101000);
            static const u32 converted[] = {105, 107, 109, 110, 111};
            for (u32 t = 0; t < sizeof converted / sizeof *converted; ++t)
                ampere_op(code, &count, 0x210, 4, 4, converted[t], 0x07ffe0ff);
            /* Register IADD3 writes P3, then IMAD.X consumes P3 for the high word. The second sum crosses the signed
               boundary without an unsigned carry. Destinations alias their low and high input registers. */
            for (u32 test = 0; test < 2; ++test) {
                ampere_op(code, &count, 0x802, 112, 0, test ? 0x7fffff00u : 0xffffff00u, 0xf00);
                ampere_op(code, &count, 0x802, 113, 0, 0x200, 0xf00);
                ampere_op(code, &count, 0x802, 114, 0, test ? 7 : 5, 0xf00);
                ampere_op(code, &count, 0x802, 115, 0, 1, 0xf00);
                ampere_op(code, &count, 0x210, 112, 112, 113, 0x07f7e0ff); /* P3 = unsigned carry */
                ampere_op(code, &count, 0x224, 114, 114, 115, 0x018e04ff); /* R114 = R114 * 1 + RZ + P3 */
                ampere_op(code, &count, 0x210, 4, 4, 112, 0x07ffe0ff);
                ampere_op(code, &count, 0x210, 4, 4, 114, 0x07ffe0ff);
            }
        } else if (transform == 10) {
            /* The integer forms of the game's vertex code: funnel shifts, F2I to U16, R2P, and a 64-bit address sum as
               IADD3 with a carry out plus IMAD.X with that carry in, both reading constants, and signed IMADs. */
            ampere_op(code, &count, 0x819, 4, 0, 3, 0x000006ff);             /* SHF.L.U32 R4 = R0 << 3 */
            ampere_op(code, &count, 0x810, 9, 0, 0u - 40, 0x07ffe0ff);         /* R9 = R0 - 40 */
            ampere_op(code, &count, 0x819, 5, 255, 2, 0x00011409);           /* SHF.R.S32.HI R5 = R9 >> 2 */
            ampere_op(code, &count, 0x810, 7, 0, 0x89abcdef, 0x07ffe0ff);
            ampere_op(code, &count, 0x202, 8, 0, 0, 0xf00);
            ampere_op(code, &count, 0x819, 6, 7, 4, 0x00001608);             /* SHF.R.U32 R6 = R8:R7 >> 4 */
            ampere_op(code, &count, 0x245, 10, 0, 0, 0x00201000);
            ampere_op(code, &count, 0x820, 10, 10, 0x44899000, 0x00410000); /* R10 = R0 * 1100.5 */
            ampere_op(code, &count, 0x305, 11, 0, 10, 0x00212800);           /* F2I.U16.FTZ R11, round to nearest */
            ampere_op(code, &count, 0x824, 12, 0, 256, 0x078e02ff);
            ampere_op(code, &count, 0x804, 0, 12, 3, 0x00001000);            /* R2P P0, P1 from byte 1 */
            ampere_op(code, &count, 0x802, 14, 0, 0x100, 0xf00);
            ampere_op(code, &count, 0x207, 13, 14, 255, 0);                  /* P0 ? 0x100 : 0 */
            ampere_op(code, &count, 0x207, 15, 14, 255, 0x00800000);         /* P1 ? 0x100 : 0 */
            ampere_op(code, &count, 0x810, 16, 0, 0xedcbffe0, 0x07ffe0ff);
            ampere_op(code, &count, 0xa10, 17, 16, 0x10u << 6, 0x07f5e0ff);   /* R17 = R16 + c[0][0x10], P2 carry */
            ampere_op(code, &count, 0x624, 18, 255, 0x14u << 6, 0x010e06ff);  /* R18 = c[0][0x14] + P2 */
            ampere_op(code, &count, 0x624, 19, 0, 0x18u << 6, 0x078e020e);    /* R19 = R0 * R14 + c[0][0x18] */
            ampere_op(code, &count, 0xa24, 20, 0, 0x1cu << 6, 0x078e020e);    /* R20 = R0 * c[0][0x1c] + R14 */
            ampere_op(code, &count, 0x820, 22, 10, 0x3c23d70a, 0x00410000);   /* R22 = R10 * 0.01 */
            ampere_op(code, &count, 0x305, 21, 0, 22, 0x0021e100);           /* F2I.S8.TRUNC: 127 from thread 12 */
            ampere_op(code, &count, 0x820, 24, 10, 0xbc23d70a, 0x00410000);   /* R24 = R10 * -0.01 */
            ampere_op(code, &count, 0x305, 23, 0, 24, 0x0021e100);           /* -128 from thread 12 */
            static const u32 terms[] = {5, 6, 11, 13, 15, 17, 18, 19, 20, 21, 23};
            for (u32 t = 0; t < sizeof terms / sizeof *terms; ++t)
                ampere_op(code, &count, 0x210, 4, 4, terms[t], 0x07ffe0ff);
        } else if (transform == 8) {
            ampere_op(code, &count, 0x80c, 0, 1, 16, 0x03f01070); /* varying P0 */
            ampere_op(code, &count, 0x20c, 0, 255, 255, 0x03f22070); /* P1 = true */
            ampere_op(code, &count, 0x20b, 0, 255, 255, 0x03fc1000); /* P6 = false */
            ampere_op(code, &count, 0x9c3, 2, 0, 0, 37u << 8);
            ampere_op(code, &count, 0x882, 3, 0, 32, 0);
            ampere_op(code, &count, 0x88c, 0, 2, 0, 0x0bf02070); /* UP0 = workgroup == 0 */
            ampere_op(code, &count, 0x28c, 0, 2, 3, 0x0bf21070); /* UP1 = workgroup < 32 */
            ampere_op(code, &count, 0x887, 3, 3, 64, 0x08000000);
            ampere_op(code, &count, 0x892, 63, 2, 1, 0x0f82c03f); /* UP1 = NZ(workgroup & 1), discard data */
            ampere_op(code, &count, 0x287, 4, 3, 2, 0x08800000);
            ampere_op(code, &count, 0x88c, 0, 2, 0, 0x08f42870); /* UP2 = (workgroup == 0) XOR UP1 */
            ampere_op(code, &count, 0x886, 5, 0, 0, 0x01060000); /* All(UP2), UP3 */
            ampere_op(code, &count, 0x2bd, 5, 0, 5, 0x080e0000);
            ampere_op(code, &count, 0x887, 6, 3, 7, 0x0c800000); /* inverted UP1 */
            ampere_op(code, &count, 0x887, 6, 6, 0xdead, 0x09800000); /* UP3 must survive other UP reads */
            ampere_op(code, &count, 0xc02, 4, 0, 4, 0x08000f00);
            ampere_op(code, &count, 0xc02, 8, 0, 5, 0x08000f00);
            ampere_op(code, &count, 0x210, 4, 4, 8, 0x07ffe0ff);
            ampere_op(code, &count, 0xc02, 8, 0, 6, 0x08000f00);
            ampere_op(code, &count, 0x210, 4, 4, 8, 0x07ffe0ff);
            /* UMOV under a uniform guard: @UP0 replaces UR9 = 0x4000 with 0x8000, @!UP0 UR10 = 0 with 0x10000 */
            ampere_op(code, &count, 0x882, 9, 0, 0x4000, 0);
            ampere_op(code, &count, 0x882, 9, 0, 0x8000, 0);
            snx_st_le32(code + (count - 1) * 16, 0x00090882); /* @UP0 */
            ampere_op(code, &count, 0x882, 10, 0, 0, 0);
            ampere_op(code, &count, 0x882, 10, 0, 0x10000, 0);
            snx_st_le32(code + (count - 1) * 16, 0x000a8882); /* @!UP0 */
            ampere_op(code, &count, 0xc02, 9, 0, 9, 0x08000f00);
            ampere_op(code, &count, 0x210, 4, 4, 9, 0x07ffe0ff);
            ampere_op(code, &count, 0xc02, 9, 0, 10, 0x08000f00);
            ampere_op(code, &count, 0x210, 4, 4, 9, 0x07ffe0ff);
            ampere_op(code, &count, 0x81c, 0, 0, 0, 0x0074eb0a); /* P2 = P0 XOR UP0 */
            ampere_op(code, &count, 0x802, 9, 0, 0x100, 0xf00);
            ampere_op(code, &count, 0x207, 9, 9, 255, 0x01000000);
            ampere_op(code, &count, 0x210, 4, 4, 9, 0x07ffe0ff);
            ampere_op(code, &count, 0x802, 9, 0, 0x1000, 0xf00);
            ampere_op(code, &count, 0x207, 9, 9, 255, 0);
            ampere_op(code, &count, 0x210, 4, 4, 9, 0x07ffe0ff);
            ampere_op(code, &count, 0xc0c, 0, 1, 2, 0x0bfa1070); /* P5 = TID.X < workgroup */
            ampere_op(code, &count, 0x802, 9, 0, 0x200, 0xf00);
            ampere_op(code, &count, 0x207, 9, 9, 255, 0x02800000);
            ampere_op(code, &count, 0x210, 4, 4, 9, 0x07ffe0ff);
            ampere_op(code, &count, 0x207, 4, 4, 255, 0x00800000); /* ordinary P1 is still true */
            ampere_op(code, &count, 0x207, 4, 255, 4, 0x03000000); /* ordinary P6 is still false */
            ampere_op(code, &count, 0x210, 4, 4, 0, 0x07ffe0ff);
        } else if (transform == 7 || transform == 25) {
            /* The call sits in two open reconvergence scopes, as a game's fragment programs call their leaf: the
               return must come back inside them, and both BSYNCs must still find their tokens */
            ampere_op(code, &count, 0x202, 10, 0, 0, 0xf00); /* keep the invocation ID before R0 becomes a link */
            u32 outer = count;
            ampere_op(code, &count, 0x945, 2, 0, 0, 0x03800000);
            u32 inner = count;
            ampere_op(code, &count, 0x945, 1, 0, 0, 0x03800000);
            link_setup = count;
            ampere_op(code, &count, 0x802, 0, 0, 0, 0xf00);
            call = count;
            ampere_op(code, &count, 0x944, 0, 0, 0, 0x03c00000);
            snx_st_le32(code + inner * 16 + 4, (count - inner - 1) * 16);
            ampere_op(code, &count, 0x941, 1, 0, 0, 0x03800000);
            snx_st_le32(code + outer * 16 + 4, (count - outer - 1) * 16);
            ampere_op(code, &count, 0x941, 2, 0, 0, 0x03800000);
            ampere_op(code, &count, 0x210, 4, 4, 10, 0x07ffe0ff);
        } else if (transform == 6 || (transform >= 35 && transform <= 37)) {
            ampere_op(code, &count, 0x812, 12, 1, 7, 0x078ec0ff);
            ampere_op(code, &count, 0x819, 13, 255, 3, 0x00011601);
            ampere_op(code, &count, 0x919, 14, 0, 0, 37u << 8);
            for (u32 k = 0; k < 4; ++k)
                ampere_op(code, &count, 0x810, 4 + k, 0, 0x10203040u * (k + 1), 0x07ffe0ff);
            ampere_op(code, &count, 0xb9b, 0, 12, 0x80000204, 0x0010af00);
            if (transform == 37) {
                /* A real companion image is still written. The second descriptor is unbound: its stores must not
                   persist into its loads, and must not suppress this dispatch's real global and image outputs. */
                ampere_op(code, &count, 0xb9b, 0, 12, 0x80000c04, 0x0010af00);
                ampere_op(code, &count, 0xb97, 8, 12, 0x80000c00, 0x001e0f00);
                for (u32 k = 0; k < 4; ++k) {
                    ampere_op(code, &count, 0x810, 4 + k, 8 + k, 0x12340000 + k, 0x07ffe0ff);
                    ampere_op(code, &count, 0x210, 4 + k, 4 + k, 0, 0x07ffe0ff);
                }
            } else if (transform >= 35) {
                ampere_op(code, &count, 0x810, 16, 14, 0xabcd0000, 0x07ffe0ff);
                ampere_op(code, &count, 0x202, 17, 0, 12, 0xf00);
                ampere_op(code, &count, 0xb66, 16, 16, 0xb000020d,
                          transform == 35 ? 0x009e0f0c : 0x009e070c);
                ampere_op(code, &count, 0x202, 4, 0, 16, 0xf00);
                ampere_op(code, &count, 0x202, 5, 0, 17, 0xf00);
                ampere_op(code, &count, 0x202, 6, 0, 12, 0xf00);
                if (transform == 35) ampere_op(code, &count, 0x202, 7, 0, 13, 0xf00);
                else ampere_op(code, &count, 0x802, 7, 0, 0xdeadbeef, 0xf00);
            } else {
                ampere_op(code, &count, 0xb97, 8, 12, 0x80000200, 0x001e0f00);
                ampere_op(code, &count, 0x245, 17, 0, 12, 0x00201000);
                ampere_op(code, &count, 0x245, 18, 0, 13, 0x00201000);
                ampere_op(code, &count, 0x821, 17, 17, 0x3f000000, 0x00010000);
                ampere_op(code, &count, 0x821, 18, 18, 0x3f000000, 0x00010000);
                ampere_op(code, &count, 0x820, 17, 17, 0x3e000000, 0x00410000);
                ampere_op(code, &count, 0x820, 18, 18, 0x3e800000, 0x00410000);
                ampere_op(code, &count, 0x202, 16, 0, 14, 0xf00);
                ampere_op(code, &count, 0x802, 19, 0, 0, 0xf00);
                ampere_op(code, &count, 0xb60, 4, 16, 0xb0000212, 0x019e0f06);
                ampere_op(code, &count, 0x91a, 0, 0, 0xd0c0, 0);
                ampere_op(code, &count, 0x212, 4, 4, 9, 0x078e9606);
                ampere_op(code, &count, 0x210, 4, 4, 11, 0x07ffe0ff);
            }
        } else if (transform == 5) {
            ampere_op(code, &count, 0x812, 9, 1, 15, 0x078ec0ff);
            ampere_op(code, &count, 0x824, 9, 9, 8, 0x078e02ff);
            ampere_op(code, &count, 0xb82, 4, 9, 16u << 6, 5u << 9);
            ampere_op(code, &count, 0x212, 4, 4, 5, 0x078e3cff);
            ampere_op(code, &count, 0x210, 4, 4, 0, 0x07ffe0ff);
        } else if (transform == 4) {
            for (u32 bit = 0; bit < 3; ++bit)
                ampere_op(code, &count, 0x812, 255, 1, 1u << bit, 0x0780c0ff | bit << 17);
            ampere_op(code, &count, 0x81c, 0xd2, 0, 0, 0x00103226); /* P0 = XOR, P1 = second LUT */
            u32 scope = count;
            ampere_op(code, &count, 0x945, 0, 0, 0, 0x03800000);
            u32 choose = count;
            ampere_op(code, &count, 0x947, 0, 0, 0, 0x03800000);
            snx_st_le32(code + choose * 16, 0x8947); /* @!P0 BRA else */
            ampere_op(code, &count, 0x802, 4, 0, 0x11110000, 0xf00);
            u32 skip = count;
            ampere_op(code, &count, 0x947, 0, 0, 0, 0x03800000);
            u32 other = count;
            ampere_op(code, &count, 0x802, 4, 0, 0x22220000, 0xf00);
            u32 sync = count;
            ampere_op(code, &count, 0x941, 0, 0, 0, 0x03800000);
            snx_st_le32(code + scope * 16 + 4, (count - scope - 1) * 16);
            snx_st_le32(code + choose * 16 + 4, (other - choose - 1) * 16);
            snx_st_le32(code + skip * 16 + 4, (sync - skip - 1) * 16);
            ampere_op(code, &count, 0x802, 9, 0, 0x100, 0xf00);
            ampere_op(code, &count, 0x207, 9, 9, 255, 0x00800000);
            ampere_op(code, &count, 0x210, 4, 4, 9, 0x07ffe0ff);
            ampere_op(code, &count, 0x210, 4, 4, 0, 0x07ffe0ff);
            ampere_op(code, &count, 0x802, 8, 0, 0, 0xf00);
            u32 loop_scope = count;
            ampere_op(code, &count, 0x945, 1, 0, 0, 0x03800000);
            u32 loop = count;
            ampere_op(code, &count, 0x810, 8, 8, 1, 0x07ffe0ff);
            ampere_op(code, &count, 0x210, 4, 4, 8, 0x07ffe0ff);
            ampere_op(code, &count, 0x80c, 0, 8, 3, 0x03f21070); /* P1 = R8 < 3 */
            u32 stop = count;
            ampere_op(code, &count, 0x944, 0, 0, 16, 0x03c00000);
            snx_st_le32(code + stop * 16, 0x9944); /* @!P1 CALL next-next, without a return frame */
            u32 back = count;
            ampere_op(code, &count, 0x947, 0, 0, (loop - back - 1) * 16, 0x0383ffff);
            ampere_op(code, &count, 0x941, 1, 0, 0, 0x03800000);
            snx_st_le32(code + loop_scope * 16 + 4, (count - loop_scope - 1) * 16);
        } else if (transform == 3) {
            ampere_op(code, &count, 0x981, 4, 2, 0xfffff800, 0x081ea100 | width << 9);
            ampere_op(code, &count, 0x20a, 9, 4, 4, 0x03817000);
            ampere_op(code, &count, 0x305, 4, 0, 4, 0x0021f100);
            ampere_op(code, &count, 0xb05, 8, 0, 0, 0x0021f100); /* c[0][0] = -1.75 */
            ampere_op(code, &count, 0x210, 4, 4, 8, 0x07ffe0ff);
            ampere_op(code, &count, 0x210, 4, 4, 9, 0x07ffe0ff);
        } else if (transform == 1) {
            /* In-place loads and stores alias the same mapping, across its 32-bit carry boundary. */
            ampere_op(code, &count, 0x981, 4, 2, 0xfffff800, 0x081ea100 | width << 9);
            ampere_op(code, &count, 0x91a, 0, 0, 0xd040, 0);
            ampere_op(code, &count, 0x424, 8, 255, 2, 0x078e00ff);
            for (u32 i = 0; i < components; ++i) {
                ampere_op(code, &count, 0x817, 4 + i, 4 + i, 19, 0x03800000); /* unsigned minimum */
                ampere_op(code, &count, 0x817, 4 + i, 4 + i, 5, 0x07800000);  /* unsigned maximum */
                ampere_op(code, &count, 0x224, 4 + i, 4 + i, 8, 0x078e00ff);
                ampere_op(code, &count, 0x224, 4 + i, 255, 255, 0x078e0a00 | (4 + i));
                ampere_op(code, &count, i & 1 ? 0x811 : 0xa11, 4 + i, 4 + i, i & 1 ? 3 : 4u << 6, 0x078e10ff);
                ampere_op(code, &count, 0x245, 4 + i, 0, 4 + i, 0x00201400);
                ampere_op(code, &count, 0x820, 4 + i, 4 + i, 0x3f000000, 0x00410000);
            }
        } else {
            if (transform == 2) {
                ampere_op(code, &count, 0x80c, 0, 1, 3, 0x03f04070); /* P0 = TID.X > 3 */
                ampere_op(code, &count, 0x20b, 0, 255, 255, 0x03fc1000);     /* P6 = false */
                ampere_op(code, &count, 0x3c2, 2, 0, 0, 0x000e0000);
                snx_st_le32(code + (count - 1) * 16, 0x000203c2); /* @P0 R2UR UR2, R0 */
                ampere_op(code, &count, 0x899, 2, 2, 1, 0x0800063f);
                ampere_op(code, &count, 0x882, 3, 0, 2, 0);
                ampere_op(code, &count, 0x2a4, 2, 2, 3, 0x0f8e023f);
                ampere_op(code, &count, 0x890, 2, 2, 1, 0x0fffe03f);
                ampere_op(code, &count, 0x891, 2, 2, 3, 0x0f8e083f);
                ampere_op(code, &count, 0xc02, 4, 0, 2, 0x08000f00);
                ampere_op(code, &count, 0x207, 4, 255, 4, 0x03000000); /* P6 must remain false */
                ampere_op(code, &count, 0x812, 4, 4, 0x13579bdf, 0x078e9601); /* R4 ^ immediate ^ TID.X */
                ampere_op(code, &count, 0x882, 3, 0, 0x3f000000, 0);
                ampere_op(code, &count, 0xd08, 9, 0, 3, 0x08001000);
                ampere_op(code, &count, 0xc20, 9, 9, 3, 0x08410000);
                ampere_op(code, &count, 0x305, 9, 0, 9, 0x0020f000);
                ampere_op(code, &count, 0x210, 4, 4, 9, 0x07ffe0ff);
                ampere_op(code, &count, 0x882, 4, 0, 1, 0);
                ampere_op(code, &count, 0x2be, 4, 0, 4, 0x08000000);
                ampere_op(code, &count, 0x2bd, 4, 0, 4, 0x080e0400);
                ampere_op(code, &count, 0x886, 5, 0, 0, 0x038e0000);
                ampere_op(code, &count, 0x2bd, 5, 0, 5, 0x080e0000);
                ampere_op(code, &count, 0x290, 5, 5, 4, 0x0fffe03f);
                ampere_op(code, &count, 0xc02, 9, 0, 5, 0x08000f00);
                ampere_op(code, &count, 0x210, 4, 4, 9, 0x07ffe0ff);
                ampere_op(code, &count, 0x919, 9, 0, 0, 0);
                ampere_op(code, &count, 0x210, 4, 4, 9, 0x07ffe0ff);
            }
            ampere_op(code, &count, 0x424, 8, 255, 0x89abcdef, 0x078e00ff);
            ampere_op(code, &count, 0x210, 4, transform == 2 ? 4 : 0, 8, 0x07ffe0ff);
            for (u32 i = 1; i < components; ++i) ampere_op(code, &count, 0x810, 4 + i, 4, i, 0x07ffe0ff);
        }
        if (transform == 9) {
            ampere_op(code, &count, 0x386, 0, 12, 4, 0x0010ab00);
            ampere_op(code, &count, 0x386, 0, 12, 8u << 8 | 255, 0x0010ab00);
        } else
            ampere_op(code, &count, 0x986, 0, 2, 0xfffff804, 0x0810a100 | width << 9);
        ampere_op(code, &count, 0x94d, 0, 0, 0, 0x03800000);
        if (transform == 7 || transform == 25) {
            ++count; /* an unreachable zero-filled alignment gap before the leaf */
            u32 leaf = count;
            ampere_op(code, &count, 0x810, 4, 0, 0x11223344, 0x07ffe0ff);
            ampere_op(code, &count, 0x80c, 0, 1, 16, 0x03f01070);
            ampere_op(code, &count, 0x806, 12, 0, 0, 0x00040000); /* All(P0) is false, ballot is 0xffff */
            ampere_op(code, &count, 0x806, 13, 0, 0, 0x00060100); /* Any(P0) is true */
            ampere_op(code, &count, 0x806, 255, 0, 0, 0x000c0200); /* Eq(P0) is false */
            u32 leaf_scope = count;
            ampere_op(code, &count, 0x945, 0, 0, 0, 0x03800000);
            u32 choose = count;
            ampere_op(code, &count, 0x947, 0, 0, 0, 0x03800000);
            snx_st_le32(code + choose * 16, 0x8947);
            ampere_op(code, &count, 0x810, 4, 4, 5, 0x07ffe0ff);
            u32 skip = count;
            ampere_op(code, &count, 0x947, 0, 0, 0, 0x03800000);
            u32 other = count;
            ampere_op(code, &count, 0x810, 4, 4, 9, 0x07ffe0ff);
            u32 joined = count;
            ampere_op(code, &count, 0x941, 0, 0, 0, 0x03800000);
            snx_st_le32(code + leaf_scope * 16 + 4, (count - leaf_scope - 1) * 16);
            ampere_op(code, &count, 0xf89, 4, 4, 0x0e001f00, 0x00080000); /* BFLY 16 crosses the divergent halves */
            ampere_op(code, &count, 0x802, 15, 0, 0, 0xf00);
            ampere_op(code, &count, 0x589, 14, 4, 0x00001f0f, 0x000a0000);
            ampere_op(code, &count, 0x210, 4, 4, 14, 0x07ffe0ff);
            ampere_op(code, &count, 0x212, 4, 4, 12, 0x078ec0ff);
            ampere_op(code, &count, 0x207, 4, 255, 4, 0x01000000);
            ampere_op(code, &count, 0x207, 4, 4, 255, 0x01800000);
            ampere_op(code, &count, 0x207, 4, 4, 255, 0x02000000);
            ampere_op(code, &count, 0x207, 4, 4, 255, 0x02800000);
            ampere_op(code, &count, 0x207, 4, 255, 4, 0x03000000);
            if (transform == 25)
                ampere_op(code, &count, 0x424, 19, 255, 0, 0x078e00ff);
            for (u32 mask = 0; mask < 5; ++mask) {
                ampere_op(code, &count, 0x919, 16, 0, 0, (56 + mask) << 8);
                ampere_op(code, &count, 0x824, 16, 16, 3 + mask * 2, 0x078e02ff);
                ampere_op(code, &count, 0x212, 4, 4, 16, 0x078e3cff);
            }
            if (transform == 25)
                ampere_op(code, &count, 0x224, 18, 255, 255, 0x078e0000);
            else {
                ampere_op(code, &count, 0x202, 18, 0, 0, 0xf00);
                ampere_op(code, &count, 0x802, 19, 0, 0, 0xf00);
            }
            u32 ret = count;
            ampere_op(code, &count, 0x950, 0, 18, 0u - (ret + 1) * 16, 0x03c3ffff);
            snx_st_le32(code + link_setup * 16 + 4, (call + 1) * 16);
            snx_st_le32(code + call * 16 + 4, (leaf - call - 1) * 16);
            snx_st_le32(code + choose * 16 + 4, (other - choose - 1) * 16);
            snx_st_le32(code + skip * 16 + 4, (joined - skip - 1) * 16);
        }
        CHECK(!gmem_write(program, code, sizeof code));
        u8 params[192] = {0};
        snx_st_le32(params, 0xbfe00000);
        snx_st_le32(params + 4, 3);
        snx_st_le32(params + 8, 0x40400000); /* F32 3.0, distinct from the integer 3 above */
        for (u32 i = 0; i < 16; ++i) {
            snx_st_le32(params + 16 + i * 8, 0x12340000u + i * 17);
            snx_st_le32(params + 20 + i * 8, 0x00abcd00u + i * 257);
        }
        snx_st_le64(params + 0xb0, base);
        snx_st_le32(params + 0xb8, 1); /* not a byte count */
        if (transform == 37) snx_st_le32(params + 8, 1); /* valid image in slot one, unbound image in slot zero */
        if (transform == 33 || transform == 37) snx_st_le32(params + 48, 0);
        if (transform == 38) snx_st_le64(params + 16, base + 0x580);
        if (transform == 39) {
            static const u32 values[] = {0xfffffff3, 0, 0x80000000, 0x7fffffff};
            for (u32 k = 0; k < 4; ++k) snx_st_le32(params + 0x80 + k * 4, values[k]);
        }
        CHECK(!gmem_write(parameters, params, sizeof params));
        memset(GP(cpu), 0xcd, 0x2000);
        if (transform)
            for (u32 i = 0; i < 64; ++i)
                for (u32 k = 0; k < components; ++k)
                    snx_st_le32((u8*)GP(cpu) + 0xf80 + (i * components + k) * 4,
                                transform == 3 ? floats[i % 16] : i + k);
        if (transform == 38) {
            CHECK(!storage_write_words(base + 0x600, (const u32[1]){0}, 1));
            CHECK(!storage_write_words(base + 0x704, (const u32[1]){0}, 1));
        }
        u8 zero[4] = {0};
        CHECK(!gmem_write(semaphore, zero, sizeof zero));
        GpuChannel* ch = calloc(1, sizeof *ch);
        CHECK(ch && !g_gpu.chan[90]);
        const u64 image = program + 0x6000, descriptors = program + 0x4000;
        if (transform == 6 || (transform >= 35 && transform <= 37)) {
            /* The formatted load uses a sampled view, whose channel map must be identity, not constant zero. */
            u32 tic[8] = {1u | 4u << 7 | 4u << 10 | 4u << 13 | 4u << 16 |
                              2u << 19 | 3u << 22 | 4u << 25 | 5u << 28,
                          (u32)image, (u32)(image >> 32) | 3u << 21, 0, 7u | 5u << 23, 3u | 1u << 16, 0, 0};
            CHECK(!storage_write_words(descriptors, tic, 8));
            if (transform == 37) {
                CHECK(!storage_write_words(descriptors + 32, tic, 8));
                CHECK(!storage_write_words(descriptors, (const u32[8]){0}, 8));
            }
            CHECK(!storage_write_words(descriptors + 0x100,
                                       (const u32[8]){2u | 2u << 3 | 2u << 6, 1u | 1u << 4 | 1u << 6}, 8));
            u8 initial[2056];
            memset(initial, 0xcd, sizeof initial);
            CHECK(!gmem_write(image - 4, initial, sizeof initial));
            ch->rcomp[KEPLER_COMPUTE_TIC] = (u32)(descriptors >> 32);
            ch->rcomp[KEPLER_COMPUTE_TIC + 1] = (u32)descriptors;
            ch->rcomp[KEPLER_COMPUTE_TSC] = (u32)((descriptors + 0x100) >> 32);
            ch->rcomp[KEPLER_COMPUTE_TSC + 1] = (u32)(descriptors + 0x100);
        }
        if (transform == 33) {
            u32 tic[8] = {0x0fu | 4u << 7 | 4u << 10 | 4u << 13 | 4u << 16,
                         (u32)image, (u32)(image >> 32) | 2u << 21, 1, 3u | 1u << 23, 1, 0, 0};
            CHECK(!storage_write_words(descriptors, tic, 8));
            u8 initial[72];
            memset(initial, 0xcd, sizeof initial);
            for (u32 y = 0; y < 2; ++y)
                for (u32 x = 0; x < 4; ++x)
                    snx_st_le32(initial + 4 + y * 32 + x * 4, 0x100 * (y + 1) + x);
            CHECK(!gmem_write(image - 4, initial, sizeof initial));
            ch->rcomp[KEPLER_COMPUTE_TIC] = (u32)(descriptors >> 32);
            ch->rcomp[KEPLER_COMPUTE_TIC + 1] = (u32)descriptors;
        }
        ch->id = 90;
        g_gpu.chan[90] = ch;
        u32 q[64] = {0};
        q[12] = transform == 32 ? 1 : 2;
        q[13] = 1;
        q[14] = 1;
        q[18] = 0x30 | (transform == 32 ? 64u : 32u) << 16;
        q[19] = 1 | 1u << 16;
        q[20] = 1;
        if (transform == 11 || (transform >= 20 && transform <= 24) || transform == 32)
            q[17] = 0x400; /* QMD_SHARED_ALLOC */
        q[32] = (u32)parameters;
        q[33] = (u32)(parameters >> 32) | 12u << 19; /* 192 bytes */
        q[48] = (u32)program;
        q[49] = (u32)(program >> 32);
        q[24] = (u32)semaphore;
        q[25] = (u32)(semaphore >> 32) | 0x44800000; /* one-word ADD release */
        q[26] = 1;
        u64 dispatched = g_vk_dispatches;
        /* Each width installs different code at the same address, so invalidate the instruction cache as the guest does. */
        compute_method(ch, KEPLER_COMPUTE_INVALIDATE_SHADER_CACHES, KEPLER_COMPUTE_INVALIDATE_INSTRUCTION, 1);
        compute_method(ch, KEPLER_COMPUTE_INLINE_QMD_ADDRESS, 0, 1);
        compute_method(ch, KEPLER_COMPUTE_INLINE_QMD_ADDRESS + 1, (u32)((program + 0x1000) >> 8), 1);
        for (u32 i = 0; i < 64; ++i) compute_method(ch, KEPLER_COMPUTE_INLINE_QMD_DATA + i, q[i], 1);
        CHECK(g_vk_dispatches == dispatched + 1);
        vk_release_need(semaphore, 4);
        u8 released[4];
        CHECK(!gmem_read(semaphore, released, sizeof released) && snx_ld_le32(released) == 1);
        if (transform == 6 || (transform >= 35 && transform <= 37)) {
            u8 expected[2056], result[2056];
            memset(expected, 0xcd, sizeof expected);
            for (u32 i = 0; i < 64; ++i) {
                u32 lane = i & 31;
                u64 offset = (i / 32) * 1024 + bl_offset((lane & 7) * 16, lane / 8, 0, 128, 4, 0, 0);
                for (u32 k = 0; k < 4; ++k) snx_st_le32(expected + 4 + offset + k * 4, 0x10203040u * (k + 1) + i);
            }
            CHECK(!gmem_read(image - 4, result, sizeof result));
            CHECK(!memcmp(expected, result, sizeof result));
        }
        if (transform == 33) {
            u8 expected[72], result[72];
            memset(expected, 0xcd, sizeof expected);
            for (u32 y = 0; y < 2; ++y)
                for (u32 x = 0; x < 4; ++x)
                    snx_st_le32(expected + 4 + y * 32 + x * 4, 0x100 * (y + 1) + x);
            for (u32 i = 0; i < 64; ++i) {
                u8* texel = expected + 4 + (i / 32) * 32 + (i & 3) * 4;
                snx_st_le32(texel, snx_ld_le32(texel) + i + 1);
            }
            CHECK(!gmem_read(image - 4, result, sizeof result));
            CHECK(!memcmp(expected, result, sizeof result));
        }
        u8 data[64 * 16 + 8];
        u32 size = 64 * components * 4 + 8;
        CHECK(!gmem_read(base - 4, data, size));
        CHECK(snx_ld_le32(data) == 0xcdcdcdcd && snx_ld_le32(data + size - 4) == 0xcdcdcdcd);
        u32 atomic_seen[2] = {0};
        for (u32 i = 0; i < 64; ++i)
            for (u32 k = 0; k < components; ++k) {
                u32 got = snx_ld_le32(data + 4 + (i * components + k) * 4), want = 0x89abcdefu + i + k;
                if (transform == 33) want = i + 1;
                if (transform == 35 || transform == 36)
                    want = transform == 36 && k == 3 ? 0xdeadbeef : 0x10203040u * (k + 1) + i;
                if (transform == 37 || transform == 38) want = 0x12340000 + i + k;
                if (transform == 39) {
                    static const u32 magnitudes[] = {13, 0, 0x80000000, 0x7fffffff};
                    want = magnitudes[k];
                }
                if (transform == 40) {
                    static const u32 packed[] = {0xc200be55, 0xc600be00, 0x3800c400, 0xc000bd00};
                    want = packed[k];
                }
                if (transform == 41) {
                    static const u32 halves[] = {0x4400, 0x4100, 0x4000, 0x3c00};
                    want = halves[k];
                }
                if (transform == 42) want = (k == 1 ? 0x56780000u : 0x12340000u) + i;
                if (transform == 34) {
                    static const u32 shifts[] = {0, 1, 30, 31};
                    u32 value = 0x87654321u + i * 0x03456789u, shift = shifts[i & 3];
                    u32 hi = shift ? value >> (32 - shift) : 0;
                    if (k && (value & 0x80000000u)) hi |= 0xffffffffu << shift;
                    want = hi + (k == 0 ? 0x12340000u + i : k == 1 ? 0xfedc0000u + i
                                                     : k == 2 ? 0xdead0000u + i : 0);
                }
                if (transform == 32) want = 0x12340000u + (i ^ 16) + (i < 32 ? 0x1000 : 0);
                if (transform == 31) {
                    u32 value = 0x87654321u + i * 0x01234567u, bits = k < 2 ? i & 31 : 32;
                    want = bits == 32 ? value : bits ? value & ((1u << bits) - 1) : 0;
                    if (k == 1 && bits && (want >> (bits - 1))) want |= ~((1u << bits) - 1);
                }
                if (transform == 30) {
                    u32 pair = (i & 15) % 9;
                    static const float a[2] = {-2, 1}, b[2] = {0.5f, -4};
                    want = 0;
                    for (u32 half = 0; half < 2; ++half) {
                        u32 left = pair / 3 ? pair / 3 - 1 : half, right = pair % 3 ? pair % 3 - 1 : half;
                        want |= (u32)float_to_half(a[left] * b[right]) << (half * 16);
                    }
                }
                if (transform == 29)
                    want = k < 2 ? 0 : k == 2 ? 0x87654321u : 0x87654321u >> 1;
                if (transform == 16) want = 0x80000100u + 7u + 0x80000001u + 7u;
                if (transform == 28)
                    want = !(k & 1) ? 0x87654321u << (i & 31) : i < 32 ? 0x87654321u << i : 0;
                if (transform == 27) {
                    static const u32 products[4] = {0xc400bc00, 0xb4004200, 0x3e00bf00, 0x3c004400};
                    want = products[k];
                }
                if (transform == 26) {
                    u32 a = i + 17, c = 0x89abcdef;
                    want = (k == 1 ? 0u - a : a) + (k == 2 ? 0u - 3 : 3) + (k == 3 ? 0u - c : c);
                }
                if (transform == 17 || transform == 18) {
                    u32 value = i * 0x01234567u;
                    if (transform == 18) value = ~value;
                    if (k >= 2 && (value & 0x80000000u)) value = ~value;
                    want = ~0u;
                    for (u32 bit = 0; bit < 32; ++bit)
                        if (value >> bit) want = k & 1 ? 31 - bit : bit;
                }
                if (transform == 19) {
                    static const u32 shifts[4] = {0, 1, 29, 31};
                    want = (u32)(((u64)0xe0000080u << shifts[k]) >> 32) - 1;
                }
                if (transform >= 20 && transform <= 22)
                    want = !(i & 1) && k < (1u << (transform - 20)) ? 0 : 0x11223344u + k;
                if (transform == 23) {
                    want = k >= 2 && (i & 1) ? 0xabc0 + (k & 1) : 0;
                    if (k < 2 || !(i & 1))
                        for (u32 lane = 0; lane < 32; ++lane) {
                            if (k >= 2 && (lane & 1)) continue;
                            u32 value = ((i & ~31u) + lane) * 0x9e3779b9u;
                            want = k & 1 ? (value > want ? value : want) : want + value;
                        }
                }
                if (transform == 24) {
                    if (!k) {
                        CHECK(got < 32 && !(atomic_seen[i / 32] & (1u << got)));
                        atomic_seen[i / 32] |= 1u << got;
                        continue;
                    }
                    want = k == 3 ? 0xdeadbeef : 32;
                }
                if (transform == 15) {
                    u32 form = i & 15, source = 0xc8b4a03cu + i * 0x01234567u;
                    u32 size = form < 8 ? 0 : form < 12 ? 1 : 2;
                    u32 part = size == 0 ? form & 3 : size == 1 ? form & 1 : 0;
                    u32 sign = form < 14 ? (form >> (2 - size)) & 1 : form & 1;
                    u32 bits = 8u << size, value = source >> (part * bits);
                    if (bits < 32) value &= (1u << bits) - 1;
                    i64 signed_value = sign && (value >> (bits - 1)) ? (i64)value - (1ll << bits) : value;
                    float converted = (float)signed_value;
                    memcpy(&want, &converted, sizeof want);
                }
                if (transform >= 12 && transform <= 14)
                    want = i + ((i & 31) < 16 ? 0x1040 : 0x2000) + (transform == 14 ? 9 : 0);
                if (transform == 11) {
                    u64 wide = (u64)0x89abcdefu * 0x12345u + 0x1fffffff0ull;
                    u32 lane = i & 31;
                    want = (u32)wide + (u32)(wide >> 32) + (u32)snx_popcount64(i * 0x9e3779b9u) +
                           (u32)snx_popcount64(0x89abcdefu) + 16 + 15 + (i ^ 0x89abcdefu) +
                           ((lane & 1) && (lane & 2) ? 0 : 0x1000u) + (lane & 2 ? 0 : 0xe000u);
                    want = i + 2u * want;
                    want = i * 0x12345u - (want * 0x89abcdefu + i);
                    if (i & 1) want = want * 0x89abcdefu - want;
                    want = want + 0x89abcdefu + i;
                    want = i - 0x12345u + want;
                    want = want + 1u - i;
                    if (i & 1) want += 0xfffffff0u;
                    want = (i >> 3) + want;
                    want = (want >> 31) + i;
                    if (i & 1) want = (want >> 28) + want;
                    want = want * i + 0x89abcdefu - 0x89abcdefu;
                    if (i & 1) want = i * want + 0x12345u;
                    want += 1; /* only the equal pair matches */
                    u32 high = (u32)((u64)(0x89abcdefu + i) * 0xf0f0f0f1u >> 32);
                    if (i & 1) high = (u32)((u64)high * 0xf0f0f0f1u >> 32);
                    want += high;
                    want += (0x12340008u + i < 0x12340011u ? 0x12340008u + i : 0x12340011u) + (i > 7 ? i - 4 : 3u);
                    want += 0x3c00c000u;
                    static const u32 swizzled[4] = {0x40a00000, 0xc0400000, 0x40400000, 0x40800000}; /* 5, -3, 3, 4 */
                    want += 0x7fu + 0xffffff80u + 0x7f01u + 0xffff80ffu + (0x12345u * 16 + 1) +
                            (i & 1 ? i : 0x12340011u) + (i * i + 0x1000) + swizzled[i & 3] +
                            (i < 32 ? 32 - i : i - 32) + 0xfffffffeu;
                    want += 2 * (i ^ 0xabcu) + (i + 0x5000) + 0x77;
                    want += ((i ^ 16) ^ 0xabcu) + 0xc0000000u + 0xffffffbfu + 0xbfe0u;
                    want += ((i & 31) ? i - 1 : i) + ((i & ~31u) + 3) + 0x3f800000u;
                    want += 8u + 0x10000000u + 0x3c0003c0u + 0xffffffffu; /* the register-shift SHF forms */
                    want += 0x40000000u + 0x40000000u + 0xfffffffbu + i; /* the uniform-register min/max forms */
                    want += 0x55u + 0xffff0500u; /* P2R */
                    want += 0x43480000u + 0xc65d3000u + 0x47203c00u; /* I2F of 200, -14156 and 41020 */
                    want += 0x100u + 6u + 0x80000100u + 7u; /* register low-word carries into IMAD.X */
                    want += 0x38003c00u + 0x38003800u + 0x3800c03cu + 0xc0003c00u + 0x200u; /* P2 only */
                }
                if (transform == 10) {
                    u32 shifted = i - 40u, ramp = i * 1100u + i / 2u; /* i * 1100.5, its integer part */
                    u32 rounded = (i & 1) && (ramp & 1) ? ramp + 1 : ramp; /* a half rounds to even */
                    u32 sum = 0xedcbffe0u + i + 0x12340000u;
                    want = (i << 3) + ((shifted >> 2) | (shifted & 0x80000000u ? 0xc0000000u : 0)) +
                           (((0x89abcdefu + i) >> 4) | i << 28) + (rounded < 65535 ? rounded : 65535) +
                           (i & 1 ? 0x100 : 0) + (i & 2 ? 0x100 : 0) + sum + 0x00abcd00u + (sum < 0x12340000u) +
                           i * 0x100u + 0x12340011u + i * 0x00abce01u + 0x100u;
                    /* F2I.S8.TRUNC of i * 1100.5 * 0.01 and of its negation, saturated to [-128, 127] */
                    for (int sign = 1; sign >= -1; sign -= 2) {
                        float scaled = (float)i * 1100.5f * (sign > 0 ? 0.01f : -0.01f);
                        int truncated = scaled > 127.0f ? 127 : scaled < -128.0f ? -128 : (int)scaled;
                        want += (u32)truncated;
                    }
                }
                if (transform == 9) {
                    u32 value = 0xfffffff0u + i * 16 + (i ? 0x100 : 0);
                    want = k == 0 ? value : k == 1 ? ~value : 0;
                }
                if (transform == 8)
                    want = (i < 32 ? 63 : 102) + ((((i & 31) < 16) != (i < 32)) ? 0x100 : 0) +
                           ((i & 31) < 16 ? 0x1000 : 0) + (i == 32 ? 0x200 : 0) + i + (i < 32 ? 0x8000 : 0x14000);
                if (transform == 7 || transform == 25) {
                    u32 lane = i & 31, eq = 1u << lane, lt = eq - 1;
                    u32 masks[5] = {eq, lt, lt | eq, ~(lt | eq), ~lt};
                    want = (2 * (0x11223344u + (call + 1) * 16) + (lane < 16 ? 9 : 5) + 9) & 0xffff;
                    for (u32 mask = 0; mask < 5; ++mask) want ^= masks[mask] * (3 + mask * 2);
                    want += i;
                }
                if (transform == 6)
                    want = ((0x10203040u + i) ^ (0x20406080u + i) ^ (0x306090c0u + i)) + 0x4080c100u + i;
                if (transform == 5) want = ((0x12340000u + (i & 15) * 17) ^ (0x00abcd00u + (i & 15) * 257)) + i;
                if (transform == 4)
                    want = ((0x96u >> (i & 7) & 1) ? 0x11110000u : 0x22220000u) +
                           (0x9au >> (i & 7) & 1) * 0x100u + i + 6;
                if (transform == 3) want = integers[i % 16] - 1u + ((i % 16 == 6 || i % 16 == 7) ? 0 : 0x3f800000u);
                if (transform == 2)
                    want = 0x89abcdefu + ((((i & ~31u) + 4) * 8 + 5) ^ 0x13579bdfu ^ (i & 31)) + 32 + (i & 31) + k;
                if (transform == 1) {
                    u32 clamped = i + k < 19 ? i + k : 19;
                    if (clamped < 5) clamped = 5;
                    float value = (3.0f - (float)(clamped * 8)) * 0.5f;
                    memcpy(&want, &value, sizeof want);
                }
                if (got != want)
                    fprintf(stderr, "Ampere width %u, thread %u, component %u: %08x, expected %08x\n", width, i, k, got,
                            want);
                CHECK(got == want);
            }
        if (transform == 24) CHECK(atomic_seen[0] == ~0u && atomic_seen[1] == ~0u);
        if (transform == 11) { /* 64 threads each added 1 to the counter at 0x300, the 32 even ones 3 at 0x304; RED's
                                  signed maximum at 0x308 and sum at 0x30c; the U8 and U16 stores beside the fill */
            u8 counter[20];
            CHECK(!gmem_read(base + 0x300, counter, sizeof counter) && snx_ld_le32(counter) == 0xcdcdcdcdu + 64 &&
                  snx_ld_le32(counter + 4) == 0xcdcdcdcdu + 96);
            if (snx_ld_le32(counter + 8) != 63 || snx_ld_le32(counter + 12) != 0xcdcdcdcdu + 128 ||
                snx_ld_le32(counter + 16) != 0x1234cd5au)
                fprintf(stderr, "Ampere RED and STG: %08x %08x %08x\n", snx_ld_le32(counter + 8),
                        snx_ld_le32(counter + 12), snx_ld_le32(counter + 16));
            CHECK(snx_ld_le32(counter + 8) == 63 && snx_ld_le32(counter + 12) == 0xcdcdcdcdu + 128 &&
                  snx_ld_le32(counter + 16) == 0x1234cd5au);
        }
        if (transform == 38) {
            u8 result[16];
            CHECK(!gmem_read(base + 0x5fc, result, sizeof result));
            CHECK(snx_ld_le32(result) == 0xcdcdcdcdu && snx_ld_le32(result + 4) == 2016 &&
                  snx_ld_le32(result + 8) == 0xcdcdcdcdu && snx_ld_le32(result + 12) == 0xcdcdcdcdu);
            CHECK(!gmem_read(base + 0x700, result, 12));
            CHECK(snx_ld_le32(result) == 0xcdcdcdcdu && snx_ld_le32(result + 4) == 63 &&
                  snx_ld_le32(result + 8) == 0xcdcdcdcdu);
        }
        if (transform == 15)
            for (u32 i = 0; i < 64; ++i) {
                u8 stored[16];
                CHECK(!gmem_read(base + 0x400 + i * 16, stored, sizeof stored));
                for (u32 address_form = 0; address_form < 2; ++address_form) {
                    CHECK(snx_ld_le32(stored + address_form * 8) == 0xfedca55au);
                    CHECK(snx_ld_le32(stored + address_form * 8 + 4) == 0x8076cdcdu);
                }
            }
        gpu_channel_close(90);
    }
    puts("Ampere compute: vector loads/stores, in-place arithmetic, negative displacement, address carry and QMD releases");
    return 0;
}

/* Distinct NVN descriptors cross the old 16-slot limit and exercise write-mask bit 31.
   Neighbor words must survive every write. A 33rd descriptor must reject the whole program. */
static int check_storage_capacity(void) {
    const u64 program = 0x7100000, parameters = program + 0x2000, target = program + 0x4000;
    u8 code[2048] = {0}, lowered[8192], data[32 * 32 + 8];
    u32 count = 0;
    for (u32 i = 0; i < 33; ++i) {
        ampere_op(code, &count, 0xab9, 0, 0, (0x110 + i * 16) << 6, 5u << 9);
        ampere_op(code, &count, 0x802, 4, 0, 0x12340000 + i * 101, 0xf00);
        ampere_op(code, &count, 0x986, 0, 255, 4, 0x0810a900);
    }
    ampere_op(code, &count, 0x94d, 0, 0, 0, 0x03800000);
    ShaderInfo si = {.cs_local = {1, 1, 1}};
    u32 bytes = shader_ampere_lower(code, count * 16, lowered, sizeof lowered, &si, NULL);
    size_t words = 0;
    CHECK(bytes && !si.unsupported);
    u32* spv = shader_to_spirv(lowered, bytes, ST_CS, &si, &words, 0, 1, 0, 0);
    CHECK(!spv && si.unsupported && strstr(si.first_unsupported, "global buffer count"));
    count = 32 * 3;
    ampere_op(code, &count, 0x94d, 0, 0, 0, 0x03800000);
    si = (ShaderInfo){.cs_local = {1, 1, 1}};
    bytes = shader_ampere_lower(code, count * 16, lowered, sizeof lowered, &si, NULL);
    CHECK(bytes && !si.unsupported);
    VkCbReads reads = {0};
    spv = vk_translate_spirv(lowered, bytes, ST_CS, &si, &words, NULL, &reads);
    if (g_vk.storage_buffer_slots < 32) {
        CHECK(!spv && si.unsupported && strstr(si.first_unsupported, "descriptor limit"));
        puts("Storage capacity: 32-slot program refused by the device limit");
        return 0;
    }
    CHECK(spv && !si.unsupported);
    free(spv);
    gmmu_map(program, HEAP_BASE + 0x600000, 0x10000);
    CHECK(!gmem_write(program, code, count * 16));
    for (u32 i = 0; i < 32; ++i) {
        u64 address = target + 4 + i * 32;
        CHECK(!storage_write_words(parameters + 0x110 + i * 16,
                                   (const u32[3]){(u32)address, (u32)(address >> 32), 16}, 3));
    }
    memset(data, 0xcd, sizeof data);
    CHECK(!gmem_write(target, data, sizeof data));
    u32 q[64] = {0};
    q[12] = q[13] = q[14] = 1;
    q[18] = 0x30 | 1u << 16;
    q[19] = 1 | 1u << 16;
    q[20] = 1;
    q[32] = (u32)parameters;
    q[33] = (u32)(parameters >> 32) | 64u << 19;
    q[48] = (u32)program;
    q[49] = (u32)(program >> 32);
    GpuChannel* ch = calloc(1, sizeof *ch);
    CHECK(ch && !g_gpu.chan[90]);
    ch->id = 90;
    g_gpu.chan[90] = ch;
    u64 dispatched = g_vk_dispatches;
    compute_method(ch, KEPLER_COMPUTE_INVALIDATE_SHADER_CACHES, KEPLER_COMPUTE_INVALIDATE_INSTRUCTION, 1);
    compute_method(ch, KEPLER_COMPUTE_INLINE_QMD_ADDRESS, 0, 1);
    compute_method(ch, KEPLER_COMPUTE_INLINE_QMD_ADDRESS + 1, (u32)((program + 0x1000) >> 8), 1);
    for (u32 i = 0; i < 64; ++i) compute_method(ch, KEPLER_COMPUTE_INLINE_QMD_DATA + i, q[i], 1);
    CHECK(g_vk_dispatches == dispatched + 1);
    u8 expected[sizeof data];
    memcpy(expected, data, sizeof expected);
    for (u32 i = 0; i < 32; ++i) snx_st_le32(expected + 4 + i * 32, 0x12340000 + i * 101);
    CHECK(!gmem_read(target, data, sizeof data));
    CHECK(!memcmp(data, expected, sizeof data));
    g_gpu.chan[90] = NULL;
    free(ch);
    puts("Storage capacity: all 32 GPU writes and their neighboring words match, the 33rd binding is refused");
    return 0;
}

/* Native NVN2 descriptors in cbuf 0 give {address, size}, and a dispatch's buffers can lie a few hundred bytes apart.
   A load whose base the tracking loses (an FMUL by 1.0 of the address) is matched at run time against each bound
   buffer's address and size, and the last buffer that holds it gives the value. The higher buffer is bound first.
   Taken as 16 MiB, the lower one, bound second, also held the address, and its binding (its descriptor's 0x40 bytes)
   read past its end. The loaded word is stored back through the higher buffer. Static stores through both
   descriptors, and every other byte, are checked. */
static int check_native_descriptor_sizes(void) {
    const u64 program = 0x7200000, parameters = program + 0x2000, target = program + 0x4000;
    u8 code[512] = {0}, data[0x400];
    u32 count = 0;
    ampere_op(code, &count, 0xab9, 0, 0, 0x20u << 6, 5u << 9);      /* UR0:1 = c[0][0x20], the higher buffer */
    ampere_op(code, &count, 0x802, 4, 0, 0x11111111, 0xf00);
    ampere_op(code, &count, 0x986, 0, 255, 4, 0x0810a900);           /* [UR0] = R4 */
    ampere_op(code, &count, 0xab9, 0, 0, 0x30u << 6, 5u << 9);      /* UR0:1 = c[0][0x30], the lower */
    ampere_op(code, &count, 0x802, 5, 0, 0x22222222, 0xf00);
    ampere_op(code, &count, 0x986, 0, 255, 5, 0x0810a900);           /* [UR0] = R5 */
    ampere_op(code, &count, 0xab9, 0, 0, 0x20u << 6, 5u << 9);
    ampere_op(code, &count, 0xc02, 10, 0, 0, 0x08000f00);            /* R10 = UR0 */
    ampere_op(code, &count, 0xc02, 13, 0, 1, 0x08000f00);            /* R13 = UR1 */
    ampere_op(code, &count, 0x820, 12, 10, 0x3f800000, 0x00410000); /* R12 = R10 * 1.0: the tracking stops */
    ampere_op(code, &count, 0x381, 6, 12, 0x10u << 8, 0x001ea900);  /* R6 = [R12:R13 + 0x10] */
    ampere_op(code, &count, 0x986, 0, 255, 6 | 0x20u << 8, 0x0810a900); /* [UR0 + 0x20] = R6 */
    ampere_op(code, &count, 0x94d, 0, 0, 0, 0x03800000);
    gmmu_map(program, HEAP_BASE + 0x600000, 0x10000);
    CHECK(!gmem_write(program, code, count * 16));
    u8 params[0x40] = {0};
    snx_st_le64(params + 0x20, target + 0x300);
    snx_st_le32(params + 0x28, 0x100);
    snx_st_le64(params + 0x30, target + 0x80); /* 0x280 below: no multiple of a staging alignment */
    snx_st_le32(params + 0x38, 0x40);
    CHECK(!gmem_write(parameters, params, sizeof params));
    memset(data, 0xcd, sizeof data);
    snx_st_le32(data + 0x310, 0x44444444);
    CHECK(!gmem_write(target, data, sizeof data));
    u32 q[64] = {0};
    q[12] = q[13] = q[14] = 1;
    q[18] = 0x30 | 1u << 16;
    q[19] = 1 | 1u << 16;
    q[20] = 1;
    q[32] = (u32)parameters;
    q[33] = (u32)(parameters >> 32) | 64u << 19;
    q[48] = (u32)program;
    q[49] = (u32)(program >> 32);
    GpuChannel* ch = calloc(1, sizeof *ch);
    CHECK(ch && !g_gpu.chan[90]);
    ch->id = 90;
    g_gpu.chan[90] = ch;
    u64 dispatched = g_vk_dispatches;
    compute_method(ch, KEPLER_COMPUTE_INVALIDATE_SHADER_CACHES, KEPLER_COMPUTE_INVALIDATE_INSTRUCTION, 1);
    compute_method(ch, KEPLER_COMPUTE_INLINE_QMD_ADDRESS, 0, 1);
    compute_method(ch, KEPLER_COMPUTE_INLINE_QMD_ADDRESS + 1, (u32)((program + 0x1000) >> 8), 1);
    for (u32 i = 0; i < 64; ++i) compute_method(ch, KEPLER_COMPUTE_INLINE_QMD_DATA + i, q[i], 1);
    CHECK(g_vk_dispatches == dispatched + 1);
    u8 expected[sizeof data];
    memset(expected, 0xcd, sizeof expected);
    snx_st_le32(expected + 0x80, 0x22222222);
    snx_st_le32(expected + 0x300, 0x11111111);
    snx_st_le32(expected + 0x310, 0x44444444);
    snx_st_le32(expected + 0x320, 0x44444444);
    CHECK(!gmem_read(target, data, sizeof data));
    for (u32 i = 0; i < sizeof data; i += 4)
        if (snx_ld_le32(data + i) != snx_ld_le32(expected + i))
            fprintf(stderr, "native descriptor sizes: +%03x %08x, expected %08x\n", i, snx_ld_le32(data + i),
                    snx_ld_le32(expected + i));
    CHECK(!memcmp(data, expected, sizeof data));
    g_gpu.chan[90] = NULL;
    free(ch);
    puts("Native descriptor sizes: a load with a lost base reads its own buffer, not one 0x280 bytes below");
    return 0;
}

/* A native switch: each lane loads its case's native PC from a jump table in constant bank 1 and branches to it
   (BRX) inside a reconvergence scope, and each case stores its own word. The table goes on with entries of another
   switch, native PCs outside the scope, as the compiler packs tables in one bank: the lowering's comparison with
   every instruction is cut to the scope's cases (shader_native_brx_prune), and every lane still reaches its own. */
static int check_native_jump_table(void) {
    const u64 program = 0x7300000, parameters = program + 0x2000, table = program + 0x3000, target = program + 0x4000;
    u8 code[512] = {0};
    u32 count = 0;
    ampere_op(code, &count, 0x919, 0, 0, 0, 33u << 8);              /* 0x00: R0 = TID.X */
    ampere_op(code, &count, 0x824, 6, 0, 4, 0x078e02ff);            /* 0x10: R6 = R0 * 4 */
    ampere_op(code, &count, 0xb82, 6, 6, 1u << 22, 0x800);          /* 0x20: LDC R6 = c[1][R6] */
    ampere_op(code, &count, 0x945, 0, 0, 0xb0 - 0x40, 0x03800000);  /* 0x30: BSSY B0, 0xb0 */
    ampere_op(code, &count, 0x949, 0, 6, 0u - 0x50, 0x0383ffff);    /* 0x40: BRX R6 */
    for (u32 k = 0; k < 3; ++k) {                                   /* 0x50, 0x70, 0x90: the cases */
        ampere_op(code, &count, 0x802, 4, 0, 0x111 * (k + 1), 0xf00);
        ampere_op(code, &count, 0x947, 0, 0, 0xb0 - (count + 1) * 16, 0x03800000); /* BRA 0xb0 */
    }
    ampere_op(code, &count, 0x941, 0, 0, 0, 0x03800000);            /* 0xb0: BSYNC B0 */
    ampere_op(code, &count, 0xab9, 0, 0, 0x20u << 6, 5u << 9);      /* UR0:1 = c[0][0x20] */
    ampere_op(code, &count, 0x824, 2, 0, 4, 0x078e02ff);            /* R2 = R0 * 4 */
    ampere_op(code, &count, 0x986, 0, 2, 4, 0x0810a900);            /* [R2 + UR0] = R4 */
    ampere_op(code, &count, 0x94d, 0, 0, 0, 0x03800000);            /* EXIT */
    gmmu_map(program, HEAP_BASE + 0x600000, 0x10000);
    CHECK(!gmem_write(program, code, count * 16));
    u8 params[0x40] = {0};
    snx_st_le64(params + 0x20, target);
    snx_st_le32(params + 0x28, 0x10);
    CHECK(!gmem_write(parameters, params, sizeof params));
    static const u32 entries[6] = {0x50, 0x70, 0x90, 0x10, 0xc0, 0x20}; /* then another switch's cases */
    u8 bank1[0x40] = {0};
    for (u32 k = 0; k < 6; ++k) snx_st_le32(bank1 + k * 4, entries[k]);
    CHECK(!gmem_write(table, bank1, sizeof bank1));
    u8 data[16];
    memset(data, 0xcd, sizeof data);
    CHECK(!gmem_write(target, data, sizeof data));
    u32 q[64] = {0};
    q[12] = q[13] = q[14] = 1;
    q[18] = 0x30 | 3u << 16;
    q[19] = 1 | 1u << 16;
    q[20] = 3;
    q[32] = (u32)parameters;
    q[33] = (u32)(parameters >> 32) | 64u << 19;
    q[34] = (u32)table;
    q[35] = (u32)(table >> 32) | 4u << 19;
    q[48] = (u32)program;
    q[49] = (u32)(program >> 32);
    GpuChannel* ch = calloc(1, sizeof *ch);
    CHECK(ch && !g_gpu.chan[90]);
    ch->id = 90;
    g_gpu.chan[90] = ch;
    u64 dispatched = g_vk_dispatches;
    compute_method(ch, KEPLER_COMPUTE_INVALIDATE_SHADER_CACHES, KEPLER_COMPUTE_INVALIDATE_INSTRUCTION, 1);
    compute_method(ch, KEPLER_COMPUTE_INLINE_QMD_ADDRESS, 0, 1);
    compute_method(ch, KEPLER_COMPUTE_INLINE_QMD_ADDRESS + 1, (u32)((program + 0x1000) >> 8), 1);
    for (u32 i = 0; i < 64; ++i) compute_method(ch, KEPLER_COMPUTE_INLINE_QMD_DATA + i, q[i], 1);
    CHECK(g_vk_dispatches == dispatched + 1);
    CHECK(!gmem_read(target, data, sizeof data));
    for (u32 k = 0; k < 4; ++k)
        if (snx_ld_le32(data + k * 4) != (k < 3 ? 0x111 * (k + 1) : 0xcdcdcdcdu))
            fprintf(stderr, "native jump table: lane %u stored %08x\n", k, snx_ld_le32(data + k * 4));
    for (u32 k = 0; k < 4; ++k) CHECK(snx_ld_le32(data + k * 4) == (k < 3 ? 0x111 * (k + 1) : 0xcdcdcdcdu));
    /* BRXU reads UR6, not the poisoned ordinary R6. All three lanes take the second case. */
    u32 setup = 1; /* Keep the lane ID that addresses each output word. */
    ampere_op(code, &setup, 0x882, 6, 0, 0x70, 0);
    ampere_op(code, &setup, 0x802, 6, 0, 0x50, 0xf00);
    setup = 4;
    ampere_op(code, &setup, 0x958, 0, 6, 0u - 0x50, 0x0b83ffff);
    CHECK(!gmem_write(program, code, count * 16));
    memset(data, 0xcd, sizeof data);
    CHECK(!gmem_write(target, data, sizeof data));
    dispatched = g_vk_dispatches;
    compute_method(ch, KEPLER_COMPUTE_INVALIDATE_SHADER_CACHES, KEPLER_COMPUTE_INVALIDATE_INSTRUCTION, 1);
    compute_method(ch, KEPLER_COMPUTE_INLINE_QMD_ADDRESS, 0, 1);
    compute_method(ch, KEPLER_COMPUTE_INLINE_QMD_ADDRESS + 1, (u32)((program + 0x1000) >> 8), 1);
    for (u32 i = 0; i < 64; ++i) compute_method(ch, KEPLER_COMPUTE_INLINE_QMD_DATA + i, q[i], 1);
    CHECK(g_vk_dispatches == dispatched + 1);
    CHECK(!gmem_read(target, data, sizeof data));
    for (u32 k = 0; k < 4; ++k) CHECK(snx_ld_le32(data + k * 4) == (k < 3 ? 0x222 : 0xcdcdcdcdu));
    g_gpu.chan[90] = NULL;
    free(ch);
    puts("Native jump tables: per-lane BRX and uniform BRXU reach their relocated cases");
    return 0;
}

/* A multiple of 1/8 in [-2, 6): products are exact in F32, and their sums of a few hundred at 1/64 need F16's
   rounding after the eighth */
static u16 matrix_half(u32 seed) { return float_to_half((float)((int)(seed * 2654435761u >> 26) - 16) / 8.0f); }
/* The half that F2F.F16 gives: rounded to nearest even as on the console, or toward zero. The translator converts
   with packHalf2x16, whose rounding Vulkan leaves to the device, and lavapipe truncates */
static u16 round_half(float f, u32 truncate) {
    u16 h = float_to_half(f);
    return truncate && fabsf(half_to_float(h)) > fabsf(f) ? (u16)(h - 1) : h;
}
/* MOVM.16.MT88 on the CPU: lane t holds row t / 4, columns 2(t % 4) and 2(t % 4) + 1 of an 8x8 matrix of halves,
   so element (r, c) is half c % 2 of lane 4r + c / 2. Half h of lane t of the result is the source's
   (2(t % 4) + h, t / 4) */
static void transpose_lanes(const u32* in, u32* out) {
    for (u32 t = 0; t < 32; ++t) {
        u32 word = 0;
        for (u32 h = 0; h < 2; ++h) {
            u32 row = 2 * (t % 4) + h, col = t / 4;
            word |= ((in[4 * row + col / 2] >> 16 * (col % 2)) & 0xffff) << 16 * h;
        }
        out[t] = word;
    }
}

/* HMMA.16816.F16, MOVM.16.MT88 and LDSM.16.M88 of one warp. HMMA is held to fmaf in k order, rounded to F16 after
   k = 7 and at the end, from C, as eden-ns2 models the tensor core. LDSM's lane 8m + r gives the address of row
   7 - r of matrix m, so a mistake in the lane order cannot cancel out */
static int check_ampere_matrix(void) {
    const u64 program = 0x7200000, parameters = program + 0x2000, buffer = program + 0x4000;
    u8 code[1024] = {0};
    u32 count = 0;
    ampere_op(code, &count, 0x919, 1, 0, 0, 33u << 8);         /* R1 = TID.X */
    ampere_op(code, &count, 0x824, 2, 1, 4, 0x078e02ff);       /* R2 = TID * 4 */
    ampere_op(code, &count, 0xab9, 0, 0, 0xb0u << 6, 5u << 9); /* UR0:1 = c[0][0xb0] */
    for (u32 r = 0; r < 8; ++r) /* A0..A3, B0, B1, C0 and C1 in R4..R11 */
        ampere_op(code, &count, 0x981, 4 + r, 2, r * 0x80u << 8, 0x081e8900);
    ampere_op(code, &count, 0x23c, 12, 4, 8, 0x80a); /* R12:R13 = A B + C */
    ampere_op(code, &count, 0x23c, 14, 4, 8, 0x8ff); /* R14:R15 = A B */
    ampere_op(code, &count, 0x23a, 16, 4, 0, 0);     /* R16 = A0 transposed */
    for (u32 m = 0; m < 4; ++m)                      /* shared[0x80m + 4 TID] = R4 + m */
        ampere_op(code, &count, 0x388, 0, 2, (4 + m) | m * 0x80u << 8, 0x800);
    ampere_op(code, &count, 0xb1d, 0, 0, 0, 0);
    ampere_op(code, &count, 0x824, 17, 1, 16, 0x078e02ff);
    ampere_op(code, &count, 0x812, 17, 17, 0x70, 0x078e3cff); /* R17 = 16 TID ^ 0x70: row 7 - r */
    ampere_op(code, &count, 0x83b, 20, 17, 0, 0x200);         /* LDSM.x4 */
    ampere_op(code, &count, 0x83b, 24, 17, 0, 0x4200);        /* LDSM.x4.T */
    ampere_op(code, &count, 0x83b, 28, 17, 0x80u << 8, 0);    /* LDSM.x1 of matrix 1, by the offset */
    static const u32 outputs[] = {12, 13, 14, 15, 16, 20, 21, 22, 23, 24, 25, 26, 27, 28};
    const u32 noutputs = sizeof outputs / sizeof *outputs;
    for (u32 k = 0; k < noutputs; ++k)
        ampere_op(code, &count, 0x986, 0, 2, outputs[k] | (0x400u + k * 0x80u) << 8, 0x0810a900);
    ampere_op(code, &count, 0x94d, 0, 0, 0, 0x03800000);

    u16 A[16][16], B[16][8], C[16][8];
    for (u32 r = 0; r < 16; ++r)
        for (u32 k = 0; k < 16; ++k) A[r][k] = matrix_half(r * 16 + k);
    for (u32 k = 0; k < 16; ++k)
        for (u32 c = 0; c < 8; ++c) B[k][c] = matrix_half(512 + k * 8 + c);
    for (u32 r = 0; r < 16; ++r)
        for (u32 c = 0; c < 8; ++c) C[r][c] = matrix_half(1024 + r * 8 + c);
    /* lane t of group g = t / 4 and i = t % 4: PTX's mma.m16n8k16 fragments */
    u32 in[8][32], want[14][32], toward_zero[4][32]; /* HMMA's results when the host's F16 conversions truncate */
    for (u32 t = 0; t < 32; ++t) {
        u32 g = t / 4, i = t % 4;
        for (u32 r = 0; r < 4; ++r) {
            u32 row = g + 8 * (r & 1), k = 2 * i + 8 * (r >> 1);
            in[r][t] = A[row][k] | (u32)A[row][k + 1] << 16;
        }
        for (u32 r = 0; r < 2; ++r) in[4 + r][t] = B[2 * i + 8 * r][g] | (u32)B[2 * i + 8 * r + 1][g] << 16;
        for (u32 r = 0; r < 2; ++r) in[6 + r][t] = C[g + 8 * r][2 * i] | (u32)C[g + 8 * r][2 * i + 1] << 16;
        for (u32 no_c = 0; no_c < 2; ++no_c) /* R12:R13 with C, R14:R15 with RZ */
            for (u32 r = 0; r < 2; ++r)
                for (u32 truncate = 0; truncate < 2; ++truncate) {
                    u32 word = 0;
                    for (u32 col = 0; col < 2; ++col) {
                        u32 row = g + 8 * r, column = 2 * i + col;
                        float acc = no_c ? 0.0f : half_to_float(C[row][column]);
                        for (u32 k = 0; k < 16; ++k) {
                            acc = fmaf(half_to_float(A[row][k]), half_to_float(B[k][column]), acc);
                            if (k == 7) acc = half_to_float(round_half(acc, truncate));
                        }
                        word |= (u32)round_half(acc, truncate) << 16 * col;
                    }
                    (truncate ? toward_zero : want)[2 * no_c + r][t] = word;
                }
    }
    for (u32 t = 0; t < 32; ++t) { /* lane t's row t / 4 comes from lane 8m + t / 4, whose address is row 7 - t / 4 */
        for (u32 m = 0; m < 4; ++m) want[5 + m][t] = in[m][(7 - t / 4) * 4 + t % 4];
        want[13][t] = in[1][(7 - t / 4) * 4 + t % 4];
    }
    transpose_lanes(in[0], want[4]);
    for (u32 m = 0; m < 4; ++m) transpose_lanes(want[5 + m], want[9 + m]);

    gmmu_map(program, HEAP_BASE + 0x700000, 0x10000);
    CHECK(!gmem_write(program, code, count * 16));
    CHECK(!storage_write_words(parameters + 0xb0, (const u32[2]){(u32)buffer, (u32)(buffer >> 32)}, 2));
    for (u32 r = 0; r < 8; ++r) CHECK(!storage_write_words(buffer + r * 0x80, in[r], 32));
    u32 q[64] = {0};
    q[12] = q[13] = q[14] = 1;
    q[17] = 0x200; /* the shared memory size */
    q[18] = 0x30 | 32u << 16;
    q[19] = 1 | 1u << 16;
    q[20] = 1;
    q[32] = (u32)parameters;
    q[33] = (u32)(parameters >> 32) | 64u << 19;
    q[48] = (u32)program;
    q[49] = (u32)(program >> 32);
    GpuChannel* ch = calloc(1, sizeof *ch);
    CHECK(ch && !g_gpu.chan[90]);
    ch->id = 90;
    g_gpu.chan[90] = ch;
    u64 dispatched = g_vk_dispatches;
    compute_method(ch, KEPLER_COMPUTE_INVALIDATE_SHADER_CACHES, KEPLER_COMPUTE_INVALIDATE_INSTRUCTION, 1);
    compute_method(ch, KEPLER_COMPUTE_INLINE_QMD_ADDRESS, 0, 1);
    compute_method(ch, KEPLER_COMPUTE_INLINE_QMD_ADDRESS + 1, (u32)((program + 0x1000) >> 8), 1);
    for (u32 i = 0; i < 64; ++i) compute_method(ch, KEPLER_COMPUTE_INLINE_QMD_DATA + i, q[i], 1);
    CHECK(g_vk_dispatches == dispatched + 1);
    static const char* const names[] = {"HMMA row g",      "HMMA row g + 8",  "HMMA (RZ) row g", "HMMA (RZ) row g + 8",
                                        "MOVM",            "LDSM.x4 0",       "LDSM.x4 1",       "LDSM.x4 2",
                                        "LDSM.x4 3",       "LDSM.x4.T 0",     "LDSM.x4.T 1",     "LDSM.x4.T 2",
                                        "LDSM.x4.T 3",     "LDSM.x1 offset"};
    for (u32 k = 0; k < noutputs; ++k) {
        u8 got[32 * 4];
        CHECK(!gmem_read(buffer + 0x400 + k * 0x80, got, sizeof got));
        for (u32 t = 0; t < 32; ++t)
            if (snx_ld_le32(got + t * 4) != want[k][t] && (k >= 4 || snx_ld_le32(got + t * 4) != toward_zero[k][t])) {
                fprintf(stderr, "Ampere matrix %s, lane %u: %08x, expected %08x\n", names[k], t,
                        snx_ld_le32(got + t * 4), want[k][t]);
                return 1;
            }
    }
    g_gpu.chan[90] = NULL;
    free(ch);
    puts("Ampere matrix: HMMA.16816.F16 with and without C, MOVM, and LDSM x4, x4.T and x1 match the CPU");
    return 0;
}

/* Both stages are native, with poisoned Maxwell address registers. Check pixels, then change code at the same
   address and invalidate instructions to check the native source fingerprint and the kept draw state. */
static int check_ampere_graphics(void) {
    const u64 address = 0x570000000ull, cpu = HEAP_BASE + 0x400000;
    const u64 fragment = address + 0x10000, positions = address + 0x11000;
    const u64 parameters = address + 0x12000, target = address + 0x18000;
    gmmu_map(address, cpu, 0x20000);
    u8 vertex[512] = {0}, pixel[2048] = {0}, data[48];
    snx_st_le32(vertex, 0x81); /* native SPH version 4, as in captured graphics programs */
    snx_st_le32(pixel, 0x81);
    u32 count = 0;
    ampere_op(vertex + 128, &count, 0x321, 0, 255, 0x80ff, 0xc00);
    ampere_op(vertex + 128, &count, 0x322, 0, 255, 0x7000, 0xcff);
    ampere_op(vertex + 128, &count, 0x882, 2, 0, 8, 0);
    ampere_op(vertex + 128, &count, 0xabb, 4, 2, 3u << 22, 0x08000a00);
    ampere_op(vertex + 128, &count, 0xc02, 4, 0, 4, 0x08000f00);
    ampere_op(vertex + 128, &count, 0xc02, 5, 0, 5, 0x08000f00);
    ampere_op(vertex + 128, &count, 0x322, 0, 255, 0x8004, 0x4ff);
    ampere_op(vertex + 128, &count, 0x802, 6, 0, 2, 0xf00);
    ampere_op(vertex + 128, &count, 0x322, 0, 255, 0x5c06, 255); /* VPRS table index, not position or a varying */
    ampere_op(vertex + 128, &count, 0x94d, 0, 0, 0, 0x03800000);
    count = 0;
    snx_st_le32(pixel + 72, 15); /* RGBA output mask in the SPH */
    snx_st_le32(pixel + 24, 0x55); /* generic input 0, flat component modes */
    ampere_op(pixel + 128, &count, 0x802, 0, 0, 0xbf000000, 0xf00);
    ampere_op(pixel + 128, &count, 0x802, 1, 0, 0x3f000000, 0xf00);
    ampere_op(pixel + 128, &count, 0x802, 2, 0, 0xbe800000, 0xf00);
    ampere_op(pixel + 128, &count, 0x802, 3, 0, 0x3f800000, 0xf00);
    ampere_op(pixel + 128, &count, 0xa20, 0, 0, 3u << 22, 0x00410200); /* abs(R0) * c[3][0] */
    ampere_op(pixel + 128, &count, 0x621, 1, 1, 3u << 22 | 4u << 6, 0x00010100); /* -R1 + c[3][4] */
    ampere_op(pixel + 128, &count, 0x882, 2, 0, 0x3f800000, 0);
    ampere_op(pixel + 128, &count, 0x882, 3, 0, 0x80, 0);
    ampere_op(pixel + 128, &count, 0x526, 6, 3, 255, 0x080e0000);
    ampere_op(pixel + 128, &count, 0x526, 7, 3, 255, 0x080e0001);
    ampere_op(pixel + 128, &count, 0x220, 6, 6, 7, 0x00410000);
    ampere_op(pixel + 128, &count, 0x220, 2, 2, 6, 0x00410200);
    ampere_op(pixel + 128, &count, 0xc23, 2, 2, 2, 0x08010201);
    ampere_op(pixel + 128, &count, 0xa09, 2, 2, 3u << 22, 0x07810000); /* max(R2, 0.5) */
    ampere_op(pixel + 128, &count, 0x208, 1, 2, 255, 0x03800300); /* select -abs(R2), not raw R2 */
    ampere_op(pixel + 128, &count, 0x821, 1, 1, 0x3f800000, 0x00010000);
    ampere_op(pixel + 128, &count, 0x307, 0, 0, 0, 0x00209000); /* ceil(0.25) */
    ampere_op(pixel + 128, &count, 0x820, 0, 0, 0x3e000000, 0x00510000);
    ampere_op(pixel + 128, &count, 0x94d, 0, 0, 0, 0x03800000);
    pixel[480] = 0xff; /* unrelated bytes beyond EXIT */
    CHECK(!gmem_write(address, vertex, sizeof vertex));
    CHECK(!gmem_write(fragment, pixel, sizeof pixel));
    const float position[12] = {-1, -1, 0, 1, 3, -1, 0, 1, -1, 3, 0, 1};
    for (u32 i = 0; i < 12; ++i) {
        u32 word;
        memcpy(&word, position + i, sizeof word);
        snx_st_le32(data + i * 4, word);
    }
    CHECK(!gmem_write(positions, data, sizeof data));
    snx_st_le32(data, 0x3f000000);
    snx_st_le32(data + 4, 0x3f400000);
    snx_st_le32(data + 8, 0x40000000);
    snx_st_le32(data + 12, 0x3f800000);
    CHECK(!gmem_write(parameters, data, 16));
    GpuChannel* ch = calloc(1, sizeof *ch);
    CHECK(ch);
    ch->id = 91;
    ch->sub_class[0] = CLASS_AMPERE_B;
    ch->ds.bind.cb_addr[0][3] = parameters;
    ch->ds.bind.cb_size[0][3] = 16;
    ch->ds.bind.cb_addr[4][3] = parameters;
    ch->ds.bind.cb_size[4][3] = 16;
    u32* r = ch->r3d;
    r[0x583] = r[0x811] = r[0x851] = 0xdead0000; /* must not be read as native program addresses */
    r[0x815] = (u32)(address >> 32);
    r[0x816] = (u32)address;
    r[0x855] = (u32)(fragment >> 32);
    r[0x856] = (u32)fragment;
    r[0x850] = 1;
    r[0x64b] = 1;
    r[0x286] = 0x6420;
    r[0x3fd] = r[0x3fe] = 4u << 16;
    r[0x680] = 0x1111;
    r[MAXWELL3D_RASTERIZE_ENABLE] = 1;
    const float transform[6] = {2, 2, 0.5f, 2, 2, 0.5f};
    memcpy(r + 0x280, transform, sizeof transform);
    r[0x458] = 1u << 21 | 7u << 27; /* RGBA32F */
    r[0x700] = 0x1000 | 16;
    maxwell3d_method(ch, 0x180, 0, 0);
    maxwell3d_method(ch, 0x181, sizeof data, 1);
    maxwell3d_method(ch, 0x701, (u32)(positions >> 32), 0);
    maxwell3d_method(ch, 0x702, (u32)positions, 1);
    ch->r3d_dirty = ~0u;
    CHECK(!mesh_pixel(ch, target, 0, (const u8[4]){64, 64, 191, 255}));
    /* A nonzero VPRS selector must preserve position and varyings. Coarse color-only shading is rendered at
       full rate, but culling, supersampling and reserved rates must leave the clear intact. */
    const u32 vprs = 0x2a00 / 4 + 2 * 4;
    maxwell3d_method(ch, 0x2cc / 4, 1, 1);
    maxwell3d_method(ch, vprs, 1, 1);
    maxwell3d_method(ch, vprs + 1, 0x88888888, 1);
    maxwell3d_method(ch, vprs + 2, 0xbbbbbbb5, 1);
    CHECK(!mesh_pixel(ch, target, 0, (const u8[4]){64, 64, 191, 255}));
    const u32 refused_rates[] = {0, 4, 12, 15};
    for (u32 i = 0; i < sizeof refused_rates / sizeof *refused_rates; ++i) {
        maxwell3d_method(ch, vprs + 2, 0x05555555 | refused_rates[i] << 28, 1);
        CHECK(!mesh_pixel_check(ch, target, 0, (const u8[4]){255, 0, 0, 255}, 15, 0));
    }
    maxwell3d_method(ch, vprs, 0, 1);
    CHECK(!mesh_pixel(ch, target, 0, (const u8[4]){64, 64, 191, 255}));
    maxwell3d_method(ch, 0x181, 0, 1);
    CHECK(!mesh_pixel(ch, target, 0, (const u8[4]){255, 0, 0, 255}));
    maxwell3d_method(ch, 0x181, sizeof data, 1);
    CHECK(!mesh_pixel(ch, target, 0, (const u8[4]){64, 64, 191, 255}));
    /* The last stream, with the address written before its size. */
    maxwell3d_method(ch, 0x77C, 0x1000 | 16, 1);
    maxwell3d_method(ch, 0x77D, (u32)(positions >> 32), 0);
    maxwell3d_method(ch, 0x77E, (u32)positions, 1);
    maxwell3d_method(ch, 0x1BE, 0, 0);
    maxwell3d_method(ch, 0x1BF, sizeof data, 1);
    maxwell3d_method(ch, 0x458, 31 | 1u << 21 | 7u << 27, 1);
    CHECK(!mesh_pixel(ch, target, 0, (const u8[4]){64, 64, 191, 255}));
    snx_st_le32(pixel + 128 + 3 * 16 + 4, 0x3f000000); /* alpha becomes 0.5 */
    CHECK(!gmem_write(fragment, pixel, sizeof pixel));
    maxwell3d_method(ch, MAXWELL3D_INVALIDATE_SHADER_CACHE_NO_WFI, 1, 1);
    CHECK(!mesh_pixel(ch, target, 0, (const u8[4]){64, 64, 191, 128}));
    /* Native centroid IPA must read varying values, not a constant or the instruction's multiplier. With the
       renderer's single sample its location is the pixel centre. Exercise flat, perspective and linear inputs,
       including a centroid read of 1/w beside the varying. Vertex x/y become x/y * 0.125 + 0.25. */
    u8 centroid_vertex[512] = {0};
    snx_st_le32(centroid_vertex, 0x81);
    count = 0;
    ampere_op(centroid_vertex + 128, &count, 0x321, 0, 255, 0x80ff, 0xc00);
    ampere_op(centroid_vertex + 128, &count, 0x322, 0, 255, 0x7000, 0xcff);
    for (u32 component = 0; component < 2; ++component) {
        ampere_op(centroid_vertex + 128, &count, 0x820, component, component, 0x3e000000, 0x00410000);
        ampere_op(centroid_vertex + 128, &count, 0x821, component, component, 0x3e800000, 0);
    }
    ampere_op(centroid_vertex + 128, &count, 0x322, 0, 255, 0x8000, 0xcff);
    ampere_op(centroid_vertex + 128, &count, 0x94d, 0, 0, 0, 0x03800000);
    CHECK(!gmem_write(address, centroid_vertex, sizeof centroid_vertex));
    for (u32 mode = 1; mode <= 3; ++mode) {
        snx_st_le32(pixel + 24, mode * 0x55);
        count = 0;
        ampere_op(pixel + 128, &count, 0x326, 0, 0, 255, 0x000e1020);
        ampere_op(pixel + 128, &count, 0x326, 1, 0, 255, 0x000e1021);
        ampere_op(pixel + 128, &count, 0x326, 2, 0, 255, 0x000e101f);
        ampere_op(pixel + 128, &count, 0x326, 3, 0, 255, 0x000e5023);
        ampere_op(pixel + 128, &count, 0x94d, 0, 0, 0, 0x03800000);
        CHECK(!gmem_write(fragment, pixel, sizeof pixel));
        maxwell3d_method(ch, MAXWELL3D_INVALIDATE_SHADER_CACHE_NO_WFI, 1, 1);
        CHECK(!mesh_pixel(ch, target, 0, (const u8[4]){mode == 1 ? 32 : 56, mode == 1 ? 32 : 56, 255, 255}));
    }
    snx_st_le32(pixel + 24, 0x55);
    CHECK(!gmem_write(address, vertex, sizeof vertex));
    /* ULDC uses UP, not a same-numbered ordinary predicate. Keep the two banks opposite, take both guard
       polarities and preserve both words of a predicated-off 64-bit destination. Indexed loads use UR2 = 8. */
    for (u32 indexed = 0; indexed < 2; ++indexed)
        for (u32 active = 0; active < 2; ++active) {
            count = 0;
            ampere_op(pixel + 128, &count, 0x80c, 0, 255, active ^ 1, 0x03f02070);
            ampere_op(pixel + 128, &count, 0x88c, 0, 63, active, 0x0bf02070);
            ampere_op(pixel + 128, &count, 0x882, 2, 0, 8, 0);
            for (u32 c = 0; c < 4; ++c)
                ampere_op(pixel + 128, &count, 0x882, 4 + c, 0, 0x3e000000, 0);
            for (u32 pair = 0; pair < 2; ++pair) {
                u32 at = count;
                ampere_op(pixel + 128, &count, indexed ? 0xabb : 0xab9, 4 + pair * 2, indexed ? 2 : 0,
                          3u << 22 | (indexed ? 0 : 8u << 6), (indexed ? 0x08000000 : 0) | 5u << 9);
                snx_st_le32(pixel + 128 + at * 16,
                            (snx_ld_le32(pixel + 128 + at * 16) & ~0xf000u) | pair << 15); /* @UP0, @!UP0 */
            }
            for (u32 c = 0; c < 4; ++c)
                ampere_op(pixel + 128, &count, 0xc02, c, 0, 4 + c, 0x08000f00);
            /* A wrong restoration of P0 kills the fragment. */
            u32 at = count;
            ampere_op(pixel + 128, &count, 0x95b, 0, 0, 0, 0x03800000);
            snx_st_le32(pixel + 128 + at * 16, active ? 0x895b : 0x095b);
            ampere_op(pixel + 128, &count, 0x94d, 0, 0, 0, 0x03800000);
            CHECK(!gmem_write(fragment, pixel, sizeof pixel));
            maxwell3d_method(ch, MAXWELL3D_INVALIDATE_SHADER_CACHE_NO_WFI, 1, 1);
            CHECK(!mesh_pixel(ch, target, 0, active ? (const u8[4]){32, 32, 255, 255}
                                                   : (const u8[4]){255, 255, 32, 32}));
        }
    /* USHF.R.U32 reads the low word when the high word is URZ. Check an aliased destination and shifts at
       both word boundaries, not just the captured shift of 24. A wrong bit pattern discards the pixel. */
    const u32 uniform_shifts[] = {0, 1, 24, 31, 32};
    for (u32 i = 0; i < sizeof uniform_shifts / sizeof *uniform_shifts; ++i) {
        u32 shift = uniform_shifts[i], expected = shift == 32 ? 0 : 0x87654321u >> shift;
        count = 0;
        ampere_op(pixel + 128, &count, 0x882, 8, 0, 0x87654321, 0);
        ampere_op(pixel + 128, &count, 0x899, 8, 8, shift, 0x0800163f);
        ampere_op(pixel + 128, &count, 0xc02, 4, 0, 8, 0x08000f00);
        ampere_op(pixel + 128, &count, 0x80c, 0, 4, expected, 0x03f02070);
        u32 at = count;
        ampere_op(pixel + 128, &count, 0x95b, 0, 0, 0, 0x03800000);
        snx_st_le32(pixel + 128 + at * 16, 0x895b);
        for (u32 c = 0; c < 4; ++c)
            ampere_op(pixel + 128, &count, 0x802, c, 0, c == 3 ? 0x3f800000 : 0x3e800000, 0xf00);
        ampere_op(pixel + 128, &count, 0x94d, 0, 0, 0, 0x03800000);
        CHECK(!gmem_write(fragment, pixel, sizeof pixel));
        maxwell3d_method(ch, MAXWELL3D_INVALIDATE_SHADER_CACHE_NO_WFI, 1, 1);
        CHECK(!mesh_pixel(ch, target, 0, (const u8[4]){64, 64, 64, 255}));
    }
    /* UPLOP3 must snapshot inputs when its destination is also its execution guard. UP2UR checks the packed
       result and untouched predicates. The vector P0 stays true across every uniform-bank operation. */
    const u32 uniform_luts[] = {0, 255, 4, 0x40, 0x80, 0x96, 0xe8};
    for (u32 lut_index = 0; lut_index < sizeof uniform_luts / sizeof *uniform_luts; ++lut_index)
        for (u32 bits = 0; bits < 8; ++bits)
            for (u32 invert = 0; invert < 2; ++invert) {
                u32 lut = uniform_luts[lut_index], value = bits;
                u32 index = (bits & 1) << 2 | (bits & 2) | (bits >> 2);
                if ((bits & 1) != invert) value = (bits & ~1u) | (lut >> index & 1);
                count = 0;
                ampere_op(pixel + 128, &count, 0x80c, 0, 255, 0, 0x03f02070);
                for (u32 p = 0; p < 3; ++p)
                    ampere_op(pixel + 128, &count, 0x88c, 0, 63, (bits >> p & 1) ^ 1,
                              0x0bf02070 | p << 17);
                u32 at = count;
                ampere_op(pixel + 128, &count, 0x89c, 0, 0, 0,
                          (lut & 7) | (lut >> 3) << 8 | 7u << 20 | 1u << 13 | 2u << 4);
                snx_st_le32(pixel + 128 + at * 16, 0x89c | invert << 15);
                ampere_op(pixel + 128, &count, 0x883, 8, 63, 7, 0x08000000);
                ampere_op(pixel + 128, &count, 0xc02, 4, 0, 8, 0x08000f00);
                ampere_op(pixel + 128, &count, 0x80c, 0, 4, value, 0x03f22070); /* P1, keep P0 */
                for (u32 p = 0; p < 2; ++p) {
                    at = count;
                    ampere_op(pixel + 128, &count, 0x95b, 0, 0, 0, 0x03800000);
                    snx_st_le32(pixel + 128 + at * 16, 0x895b | p << 12);
                }
                for (u32 c = 0; c < 4; ++c)
                    ampere_op(pixel + 128, &count, 0x802, c, 0, c == 3 ? 0x3f800000 : 0x3e800000, 0xf00);
                ampere_op(pixel + 128, &count, 0x94d, 0, 0, 0, 0x03800000);
                CHECK(!gmem_write(fragment, pixel, sizeof pixel));
                maxwell3d_method(ch, MAXWELL3D_INVALIDATE_SHADER_CACHE_NO_WFI, 1, 1);
                CHECK(!mesh_pixel(ch, target, 0, (const u8[4]){64, 64, 64, 255}));
            }
    /* Uniform shift counts, a uniform I2F source and a negated LEA addend occur in scene fragment programs.
       Compare exact words on the GPU, with destination aliases and source-byte/half selectors. */
    for (u32 form = 0; form < 8; ++form)
        for (u32 i = 0; i < 8; ++i) {
            const u32 shifts[8] = {0, 1, 8, 24, 31, 32, 33, 63};
            u32 value = 0x87654321u + i * 0x1234567u, shift = shifts[i], expected = 0;
            count = 0;
            ampere_op(pixel + 128, &count, 0x802, 4, 0, value, 0xf00);
            if (!form) {
                ampere_op(pixel + 128, &count, 0x882, 4, 0, shift, 0);
                ampere_op(pixel + 128, &count, 0xc19, 4, 255, 4, 0x08011e04);
                expected = value >> (shift & 31);
            } else if (form == 1) {
                ampere_op(pixel + 128, &count, 0x211, 4, 4, 0x80000004, 0x078e00ff | (shift & 31) << 11);
                expected = (value << (shift & 31)) - value;
            } else {
                u32 size = (form - 2) / 2, sign = form & 1;
                u32 part = size == 0 ? i & 3 : size == 1 ? i & 1 : 0;
                u32 bits = size == 2 ? value : value >> (part * (8u << size)) & ((1u << (8u << size)) - 1);
                i32 signed_value = (i32)bits;
                if (sign && size < 2 && (bits >> ((8u << size) - 1))) signed_value = (i32)(bits | (~0u << (8u << size)));
                float converted = sign ? (float)signed_value : (float)bits;
                memcpy(&expected, &converted, sizeof expected);
                ampere_op(pixel + 128, &count, 0x882, 4, 0, value, 0);
                ampere_op(pixel + 128, &count, 0xd06, 4, 0, 4 | part << 28, 0x08001000 | size << 20 | sign << 10);
            }
            ampere_op(pixel + 128, &count, 0x80c, 0, 4, expected, 0x03f02070);
            u32 at = count;
            ampere_op(pixel + 128, &count, 0x95b, 0, 0, 0, 0x03800000);
            snx_st_le32(pixel + 128 + at * 16, 0x895b);
            for (u32 c = 0; c < 4; ++c)
                ampere_op(pixel + 128, &count, 0x802, c, 0, c == 3 ? 0x3f800000 : 0x3e800000, 0xf00);
            ampere_op(pixel + 128, &count, 0x94d, 0, 0, 0, 0x03800000);
            CHECK(!gmem_write(fragment, pixel, sizeof pixel));
            maxwell3d_method(ch, MAXWELL3D_INVALIDATE_SHADER_CACHE_NO_WFI, 1, 1);
            CHECK(!mesh_pixel(ch, target, 0, (const u8[4]){64, 64, 64, 255}));
        }
    /* EXIT needs both its guard and flow predicate. A taken exit preserves the first colour, otherwise the
       red output changes. Include !PT (never exit), PT, and both polarities of each ordinary predicate. */
    for (u32 bits = 0; bits < 4; ++bits)
        for (u32 invert = 0; invert < 2; ++invert)
            for (u32 mode = 0; mode < 4; ++mode) {
                u32 condition = mode < 2 ? 1u | mode << 3 : mode == 2 ? 7 : 15;
                int taken = ((bits & 1) != invert) && (mode < 2 ? (bits >> 1) != mode : mode == 2);
                count = 0;
                for (u32 p = 0; p < 2; ++p)
                    ampere_op(pixel + 128, &count, 0x80c, 0, 255, (bits >> p & 1) ^ 1,
                              0x03f02070 | p << 17);
                for (u32 c = 0; c < 4; ++c)
                    ampere_op(pixel + 128, &count, 0x802, c, 0, c == 3 ? 0x3f800000 : 0x3e800000, 0xf00);
                u32 at = count;
                ampere_op(pixel + 128, &count, 0x94d, 0, 0, 0, condition << 23);
                snx_st_le32(pixel + 128 + at * 16, 0x94d | invert << 15);
                ampere_op(pixel + 128, &count, 0x802, 0, 0, 0x3f000000, 0xf00);
                ampere_op(pixel + 128, &count, 0x94d, 0, 0, 0, 0x03800000);
                CHECK(!gmem_write(fragment, pixel, sizeof pixel));
                maxwell3d_method(ch, MAXWELL3D_INVALIDATE_SHADER_CACHE_NO_WFI, 1, 1);
                CHECK(!mesh_pixel(ch, target, 0, (const u8[4]){taken ? 64 : 128, 64, 64, 255}));
            }
    /* R196 used to alias UR4, and R239 is beside the compact uniform bank. Both must survive uniform pair loads.
       Float operands also use the chosen layout, including UR6:UR7 at its last legal pair. */
    count = 0;
    ampere_op(pixel + 128, &count, 0x802, 196, 0, 0x3e800000, 0xf00);
    ampere_op(pixel + 128, &count, 0x802, 203, 0, 0x3f000000, 0xf00);
    ampere_op(pixel + 128, &count, 0x802, 239, 0, 0x3e000000, 0xf00);
    ampere_op(pixel + 128, &count, 0xab9, 4, 0, 3u << 22, 5u << 9);
    ampere_op(pixel + 128, &count, 0xab9, 6, 0, 3u << 22 | 8u << 6, 5u << 9);
    ampere_op(pixel + 128, &count, 0xc20, 0, 196, 4, 0x08410000);
    ampere_op(pixel + 128, &count, 0xc20, 1, 203, 6, 0x08410000);
    ampere_op(pixel + 128, &count, 0xc02, 2, 0, 5, 0x08000f00);
    ampere_op(pixel + 128, &count, 0xc20, 3, 239, 7, 0x08410000);
    ampere_op(pixel + 128, &count, 0x94d, 0, 0, 0, 0x03800000);
    CHECK(!gmem_write(fragment, pixel, sizeof pixel));
    maxwell3d_method(ch, MAXWELL3D_INVALIDATE_SHADER_CACHE_NO_WFI, 1, 1);
    CHECK(!mesh_pixel(ch, target, 0, (const u8[4]){32, 255, 191, 32}));
    /* Native TEX carries the descriptor bank in the instruction, independently of the engine's texture bank. */
    const u64 tic_address = address + 0x13000, tsc_address = address + 0x14000, texture = address + 0x15000;
    u32 tic[8] = {0x08 | 2u << 19 | 3u << 22 | 4u << 25 | 5u << 28,
                  (u32)texture, (u32)(texture >> 32) | 2u << 21, 1, 1u << 23 | 1u, 2, 0, 0};
    u32 tsc[8] = {2u | 2u << 3 | 2u << 6, 1u | 1u << 4 | 1u << 6};
    CHECK(!storage_write_words(tic_address, tic, 8));
    CHECK(!storage_write_words(tsc_address, tsc, 8));
    u8 texels[96] = {0};
    for (u32 y = 0; y < 3; ++y)
        for (u32 x = 0; x < 2; ++x) memcpy(texels + y * 32 + x * 4, (const u8[4]){17, 83, 149, 231}, 4);
    CHECK(!gmem_write(texture, texels, sizeof texels));
    CHECK(!storage_write_words(parameters + 64, (const u32[1]){0}, 1));
    ch->ds.bind.cb_size[4][3] = 80;
    r[0x55d] = (u32)(tic_address >> 32);
    r[0x55e] = (u32)tic_address;
    r[0x557] = (u32)(tsc_address >> 32);
    r[0x558] = (u32)tsc_address;
    count = 0;
    ampere_op(pixel + 128, &count, 0x802, 4, 0, 0x3f000000, 0xf00);
    ampere_op(pixel + 128, &count, 0x802, 5, 0, 0x3f000000, 0xf00);
    ampere_op(pixel + 128, &count, 0xb6f, 6, 255, 0x00c01000, 0x000003ff);
    ampere_op(pixel + 128, &count, 0xb60, 0, 4, 0x30c01005, 0x041e0f02);
    ampere_op(pixel + 128, &count, 0x245, 6, 0, 6, 0x00201000);
    ampere_op(pixel + 128, &count, 0x245, 7, 0, 7, 0x00201000);
    ampere_op(pixel + 128, &count, 0x220, 0, 0, 6, 0x00410000);
    ampere_op(pixel + 128, &count, 0x220, 1, 1, 7, 0x00410000);
    ampere_op(pixel + 128, &count, 0x94d, 0, 0, 0, 0x03800000);
    CHECK(!gmem_write(fragment, pixel, sizeof pixel));
    maxwell3d_method(ch, MAXWELL3D_INVALIDATE_SHADER_CACHE_NO_WFI, 1, 1);
    CHECK(!mesh_pixel(ch, target, 0, (const u8[4]){34, 249, 149, 231}));
    /* The vector layout (bit 60 clear): the coordinates are consecutive from A, so the same sample from R4, R5 */
    snx_st_le32(pixel + 128 + 3 * 16 + 4, 0x20c01005);
    CHECK(!gmem_write(fragment, pixel, sizeof pixel));
    maxwell3d_method(ch, MAXWELL3D_INVALIDATE_SHADER_CACHE_NO_WFI, 1, 1);
    CHECK(!mesh_pixel(ch, target, 0, (const u8[4]){34, 249, 149, 231}));
    /* TEX.SCR.LZ.AOFFI adds signed nibble offsets from the separate operand. Only the selected texel holds
       colour, and the destination aliases the coordinates. Check positive and negative offsets on both axes. */
    for (u32 direction = 0; direction < 4; ++direction) {
        u32 x = direction == 1, y = direction == 3 ? 2 : 0;
        u32 tx = direction < 2 ? x ^ 1 : x, ty = direction >= 2 ? 1 : y;
        memset(texels, 0, sizeof texels);
        memcpy(texels + ty * 32 + tx * 4, (const u8[4]){17, 83, 149, 231}, 4);
        CHECK(!gmem_write(texture, texels, sizeof texels));
        count = 0;
        ampere_op(pixel + 128, &count, 0x802, 4, 0, x ? 0x3f400000 : 0x3e800000, 0xf00);
        ampere_op(pixel + 128, &count, 0x802, 5, 0, y ? 0x3f555555 : 0x3e2aaaab, 0xf00);
        ampere_op(pixel + 128, &count, 0x802, 9, 0, (direction & 1 ? 15u : 1u) << (direction >= 2 ? 4 : 0), 0xf00);
        if (g_vk.subgroup_size == 32 || (g_vk.subgroup32_stages & VK_SHADER_STAGE_FRAGMENT_BIT))
            ampere_op(pixel + 128, &count, 0x948, 0, 0, 0xffffffff, 0x03800000);
        ampere_op(pixel + 128, &count, 0xb60, 4, 4, 0x30c01009, 0x009e1f06);
        for (u32 c = 0; c < 4; ++c)
            ampere_op(pixel + 128, &count, 0x202, c, 0, 4 + c, 0xf00);
        ampere_op(pixel + 128, &count, 0x94d, 0, 0, 0, 0x03800000);
        CHECK(!gmem_write(fragment, pixel, sizeof pixel));
        maxwell3d_method(ch, MAXWELL3D_INVALIDATE_SHADER_CACHE_NO_WFI, 1, 1);
        CHECK(!mesh_pixel(ch, target, 0, (const u8[4]){17, 83, 149, 231}));
        /* The explicit LOD and the offset are consecutive in R8:R9, not the same scratch word. */
        count = 3;
        ampere_op(pixel + 128, &count, 0x802, 8, 0, 0x3f800000, 0xf00);
        ampere_op(pixel + 128, &count, 0xb60, 4, 4, 0x30c01008, 0x019e1f06);
        for (u32 c = 0; c < 4; ++c)
            ampere_op(pixel + 128, &count, 0x202, c, 0, 4 + c, 0xf00);
        ampere_op(pixel + 128, &count, 0x94d, 0, 0, 0, 0x03800000);
        CHECK(!gmem_write(fragment, pixel, sizeof pixel));
        maxwell3d_method(ch, MAXWELL3D_INVALIDATE_SHADER_CACHE_NO_WFI, 1, 1);
        CHECK(!mesh_pixel(ch, target, 0, (const u8[4]){17, 83, 149, 231}));
    }
    for (u32 y = 0; y < 3; ++y)
        for (u32 x = 0; x < 2; ++x) memcpy(texels + y * 32 + x * 4, (const u8[4]){17, 83, 149, 231}, 4);
    CHECK(!gmem_write(texture, texels, sizeof texels));
    /* A single-component TXQ packs width or height into the first destination, even when it is odd. */
    for (u32 dimension = 0; dimension < 2; ++dimension) {
        count = 0;
        ampere_op(pixel + 128, &count, 0xb6f, 5, 255, 0x00c01000, 255u | (1u << dimension) << 8);
        ampere_op(pixel + 128, &count, 0x80c, 0, 5, dimension ? 3 : 2, 0x03f02070);
        u32 at = count;
        ampere_op(pixel + 128, &count, 0x95b, 0, 0, 0, 0x03800000);
        snx_st_le32(pixel + 128 + at * 16, 0x895b);
        for (u32 c = 0; c < 4; ++c)
            ampere_op(pixel + 128, &count, 0x802, c, 0, c == 3 ? 0x3f800000 : 0x3e800000, 0xf00);
        ampere_op(pixel + 128, &count, 0x94d, 0, 0, 0, 0x03800000);
        CHECK(!gmem_write(fragment, pixel, sizeof pixel));
        maxwell3d_method(ch, MAXWELL3D_INVALIDATE_SHADER_CACHE_NO_WFI, 1, 1);
        CHECK(!mesh_pixel(ch, target, 0, (const u8[4]){64, 64, 64, 255}));
    }
    /* TLD4 of component 1 (bits 87..88): the four texels around the point, each its green, 83 */
    count = 0;
    ampere_op(pixel + 128, &count, 0x802, 4, 0, 0x3f000000, 0xf00);
    ampere_op(pixel + 128, &count, 0x802, 5, 0, 0x3f000000, 0xf00);
    ampere_op(pixel + 128, &count, 0xb63, 0, 4, 0x30c01005, 0x049e0f02);
    ampere_op(pixel + 128, &count, 0x94d, 0, 0, 0, 0x03800000);
    CHECK(!gmem_write(fragment, pixel, sizeof pixel));
    maxwell3d_method(ch, MAXWELL3D_INVALIDATE_SHADER_CACHE_NO_WFI, 1, 1);
    CHECK(!mesh_pixel(ch, target, 0, (const u8[4]){83, 83, 83, 83}));
    /* Packed TEX.F16, including zero padding of an odd channel count. Compare the exact half bits before
       promotion. A wrong packed result discards the fragment and leaves the red clear instead. */
    tic[0] = (tic[0] & ~0x7ffffu) | 1u | 7u << 7 | 7u << 10 | 7u << 13 | 7u << 16; /* RGBA32F */
    CHECK(!storage_write_words(tic_address, tic, 8));
    const u32 sample[4] = {0x3e000000, 0x3e800000, 0x3f000000, 0x3f800000};
    for (u32 texel = 0; texel < 6; ++texel)
        for (u32 c = 0; c < 4; ++c) snx_st_le32(texels + texel * 16 + c * 4, sample[c]);
    CHECK(!gmem_write(texture, texels, sizeof texels));
    const u32 halves[4] = {0x3000, 0x3400, 0x3800, 0x3c00}; /* exact F16 values: no rounding-mode assumption */
    for (u32 channels = 1; channels <= 4; ++channels) {
        count = 0;
        ampere_op(pixel + 128, &count, 0x802, 4, 0, 0x3f000000, 0xf00);
        ampere_op(pixel + 128, &count, 0x802, 5, 0, 0x3f000000, 0xf00);
        ampere_op(pixel + 128, &count, 0xb60, 0, 4, 0x30c01005,
                  0x041e8000 | ((1u << channels) - 1) << 8 | (channels > 2 ? 2 : 255));
        u32 discard = 0;
        for (u32 pair = 0; pair < channels; pair += 2) {
            u32 packed = halves[pair] | (pair + 1 < channels ? halves[pair + 1] << 16 : 0);
            ampere_op(pixel + 128, &count, 0x80c, 0, pair, packed, 0x03f02070); /* P0 = packed result == expected */
            discard = count;
            ampere_op(pixel + 128, &count, 0x95b, 0, 0, 0, 0x03800000);
            snx_st_le32(pixel + 128 + discard * 16, 0x895b); /* @!P0 KILL */
        }
        for (u32 c = 0; c < 4; ++c)
            if (c < channels)
                ampere_op(pixel + 128, &count, 0x230, 4 + c, 255,
                          0x20000000 | (c & 1) << 28 | (c & ~1u), 0x4100);
            else
                ampere_op(pixel + 128, &count, 0x802, 4 + c, 0, 0, 0xf00);
        for (u32 c = 0; c < 4; ++c) ampere_op(pixel + 128, &count, 0x202, c, 0, 4 + c, 0xf00);
        ampere_op(pixel + 128, &count, 0x94d, 0, 0, 0, 0x03800000);
        CHECK(!gmem_write(fragment, pixel, sizeof pixel));
        maxwell3d_method(ch, MAXWELL3D_INVALIDATE_SHADER_CACHE_NO_WFI, 1, 1);
        u8 expected[4] = {32, channels > 1 ? 64 : 0, channels > 2 ? 128 : 0, channels > 3 ? 255 : 0};
        CHECK(!mesh_pixel(ch, target, 0, expected));
        snx_st_le32(pixel + 128 + discard * 16, 0x095b); /* @P0 KILL takes the other path */
        CHECK(!gmem_write(fragment, pixel, sizeof pixel));
        maxwell3d_method(ch, MAXWELL3D_INVALIDATE_SHADER_CACHE_NO_WFI, 1, 1);
        CHECK(!mesh_pixel(ch, target, 0, (const u8[4]){255, 0, 0, 255}));
    }
    /* TLD.LZ of texel (1, 2) alone, its integer coordinates scalar in R4 and R9 (the TEX handle's bank and offset),
       three channels split into R0:R1 and R2. The texture is pitch linear (TIC header version 2), 32 bytes a row.
       Only that texel holds the sample, so a wrong coordinate reads zero */
    memset(texels, 0, sizeof texels);
    for (u32 c = 0; c < 4; ++c) snx_st_le32(texels + 2 * 32 + 16 + c * 4, sample[c]);
    CHECK(!gmem_write(texture, texels, sizeof texels));
    count = 0;
    ampere_op(pixel + 128, &count, 0x802, 4, 0, 1, 0xf00);
    ampere_op(pixel + 128, &count, 0x802, 9, 0, 2, 0xf00);
    ampere_op(pixel + 128, &count, 0xb66, 0, 4, 0x30c01009, 0x008e0702);
    ampere_op(pixel + 128, &count, 0x802, 3, 0, 0x3f800000, 0xf00);
    ampere_op(pixel + 128, &count, 0x94d, 0, 0, 0, 0x03800000);
    CHECK(!gmem_write(fragment, pixel, sizeof pixel));
    maxwell3d_method(ch, MAXWELL3D_INVALIDATE_SHADER_CACHE_NO_WFI, 1, 1);
    CHECK(!mesh_pixel(ch, target, 0, (const u8[4]){32, 64, 128, 255}));
    /* A 1D fetch has no second coordinate. Poison R9 and fill only texel 1 of the one-row view. */
    tic[4] &= ~(15u << 23);
    tic[5] = 0;
    CHECK(!storage_write_words(tic_address, tic, 8));
    memset(texels, 0, sizeof texels);
    for (u32 c = 0; c < 4; ++c) snx_st_le32(texels + 16 + c * 4, sample[c]);
    CHECK(!gmem_write(texture, texels, sizeof texels));
    count = 0;
    ampere_op(pixel + 128, &count, 0x802, 4, 0, 1, 0xf00);
    ampere_op(pixel + 128, &count, 0x802, 9, 0, 0xdeadbeef, 0xf00);
    ampere_op(pixel + 128, &count, 0xb66, 0, 4, 0x10c010ff, 0x009e0f02);
    ampere_op(pixel + 128, &count, 0x94d, 0, 0, 0, 0x03800000);
    CHECK(!gmem_write(fragment, pixel, sizeof pixel));
    maxwell3d_method(ch, MAXWELL3D_INVALIDATE_SHADER_CACHE_NO_WFI, 1, 1);
    CHECK(!mesh_pixel(ch, target, 0, (const u8[4]){32, 64, 128, 255}));
    /* Cross the row boundary of a long buffer texture. A wrongly emitted 2D fetch cannot read this as (x, 0). */
    const u32 texel_row = 1u << SH_TEXEL_ROW_SHIFT;
    const u64 long_texture = address + 0x40000, long_cpu = HEAP_BASE + 0x500000;
    gmmu_map(long_texture, long_cpu, (u64)texel_row * 32);
    memset(GP(long_cpu), 0, (size_t)texel_row * 32);
    for (u32 c = 0; c < 4; ++c)
        snx_st_le32((u8*)GP(long_cpu) + (texel_row + 1) * 16 + c * 4, sample[c]);
    u32 long_tic[8] = {tic[0], (u32)long_texture, (u32)(long_texture >> 32), 0,
                       (texel_row + 1) | 6u << 23, 0, 0, 0};
    CHECK(!storage_write_words(tic_address, long_tic, 8));
    snx_st_le32(pixel + 132, texel_row + 1);
    CHECK(!gmem_write(fragment, pixel, sizeof pixel));
    maxwell3d_method(ch, MAXWELL3D_INVALIDATE_SHADER_CACHE_NO_WFI, 1, 1);
    CHECK(!mesh_pixel(ch, target, 0, (const u8[4]){32, 64, 128, 255}));
    tic[4] |= 1u << 23;
    tic[5] = 2;
    CHECK(!storage_write_words(tic_address, tic, 8));
    /* TMML of the 2-wide texture at u = 4 * the pixel's x, v constant: rho is 8, so the unclamped LOD (component 0,
       as the translator maps Maxwell's TMML) is 3.0, 768 in TMML's 8.8 fixed point, and the level clamped to the one
       level (component 1) is 0. Any other value discards to the red clear. The alpha is c[9][4] (0.75) through ULDC,
       bank 9 bound as in a game's vertex program */
    ch->ds.bind.cb_addr[4][9] = parameters;
    ch->ds.bind.cb_size[4][9] = 16;
    count = 0;
    ampere_op(pixel + 128, &count, 0x326, 6, 0, 255, 0x000e001c);
    ampere_op(pixel + 128, &count, 0x820, 4, 6, 0x40800000, 0x00410000);
    ampere_op(pixel + 128, &count, 0x802, 5, 0, 0x3f000000, 0xf00);
    ampere_op(pixel + 128, &count, 0x802, 1, 0, 0x55, 0xf00); /* the level must replace it */
    ampere_op(pixel + 128, &count, 0xb69, 0, 4, 0x20c010ff, 0x000003ff);
    for (u32 component = 0; component < 2; ++component) {
        ampere_op(pixel + 128, &count, 0x80c, 0, component, component ? 0 : 768, 0x03f02070);
        u32 lod_kill = count;
        ampere_op(pixel + 128, &count, 0x95b, 0, 0, 0, 0x03800000);
        snx_st_le32(pixel + 128 + lod_kill * 16, 0x895b); /* @!P0 KILL */
    }
    ampere_op(pixel + 128, &count, 0x802, 0, 0, 0x3e800000, 0xf00);
    ampere_op(pixel + 128, &count, 0x802, 1, 0, 0x3f000000, 0xf00);
    ampere_op(pixel + 128, &count, 0x802, 2, 0, 0, 0xf00);
    ampere_op(pixel + 128, &count, 0xab9, 6, 0, 9u << 22 | 4u << 6, 4u << 9);
    ampere_op(pixel + 128, &count, 0xc02, 3, 0, 6, 0x08000f00);
    ampere_op(pixel + 128, &count, 0x94d, 0, 0, 0, 0x03800000);
    CHECK(!gmem_write(fragment, pixel, sizeof pixel));
    maxwell3d_method(ch, MAXWELL3D_INVALIDATE_SHADER_CACHE_NO_WFI, 1, 1);
    CHECK(!mesh_pixel(ch, target, 0, (const u8[4]){64, 128, 0, 191}));
    /* Root tables are command-uploaded bytes, not an alias of ordinary bank 17 or a render-area heuristic.
       Exercise all eight selectors, auto-increment, a nonzero offset and the last word, then update a live table. */
    ch->ds.bind.cb_addr[4][17] = parameters + 0x800;
    ch->ds.bind.cb_size[4][17] = 16;
    CHECK(!storage_write_words(parameters + 0x800, (const u32[4]){0, 0, 0, 0x3f400000}, 4));
    for (u32 table = 0; table < 8; ++table) {
        float first = (float)(table + 1) * 0.125f, last = (float)(table + 1) * 0.0625f;
        u32 first_bits, last_bits;
        memcpy(&first_bits, &first, sizeof first_bits);
        memcpy(&last_bits, &last, sizeof last_bits);
        maxwell3d_method(ch, 0x141, table, 1);
        maxwell3d_method(ch, 0x142, first_bits, 0);
        maxwell3d_method(ch, 0x142, 0x3e800000, 1);
        maxwell3d_method(ch, 0x141, table | 252u << 8, 1);
        maxwell3d_method(ch, 0x142, last_bits, 1);
    }
    for (u32 table = 0; table < 8; ++table) {
        memset(pixel, 0, sizeof pixel);
        snx_st_le32(pixel, 0x81); /* SPH version 4, unlike a Maxwell program */
        snx_st_le32(pixel + 72, 15);
        count = 0;
        ampere_op(pixel + 128, &count, 0xab9, 6, 0, (24 + table) << 22, 0xa00);
        ampere_op(pixel + 128, &count, 0xc02, 0, 0, 6, 0x08000f00);
        ampere_op(pixel + 128, &count, 0xc02, 1, 0, 7, 0x08000f00);
        ampere_op(pixel + 128, &count, 0xa02, 2, 0, (24 + table) << 22 | 252u << 6, 0xf00);
        ampere_op(pixel + 128, &count, 0xa02, 3, 0, 17u << 22 | 12u << 6, 0xf00);
        ampere_op(pixel + 128, &count, 0x94d, 0, 0, 0, 0x03800000);
        CHECK(!gmem_write(fragment, pixel, sizeof pixel));
        maxwell3d_method(ch, MAXWELL3D_INVALIDATE_SHADER_CACHE_NO_WFI, 1, 1);
        CHECK(!mesh_pixel(ch, target, 0, (const u8[4]){(u8)(((table + 1) * 255 + 4) / 8),
                                                   64, (u8)(((table + 1) * 255 + 8) / 16), 191}));
    }
    maxwell3d_method(ch, 0x141, 7, 1);
    maxwell3d_method(ch, 0x142, 0x3e000000, 1);
    CHECK(!mesh_pixel(ch, target, 0, (const u8[4]){32, 64, 128, 191}));
    if (g_vk.storage_buffer_slots >= 32) {
        /* All fragment bindings share a page. Only slot 31 writes, through an atomic, so the second alias-map
           header and the high write-mask bit must both work. The other words are read into the red output. */
        const u64 stored = address + 0x16000;
        u8 initial[136], result[136];
        memset(initial, 0xcd, sizeof initial);
        for (u32 i = 0; i < 32; ++i) {
            u64 pointer = stored + 4 + i * 4;
            CHECK(!storage_write_words(parameters + 0x110 + i * 16,
                                       (const u32[3]){(u32)pointer, (u32)(pointer >> 32), 4}, 3));
            snx_st_le32(initial + 4 + i * 4, i == 31 ? 0xdeadbeef : 0x3c000000);
        }
        CHECK(!gmem_write(stored, initial, sizeof initial));
        memset(pixel, 0, sizeof pixel);
        snx_st_le32(pixel + 72, 15);
        count = 0;
        for (u32 i = 0; i < 3; ++i) ampere_op(pixel + 128, &count, 0x802, i, 0, 0, 0xf00);
        ampere_op(pixel + 128, &count, 0x802, 3, 0, 0x3f800000, 0xf00);
        ampere_op(pixel + 128, &count, 0x802, 5, 0, 0x3f000000, 0xf00);
        for (u32 i = 0; i < 32; ++i) {
            ampere_op(pixel + 128, &count, 0xab9, 0, 0, (0x110 + i * 16) << 6, 5u << 9);
            if (i == 31)
                ampere_op(pixel + 128, &count, 0x9a8, 255, 255, 5, 0x080ee140 | 8u << 23);
            else {
                ampere_op(pixel + 128, &count, 0x981, 4, 255, 0, 0x081e8900);
                ampere_op(pixel + 128, &count, 0x221, 0, 0, 4, 0);
            }
        }
        ampere_op(pixel + 128, &count, 0x94d, 0, 0, 0, 0x03800000);
        ch->ds.bind.cb_addr[4][0] = parameters;
        ch->ds.bind.cb_size[4][0] = 0x400;
        CHECK(!gmem_write(fragment, pixel, sizeof pixel));
        maxwell3d_method(ch, MAXWELL3D_INVALIDATE_SHADER_CACHE_NO_WFI, 1, 1);
        /* Changing the rate must not multiply fragment atomics. A refused draw leaves the pixels and every
           storage byte untouched. Full-rate tables, or a selected disabled table, allow the real write. */
        maxwell3d_method(ch, vprs, 1, 1);
        maxwell3d_method(ch, vprs + 1, 0x88888888, 1);
        maxwell3d_method(ch, vprs + 2, 0x88888888, 1);
        CHECK(!mesh_pixel_check(ch, target, 0, (const u8[4]){255, 0, 0, 255}, 15, 0));
        CHECK(!gmem_read(stored, result, sizeof result));
        CHECK(!memcmp(result, initial, sizeof result));
        maxwell3d_method(ch, 0x2cc / 4, 2 | 3u << 4, 1);
        CHECK(!mesh_pixel(ch, target, 0, (const u8[4]){62, 0, 0, 255}));
        CHECK(!gmem_write(stored, initial, sizeof initial));
        maxwell3d_method(ch, 0x2cc / 4, 1, 1);
        maxwell3d_method(ch, vprs + 1, 0x55555555, 1);
        maxwell3d_method(ch, vprs + 2, 0x55555555, 1);
        CHECK(!mesh_pixel(ch, target, 0, (const u8[4]){62, 0, 0, 255}));
        snx_st_le32(initial + 4 + 31 * 4, 0x3f000000);
        CHECK(!gmem_read(stored, result, sizeof result));
        CHECK(!memcmp(result, initial, sizeof result));
    }
    /* A graphics BSSY can name code beyond the old 128 KiB native read. The IADD3 body also grows past 128 KiB
       after lowering. Pixels after the far BSYNC prove that neither input nor normalized code was truncated.
       Change a far instruction at the same address to check the source fingerprint's full extent. */
    const u64 large_fragment = address + 0x1000000;
    const u32 far = 0x24000, large_bytes = 128 + far + 6 * 16;
    gmmu_map(large_fragment, cpu + 0x200000, 0x40000);
    u8* large_code = calloc(1, large_bytes);
    CHECK(large_code);
    snx_st_le32(large_code, 0x81);
    snx_st_le32(large_code + 72, 15);
    count = 0;
    ampere_op(large_code + 128, &count, 0x945, 0, 0, far - 16, 0x03800000);
    while (count < far / 16)
        ampere_op(large_code + 128, &count, 0x210, 4, 255, 255, 0x07ffe0ff);
    ampere_op(large_code + 128, &count, 0x941, 0, 0, 0, 0x03800000);
    for (u32 c = 0; c < 4; ++c)
        ampere_op(large_code + 128, &count, 0x802, c, 0, c == 3 ? 0x3f800000 : 0x3e800000, 0xf00);
    ampere_op(large_code + 128, &count, 0x94d, 0, 0, 0, 0x03800000);
    CHECK(!gmem_write(large_fragment, large_code, large_bytes));
    maxwell3d_method(ch, 0x855, (u32)(large_fragment >> 32), 0);
    maxwell3d_method(ch, 0x856, (u32)large_fragment, 1);
    CHECK(!mesh_pixel(ch, target, 0, (const u8[4]){64, 64, 64, 255}));
    snx_st_le32(large_code + 128 + far + 16 + 4, 0x3f000000);
    CHECK(!gmem_write(large_fragment, large_code, large_bytes));
    maxwell3d_method(ch, MAXWELL3D_INVALIDATE_SHADER_CACHE_NO_WFI, 1, 1);
    CHECK(!mesh_pixel(ch, target, 0, (const u8[4]){128, 64, 64, 255}));
    free(large_code);
    free(ch);
    puts("Ampere graphics: native stage addresses and headers, attributes, modified float arithmetic and code invalidation");
    return 0;
}

/* A standalone native mesh program: the vertex slot holds one whose SPH says tessellation init (type 1, shader 2,
   version 4), C797's MESH_CONTROL is on and the mesh registers give its vertices, primitives and invocations. It
   writes its vertices' positions and its primitives' indices through the stage buffer (ISBEWR forms with and
   without .SKEW), and the fragment program draws them. A GPU pixel proves that the module, the mesh pipeline and
   vkCmdDrawMeshTasksEXT all work, not only that the program translates. The same draw without MESH_CONTROL leaves
   the target cleared: a stage buffer access outside a mesh program is refused. */
static int check_ampere_mesh(void) {
    const u64 address = 0x578000000ull, cpu = HEAP_BASE + 0x480000;
    const u64 fragment = address + 0x10000, target = address + 0x18000;
    gmmu_map(address, cpu, 0x20000);
    u8 mesh[1024] = {0}, pixel[512] = {0};
    snx_st_le32(mesh, 0x000c0481);  /* native SPH version 4, type 1, shader type 2 (as captured mesh programs) */
    snx_st_le32(mesh + 52, 0xf000); /* OMAP: position X, Y, Z and W are attributes 28..31 */
    snx_st_le32(pixel, 0x81);
    snx_st_le32(pixel + 72, 15); /* RGBA output mask in the SPH */
    u32 count = 0;
    /* one primitive, then the three vertex indices 0, 1, 2 at bytes 4..6 of the map region. A stage buffer write's
       data field names the register holding the value */
    ampere_op(mesh + 128, &count, 0x802, 1, 0, 1, 0xf00); /* R1 = 1 */
    ampere_op(mesh + 128, &count, 0x927, 0, 255, 1 | 3u << 8, 0x8800);
    for (u32 i = 0; i < 3; i++) {
        ampere_op(mesh + 128, &count, 0x802, 1, 0, i, 0xf00); /* R1 = the index */
        ampere_op(mesh + 128, &count, 0x927, 0, 255, 1 | (4 + i) << 8, 0x8000);
    }
    /* the triangle (-1,-1), (3,-1), (-1,3): x0, y0, z, w, x1, x2 */
    ampere_op(mesh + 128, &count, 0x802, 2, 0, 0xbf800000, 0xf00); /* R2 = -1 */
    ampere_op(mesh + 128, &count, 0x802, 3, 0, 0xbf800000, 0xf00); /* R3 = -1 */
    ampere_op(mesh + 128, &count, 0x802, 4, 0, 0, 0xf00);          /* R4 = 0 */
    ampere_op(mesh + 128, &count, 0x802, 5, 0, 0x3f800000, 0xf00); /* R5 = 1 */
    ampere_op(mesh + 128, &count, 0x802, 6, 0, 0x40400000, 0xf00); /* R6 = 3 */
    ampere_op(mesh + 128, &count, 0x802, 7, 0, 0x40400000, 0xf00); /* R7 = 3 */
    /* vertex, line (0 x, 1 y, 2 z, 3 w), register: without .SKEW, then with it */
    static const struct {
        u32 vertex, line, reg, skew;
    } writes[] = {{0, 0, 2, 0}, {0, 1, 3, 1}, {0, 2, 4, 0}, {0, 3, 5, 1},
                  {1, 0, 6, 1}, {1, 1, 3, 0}, {1, 2, 4, 1}, {1, 3, 5, 0},
                  {2, 0, 2, 1}, {2, 1, 7, 0}, {2, 2, 4, 1}, {2, 3, 5, 0}};
    for (u32 i = 0; i < sizeof writes / sizeof *writes; i++) {
        u32 offset = (writes[i].line * 32 + writes[i].vertex) * 4; /* a linear byte address in the region */
        ampere_op(mesh + 128, &count, 0x927, 0, 255, writes[i].reg | offset << 8,
                  (writes[i].skew ? 0xf800 : 0xb800));
    }
    ampere_op(mesh + 128, &count, 0x94d, 0, 0, 0, 0x03800000);
    /* the fragment program writes its registers 0..3 as the color */
    count = 0;
    ampere_op(pixel + 128, &count, 0x802, 0, 0, 0x3f800000, 0xf00);
    ampere_op(pixel + 128, &count, 0x802, 1, 0, 0x3f800000, 0xf00);
    ampere_op(pixel + 128, &count, 0x802, 2, 0, 0, 0xf00);
    ampere_op(pixel + 128, &count, 0x802, 3, 0, 0x3f800000, 0xf00);
    ampere_op(pixel + 128, &count, 0x94d, 0, 0, 0, 0x03800000);
    CHECK(!gmem_write(address, mesh, sizeof mesh));
    CHECK(!gmem_write(fragment, pixel, sizeof pixel));
    GpuChannel* ch = calloc(1, sizeof *ch);
    CHECK(ch);
    ch->id = 93;
    ch->sub_class[0] = CLASS_AMPERE_B;
    u32* r = ch->r3d;
    r[0x815] = (u32)(address >> 32);
    r[0x816] = (u32)address;
    r[0x850] = 1;
    r[0x855] = (u32)(fragment >> 32);
    r[0x856] = (u32)fragment;
    r[MAXWELL3D_MESH_CONTROL] = 1;
    r[MAXWELL3D_MESH_SHADER_A] = 4 | 3u << 4 | 1u << 13; /* 3 vertices, 1 primitive */
    r[MAXWELL3D_MESH_SHADER_B] = 1u << 20;               /* one invocation */
    r[0x64b] = 1;
    r[0x286] = 0x6420;
    r[0x3fd] = r[0x3fe] = 4u << 16;
    r[0x680] = 0x1111;
    r[MAXWELL3D_RASTERIZE_ENABLE] = 1;
    const float transform[6] = {2, 2, 0.5f, 2, 2, 0.5f};
    memcpy(r + 0x280, transform, sizeof transform);
    ch->r3d_dirty = ~0u;
    /* A refused device feature must leave the translator available for the next program. */
    int has_mesh = g_vk.has_mesh, has_task = g_vk.has_task;
    for (u32 task = 0; task < 2; ++task) {
        ShaderInfo si = {0};
        si.mesh[0] = r[MAXWELL3D_MESH_SHADER_A];
        si.task = (u8)task;
        g_vk.has_mesh = task != 0;
        g_vk.has_task = 0;
        size_t words = 0;
        CHECK(!vk_translate_spirv(mesh, sizeof mesh, ST_VS, &si, &words, NULL, NULL));
        CHECK(si.unsupported && strstr(si.first_unsupported, task ? "taskShader" : "VK_EXT_mesh_shader"));
    }
    g_vk.has_mesh = has_mesh;
    g_vk.has_task = has_task;
    CHECK(!mesh_pixel(ch, target, 0, (const u8[4]){255, 255, 0, 255}));
    /* MESH_CONTROL off: the stage buffer access is refused, so the draw is skipped and the clear stays */
    r[MAXWELL3D_MESH_CONTROL] = 0;
    ch->r3d_dirty = ~0u;
    CHECK(!mesh_pixel_check(ch, target, 0, (const u8[4]){255, 0, 0, 255}, 15, 0));
    free(ch);
    puts("Ampere mesh: a standalone mesh program draws its vertices and primitive indices");
    return 0;
}
