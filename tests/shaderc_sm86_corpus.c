/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The corpus entries the shaderc_sm86 tests give snx-shaderc --sm86: an SM86 compute program after five instructions
   of padding, which a window can hold before its program, so the program's code starts 80 bytes in, past its own 32.
   And a fragment program after its header, with a TEX of the handle at c[3][0x40]: with --tex-table its texture is a
   slot of the texture table, and with --pool-map one of the pool map's (ShCaps.pool_map), which spirv-val checks in
   both of its forms, a storage buffer and a device address */
#include "common/utilities/common.h"
#include "nx/gpu/shader/shader.h"
#include "nx/gpu/shader/sm86.h"
#include <stdio.h>

/* bits lo..lo+n of an instruction's 128 */
static void set(u8* insn, unsigned lo, unsigned n, u64 v) {
    for (unsigned i = 0; i < n; i++) {
        unsigned bit = lo + i;
        if ((v >> i) & 1)
            insn[bit >> 3] |= (u8)(1u << (bit & 7));
        else
            insn[bit >> 3] &= (u8) ~(1u << (bit & 7));
    }
}
/* opcode on PT, with no barriers */
static void fix(u8* insn, u32 opcode) {
    set(insn, 0, 12, opcode);
    set(insn, 12, 3, 7);
    set(insn, 87, 3, 7);
    set(insn, 110, 3, 7);
    set(insn, 113, 3, 7);
}

/* opcode on PT, with no barriers, for an instruction that takes no bits 87-89 of its own */
static void op(u8* insn, u32 opcode) {
    set(insn, 0, 12, opcode);
    set(insn, 12, 3, 7);
    set(insn, 110, 3, 7);
    set(insn, 113, 3, 7);
}

int main(int argc, char** argv) {
    u8 code[7 * 16] = {0}; /* five of padding (all zero), EXIT, and the BRA to itself that ends a program */
    ShaderInfo si = {0};
    if (argc != 2) {
        fprintf(stderr, "usage: %s DIR: writes a corpus entry there\n", argv[0]);
        return 2;
    }
    fix(code + 5 * 16, 0x94d);
    fix(code + 6 * 16, 0x947);
    set(code + 6 * 16, 34, 48, 0xfffffffffffcull); /* -4 words: to itself */
    if (snx_mkdirs(argv[1]) || !shader_corpus_save(argv[1], code, sizeof code, sizeof code, ST_CS, &si, 1)) {
        fprintf(stderr, "cannot write the corpus entry in %s\n", argv[1]);
        return 1;
    }
    /* the fragment program: its SPH (a fragment header of version 4, every output component of target 0), then MOV32I
       R4, 0.5; MOV32I R5, 0.5; TEX.SCR R0-R3, R4, R5, c[3][0x40], 2D; EXIT; and the BRA to itself */
    u8 fs[SM86_SPH_BYTES + 5 * 16] = {0};
    u8* sph = fs;
    u8* p = fs + SM86_SPH_BYTES;
    sph[0] = (u8)(2 | 4u << 5);
    sph[1] = (u8)((4u << 5 | 5u << 10) >> 8);
    sph[18 * 4] = 0xf;
    for (int r = 0; r < 2; r++) {
        u8* mov = p + 16 * r;
        op(mov, 0x802);
        set(mov, 16, 8, 4u + (unsigned)r);
        set(mov, 24, 8, 255);
        set(mov, 32, 32, 0x3f000000);
    }
    u8* tex = p + 2 * 16;
    op(tex, 0xb60);
    set(tex, 16, 8, 0);   /* R0-R3 */
    set(tex, 24, 8, 4);   /* R4 */
    set(tex, 32, 8, 255); /* no offset register */
    set(tex, 40, 14, 0x10);
    set(tex, 54, 5, 3);
    set(tex, 60, 1, 1);
    set(tex, 61, 3, 1);
    set(tex, 64, 8, 255);
    set(tex, 72, 4, 0xf);
    set(tex, 81, 3, 7);
    fix(p + 3 * 16, 0x94d);
    fix(p + 4 * 16, 0x947);
    set(p + 4 * 16, 34, 48, 0xfffffffffffcull);
    ShaderInfo fsi = {0};
    if (!shader_corpus_save(argv[1], fs, sizeof fs, sizeof fs, ST_FS, &fsi, 1)) {
        fprintf(stderr, "cannot write the fragment program's corpus entry in %s\n", argv[1]);
        return 1;
    }
    return 0;
}
