/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The corpus entry the shaderc_sm86 test gives snx-shaderc --sm86: an SM86 compute program after five instructions of
   padding, which a window can hold before its program, so the program's code starts 80 bytes in, past its own 32 */
#include "common/utilities/common.h"
#include "nx/gpu/shader/shader.h"
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
    return 0;
}
