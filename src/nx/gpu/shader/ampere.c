/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Instruction fields and lowering adapted from eden-ns2's frontend/ampere/lower_to_maxwell.h, flow.cpp and ops_int.cpp:
   Copyright 2026 Eden Emulator Project, GPL-3.0-or-later.
   Encoding reference: Mesa NAK sm70_encode.rs, Copyright 2022 Collabora, Ltd., MIT. */
#include "nx/gpu/shader/ampere.h"
#include "nx/gpu/shader/translator.h"
#include "common/utilities/endian.h"
#include "common/utilities/bits.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Native GPRs and uniforms have two non-overlapping layouts. Keep the first for existing programs and matrix
   scratch (R176..R191). If it cannot lower a program, the second admits R0..R239 with UR0..UR7 at R240..R247,
   but no matrix scratch. Both reserve R248..R254 for lowering. Neither silently spills or aliases a bank. */
enum { DEFAULT_GPR_LIMIT = 176, WIDE_GPR_LIMIT = 240, MATRIX_FIRST = 176 };
enum { TEMP_A = 248, TEMP_B = 249, TEMP_C = 250 };
/* The packed UP bank persists across native instructions. The save register is temporary, so it can also serve
   as texture LOD storage in a different instruction. Ordinary predicates are restored after each UP operation. */
enum { UP_SAVE = 253, UP_MASK = 254, UP_SWAP = 1, UP_WRITE = 2, UP_RAW = 4 };
static u32 uniform_predicates(u32 w0, u32 w2) {
    u32 op = w0 & 0xfff;
    switch (op) {
        case 0x28c: case 0x88c: return UP_SWAP | UP_WRITE;
        case 0x89c: return UP_SWAP | UP_WRITE;
        case 0x883: return UP_RAW;
        /* UIADD3: a carry out (81..83) writes UP, .X (74) reads its carry in (87..90) from UP */
        case 0x290: case 0x890:
            return (w2 >> 17 & 7) != 7 ? UP_SWAP | UP_WRITE
                   : (w2 >> 10 & 1) || (w0 >> 12 & 15) != 7 ? UP_SWAP : 0;
        case 0x287: case 0x887: return UP_SWAP;
        /* UMOV and ULDC execute under a uniform guard. */
        case 0x882: case 0xab9: case 0xabb: return (w0 >> 12 & 15) != 7 ? UP_SWAP : 0;
        case 0x292: case 0x892:
            return (w2 >> 17 & 7) != 7 ? UP_SWAP | UP_WRITE : (w0 >> 12 & 15) != 7 ? UP_SWAP : 0;
        case 0x886: {
            u32 write = (w2 >> 17 & 7) != 7 ? UP_WRITE : 0;
            return write || (w0 >> 12 & 15) != 7 || (w2 >> 23 & 7) != 7 ? UP_SWAP | write : 0;
        }
        case 0x81c: return (w2 & 8) && (w2 >> 4 & 7) != 7 ? UP_RAW : 0;
        default: return 0;
    }
}
typedef struct {
    u8* out;
    u32 size, capacity, instructions;
    int full;
    u32 hmma; /* the HMMA.16816 subroutine's byte position, 0 for none */
} Lower;
enum { NATIVE_LIVE = 1, NATIVE_TARGET = 2 };
typedef struct {
    u32 end;
    u8 link, flags;
} NativeFunction;
typedef struct {
    u32 at, setup, link;
} NativeCall;
static int gpr(u32 r, u32 gpr_limit) { return r < gpr_limit || r == 255; }
static u32 uniform_count(u32 gpr_limit) { return gpr_limit == DEFAULT_GPR_LIMIT ? 56 : 8; }
static u32 uniform_reg(u32 r, u32 gpr_limit) {
    u32 first = gpr_limit == DEFAULT_GPR_LIMIT ? 192 : WIDE_GPR_LIMIT;
    return r < uniform_count(gpr_limit) ? first + r : r == 63 ? 255 : 0;
}
static void emit(Lower* l, u64 word) {
    u32 need = l->instructions % 3 ? 8 : 16;
    if (l->full || need > l->capacity - l->size) {
        l->full = 1;
        return;
    }
    if (need == 16) {
        snx_st_le64(l->out + l->size, 0x001f8000fc0007e0ull);
        l->size += 8;
    }
    snx_st_le64(l->out + l->size, word);
    l->size += 8;
    ++l->instructions;
}
static void mov_imm(Lower* l, u64 pred, u32 dst, u32 value) {
    emit(l, 0x010000000000f000ull | pred | dst | (u64)value << 20);
}
static void add(Lower* l, u64 pred, u32 dst, u32 a, u32 b, u64 flags) {
    emit(l, 0x5c10000000000000ull | pred | dst | (u64)a << 8 | (u64)b << 20 | flags);
}
static int constant(Lower* l, u64 pred, u32 dst, u32 operand) {
    u32 bank = operand >> 22 & 31, offset = operand >> 6 & 0xffff;
    if ((bank >= 18 && (bank < 24 || offset >= 256)) || (offset & 3) || (operand & 0xf800003fu)) return 0;
    emit(l, 0x4c98078000000000ull | pred | dst | (u64)(offset / 4) << 20 | (u64)bank << 34);
    return 1;
}
/* where the next emitted instruction goes: a scheduling word precedes every third */
static u32 next_pc(const Lower* l) { return l->size + (l->instructions % 3 ? 0 : 8); }
static void lane_id(Lower* l, u32 dst) { emit(l, 0xf0c8000000070000ull | dst); } /* S2R SR_LANEID */
static void shuffle(Lower* l, u32 dst, u32 src, u32 lane) { /* SHFL.IDX of a register lane over the warp */
    emit(l, 0xef10000000070000ull | dst | (u64)src << 8 | (u64)lane << 20 | 1ull << 29 | 0x1full << 34 | 7ull << 48);
}
static void half_to_float(Lower* l, u32 dst, u32 src, u32 high) { /* F2F.F32.F16 of a half of src */
    emit(l, 0x5ca8000000070000ull | dst | 2ull << 8 | 1ull << 10 | (u64)src << 20 | (u64)high << 41);
}
static void float_to_half(Lower* l, u32 reg) { /* F2F.F16.F32, rounded to nearest, the high half cleared */
    emit(l, 0x5ca8000000070000ull | reg | 1ull << 8 | 2ull << 10 | (u64)reg << 20);
}
static void lop_imm(Lower* l, u32 dst, u32 src, u32 mask) { /* LOP32I.AND */
    emit(l, 0x0400000000070000ull | dst | (u64)src << 8 | (u64)mask << 20);
}
static void shift_imm(Lower* l, u32 dst, u32 src, u32 amount, int left) {
    emit(l, (left ? 0x3848000000070000ull : 0x3828000000070000ull) | dst | (u64)src << 8 | (u64)amount << 20);
}
static void add_imm(Lower* l, u32 dst, u32 src, u32 value) { /* IADD32I */
    emit(l, 0x1c00000000070000ull | dst | (u64)src << 8 | (u64)value << 20);
}
static void move(Lower* l, u32 dst, u32 src) { emit(l, 0x5c98078000070000ull | dst | (u64)src << 20); }

/* MOVM.16.MT88's transpose of an 8x8 matrix of 16-bit elements, as eden-ns2's transpose_matrix: lane t holds row
   t / 4, columns 2(t % 4) (low half) and 2(t % 4) + 1 (high half). Element (r, c) of the result is (c, r) of the
   source, so lane t takes half (t / 4) % 2 of lanes 8(t % 4) + t / 8 and 8(t % 4) + 4 + t / 8. The target is written
   last, so it may be the source */
static void transpose_matrix(Lower* l, u32 target, u32 source) {
    lane_id(l, TEMP_A);
    lop_imm(l, TEMP_B, TEMP_A, 3);
    shift_imm(l, TEMP_B, TEMP_B, 3, 1);
    shift_imm(l, TEMP_C, TEMP_A, 3, 0);
    add(l, 7ull << 16, TEMP_B, TEMP_B, TEMP_C, 0);
    lop_imm(l, TEMP_C, TEMP_A, 4);
    shift_imm(l, TEMP_C, TEMP_C, 2, 1); /* 0 or 16: the half to take */
    for (u32 half = 0; half < 2; ++half) {
        if (half) add_imm(l, TEMP_B, TEMP_B, 4);
        shuffle(l, 251 + half, source, TEMP_B);
        emit(l, 0x5c28000000070000ull | (251 + half) | (u64)(251 + half) << 8 | (u64)TEMP_C << 20);
        lop_imm(l, 251 + half, 251 + half, 0xffff);
    }
    shift_imm(l, 252, 252, 16, 1);
    emit(l, 0x5c40000000070000ull | target | 251ull << 8 | 252ull << 20 | 1ull << 41 | 7ull << 48);
}

/* HMMA.16816.F16, D (16x8) = A (16x16) B (16x8) + C, as a subroutine that the program calls once for each, with A in
   R176..R179, B in R180..R181 and C in R182..R183, and D returned in R176..R177. Lane t of group g = t / 4 and
   i = t % 4 holds A's rows g and g + 8 at k 2i, 2i + 1 (A0, A1) and 8 + 2i, 9 + 2i (A2, A3), B's k 2i, 2i + 1 (B0)
   and 8 + 2i, 9 + 2i (B1) of column g, and C's and D's rows g and g + 8 at columns 2i, 2i + 1. As eden-ns2 models
   the tensor core: products accumulate in F32, rounded to F16 after each 8 of them. Its scratch: the four
   accumulators (R184..R187), A's (R188, R189) and B's (R190, R191) halves of the lanes that hold step k, TEMP_A for
   8i, TEMP_B for a lane, TEMP_C, 251 and 252 for the factors */
static void emit_hmma(Lower* l) {
    const u32 S = MATRIX_FIRST, acc = S + 8;
    for (u32 row = 0; row < 2; ++row)
        for (u32 col = 0; col < 2; ++col) half_to_float(l, acc + row * 2 + col, S + 6 + row, col);
    lane_id(l, TEMP_A);
    lop_imm(l, TEMP_A, TEMP_A, 3);
    shift_imm(l, TEMP_A, TEMP_A, 3, 1);
    for (u32 pair = 0; pair < 8; ++pair) {
        u32 p = pair & 3, high = pair >> 2;
        /* A from lane 4g + p: SHFL.IDX with an immediate lane in the 4-lane segment (0x1c) of the caller's group */
        for (u32 row = 0; row < 2; ++row)
            emit(l, 0xef10000000070000ull | (S + 12 + row) | (u64)(S + row + 2 * high) << 8 | (u64)p << 20 |
                        1ull << 28 | 1ull << 29 | 0x1c1full << 34 | 7ull << 48);
        /* B from lanes 8i + p (column 2i) and 8i + 4 + p (column 2i + 1) */
        for (u32 col = 0; col < 2; ++col) {
            add_imm(l, TEMP_B, TEMP_A, p + 4 * col);
            shuffle(l, S + 14 + col, S + 4 + high, TEMP_B);
        }
        for (u32 half = 0; half < 2; ++half) {
            half_to_float(l, 251, S + 14, half);
            half_to_float(l, 252, S + 15, half);
            for (u32 row = 0; row < 2; ++row) {
                half_to_float(l, TEMP_C, S + 12 + row, half);
                for (u32 col = 0; col < 2; ++col) /* FFMA acc = A * B + acc */
                    emit(l, 0x5980000000070000ull | (acc + row * 2 + col) | (u64)TEMP_C << 8 |
                                (u64)(251 + col) << 20 | (u64)(acc + row * 2 + col) << 39);
            }
        }
        if (p == 3)
            for (u32 reg = acc; reg < acc + 4; ++reg) {
                float_to_half(l, reg);
                if (pair < 7) half_to_float(l, reg, reg, 0);
            }
    }
    for (u32 row = 0; row < 2; ++row) /* ISCADD: column 2i + 1 in the high half */
        emit(l, 0x5c18000000070000ull | (S + row) | (u64)(acc + row * 2 + 1) << 8 | (u64)(acc + row * 2) << 20 |
                    16ull << 39);
    emit(l, 0xe32000000007000full); /* RET */
}
/* The three floating operands share their form selector and ABS/NEG fields. Materialize a modified source
   before writing a destination, including when that destination aliases one of the native sources. */
static int float_source(Lower* l, u64 pred, u32 w0, u32 w1, u32 w2, u32 which, u32* source, u32 gpr_limit) {
    u32 form = w0 >> 9 & 7, kind = 0, reg = w0 >> 24;
    u32 absolute = w2 >> 9 & 1, negate = w2 >> 8 & 1, temporary = TEMP_A + which;
    if (which) {
        int wide = which == 1 ? form >= 4 && form <= 6 : form == 2 || form == 3 || form == 7;
        if (wide) {
            kind = form >= 6 ? 3 : form == 2 || form == 4 ? 1 : 2;
            reg = w1 & 255;
            absolute = kind == 1 ? 0 : w1 >> 30 & 1;
            negate = kind == 1 ? 0 : w1 >> 31;
        } else {
            int upper = which == 2 || form == 2 || form == 3 || form == 7;
            if (!upper && (w1 & 0x3fffff00u)) return 0;
            reg = upper ? w2 & 255 : w1 & 255;
            absolute = upper ? w2 >> 10 & 1 : w1 >> 30 & 1;
            negate = upper ? w2 >> 11 & 1 : w1 >> 31;
        }
    }
    if (kind == 1) {
        mov_imm(l, pred, temporary, w1);
        reg = temporary;
    } else if (kind == 2) {
        if (!constant(l, pred, temporary, w1 & 0x3fffffffu)) return 0;
        reg = temporary;
    } else if (kind == 3) {
        if (w1 & 0x3fffff00u || !(reg = uniform_reg(reg, gpr_limit))) return 0;
    } else if (!gpr(reg, gpr_limit)) return 0;
    if (absolute || negate) {
        if (reg != temporary) emit(l, 0x5c98078000000000ull | pred | temporary | (u64)reg << 20);
        if (absolute)
            emit(l, 0x0400000000000000ull | pred | temporary | (u64)temporary << 8 | 0x7fffffffull << 20);
        if (negate)
            emit(l, 0x0400000000000000ull | pred | temporary | (u64)temporary << 8 | 0x80000000ull << 20 | 2ull << 53);
        reg = temporary;
    }
    *source = reg;
    return 1;
}
static u32 reject(ShaderInfo* si, u32 offset, u32 op, const char* why) {
    si->unsupported = 1;
    snprintf(si->first_unsupported, sizeof si->first_unsupported, "Ampere opcode %03x at +%x: %s", op, offset, why);
    return 0;
}

/* NOINC does not push a native return frame. The compiler uses this jump over a loop's back-edge as a break. */
static int loop_break_call(const u8* code, u32 at, u32 bytes) {
    return bytes - at >= 32 && (snx_ld_le32(code + at) & ~0xf000u) == 0x944 &&
           snx_ld_le32(code + at + 4) == 16 && snx_ld_le32(code + at + 8) == 0x03c00000 &&
           snx_ld_le32(code + at + 16) == 0x7947 &&
           (snx_ld_le32(code + at + 24) & ~0x3ffffu) == 0x03800000 &&
           (snx_ld_le32(code + at + 24) & 0x20000);
}

/* A passthrough geometry program's exit hook: LEPC Rr, MOV Rk = d, IADD3 Rr += Rk with a carry out, IMAD.X Rr+1 +=
   that carry, then BMOV.64 ATEXIT_PC, Rr (0x357). At LEPC's own address plus d, OUT.FINAL, BMOV.64 ATEXIT_PC, 0
   (0x957) and EXIT run when the program ends (OUT.FINAL as NAK encodes it: no destination, a handle, stream RZ, no
   emit or cut). The hook lies past the main body's last EXIT, where no branch goes. Only LEPC's address reaches it.
   Maxwell has no OUT.FINAL, and the translator's passthrough geometry emits its primitive when the program ends.
   Thus the hook has nothing to do in the lowering, and its address has no other reader. Returns the hook's offset
   for the LEPC at at, 0 when no hook starts there. The hook's own words are exit_hook_body's. */
static u32 exit_hook_prologue(const u8* code, u32 bytes, u32 at) {
    if (bytes - at < 80) return 0;
    u32 w[5][3];
    for (u32 i = 0; i < 5; ++i)
        for (u32 k = 0; k < 3; ++k) w[i][k] = snx_ld_le32(code + at + i * 16 + k * 4);
    u32 r = w[0][0] >> 16 & 255, temp = w[1][0] >> 16 & 255, carry = w[2][2] >> 17 & 7;
    if (w[0][0] != (0x734eu | r << 16) || w[0][1] || w[0][2] || r + 1 >= DEFAULT_GPR_LIMIT ||
        (w[1][0] & 0xff00ffffu) != 0x7802 || temp == r || temp == r + 1 || w[1][2] != 0xf00 ||
        w[2][0] != (0x7210u | r << 16 | r << 24) || w[2][1] != temp || (w[2][2] & ~0x000e0000u) != 0x07f1e0ff ||
        carry == 7 || w[3][0] != (0xff007224u | (r + 1) << 16) || w[3][1] != 255 ||
        w[3][2] != (0x000e0400u | (r + 1) | carry << 23) || w[4][0] != (0x7357u | r << 24) || w[4][1] || w[4][2])
        return 0;
    u64 target = (u64)at + w[1][1];
    return target > at + 80 && !(target & 15) && target <= SHADER_AMPERE_NATIVE_MAX - 48 ? (u32)target : 0;
}
static int exit_hook_body(const u8* code, u32 bytes, u32 hook) {
    return hook && bytes >= 48 && hook <= bytes - 48 && (snx_ld_le32(code + hook) & 0x00ffffffu) == 0xff7324 &&
           snx_ld_le32(code + hook + 4) == 255 && !snx_ld_le32(code + hook + 8) &&
           snx_ld_le32(code + hook + 16) == 0x7957 && !snx_ld_le32(code + hook + 20) &&
           !snx_ld_le32(code + hook + 24) && snx_ld_le32(code + hook + 32) == 0x794d &&
           !snx_ld_le32(code + hook + 36) && snx_ld_le32(code + hook + 40) == 0x03800000;
}
/* The LEPC of the main body's verified exit hook, or ~0u for none */
static u32 exit_hook_at(const u8* code, u32 bytes, u32 main_end) {
    for (u32 at = 0; at < main_end; at += 16)
        if ((snx_ld_le32(code + at) & 0xfff) == 0x34e)
            return exit_hook_body(code, bytes, exit_hook_prologue(code, main_end, at)) ? at : ~0u;
    return ~0u;
}

static i64 branch_target(const u8* code, u32 at);

/* hook: the LEPC that starts the program's verified exit hook (exit_hook_at), or ~0u */
static u32 lower_program(const u8* code, u32 bytes, u8* out, u32 capacity, ShaderInfo* si, u32* positions,
                         const NativeFunction* functions, u32 main_end, int up_live, u32 gpr_limit, u32 hook) {
    memset(out, 0, 80); /* the SPIR-V frontend's synthetic native header */
    snx_st_le32(out, 0x81); /* SPH version 4 identifies native-only adapters, including compute's warp barrier */
    Lower l = {.out = out, .size = 80, .capacity = capacity};
    if (gpr_limit != DEFAULT_GPR_LIMIT)
        for (u32 at = 0; at < bytes; at += 16) {
            if (functions && !(functions[at / 16].flags & NATIVE_LIVE)) continue;
            u32 op = snx_ld_le32(code + at) & 0xfff;
            if (op == 0x23c || op == 0x23a || op == 0x83b)
                return reject(si, at, op, "matrix scratch overlaps the wider vector bank");
        }
    u8 tokens[16];
    u32 depth = 0;
    for (u32 at = 0; at < bytes; at += 16)
        if ((!functions || (functions[at / 16].flags & NATIVE_LIVE)) &&
            uniform_predicates(snx_ld_le32(code + at), snx_ld_le32(code + at + 8))) {
            up_live = 1;
            break;
        }
    if (up_live) {
        /* Initialize only the lowerer's packed state, not the game's vector registers or predicates. */
        mov_imm(&l, 7ull << 16, UP_MASK, 0);
    }
    /* One HMMA subroutine for every HMMA of the program, at its start, which a BRA skips: each HMMA would otherwise
       expand to about 200 instructions where the game's upscaler has 240 */
    for (u32 at = 0; at < bytes; at += 16)
        if ((!functions || (functions[at / 16].flags & NATIVE_LIVE)) && (snx_ld_le32(code + at) & 0xfff) == 0x23c) {
            u32 skip = next_pc(&l);
            emit(&l, 0xe24000000007000full);
            l.hmma = next_pc(&l);
            emit_hmma(&l);
            if (!l.full)
                snx_st_le64(l.out + skip,
                            snx_ld_le64(l.out + skip) | (u64)((next_pc(&l) - skip - 8) & 0xffffff) << 20);
            break;
        }
    for (u32 at = 0; at < bytes; at += 16) {
        if (functions && !(functions[at / 16].flags & NATIVE_LIVE)) continue;
        if (functions && functions[at / 16].end) depth = 0;
        u32 w0 = snx_ld_le32(code + at), w1 = snx_ld_le32(code + at + 4);
        u32 w2 = snx_ld_le32(code + at + 8), control = snx_ld_le32(code + at + 12);
        u32 op = w0 & 0xfff, guard = w0 >> 12 & 15, d = w0 >> 16 & 255, a = w0 >> 24;
        u64 pred = (u64)guard << 16;
        if (control & 0x1ff) return reject(si, at, op, "reserved control bits");
        if (positions) positions[at / 16] = l.size + (l.instructions % 3 ? 0 : 8);
        u32 up = uniform_predicates(w0, w2);
        if (up & UP_SWAP) {
            emit(&l, 0x38e8000000070000ull | UP_SAVE | (u64)255 << 8 | (u64)0x7f << 20);
            emit(&l, 0x38f0000000070000ull | (u64)UP_MASK << 8 | (u64)0x7f << 20);
        }
        switch (op) {
            case 0x918: /* NOP */
                if ((w0 >> 16) || w1 || w2) return reject(si, at, op, "NOP form");
                emit(&l, 0x50b0000000000000ull | pred);
                break;
            case 0x91a: /* DEPBAR for ordinary register producers, not asynchronous-copy groups */
                if (w0 != 0x791a || (w1 & ~0xfc0u) != 0xd000 || w2)
                    return reject(si, at, op, "dependency scoreboard mode");
                /* SPIR-V register producers are synchronous. Async-copy opcodes are refused, so no native group
                   remains outstanding. This is a dependency wait, not a memory fence or a completion release. */
                emit(&l, 0xf0f0000000070000ull);
                break;
            case 0x34e: /* LEPC that starts a verified exit hook: its address has no reader but the hook */
                if (at != hook) return reject(si, at, op, "program counter load outside an exit hook");
                mov_imm(&l, pred, d, 0);
                mov_imm(&l, pred, d + 1, 0);
                break;
            case 0x357: /* BMOV.64 ATEXIT_PC of the exit hook (exit_hook_prologue): nothing to do in the lowering */
                if (hook == ~0u || at != hook + 64) return reject(si, at, op, "exit hook form");
                break;
            case 0x919:   /* S2R, SR_INVOCATION_INFO (29) with Maxwell's number */
            case 0x9c3: { /* S2UR */
                u32 special = w2 >> 8 & 255;
                if (a || w1 || w2 != special << 8 ||
                    !((op == 0x919 && (special == 0 || special == 29 || (special >= 56 && special <= 60))) ||
                      (special >= 33 && special <= 35) || (special >= 37 && special <= 39)))
                    return reject(si, at, op, "special register");
                u32 target = op == 0x9c3 ? uniform_reg(d, gpr_limit) : d;
                if (op == 0x9c3 ? !target || guard != 7 : !gpr(d, gpr_limit))
                    return reject(si, at, op, "special register destination");
                emit(&l, 0xf0c8000000000000ull | pred | target | (u64)special << 20);
                break;
            }
            case 0x202: /* MOV register */
                if (!gpr(d, gpr_limit) || a || !gpr(w1, gpr_limit) || w2 != 0xf00)
                    return reject(si, at, op, "register move form");
                emit(&l, 0x5c98078000000000ull | pred | d | (u64)w1 << 20);
                break;
            case 0x802: /* MOV immediate */
                if (!gpr(d, gpr_limit) || a || w2 != 0xf00) return reject(si, at, op, "immediate move form");
                mov_imm(&l, pred, d, w1);
                break;
            case 0xa02:
            case 0x624:   /* MOV cbuf, or low-word IMAD with a cbuf addend */
            case 0xa24: { /* or with a cbuf multiplier. The signed flag (bit 73) can't change a product's low word */
                u32 bank = w1 >> 22 & 31, offset = w1 >> 6 & 0xffff, b = w2 & 255;
                /* IMAD.X (bit 74) adds the carry of the predicate in 87..90: the high word of a 64-bit address sum */
                int carry_in = op == 0x624 && (w2 & ~0x07800200u) == (0x000e0400u | b);
                if (!gpr(d, gpr_limit) || (bank >= 18 && (bank < 24 || offset >= 256)) || (offset & 3) || (w1 & 0xf800003fu) ||
                    (op == 0xa02 ? a || w2 != 0xf00
                                 : !gpr(a, gpr_limit) || !gpr(b, gpr_limit) || (!carry_in && (w2 & ~0x200u) != (0x078e0000u | b))))
                    return reject(si, at, op, "constant move/multiply form");
                u32 target = op == 0xa02 ? d : TEMP_C;
                emit(&l, 0x4c98078000000000ull | pred | target | (u64)(offset / 4) << 20 | (u64)bank << 34);
                if (carry_in) { /* Ra * Rb + c + carry */
                    mov_imm(&l, pred, TEMP_A, 1);
                    emit(&l, 0x5ca0000000000000ull | pred | TEMP_A | (u64)TEMP_A << 8 | (u64)255 << 20 |
                                 (u64)(w2 >> 23 & 15) << 39);
                    emit(&l, 0x5c38000000000000ull | pred | TEMP_B | (u64)a << 8 | (u64)b << 20);
                    add(&l, pred, TEMP_B, TEMP_B, TEMP_C, 0);
                    add(&l, pred, d, TEMP_B, TEMP_A, 0);
                } else if (op == 0x624) { /* Ra * Rb + c */
                    emit(&l, 0x5c38000000000000ull | pred | TEMP_A | (u64)a << 8 | (u64)b << 20);
                    add(&l, pred, d, TEMP_A, TEMP_C, 0);
                } else if (op == 0xa24) { /* Ra * c + Rb */
                    emit(&l, 0x5c38000000000000ull | pred | TEMP_A | (u64)a << 8 | (u64)TEMP_C << 20);
                    add(&l, pred, d, TEMP_A, b, 0);
                }
                break;
            }
            case 0x805: /* CS2R.64 SRZ */
                if (a || w1 || w2 != 0x1ff00 || (d & 1) || d + 1 >= gpr_limit)
                    return reject(si, at, op, "special zero pair form");
                mov_imm(&l, pred, d, 0);
                mov_imm(&l, pred, d + 1, 0);
                break;
            case 0x806: { /* VOTE: ballot and an independent All/Any/Eq predicate */
                u32 mode = w2 >> 8 & 3;
                if (!gpr(d, gpr_limit) || a || w1 || mode > 2 || (w2 & ~0x078e0300u))
                    return reject(si, at, op, "vote form");
                emit(&l, 0x50d8000000000000ull | pred | d | (u64)(w2 >> 23 & 15) << 39 |
                             (u64)(w2 >> 17 & 7) << 45 | (u64)mode << 48);
                break;
            }
            case 0x886: { /* VOTEU uses the independent uniform-predicate bank. */
                u32 target = uniform_reg(d, gpr_limit), mode = w2 >> 8 & 3, qualifier = w2 >> 23 & 15;
                if (!target || a || w1 || mode > 2 || (w2 & ~0x078e0300u))
                    return reject(si, at, op, "uniform vote predicate form");
                emit(&l, 0x50d8000000000000ull | pred | target | (u64)qualifier << 39 |
                             (u64)(w2 >> 17 & 7) << 45 | (u64)mode << 48);
                break;
            }
            case 0x2bf: { /* UPOPC: the population count of a uniform register, into a uniform register */
                u32 target = uniform_reg(d, gpr_limit), source = uniform_reg(w1, gpr_limit);
                if (!target || target == 255 || !source || a || guard != 7 || w2 != 0x08000000)
                    return reject(si, at, op, "uniform population count form");
                emit(&l, 0x5c08000000000000ull | pred | target | (u64)source << 20);
                break;
            }
            case 0x2be:
            case 0x2bd: { /* UBREV / UFLO.U32, including its normalization-shift result */
                u32 target = uniform_reg(d, gpr_limit), source = uniform_reg(w1, gpr_limit);
                if (guard != 7 || !target || target == 255 || !source || a ||
                    (op == 0x2be ? w2 != 0x08000000 : (w2 & ~0x400u) != 0x080e0000))
                    return reject(si, at, op, "uniform bit reverse/leading-bit form");
                if (op == 0x2be)
                    emit(&l, 0x3828000000000000ull | pred | target | (u64)source << 8 |
                                 (u64)1 << 39 | (u64)1 << 40);
                else
                    emit(&l, 0x5c30000000000000ull | pred | target | (u64)source << 20 |
                                 (u64)(w2 >> 10 & 1) << 41);
                break;
            }
            case 0xc02: { /* MOV from a uniform register */
                u32 source = uniform_reg(w1, gpr_limit);
                if (!gpr(d, gpr_limit) || a || !source || w2 != 0x08000f00)
                    return reject(si, at, op, "uniform-to-register move form");
                emit(&l, 0x5c98078000000000ull | pred | d | (u64)source << 20);
                break;
            }
            case 0x3c2: { /* R2UR broadcasts the first active lane, not each lane's own value. */
                u32 target = uniform_reg(d, gpr_limit);
                if (!target || target == 255 || !gpr(a, gpr_limit) || w1 || w2 != 0x000e0000)
                    return reject(si, at, op, "register-to-uniform broadcast form");
                emit(&l, 0x38e8000000070000ull | TEMP_C | (u64)255 << 8 | (u64)0x40 << 20);
                emit(&l, 0x50d8000000070000ull | TEMP_A | (u64)guard << 39 | (u64)6 << 45 | (u64)1 << 48);
                emit(&l, 0x5c10000000070000ull | TEMP_B | (u64)TEMP_A << 8 | (u64)255 << 20 | (u64)1 << 49);
                emit(&l, 0x5c40000000070000ull | TEMP_A | (u64)TEMP_A << 8 | (u64)TEMP_B << 20 | (u64)7 << 48);
                emit(&l, 0x5c30000000070000ull | TEMP_A | (u64)TEMP_A << 20);
                emit(&l, 0xef10000000070000ull | TEMP_B | (u64)a << 8 | (u64)TEMP_A << 20 |
                             (u64)1 << 29 | (u64)31 << 34 | (u64)7 << 48);
                emit(&l, 0x5ca0000000070000ull | target | (u64)TEMP_B << 8 | (u64)target << 20 | (u64)6 << 39);
                emit(&l, 0x38f0000000070000ull | (u64)TEMP_C << 8 | (u64)0x40 << 20);
                break;
            }
            case 0x3c4: { /* REDUX.SUM or .MAX.U32 over the active warp, as PokeDinner lowers it. An inactive lane's
                             shuffle value is undefined, so mask it to the reduction identity with the active ballot. */
                u32 target = uniform_reg(d, gpr_limit);
                if (guard != 7 || !target || target == 255 || !gpr(a, gpr_limit) || w1 || (w2 != 0xc000 && w2 != 0x14000))
                    return reject(si, at, op, "active warp reduction form");
                emit(&l, 0x50d8000000070000ull | TEMP_A | 7ull << 39 | 7ull << 45 | 1ull << 48);
                mov_imm(&l, pred, target, 0);
                for (u32 lane = 0; lane < 32; ++lane) {
                    emit(&l, 0xef10000000070000ull | TEMP_B | (u64)a << 8 | (u64)lane << 20 |
                                 1ull << 28 | 1ull << 29 | 31ull << 34 | 7ull << 48);
                    shift_imm(&l, TEMP_C, TEMP_A, lane, 0);
                    lop_imm(&l, TEMP_C, TEMP_C, 1);
                    add(&l, pred, TEMP_C, 255, TEMP_C, 1ull << 48);
                    emit(&l, 0x5c40000000070000ull | TEMP_B | (u64)TEMP_B << 8 | (u64)TEMP_C << 20 | 7ull << 48);
                    if (w2 == 0x14000)
                        emit(&l, 0x5c20000000070000ull | target | (u64)target << 8 | (u64)TEMP_B << 20 | 15ull << 39);
                    else
                        add(&l, pred, target, target, TEMP_B, 0);
                }
                break;
            }
            case 0x23c: { /* HMMA.16816.F16 (75), unguarded: the operands go to the subroutine's registers (emit_hmma) */
                u32 b = w1 & 255, c = w2 & 255, S = MATRIX_FIRST;
                if (guard != 7 || !l.hmma || (w1 & ~255u) || (w2 & ~255u) != 0x800 || (d & 1) || d + 2 > gpr_limit ||
                    (a & 3) || a + 4 > gpr_limit || (b & 1) || b + 2 > gpr_limit ||
                    (c != 255 && ((c & 1) || c + 2 > gpr_limit)))
                    return reject(si, at, op, "half matrix multiply form");
                for (u32 i = 0; i < 4; ++i) move(&l, S + i, a + i);
                for (u32 i = 0; i < 2; ++i) move(&l, S + 4 + i, b + i);
                for (u32 i = 0; i < 2; ++i) move(&l, S + 6 + i, c == 255 ? 255 : c + i);
                u32 pc = next_pc(&l);
                emit(&l, 0xe26000000007000full | (u64)((l.hmma - pc - 8) & 0xffffff) << 20); /* CAL */
                for (u32 i = 0; i < 2; ++i) move(&l, d + i, S + i);
                break;
            }
            case 0x23a: /* MOVM.16.MT88, unguarded */
                if (guard != 7 || w1 || w2 || d >= gpr_limit || a >= gpr_limit)
                    return reject(si, at, op, "matrix transpose form");
                transpose_matrix(&l, d, a);
                break;
            case 0x83b: { /* LDSM.16.M88 of 1, 2 or 4 matrices (72..73), .T (78), unguarded, without a uniform offset.
                             Lanes 8m..8m + 7 hold the addresses of matrix m's rows: lane t loads the 32 bits at byte
                             4(t % 4) of row t / 4, from lane 8m + t / 4's address. As eden-ns2 lowers it */
                u32 shape = w2 >> 8 & 3, count = 1u << shape, offset = w1 >> 8, S = MATRIX_FIRST;
                if (guard != 7 || shape == 3 || (w1 & 255) || (w2 & ~0x4300u) || a >= gpr_limit || d >= gpr_limit ||
                    (d & (count - 1)) || d + count > gpr_limit)
                    return reject(si, at, op, "matrix load form");
                lane_id(&l, TEMP_A);
                shift_imm(&l, TEMP_B, TEMP_A, 2, 0);
                lop_imm(&l, TEMP_C, TEMP_A, 3);
                shift_imm(&l, TEMP_C, TEMP_C, 2, 1);
                for (u32 m = 0; m < count; ++m) { /* LDS.32 [lane 8m + t / 4's address + 4(t % 4) + offset] */
                    add_imm(&l, 251, TEMP_B, 8 * m);
                    shuffle(&l, 251, a, 251);
                    add(&l, 7ull << 16, 251, 251, TEMP_C, 0);
                    emit(&l, 0xef48000000070000ull | (S + m) | 251ull << 8 | (u64)offset << 20 | 4ull << 48);
                }
                for (u32 m = 0; (w2 >> 14 & 1) && m < count; ++m) transpose_matrix(&l, S + m, S + m);
                for (u32 m = 0; m < count; ++m) move(&l, d + m, S + m);
                break;
            }
            case 0x234:
            case 0x434: { /* HSETP2 of packed registers or an immediate pair. Maxwell keeps the source selectors,
                             predicates, compare and combination fields. */
                u32 b = op == 0x434 ? TEMP_C : w1 & 255, combine = w2 >> 5 & 3;
                if (d || !gpr(a, gpr_limit) || (w2 & 0x0800001fu) || combine > 2 ||
                    (op == 0x234 && (!gpr(b, gpr_limit) || (w1 & 0x0fffff00u))) ||
                    (w2 >> 10 & 3) == 1 || (op == 0x234 && (w1 >> 28 & 3) == 1))
                    return reject(si, at, op, "packed half predicate form");
                u32 flags = op == 0x434 ? 0 : w1 >> 28;
                if (op == 0x434) mov_imm(&l, pred, b, w1);
                emit(&l, 0x5d20000000000000ull | pred | (w2 >> 20 & 7) | (u64)(w2 >> 17 & 7) << 3 |
                             (u64)(w2 >> 16 & 1) << 6 | (u64)a << 8 | (u64)b << 20 | (u64)(flags & 3) << 28 |
                             (u64)(flags >> 2 & 1) << 30 | (u64)(flags >> 3) << 31 | (u64)(w2 >> 12 & 15) << 35 |
                             (u64)(w2 >> 23 & 7) << 39 | (u64)(w2 >> 26 & 1) << 42 | (u64)(w2 >> 8 & 1) << 43 |
                             (u64)(w2 >> 9 & 1) << 44 | (u64)combine << 45 | (u64)(w2 >> 10 & 3) << 47 |
                             (u64)(w2 >> 7 & 1) << 49);
                break;
            }
            case 0x816: /* PRMT with an immediate selector in its index mode: Maxwell's PRMT, the bytes of C:A */
                if (!gpr(d, gpr_limit) || !gpr(a, gpr_limit) || !gpr(w2 & 255, gpr_limit) || w1 > 0xffff || (w2 & ~255u))
                    return reject(si, at, op, "permutation form");
                emit(&l, 0x36c0000000000000ull | pred | d | (u64)a << 8 | (u64)w1 << 20 | (u64)(w2 & 255) << 39);
                break;
            case 0x240:
            case 0x840: { /* HMNMX2 of packed halves (B a register with 60..63, or an immediate pair), the minimum when
                             87..90 hold: each half in F32 through FMNMX, exact for a minimum or maximum. A's swizzle
                             74..75, NEG 72, ABS 73. Only the packed result is guarded */
                u32 b = op == 0x240 ? w1 & 255 : TEMP_C, b_mode = op == 0x240 ? w1 >> 28 : 0;
                if (!gpr(d, gpr_limit) || !gpr(a, gpr_limit) || (op == 0x240 && (!gpr(b, gpr_limit) || (w1 & 0x0fffff00u))) ||
                    (w2 & ~0x07800f00u) || (w2 >> 10 & 3) == 1 || (b_mode & 3) == 1)
                    return reject(si, at, op, "half minimum/maximum form");
                if (op == 0x840) mov_imm(&l, 7ull << 16, TEMP_C, w1);
                for (u32 lane = 0; lane < 2; ++lane) {
                    u32 swizzles[2] = {w2 >> 10 & 3, b_mode & 3}, sources[2] = {a, b};
                    u32 negate[2] = {w2 >> 8 & 1, b_mode >> 3 & 1}, absolute[2] = {w2 >> 9 & 1, b_mode >> 2 & 1};
                    for (u32 k = 0; k < 2; ++k) /* F2F.F32.F16: H1 when swizzle 3, or 0 (both) for the high lane */
                        emit(&l, 0x5ca8000000070000ull | (TEMP_A + k) | 2ull << 8 | 1ull << 10 | (u64)sources[k] << 20 |
                                     (u64)(swizzles[k] == 3 || (swizzles[k] == 0 && lane)) << 41 |
                                     (u64)negate[k] << 45 | (u64)absolute[k] << 49);
                    emit(&l, 0x5c60000000070000ull | (251 + lane) | (u64)TEMP_A << 8 | (u64)TEMP_B << 20 |
                                 (u64)(w2 >> 23 & 15) << 39);
                    float_to_half(&l, 251 + lane);
                }
                emit(&l, 0x5c18000000000000ull | pred | d | 252ull << 8 | 251ull << 20 | 16ull << 39);
                break;
            }
            case 0x3a1: { /* MATCH.ALL.U64 (73) with only its predicate (81..83): whether each active lane holds the
                             first active lane's pair. As PokeDinner and Eglantine lower it, with P6 saved around it */
                u32 target = w2 >> 17 & 7;
                if (guard != 7 || d != 255 || a >= gpr_limit - 1 || (a & 1) || w1 || (w2 & ~0x000e0000u) != 0x200 ||
                    target >= 6)
                    return reject(si, at, op, "warp match form");
                emit(&l, 0x38e8000000070000ull | TEMP_C | (u64)255 << 8 | (u64)0x40 << 20);
                emit(&l, 0x50d8000000070000ull | TEMP_A | (u64)7 << 39 | (u64)7 << 45 | (u64)1 << 48);
                emit(&l, 0x5c10000000070000ull | TEMP_B | (u64)TEMP_A << 8 | (u64)255 << 20 | (u64)1 << 49);
                emit(&l, 0x5c40000000070000ull | TEMP_A | (u64)TEMP_A << 8 | (u64)TEMP_B << 20 | (u64)7 << 48);
                emit(&l, 0x5c30000000070000ull | TEMP_A | (u64)TEMP_A << 20);
                for (u32 word = 0; word < 2; ++word) { /* P6 = the low word matches, then the high word too */
                    emit(&l, 0xef10000000070000ull | TEMP_B | (u64)(a + word) << 8 | (u64)TEMP_A << 20 |
                                 (u64)1 << 29 | (u64)31 << 34 | (u64)7 << 48);
                    emit(&l, 0x5b60000000070000ull | 7 | (u64)6 << 3 | (u64)(a + word) << 8 | (u64)TEMP_B << 20 |
                                 (u64)(word ? 6 : 7) << 39 | (u64)2 << 49);
                }
                emit(&l, 0x50d8000000070000ull | 255 | (u64)6 << 39 | (u64)target << 45);
                emit(&l, 0x38f0000000070000ull | (u64)TEMP_C << 8 | (u64)0x40 << 20);
                break;
            }
            case 0x882: { /* UMOV immediate, under a uniform guard swapped into P (uniform_predicates) */
                u32 target = uniform_reg(d, gpr_limit);
                if (a || !target || target == 255 || w2)
                    return reject(si, at, op, "uniform immediate move form");
                mov_imm(&l, pred, target, w1);
                break;
            }
            case 0xc82: { /* UMOV, no uniform predicate */
                u32 target = uniform_reg(d, gpr_limit), source = uniform_reg(w1, gpr_limit);
                if (guard != 7 || a || !target || target == 255 || !source || w2 != 0x08000000)
                    return reject(si, at, op, "uniform move form");
                emit(&l, 0x5c98078000000000ull | pred | target | (u64)source << 20);
                break;
            }
            case 0x424: { /* IMAD with an immediate addend (ALU form 2: the multiplier in 64..71), RZ * RZ as the move.
                             No product negation (75); the signedness (73) can't change the low word */
                u32 b = w2 & 255;
                if (!gpr(d, gpr_limit) || !gpr(a, gpr_limit) || !gpr(b, gpr_limit) || (w2 & ~0x2ffu) != 0x078e0000)
                    return reject(si, at, op, "integer move form");
                if (a == 255 || b == 255) {
                    mov_imm(&l, pred, d, w1);
                    break;
                }
                emit(&l, 0x5c38000000000000ull | pred | TEMP_A | (u64)a << 8 | (u64)b << 20);
                mov_imm(&l, pred, TEMP_C, w1);
                add(&l, pred, d, TEMP_A, TEMP_C, 0);
                break;
            }
            case 0x224:
            case 0xc24:
            case 0x2a4: { /* IMAD/UIMAD low word, register or uniform multiplier */
                u32 b = w1 & 255, c = w2 & 255, target = d, x = a;
                int uniform = op == 0x2a4;
                if (uniform) {
                    target = uniform_reg(d, gpr_limit);
                    x = uniform_reg(a, gpr_limit);
                    b = uniform_reg(b, gpr_limit);
                    c = uniform_reg(c, gpr_limit);
                    if (guard != 7 || !target || target == 255 || !x || !b || !c)
                        return reject(si, at, op, "uniform integer multiply registers");
                } else if (!gpr(d, gpr_limit) || !gpr(a, gpr_limit) || !gpr(c, gpr_limit) || (op != 0xc24 && !gpr(b, gpr_limit)))
                    return reject(si, at, op, "integer multiply registers");
                if (op == 0xc24) {
                    b = uniform_reg(b, gpr_limit);
                    if (!b) return reject(si, at, op, "uniform integer multiplier");
                }
                /* IMAD.X (bit 74) adds the carry of the predicate in 87..90, as 0x624's: the high word of a 64-bit sum */
                int carry_in = op == 0x224 && !(w1 & ~255u) && (w2 & ~0x07800200u) == (0x000e0400u | c);
                if (!carry_in && ((w1 & ~255u) || (w2 & ~0xaffu) != (op == 0x224 ? 0x078e0000u : 0x0f8e0000u)))
                    return reject(si, at, op, "integer register multiply form");
                if (carry_in) {
                    mov_imm(&l, pred, TEMP_A, 1);
                    emit(&l, 0x5ca0000000000000ull | pred | TEMP_A | (u64)TEMP_A << 8 | (u64)255 << 20 |
                                 (u64)(w2 >> 23 & 15) << 39);
                    emit(&l, 0x5c38000000000000ull | pred | TEMP_B | (u64)x << 8 | (u64)b << 20);
                    add(&l, pred, TEMP_B, TEMP_B, c, 0);
                    add(&l, pred, target, TEMP_B, TEMP_A, 0);
                    break;
                }
                emit(&l, 0x5c38000000000000ull | pred | TEMP_A | (u64)x << 8 | (u64)b << 20);
                add(&l, pred, target, TEMP_A, c, (u64)(w2 >> 11 & 1) << 48);
                break;
            }
            case 0xe24: { /* IMAD low word, uniform addend (ALU form 7: the UR in 32..39, its negation in 63, the
                             multiplier in 64..71), without product negation (75) */
                u32 b = w2 & 255, c = uniform_reg(w1 & 255, gpr_limit);
                if (!gpr(d, gpr_limit) || !gpr(a, gpr_limit) || !gpr(b, gpr_limit) || !c || (w1 & ~0x800000ffu) || (w2 & ~0x2ffu) != 0x0f8e0000)
                    return reject(si, at, op, "integer multiply uniform addend form");
                emit(&l, 0x5c38000000000000ull | pred | TEMP_A | (u64)a << 8 | (u64)b << 20);
                add(&l, pred, d, TEMP_A, c, (u64)(w1 >> 31) << 48);
                break;
            }
            case 0x824: { /* IMAD low word, immediate multiplier */
                u32 c = w2 & 255;
                if (!gpr(d, gpr_limit) || !gpr(a, gpr_limit) || !gpr(c, gpr_limit) || (w2 & ~0xaffu) != 0x078e0000)
                    return reject(si, at, op, "integer multiply form");
                emit(&l, 0x1f00000000000000ull | pred | TEMP_A | (u64)a << 8 | (u64)w1 << 20);
                add(&l, pred, d, TEMP_A, c, (u64)(w2 >> 11 & 1) << 48);
                break;
            }
            case 0x8a4: { /* UIMAD low word, immediate multiplier */
                u32 target = uniform_reg(d, gpr_limit), x = uniform_reg(a, gpr_limit), c = uniform_reg(w2 & 255, gpr_limit);
                if (guard != 7 || !target || target == 255 || !x || !c || (w2 & ~0xaffu) != 0x0f8e0000)
                    return reject(si, at, op, "uniform integer multiply form");
                emit(&l, 0x1f00000000000000ull | pred | TEMP_A | (u64)x << 8 | (u64)w1 << 20);
                add(&l, pred, target, TEMP_A, c, (u64)(w2 >> 11 & 1) << 48);
                break;
            }
            case 0x822: { /* FSWZADD without NDV, rounding or FTZ (77..80). Each quad lane's two bits of the swizzle (32..39,
                             lane 0 the highest) are the same on Maxwell, but codes 1 and 2 swap: SM70's 1 is a - b and
                             2 b - a (NAK's SUB and SUBR), Maxwell's 1 b - a and 2 a - b. As PokeDinner maps them */
                u32 b = w2 & 255;
                if (!gpr(d, gpr_limit) || !gpr(a, gpr_limit) || !gpr(b, gpr_limit) || w1 > 255 || w2 > 255) return reject(si, at, op, "swizzled add form");
                u32 maxwell = (w1 & 0x55) << 1 | (w1 & 0xaa) >> 1;
                emit(&l, 0x50f8000000000000ull | pred | d | (u64)a << 8 | (u64)b << 20 | (u64)maxwell << 28);
                break;
            }
            case 0x213:
            case 0xa13: { /* IABS: max(x, 0 - x), signed, including INT_MIN's wrapping result. */
                u32 source = w1 & 255;
                if (!gpr(d, gpr_limit) || a || w2) return reject(si, at, op, "integer absolute form");
                if (op == 0xa13) {
                    if (!constant(&l, pred, TEMP_B, w1)) return reject(si, at, op, "integer absolute constant");
                    source = TEMP_B;
                } else if (!gpr(source, gpr_limit) || (w1 & ~255u))
                    return reject(si, at, op, "integer absolute source");
                add(&l, pred, TEMP_A, 255, source, 1ull << 48);
                emit(&l, 0x5c20000000000000ull | pred | d | (u64)source << 8 | (u64)TEMP_A << 20 | 15ull << 39 |
                             1ull << 48);
                break;
            }
            case 0xabb: { /* Indexed ULDC, with a uniform byte offset and a distinct uniform destination. */
                u32 width = w2 >> 9 & 7, bank = w1 >> 22 & 31, offset = w1 >> 6 & 0xffff;
                u32 target = uniform_reg(d, gpr_limit), index = uniform_reg(a, gpr_limit);
                if (!target || target == 255 || !index || bank >= 18 || (w1 & 0xf800003fu) ||
                    (width != 4 && width != 5) || w2 != (0x08000000u | width << 9) ||
                    (width == 5 && ((d & 1) || d + 1 >= uniform_count(gpr_limit))))
                    return reject(si, at, op, "indexed uniform constant load form");
                emit(&l, 0xef90000000000000ull | pred | target | (u64)index << 8 | (u64)offset << 20 |
                             (u64)bank << 36 | (u64)width << 48);
                break;
            }
            case 0xab9: { /* ULDC from a direct constant-buffer address */
                u32 width = w2 >> 9 & 7, bank = w1 >> 22 & 31, offset = w1 >> 6 & 0xffff;
                u32 target = uniform_reg(d, gpr_limit), count = width == 5 ? 2 : 1;
                if (a || !target || target == 255 || (width != 4 && width != 5) ||
                    (count == 2 && ((d & 1) || d + 1 >= uniform_count(gpr_limit))) || (offset & 3) ||
                    offset > 0xffffu - (count - 1) * 4 || (w1 & 0xf800003fu) || w2 != width << 9)
                    return reject(si, at, op, "uniform constant load form");
                for (u32 i = 0; i < count; ++i)
                    if (!constant(&l, pred, target + i, bank << 22 | (offset + i * 4) << 6))
                        return reject(si, at, op, "uniform constant load bank or offset");
                break;
            }
            case 0xb82: { /* Indexed LDC of 8 to 64 bits (Maxwell's sizes), with a byte-addressed register offset */
                u32 width = w2 >> 9 & 7, bank = w1 >> 22 & 31, offset = w1 >> 6 & 0xffff;
                if (!gpr(a, gpr_limit) || bank >= 18 || (w1 & 0xf800003fu) || width > 5 ||
                    w2 != width << 9 || d >= gpr_limit || (width == 5 && ((d & 1) || d + 1 >= gpr_limit)))
                    return reject(si, at, op, "indexed constant load form");
                emit(&l, 0xef90000000000000ull | pred | d | (u64)a << 8 | (u64)offset << 20 |
                             (u64)bank << 36 | (u64)width << 48);
                break;
            }
            case 0xb1d: /* BAR.SYNC 0 of the whole workgroup, as PokeDinner lowers it */
                if (w1 || w2 || guard != 7) return reject(si, at, op, "workgroup barrier form");
                emit(&l, 0xf0a8000000000000ull | pred | 1ull << 43 | 1ull << 44 | 7ull << 39);
                break;
            case 0x948: /* WARPSYNC of the complete native warp. Partial masks and guarded barriers stay refused. */
                if ((w0 >> 12) != 7 || w1 != 0xffffffffu || w2 != 0x03800000)
                    return reject(si, at, op, "warp barrier mask or guard");
                emit(&l, SH_AMPERE_WARPSYNC);
                break;
            case 0x59f: { /* SURED.D.2D.ADD.STRONG.GPU.IGN, U32, through a direct descriptor. The encoding is
                             confirmed by nvdisasm SM86. Maxwell's SURED.D uses the same texel coordinates. */
                u32 data = w1 & 255, bank = w1 >> 22 & 31, offset = w1 >> 8 & 0x3fff;
                if (d || a + 1 >= gpr_limit || !gpr(data, gpr_limit) || (w1 & 0xf8000000u) != 0x60000000u ||
                    bank >= 18 || offset > 0x1fff || w2 != 0x0010e000)
                    return reject(si, at, op, "surface reduction form");
                emit(&l, 0x4c98078000070000ull | TEMP_C | (u64)offset << 20 | (u64)bank << 34);
                emit(&l, 0xef98000000000100ull | pred);
                emit(&l, 0xeb50000000000000ull | pred | data | (u64)a << 8 | 3ull << 33 | (u64)TEMP_C << 39);
                emit(&l, 0xef98000000000100ull | pred);
                break;
            }
            case 0x992: { /* MEMBAR.SC: CTA, GPU, system or virtual-channel scope. The host uses Device for the
                             latter three, which is conservative for this single-device runtime. */
                u32 scope = w2 >> 12 & 7;
                if ((w0 >> 16) || w1 || w2 != scope << 12 || (scope != 0 && scope != 2 && scope != 3 && scope != 5))
                    return reject(si, at, op, "memory barrier scope");
                emit(&l, 0xef98000000000000ull | pred | (u64)(scope == 0 ? 0 : scope == 2 ? 1 : scope == 3 ? 2 : 3) << 8);
                break;
            }
            case 0x9ab: /* ERRBAR: order prior memory operations, not a workgroup execution barrier. */
                if ((w0 >> 16) || w1 || w2) return reject(si, at, op, "error barrier form");
                emit(&l, 0xef98000000000100ull | pred);
                break;
            case 0x38c: { /* ATOMS.ADD.U32: Maxwell's shared atomic uses a signed word displacement. */
                u32 data = w1 & 255;
                if (!gpr(d, gpr_limit) || !gpr(a, gpr_limit) || !gpr(data, gpr_limit) || w2 || (w1 & 0x300u))
                    return reject(si, at, op, "shared atomic add form");
                emit(&l, 0xec00000000000000ull | pred | d | (u64)a << 8 | (u64)data << 20 | (u64)(w1 >> 10) << 30);
                break;
            }
            case 0x989: { /* SHFL with an immediate lane (53..57) and a register clamp/segment mask (64..71) */
                u32 lane = w1 >> 21 & 31, mode = w1 >> 26 & 3, mask = w2 & 255;
                if (!gpr(d, gpr_limit) || !gpr(a, gpr_limit) || !gpr(mask, gpr_limit) || (w1 & 0xf01fffffu) || (w2 & ~0x000e00ffu))
                    return reject(si, at, op, "shuffle register mask form");
                emit(&l, 0xef10000000000000ull | pred | d | (u64)a << 8 | (u64)lane << 20 | 1ull << 28 |
                             (u64)mode << 30 | (u64)mask << 39 | (u64)(w2 >> 17 & 7) << 48);
                break;
            }
            case 0x589:
            case 0xf89: { /* SHFL with a register/immediate lane selector and an immediate segmentation mask */
                u32 lane = op == 0xf89 ? w1 >> 21 & 31 : w1 & 255, mask = w1 >> 8 & 0x1fff;
                u32 mode = w1 >> 26 & 3;
                if (!gpr(d, gpr_limit) || !gpr(a, gpr_limit) || (op == 0x589 && !gpr(lane, gpr_limit)) ||
                    (w1 & ~(op == 0xf89 ? 0x0fffff00u : 0x0c1fffffu)) || (mask & 0xe0) ||
                    (w2 & ~0x000e0000u))
                    return reject(si, at, op, "shuffle lane/mask form");
                emit(&l, 0xef10000000000000ull | pred | d | (u64)a << 8 | (u64)lane << 20 |
                             (u64)(op == 0xf89) << 28 | (u64)1 << 29 | (u64)mode << 30 |
                             (u64)mask << 34 | (u64)(w2 >> 17 & 7) << 48);
                break;
            }
            case 0x819: { /* SHF with an immediate shift, 32-bit types (NAK: type 73..74, wrap 75, right 76, high 80) */
                u32 high = w2 & 255, shift = w1, type = w2 >> 9 & 3;
                int right = (w2 >> 12 & 1) != 0, upper = (w2 >> 16 & 1) != 0;
                if (!gpr(d, gpr_limit) || !gpr(a, gpr_limit) || !gpr(high, gpr_limit) || type < 2 || (w2 & ~0x116ffu) ||
                    shift > (upper && !(right && type == 2) ? 32u : 31u) || (upper && right && a != 255))
                    return reject(si, at, op, "funnel shift form");
                if (upper && right) /* the high word alone, its low word zero: an arithmetic or logical shift */
                    emit(&l, 0x3828000000000000ull | pred | d | (u64)high << 8 | (u64)shift << 20 |
                                 (u64)(type == 2) << 48);
                else if (upper) /* left, the high word: Maxwell's SHF.L */
                    emit(&l, 0x36f8000000000000ull | pred | d | (u64)a << 8 | (u64)shift << 20 | (u64)high << 39);
                else if (right) /* right, the low word: Maxwell's SHF.R, which a signed type doesn't change */
                    emit(&l, 0x38f8000000000000ull | pred | d | (u64)a << 8 | (u64)shift << 20 | (u64)high << 39);
                else /* left, the low word: the low source shifted, whatever the high one holds */
                    emit(&l, 0x3848000000000000ull | pred | d | (u64)a << 8 | (u64)shift << 20);
                break;
            }
            case 0x219:
            case 0xc19:   /* SHF with a uniform shift count, identified by nvdisasm SM87. */
            case 0x419: { /* SHF with a register shift: 219 shift 32..39 and high 64..71, 419 shift 64..71 and an
                             immediate high word 32..63. 32-bit types. */
                u32 shift = op == 0x419 ? w2 & 255 : op == 0xc19 ? uniform_reg(w1, gpr_limit) : w1;
                u32 high = op == 0x419 ? 251 : w2 & 255, type = w2 >> 9 & 3;
                int right = (w2 >> 12 & 1) != 0, upper = (w2 >> 16 & 1) != 0;
                if (!gpr(d, gpr_limit) || !gpr(a, gpr_limit) || (op == 0xc19 ? !shift : !gpr(shift, gpr_limit)) ||
                    (op != 0x419 && !gpr(high, gpr_limit)) || type < 2 ||
                    (w2 & ~0x11effu) != (op == 0xc19 ? 0x08000000u : 0) ||
                    (!right && !upper && (op == 0x419 || high != 255)) ||
                    (right && upper && a != 255))
                    return reject(si, at, op, "funnel shift form");
                if (op == 0x419) mov_imm(&l, pred, 251, w1);
                if (!right && !upper) {
                    emit(&l, 0x5c48000000000000ull | pred | d | (u64)a << 8 | (u64)shift << 20 |
                                 (u64)(w2 >> 11 & 1) << 39);
                    break;
                }
                if (upper && right) /* the high word alone: Maxwell's SHR, .W (39) for the wrap (75) */
                    emit(&l, 0x5c28000000000000ull | pred | d | (u64)high << 8 | (u64)shift << 20 |
                                 (u64)(w2 >> 11 & 1) << 39 | (u64)(type == 2) << 48);
                else /* Maxwell's SHF.L gives the high word and its SHF.R the low one, .W (50) for the wrap */
                    emit(&l, (right ? 0x5cf8000000000000ull : 0x5bf8000000000000ull) | pred | d | (u64)a << 8 |
                                 (u64)shift << 20 | (u64)high << 39 | (u64)(w2 >> 11 & 1) << 50);
                break;
            }
            case 0x20a:
            case 0x80a:
            case 0xa0a: { /* FSET.BF produces 0.0 or 1.0, with a predicate accumulator. */
                u32 x, y, combine = w2 >> 10 & 3;
                if (!gpr(d, gpr_limit) || combine > 2 || (w2 & ~0x0781ff00u) ||
                    !float_source(&l, pred, w0, w1, w2, 0, &x, gpr_limit) || !float_source(&l, pred, w0, w1, w2, 1, &y, gpr_limit))
                    return reject(si, at, op, "floating comparison result form");
                emit(&l, 0x5800000000000000ull | pred | d | (u64)x << 8 | (u64)y << 20 |
                             (u64)(w2 >> 23 & 15) << 39 | (u64)combine << 45 | (u64)(w2 >> 12 & 15) << 48 |
                             (u64)1 << 52 | (u64)(w2 >> 16 & 1) << 55);
                break;
            }
            case 0x20b:
            case 0x80b:
            case 0xa0b: { /* FSETP, with two predicate outputs and a predicate accumulator */
                u32 x, y, combine = w2 >> 10 & 3;
                if (d || (w2 & 0xf80000ffu) || combine > 2 ||
                    !float_source(&l, pred, w0, w1, w2, 0, &x, gpr_limit) || !float_source(&l, pred, w0, w1, w2, 1, &y, gpr_limit))
                    return reject(si, at, op, "floating predicate compare form");
                emit(&l, 0x5bb0000000000000ull | pred | (w2 >> 20 & 7) | (u64)(w2 >> 17 & 7) << 3 |
                             (u64)x << 8 | (u64)y << 20 | (u64)(w2 >> 23 & 15) << 39 | (u64)combine << 45 |
                             (u64)(w2 >> 16 & 1) << 47 | (u64)(w2 >> 12 & 15) << 48);
                break;
            }
            case 0x20c:
            case 0x80c:
            case 0xa0c:
            case 0xc0c:
            case 0x28c:
            case 0x88c: { /* ISETP/UISETP without an extended comparison */
                int uniform = op == 0x28c || op == 0x88c;
                int uniform_b = uniform || op == 0xc0c;
                u32 x = uniform ? uniform_reg(a, gpr_limit) : a, b = w1, combine = w2 >> 10 & 3;
                if (d || (uniform ? !x : !gpr(x, gpr_limit)) || combine > 2 ||
                    (w2 & ~0x08000000u & ~0x07fe7ef0u) || ((w2 >> 27 & 1) != (u32)uniform_b))
                    return reject(si, at, op, "integer predicate compare form");
                if (op == 0x80c || op == 0x88c) {
                    b = TEMP_C;
                    mov_imm(&l, pred, b, w1);
                } else if (op == 0xa0c) {
                    b = TEMP_C;
                    if (!constant(&l, pred, b, w1)) return reject(si, at, op, "integer comparison constant");
                } else if (uniform_b) {
                    if (!(b = uniform_reg(w1, gpr_limit))) return reject(si, at, op, "integer comparison uniform register");
                } else if (!gpr(b, gpr_limit)) return reject(si, at, op, "integer comparison register");
                emit(&l, 0x5b60000000000000ull | pred | (w2 >> 20 & 7) | (u64)(w2 >> 17 & 7) << 3 |
                             (u64)x << 8 | (u64)b << 20 | (u64)(w2 >> 23 & 15) << 39 | (u64)combine << 45 |
                             (u64)(w2 >> 9 & 1) << 48 | (u64)(w2 >> 12 & 7) << 49);
                break;
            }
            case 0x212:
            case 0x812:
            case 0xa12:
            case 0xc12:   /* its second operand a uniform register */
            case 0x292:
            case 0x892: { /* LOP3/ULOP3.LUT with the canonical NZ predicate output */
                int uniform = op == 0x292 || op == 0x892, uniform_b = uniform || op == 0xc12;
                u32 target = uniform ? uniform_reg(d, gpr_limit) : d, x = uniform ? uniform_reg(a, gpr_limit) : a;
                u32 b = w1, c = uniform ? uniform_reg(w2 & 255, gpr_limit) : w2 & 255;
                if ((uniform ? !target || !x || !c : !gpr(d, gpr_limit) || !gpr(a, gpr_limit) || !gpr(c, gpr_limit)) ||
                    (w2 & ~0x000effffu) != (uniform_b ? 0x0f800000u : 0x07800000u))
                    return reject(si, at, op, "three-input logic predicate form");
                if (op == 0x812 || op == 0x892) {
                    b = TEMP_C;
                    mov_imm(&l, pred, b, w1);
                } else if (op == 0xa12) {
                    b = TEMP_C;
                    if (!constant(&l, pred, b, w1)) return reject(si, at, op, "logic constant source");
                } else if (uniform_b) {
                    if (!(b = uniform_reg(w1, gpr_limit))) return reject(si, at, op, "logic uniform register source");
                } else if (!gpr(b, gpr_limit)) return reject(si, at, op, "logic register source");
                emit(&l, 0x5be0000000000000ull | pred | target | (u64)x << 8 | (u64)b << 20 |
                             (u64)(w2 >> 8 & 255) << 28 | (u64)3 << 36 | (u64)c << 39 |
                             (u64)(w2 >> 17 & 7) << 48);
                break;
            }
            case 0x89c: /* UPLOP3 has one LUT output and a uniform execution guard. Use the same snapshot as PLOP3. */
            case 0x81c: { /* PLOP3 has two independent truth tables. Snapshot all inputs before either output. */
                u32 targets[2] = {w2 >> 17 & 7, w2 >> 20 & 7};
                u32 luts[2] = {(w2 & 7) | (w2 >> 8 & 31) << 3, d};
                u32 sources[3] = {w2 >> 23 & 15, w2 >> 13 & 15, w2 >> 4 & 15};
                if (a || w1 || (w2 & 0xf8000000u) ||
                    (op == 0x89c && (d || (w2 & 8) || targets[1] != 7)))
                    return reject(si, at, op, "predicate logic reserved form");
                mov_imm(&l, pred, 251, ~0u);
                for (u32 i = 0; i < 3; ++i) {
                    if (i == 2 && (w2 & 8) && (sources[i] & 7) != 7) {
                        emit(&l, 0x3828000000000000ull | pred | TEMP_C | (u64)UP_MASK << 8 |
                                     (u64)(sources[i] & 7) << 20);
                        emit(&l, 0x0400000000000000ull | pred | TEMP_C | (u64)TEMP_C << 8 | 1ull << 20);
                        add(&l, pred, TEMP_C, 255, TEMP_C, 1ull << 48);
                        if (sources[i] & 8)
                            emit(&l, 0x0400000000000000ull | pred | TEMP_C | (u64)TEMP_C << 8 |
                                         0xffffffffull << 20 | 2ull << 53);
                    } else
                        emit(&l, 0x5ca0000000000000ull | pred | (TEMP_A + i) | (u64)251 << 8 |
                                     (u64)255 << 20 | (u64)sources[i] << 39);
                }
                for (u32 i = 0; i < 2; ++i)
                    if (targets[i] != 7)
                        emit(&l, 0x5be0000000000000ull | pred | (251 + i) | (u64)TEMP_A << 8 |
                                     (u64)TEMP_B << 20 | (u64)luts[i] << 28 | (u64)TEMP_C << 39 | (u64)7 << 48);
                /* If an output replaces the execution guard, write it last. */
                u32 first = (guard & 7) != 7 && targets[0] == (guard & 7);
                for (u32 j = 0; j < 2; ++j) {
                    u32 i = j ^ first;
                    if (targets[i] != 7)
                        emit(&l, 0x5b60000000000000ull | pred | 7 | (u64)targets[i] << 3 |
                                     (u64)(251 + i) << 8 | (u64)255 << 20 | (u64)7 << 39 | (u64)5 << 49);
                }
                break;
            }
            case 0x883: { /* UP2UR with URZ as the merge source: native bit k is UPk, as in our packed bank. */
                u32 target = uniform_reg(d, gpr_limit);
                if (!target || a != 63 || guard != 7 || w1 > 0x7f || w2 != 0x08000000)
                    return reject(si, at, op, "uniform predicate-to-register form");
                lop_imm(&l, target, UP_MASK, w1);
                break;
            }
            case 0x287:
            case 0x887: { /* USEL reads UP without changing the vector predicate bank. */
                u32 target = uniform_reg(d, gpr_limit), x = uniform_reg(a, gpr_limit), b = op == 0x887 ? TEMP_C : uniform_reg(w1, gpr_limit);
                if (!target || !x || !b || (w2 & ~0x07800000u) != 0x08000000)
                    return reject(si, at, op, "uniform select form");
                if (op == 0x887) mov_imm(&l, pred, b, w1);
                emit(&l, 0x5ca0000000000000ull | pred | target | (u64)x << 8 | (u64)b << 20 |
                             (u64)(w2 >> 23 & 15) << 39);
                break;
            }
            case 0x207:
            case 0x807:
            case 0xa07: { /* SEL is bitwise, not a floating-point operation; B a register, an immediate or a constant */
                u32 source = op == 0x207 ? w1 : TEMP_C;
                if (!gpr(d, gpr_limit) || !gpr(a, gpr_limit) || (op == 0x207 && !gpr(source, gpr_limit)) || (w2 & ~0x07800000u))
                    return reject(si, at, op, "select form");
                if (op == 0x807) mov_imm(&l, pred, TEMP_C, w1);
                if (op == 0xa07 && !constant(&l, pred, TEMP_C, w1)) return reject(si, at, op, "select constant");
                emit(&l, 0x5ca0000000000000ull | pred | d | (u64)a << 8 | (u64)source << 20 |
                             (u64)(w2 >> 23 & 15) << 39);
                break;
            }
            case 0x208:
            case 0x808:
            case 0xa08: { /* FSEL applies float modifiers before the bitwise predicate selection. */
                u32 x, y;
                if (!gpr(d, gpr_limit) || (w2 & ~0x07800300u) ||
                    !float_source(&l, pred, w0, w1, w2, 0, &x, gpr_limit) || !float_source(&l, pred, w0, w1, w2, 1, &y, gpr_limit))
                    return reject(si, at, op, "floating select form");
                emit(&l, 0x5ca0000000000000ull | pred | d | (u64)x << 8 | (u64)y << 20 |
                             (u64)(w2 >> 23 & 15) << 39);
                break;
            }
            case 0x290:
            case 0x890: { /* UIADD3: plain, with its carry out to a UP, or .X with its carry in from one. The UP bank is
                             in P0..P6 meanwhile (uniform_predicates). Carries take a zero third operand, no negation */
                u32 target = uniform_reg(d, gpr_limit), x = uniform_reg(a, gpr_limit), c = uniform_reg(w2 & 255, gpr_limit);
                u32 b = op == 0x890 ? TEMP_C : uniform_reg(w1 & 255, gpr_limit), carry = w2 >> 17 & 7;
                int plain = (w2 & ~0x93fu) == 0x0fffe000;
                int carry_out = (w2 & ~0x000e00ffu) == 0x0ff1e000 && carry != 7 && c == 255;
                int carry_in = (w2 & ~0x078000ffu) == 0x087fe400 && c == 255;
                if (!target || !x || !b || !c || !(plain || carry_out || carry_in) ||
                    (op == 0x290 && (w1 & ~(plain ? 0x800000ffu : 0xffu))))
                    return reject(si, at, op, "uniform integer add form");
                if (op == 0x890) mov_imm(&l, pred, b, w1);
                if (carry_out) { /* the sum wrapped exactly when it is below an addend */
                    add(&l, pred, TEMP_A, x, b, 0);
                    emit(&l, 0x5b60000000000000ull | pred | 7 | (u64)carry << 3 | (u64)TEMP_A << 8 | (u64)b << 20 |
                                 (u64)7 << 39 | (u64)1 << 49);
                    emit(&l, 0x5c98078000000000ull | pred | target | (u64)TEMP_A << 20);
                } else if (carry_in) {
                    mov_imm(&l, pred, TEMP_A, 1);
                    emit(&l, 0x5ca0000000000000ull | pred | TEMP_A | (u64)TEMP_A << 8 | (u64)255 << 20 |
                                 (u64)(w2 >> 23 & 15) << 39);
                    add(&l, pred, TEMP_B, x, b, 0);
                    add(&l, pred, target, TEMP_B, TEMP_A, 0);
                } else {
                    u64 flags = (u64)(w2 >> 8 & 1) << 49;
                    if (op == 0x290) flags |= (u64)(w1 >> 31) << 48;
                    add(&l, pred, TEMP_A, x, b, flags);
                    add(&l, pred, target, TEMP_A, c, (u64)(w2 >> 11 & 1) << 48);
                }
                break;
            }
            case 0x299: { /* USHF.U32 with a uniform shift and a zero word outside the selected result. */
                u32 target = uniform_reg(d, gpr_limit), low = uniform_reg(a, gpr_limit), high = uniform_reg(w2 & 255, gpr_limit);
                u32 shift = uniform_reg(w1, gpr_limit), source;
                int right = w2 >> 12 & 1, upper = w2 >> 16 & 1;
                if (guard != 7 || !target || target == 255 || !low || !high || !shift ||
                    (w2 & ~0x118ffu) != 0x08000600u ||
                    (upper ? low != 255 : high != 255))
                    return reject(si, at, op, "uniform register shift form");
                source = upper ? high : low;
                emit(&l, (right ? 0x5c28000000000000ull : 0x5c48000000000000ull) | pred | target |
                             (u64)source << 8 | (u64)shift << 20 | (u64)(w2 >> 11 & 1) << 39);
                break;
            }
            case 0x899: { /* USHF with a zero word outside the selected half */
                u32 target = uniform_reg(d, gpr_limit), low = a, high = w2 & 255, source, flags = w2 & ~255u;
                int high_right = flags == 0x08011400 || flags == 0x08011600;
                int low_right = flags == 0x08001600 && high == 63;
                int upper = flags == 0x08010600 && low == 63;
                if (guard != 7 || !target || target == 255 || w1 > 32 ||
                    (high_right ? low != 63 || high == 63 : !upper && !low_right &&
                                                               (flags != 0x08000600 || high != 63)) ||
                    !(source = uniform_reg(high_right || upper ? high : low, gpr_limit)))
                    return reject(si, at, op, "uniform shift form");
                emit(&l, (high_right || low_right ? 0x3828000000000000ull : 0x3848000000000000ull) |
                             pred | target | (u64)source << 8 | (u64)w1 << 20 |
                             (u64)(flags == 0x08011400) << 48);
                break;
            }
            case 0xa10: { /* IADD3 with a constant: .X with one predicate carry in, or the low half of a 64-bit add,
                             its carry out to a predicate (NAK: carry ins 77..80 and 87..90, carry outs 81..86) */
                if (!gpr(d, gpr_limit) || !gpr(a, gpr_limit)) return reject(si, at, op, "constant add carry form");
                u32 c = w2 & 255;
                int plain = (w2 & ~0x9ffu) == 0x07ffe000;
                if (!constant(&l, pred, TEMP_C, plain ? w1 & 0x7fffffffu : w1))
                    return reject(si, at, op, "add carry constant");
                if (plain) {
                    if (!gpr(c, gpr_limit)) return reject(si, at, op, "constant add third source");
                    add(&l, pred, TEMP_A, a, TEMP_C, (u64)(w2 >> 8 & 1) << 49 | (u64)(w1 >> 31) << 48);
                    add(&l, pred, d, TEMP_A, c, (u64)(w2 >> 11 & 1) << 48);
                    break;
                }
                if ((w2 & ~0x000e0000u) == 0x07f1e0ff) { /* no carry in, no third operand, one carry out */
                    u32 carry = w2 >> 17 & 7;
                    add(&l, pred, TEMP_B, a, TEMP_C, 0);
                    emit(&l, 0x5c98078000000000ull | pred | d | (u64)TEMP_B << 20);
                    /* the addition wrapped exactly when the sum is below an addend */
                    if (carry != 7)
                        emit(&l, 0x5b60000000000000ull | pred | 7 | (u64)carry << 3 | (u64)TEMP_B << 8 |
                                     (u64)TEMP_C << 20 | (u64)7 << 39 | (u64)1 << 49);
                    break;
                }
                if ((w2 & ~0x07800000u) != 0x007fe4ff) return reject(si, at, op, "constant add carry form");
                mov_imm(&l, pred, TEMP_A, 1);
                emit(&l, 0x5ca0000000000000ull | pred | TEMP_A | (u64)TEMP_A << 8 |
                             (u64)255 << 20 | (u64)(w2 >> 23 & 15) << 39);
                add(&l, pred, TEMP_B, a, TEMP_C, 0);
                add(&l, pred, d, TEMP_B, TEMP_A, 0);
                break;
            }
            case 0x210:   /* IADD3 register */
            case 0xc10:   /* IADD3 uniform register (ALU form 6: the UR in bits 32..39, bit 91 set) */
            case 0x810: { /* IADD3 immediate, without predicate carry inputs */
                u32 c = w2 & 255, b = op == 0x810 ? TEMP_C : op == 0xc10 ? uniform_reg(w1 & 255, gpr_limit) : w1 & 255;
                if ((op == 0x210 || op == 0x810) && (w2 & ~0x000e0000u) == 0x07f1e0ff && (w2 >> 17 & 7) != 7) {
                    /* the low half of a 64-bit add, its carry out to a predicate, as 0xa10's */
                    u32 carry = w2 >> 17 & 7;
                    if (!gpr(d, gpr_limit) || !gpr(a, gpr_limit) || (op == 0x210 && (!gpr(b, gpr_limit) || (w1 & ~255u))))
                        return reject(si, at, op, "integer add carry/modifier form");
                    if (op == 0x810) mov_imm(&l, pred, TEMP_C, w1);
                    /* If the native guard names the carry output, commit the register before changing that guard.
                       Only that ordering can need an addend saved when both inputs alias the destination. */
                    int guard_carry = (guard & 7) == carry;
                    u32 compare = b;
                    add(&l, pred, TEMP_B, a, b, 0);
                    if (guard_carry) {
                        compare = d == b ? a : b;
                        if (d == a && d == b && d != 255) {
                            emit(&l, 0x5c98078000000000ull | pred | TEMP_C | (u64)b << 20);
                            compare = TEMP_C;
                        }
                        emit(&l, 0x5c98078000000000ull | pred | d | (u64)TEMP_B << 20);
                    }
                    emit(&l, 0x5b60000000000000ull | pred | 7 | (u64)carry << 3 | (u64)TEMP_B << 8 | (u64)compare << 20 |
                                 (u64)7 << 39 | (u64)1 << 49);
                    if (!guard_carry) emit(&l, 0x5c98078000000000ull | pred | d | (u64)TEMP_B << 20);
                    break;
                }
                if (!gpr(d, gpr_limit) || !gpr(a, gpr_limit) || !gpr(c, gpr_limit) ||
                    (op != 0x810 && (!(op == 0xc10 ? b : gpr(b, gpr_limit)) || (w1 & ~0x800000ffu))) ||
                    (w2 & ~0x9ffu) != (op == 0xc10 ? 0x0fffe000u : 0x07ffe000u))
                    return reject(si, at, op, "integer add carry/modifier form");
                if (op == 0x810) mov_imm(&l, pred, TEMP_C, w1);
                u64 flags = (u64)(w2 >> 8 & 1) << 49;
                if (op != 0x810) flags |= (u64)(w1 >> 31) << 48;
                add(&l, pred, TEMP_A, a, b, flags);
                add(&l, pred, d, TEMP_A, c, (u64)(w2 >> 11 & 1) << 48);
                break;
            }
            case 0x211:
            case 0x811:
            case 0xa11: { /* LEA low word and its addition carry, or LEA.HI[.SX32] (80/73) of a zero high word
                             without carry out, carry input or negation */
                u32 b = op == 0x211 ? w1 & 255 : TEMP_C, shift = w2 >> 11 & 31;
                int high = (w2 & ~0xfa00u) == 0x078f00ffu;
                int negate = op == 0x211 && (w1 >> 31);
                if (!gpr(d, gpr_limit) || !gpr(a, gpr_limit) || (op == 0x211 && (!gpr(b, gpr_limit) || (w1 & ~0x800000ffu))) ||
                    (negate && (high || (w2 >> 17 & 7) != 7)) ||
                    (!high && (w2 & ~0x000ef800u) != 0x078000ff))
                    return reject(si, at, op, "effective address carry/modifier form");
                if (op == 0x811) mov_imm(&l, pred, TEMP_C, w1);
                if (op == 0xa11 && !constant(&l, pred, b, w1)) return reject(si, at, op, "effective address constant");
                if (negate) { /* LEA Rd, Ra, -Rb, shift: (Ra << shift) - Rb, modulo 32 bits. */
                    emit(&l, 0x3848000000000000ull | pred | TEMP_A | (u64)a << 8 | (u64)shift << 20);
                    add(&l, pred, d, TEMP_A, b, 1ull << 48);
                    break;
                }
                if (high) { /* high of a zero-extended or sign-extended input shifted left by 0..31 */
                    if (!shift && !(w2 & 0x200)) mov_imm(&l, pred, TEMP_A, 0);
                    else emit(&l, 0x3828000000000000ull | pred | TEMP_A | (u64)a << 8 |
                                      (u64)(shift ? 32 - shift : 31) << 20 | (u64)(w2 >> 9 & 1) << 48);
                    add(&l, pred, d, TEMP_A, b, 0);
                    break;
                }
                u32 carry = w2 >> 17 & 7, result = d == 255 && carry != 7 ? TEMP_B : d;
                if (carry != 7 && b == d && d != 255) {
                    emit(&l, 0x5c98078000000000ull | pred | TEMP_C | (u64)b << 20);
                    b = TEMP_C;
                }
                emit(&l, 0x5c18000000000000ull | pred | result | (u64)a << 8 | (u64)b << 20 | (u64)shift << 39);
                if (carry != 7)
                    emit(&l, 0x5b60000000000000ull | pred | 7 | (u64)carry << 3 | (u64)result << 8 |
                                 (u64)b << 20 | (u64)7 << 39 | (u64)1 << 49);
                break;
            }
            case 0x291:
            case 0x891: { /* ULEA low word or .HI of a zero high word, no carry or negation */
                u32 target = uniform_reg(d, gpr_limit), x = uniform_reg(a, gpr_limit);
                u32 b = op == 0x891 ? TEMP_C : uniform_reg(w1, gpr_limit), shift = w2 >> 11 & 31;
                int high = w2 >> 16 & 1;
                if (guard != 7 || !target || target == 255 || !x || !b || (w2 & ~0x1f800u) != 0x0f8e003f)
                    return reject(si, at, op, "uniform effective address form");
                if (op == 0x891) mov_imm(&l, pred, b, w1);
                if (high) {
                    emit(&l, 0x3828000000000000ull | pred | TEMP_A | (u64)x << 8 | (u64)(32 - shift) << 20);
                    add(&l, pred, target, TEMP_A, b, 0);
                } else
                    emit(&l, 0x5c18000000000000ull | pred | target | (u64)x << 8 | (u64)b << 20 | (u64)shift << 39);
                break;
            }
            case 0x217:
            case 0x817:
            case 0xa17:
            case 0xc17: { /* IMNMX of a register, an immediate, a constant or a uniform register (with bit 91); a
                             predicate selects minimum or maximum */
                u32 b = op == 0x217 ? w1 & 255 : op == 0xc17 ? uniform_reg(w1 & 63, gpr_limit) : TEMP_C;
                if (!gpr(d, gpr_limit) || !gpr(a, gpr_limit) || (op == 0x217 && (!gpr(b, gpr_limit) || (w1 & ~255u))) ||
                    (op == 0xc17 && (!b || (w1 & ~63u))) ||
                    (w2 & ~0x04000200u) != (op == 0xc17 ? 0x0b800000u : 0x03800000u))
                    return reject(si, at, op, "integer min/max form");
                if (op == 0x817) mov_imm(&l, pred, TEMP_C, w1);
                if (op == 0xa17 && !constant(&l, pred, TEMP_C, w1)) return reject(si, at, op, "min/max constant");
                emit(&l, 0x5c20000000000000ull | pred | d | (u64)a << 8 | (u64)b << 20 |
                             (u64)(w2 >> 23 & 15) << 39 | (u64)(w2 >> 9 & 1) << 48);
                break;
            }
            case 0x245:
            case 0xa45:
            case 0xc45: { /* I2FP.F32.U32/S32 */
                u32 source = w1, flags = w2 & ~0x08000000u;
                if (!gpr(d, gpr_limit) || a || (flags & ~0xc400u) != 0x00201000 ||
                    ((flags >> 14 & 3) != 0 && ((flags >> 14 & 3) != 3 || (flags & 0x400))) ||
                    ((w2 >> 27 & 1) != (op == 0xc45)))
                    return reject(si, at, op, "integer-to-float form");
                if (op == 0xa45) {
                    source = TEMP_C;
                    if (!constant(&l, pred, source, w1)) return reject(si, at, op, "integer constant source");
                } else if (op == 0xc45) {
                    source = uniform_reg(w1, gpr_limit);
                    if (!source) return reject(si, at, op, "integer uniform source");
                } else if (!gpr(source, gpr_limit)) return reject(si, at, op, "integer register source");
                emit(&l, 0x5cb8000000000a00ull | pred | d | (u64)source << 20 |
                             (u64)(flags >> 10 & 1) << 13 | (u64)(flags >> 14 & 3) << 39);
                break;
            }
            case 0x306:
            case 0xd06: { /* I2F.F32 of an 8, 16 or 32-bit register or uniform register (NAK: signed 74, destination size 75..76, rounding
                             78..79, source size 84..85), to Maxwell's I2F with the same source format. Bits 60..61
                             select the byte or half of the source (eden-ns2). Maxwell's byte selector (41..42) names
                             a half by its first byte, so H1 is 2 (as PokeDinner maps it) */
                u32 size = w2 >> 20 & 3, source = op == 0xd06 ? uniform_reg(w1 & 255, gpr_limit) : w1 & 255;
                u32 part = w1 >> 28 & 3;
                if (!gpr(d, gpr_limit) || a || (op == 0xd06 ? !source : !gpr(source, gpr_limit)) || (w1 & ~0x300000ffu) ||
                    (w2 & ~0x300400u) != (op == 0xd06 ? 0x08001000u : 0x1000u) || size == 3 ||
                    (size == 1 && part > 1) || (size == 2 && part))
                    return reject(si, at, op, "integer-to-float form");
                emit(&l, 0x5cb8000000000200ull | pred | d | (u64)source << 20 | (u64)size << 10 |
                             (u64)(w2 >> 10 & 1) << 13 | (u64)(size == 1 ? part * 2 : part) << 41);
                break;
            }
            case 0x307: { /* FRND.F32, without flush-to-zero */
                u32 source;
                if (!gpr(d, gpr_limit) || a || (w2 & ~0xc000u) != 0x00201000 ||
                    !float_source(&l, pred, w0, w1, w2, 1, &source, gpr_limit))
                    return reject(si, at, op, "float rounding form");
                emit(&l, 0x5ca8000000000a00ull | pred | d | (u64)source << 20 |
                             (u64)(8 + (w2 >> 14 & 3)) << 39);
                break;
            }
            case 0x305:
            case 0xb05:
            case 0xd05: { /* F2I.F32 to an 8-, 16- or 32-bit integer (NAK: signed 72, size 75..76, round 78..79, FTZ
                             80). FTZ can't change the integer of rounding to nearest or of truncation, only of floor
                             and ceiling, which the translator doesn't flush. The translator saturates 16 and 32 bits.
                             8 bits convert to 32 and clamp: the same saturation, and NaN still 0 */
                u32 source, size = w2 >> 11 & 3, round = w2 >> 14 & 3, is_signed = w2 >> 8 & 1;
                if (!gpr(d, gpr_limit) || a || (w2 & ~0x0801d900u) != 0x00202000 || (w2 >> 27 & 1) != (op == 0xd05) || size == 3 ||
                    ((w2 & 0x10000) && (round == 1 || round == 2)) || !float_source(&l, pred, w0, w1, w2, 1, &source, gpr_limit))
                    return reject(si, at, op, "float-to-integer form");
                emit(&l, 0x5cb0000000000800ull | pred | d | (u64)source << 20 | (u64)(size ? size : 2) << 8 |
                             (u64)is_signed << 12 | (u64)round << 39);
                if (!size) { /* IMNMX: PT selects the minimum, !PT the maximum */
                    mov_imm(&l, pred, TEMP_C, is_signed ? 127 : 255);
                    emit(&l, 0x5c20000000000000ull | pred | d | (u64)d << 8 | (u64)TEMP_C << 20 | 7ull << 39 |
                                 (u64)is_signed << 48);
                    mov_imm(&l, pred, TEMP_C, is_signed ? 0xffffff80u : 0);
                    emit(&l, 0x5c20000000000000ull | pred | d | (u64)d << 8 | (u64)TEMP_C << 20 | 15ull << 39 |
                                 (u64)is_signed << 48);
                }
                break;
            }
            case 0x804: { /* R2P PR, Ra.Bn, mask: the predicates the immediate mask names, from the selected byte */
                if (d || !gpr(a, gpr_limit) || w1 > 0x7f || (w2 & ~0x3000u)) return reject(si, at, op, "register to predicates form");
                emit(&l, 0x38f0000000000000ull | pred | (u64)a << 8 | (u64)w1 << 20 | (u64)(w2 >> 12 & 3) << 41);
                break;
            }
            case 0x803: { /* P2R Rd, PR, Ra.Bn, mask: Ra with the predicates the immediate mask names in the selected
                             byte (76..77), as PokeDinner lowers it */
                if (!gpr(d, gpr_limit) || !gpr(a, gpr_limit) || w1 > 0x7f || (w2 & ~0x3000u)) return reject(si, at, op, "predicates to register form");
                emit(&l, 0x38e8000000000000ull | pred | d | (u64)a << 8 | (u64)w1 << 20 | (u64)(w2 >> 12 & 3) << 41);
                break;
            }
            case 0x209:
            case 0x809:
            case 0xa09:
            case 0xc09: { /* FMNMX.F32, without NaN propagation. The uniform register form sets bit 91. */
                u32 x, y;
                if (!gpr(d, gpr_limit) || (w2 & ~0x07810300u) != (op == 0xc09 ? 0x08000000u : 0) ||
                    !float_source(&l, pred, w0, w1, w2, 0, &x, gpr_limit) || !float_source(&l, pred, w0, w1, w2, 1, &y, gpr_limit))
                    return reject(si, at, op, "float min/max form");
                emit(&l, 0x5c60000000000000ull | pred | d | (u64)x << 8 | (u64)y << 20 |
                             (u64)(w2 >> 23 & 15) << 39 | (u64)(w2 >> 16 & 1) << 44);
                break;
            }
            case 0x220:
            case 0x820:
            case 0xa20:
            case 0xc20: { /* FMUL.F32, scaled forms require FTZ and round-to-nearest. */
                u32 x, y, scale = w2 >> 20 & 7;
                if (!gpr(d, gpr_limit) || (w2 & ~0x0871e300u) || (w2 >> 27 & 1) != (op == 0xc20) || !scale ||
                    (scale != 4 && (!(w2 & 0x10000) || (w2 & 0xc000))) ||
                    !float_source(&l, pred, w0, w1, w2, 0, &x, gpr_limit) || !float_source(&l, pred, w0, w1, w2, 1, &y, gpr_limit))
                    return reject(si, at, op, "float multiply form");
                emit(&l, 0x5c68000000000000ull | pred | d | (u64)x << 8 | (u64)y << 20 |
                             (u64)(w2 >> 14 & 3) << 39 | (u64)(scale < 4 ? 4 - scale : scale > 4 ? 11 - scale : 0) << 41 |
                             (u64)(w2 >> 16 & 1) << 44 |
                             (u64)(w2 >> 13 & 1) << 50);
                break;
            }
            case 0x221:
            case 0x421:
            case 0x621:
            case 0x821:
            case 0xa21:
            case 0xe21: { /* FADD.F32, the second operand a register, an immediate, a constant or a uniform register */
                u32 x, y, which = op == 0x421 || op == 0x621 || op == 0xe21 ? 2 : 1;
                if (!gpr(d, gpr_limit) || (w2 & ~0x1e300u) != (op == 0xe21 ? 0x08000000u : 0u) ||
                    !float_source(&l, pred, w0, w1, w2, 0, &x, gpr_limit) ||
                    !float_source(&l, pred, w0, w1, w2, which, &y, gpr_limit))
                    return reject(si, at, op, "float add form");
                emit(&l, 0x5c58000000000000ull | pred | d | (u64)x << 8 | (u64)y << 20 |
                             (u64)(w2 >> 14 & 3) << 39 | (u64)(w2 >> 16 & 1) << 44 |
                             (u64)(w2 >> 13 & 1) << 50);
                break;
            }
            case 0x223:
            case 0x423:
            case 0x623:
            case 0x823:
            case 0xa23:
            case 0xc23:
            case 0xe23: { /* FFMA.F32 */
                u32 x, y, z;
                if (!gpr(d, gpr_limit) || (w2 & ~0x0801efffu) || (w2 >> 27 & 1) != (op == 0xc23 || op == 0xe23) ||
                    !float_source(&l, pred, w0, w1, w2, 0, &x, gpr_limit) ||
                    !float_source(&l, pred, w0, w1, w2, 1, &y, gpr_limit) || !float_source(&l, pred, w0, w1, w2, 2, &z, gpr_limit))
                    return reject(si, at, op, "float fused multiply-add form");
                emit(&l, 0x5980000000000000ull | pred | d | (u64)x << 8 | (u64)y << 20 | (u64)z << 39 |
                             (u64)(w2 >> 13 & 1) << 50 | (u64)(w2 >> 14 & 3) << 51 | (u64)(w2 >> 16 & 1) << 53);
                break;
            }
            case 0x230: { /* Packed HADD2, or the compiler's exact HADD2.F32 -RZ, Rb.H0/H1 promotion. */
                u32 source = w1 & 255, high = w1 >> 28 & 1;
                if (w2 & 0x4000) {
                    if (d >= gpr_limit || a != 255 || !gpr(source, gpr_limit) ||
                        (w1 & ~0x100000ffu) != 0x20000000 || w2 != 0x00004100)
                        return reject(si, at, op, "half promotion form");
                    emit(&l, 0x5ca8000000000000ull | pred | d | 2ull << 8 | 1ull << 10 |
                                 (u64)source << 20 | (u64)high << 41);
                } else {
                    if (!gpr(d, gpr_limit) || !gpr(a, gpr_limit) || !gpr(source, gpr_limit) ||
                        (w1 & 0x0fffff00u) || (w2 & ~0x12f00u))
                        return reject(si, at, op, "packed half add form");
                    emit(&l, 0x5d10000000000000ull | pred | d | (u64)a << 8 | (u64)source << 20 |
                                 (u64)(w1 >> 28) << 28 | (u64)(w2 >> 13 & 1) << 32 |
                                 (u64)(w2 >> 16 & 1) << 39 | (u64)(w2 >> 8 & 1) << 43 |
                                 (u64)(w2 >> 9 & 1) << 44 | (u64)(w2 >> 10 & 3) << 47);
                }
                break;
            }
            case 0x431: { /* HFMA2 with a packed immediate addend and register multiplier (ALU form 2). */
                u32 b = w2 & 255;
                if (!gpr(d, gpr_limit) || !gpr(a, gpr_limit) || !gpr(b, gpr_limit) ||
                    (w2 & ~0x00060cffu))
                    return reject(si, at, op, "packed half fused multiply-add form");
                mov_imm(&l, pred, TEMP_C, w1);
                emit(&l, 0x5d00000000000000ull | pred | d | (u64)a << 8 | (u64)b << 20 |
                             (u64)(w2 >> 17 & 3) << 28 | (u64)TEMP_C << 39 | (u64)(w2 >> 10 & 3) << 47);
                break;
            }
            case 0x321:
            case 0x322: { /* ALD/AST with direct, non-patch attribute addresses */
                u32 address = w1 >> 8 & 1023, count = (w2 >> 10 & 3) + 1;
                u32 value = op == 0x321 ? d : w1 & 255, vertex = op == 0x321 ? w1 & 255 : w2 & 255;
                if (a != 255 || !gpr(vertex, gpr_limit) || (address & 3) || address + count * 4 > 0x400 ||
                    (w1 & ~0x3ffffu) || (op == 0x322 && d) || (w2 & ~(0xc00u | (op == 0x322 ? 255u : 0u))) ||
                    (value == 255 ? op != 0x322 : value + count > gpr_limit))
                    return reject(si, at, op, "attribute address/register form");
                if (value == 255) {
                    for (u32 i = 0; i < count; ++i)
                        emit(&l, 0xeff0000000000000ull | pred | 255 | 255ull << 8 |
                                     (u64)(address + i * 4) << 20 | (u64)vertex << 39);
                } else {
                    emit(&l, (op == 0x321 ? 0xefd8000000000000ull : 0xeff0000000000000ull) |
                                 pred | value | 255ull << 8 | (u64)address << 20 | (u64)vertex << 39 |
                                 (u64)(count - 1) << 47);
                }
                break;
            }
            case 0xb6f: { /* TXQ dimensions or mip count, through the instruction's explicit descriptor bank */
                u32 mask = w2 >> 8 & 15, bank = w1 >> 22 & 31, offset = w1 >> 8 & 0x3fff;
                if (!gpr(a, gpr_limit) || bank >= 18 || offset > 0x1fff || (w1 & 0xf80000ffu) ||
                    (w2 & ~0x04000f00u) != 255 || (mask != 1 && mask != 2 && mask != 3 && mask != 8) ||
                    (mask == 3 ? (d & 1) || d + 1 >= gpr_limit : d >= gpr_limit))
                    return reject(si, at, op, "texture query form");
                emit(&l, 0x4c98078000070000ull | TEMP_A | (u64)offset << 20 | (u64)bank << 34);
                emit(&l, 0x5c98078000000000ull | pred | TEMP_B | (u64)a << 20);
                emit(&l, 0xdf50000000000000ull | pred | d | (u64)TEMP_A << 8 |
                             (u64)1 << 22 | (u64)mask << 31);
                break;
            }
            case 0xb69: { /* TMML through the instruction's descriptor bank (NAK: handle 40..58, dimension 61..63, mask
                             72..75, NDV 77). The coordinates fill A on, an array's layer first, as TEX's. Maxwell's
                             TMML.B reads them from Ra and the handle from Rb, and writes the masked components on */
                static const u8 coordinates_of[8] = {1, 2, 3, 3, 2, 3, 0, 4}, maxwell_dim[8] = {0, 2, 4, 6, 1, 3, 0, 7};
                u32 mask = w2 >> 8 & 15, bank = w1 >> 22 & 31, offset = w1 >> 8 & 0x3fff, dim = w1 >> 29;
                u32 count = snx_popcount64(mask);
                if (!coordinates_of[dim] || bank >= 18 || offset > 0x1fff || (w1 & 0x18000000u) || (w1 & 255) != 255 ||
                    (w2 & ~0x04000f00u) != 255 || !count || count > 2 || d + count > gpr_limit ||
                    a + coordinates_of[dim] > gpr_limit)
                    return reject(si, at, op, "texture LOD query form");
                emit(&l, 0x4c98078000070000ull | TEMP_C | (u64)offset << 20 | (u64)bank << 34);
                emit(&l, 0xdf60000000000000ull | pred | d | (u64)a << 8 | (u64)TEMP_C << 20 |
                             (u64)maxwell_dim[dim] << 28 | (u64)mask << 31);
                break;
            }
            case 0xb60:   /* TEX through the instruction's descriptor bank, with split destinations. Its operands are
                             one list: the array index, the coordinates, the LOD or bias, the depth reference. With bit
                             60 they fill A and B alone when there are two, else the pairs A, A+1 then B on. Without it
                             the coordinates fill A on, and the one other operand is B (NAK: dimension 61..63, depth
                             compare 78, LOD mode 87..89; the layouts as eden-ns2's ops_texture.cpp reads them).
                             Maxwell's TEX.B reads the coordinates from Ra on, and the handle, then the LOD, then the
                             depth reference from Rb on. */
            case 0xb63: { /* TLD4, the same but with no LOD: 87..88 select the gathered component. No texel offsets */
                static const u8 coordinates_of[8] = {1, 2, 3, 3, 2, 3, 0, 4}, maxwell_dim[8] = {0, 2, 4, 6, 1, 3, 0, 7};
                int gather = op == 0xb63;
                u32 b = w1 & 255, second = w2 & 255, mask = w2 >> 8 & 15;
                u32 bank = w1 >> 22 & 31, offset = w1 >> 8 & 0x3fff, lod = gather ? 0 : w2 >> 23 & 7, dim = w1 >> 29;
                u32 count = snx_popcount64(mask), coordinates = coordinates_of[dim], scalar = w1 >> 28 & 1;
                u32 compare = w2 >> 14 & 1, has_lod = lod == 2 || lod == 3, offsets = !gather && (w2 >> 12 & 1);
                u32 operands = coordinates + has_lod + offsets + compare, half = w2 >> 15 & 1;
                u32 list[6] = {0};
                int bad = !coordinates || (w1 & 0x08000000) || bank >= 18 || offset > 0x1fff || !count || lod > 3 ||
                          (w2 & ~0x07f0ffffu) != 0x000e0000 || (w2 >> 20 & 7) > 5 || operands > 6 ||
                          (gather && (half || (w2 & 0x02003000u) || maxwell_dim[dim] == 4)) ||
                          (offsets && (compare || dim == 3 || dim == 7 || (has_lod && up_live))) ||
                          (!scalar && operands > coordinates + 1) ||
                          (scalar && operands > 2 && ((a & 1) || (operands > 3 && (b & 1)))) ||
                          (compare && has_lod && up_live) || /* the depth reference would be staged in UP_MASK */
                          d >= gpr_limit ||
                          (half ? (count > 2 ? second >= gpr_limit : second != 255)
                                : ((count > 1 && (d & 1)) || d + (count > 1 ? 2 : 1) > gpr_limit ||
                                   (count > 2 ? second >= gpr_limit ||
                                                    (count == 4 && ((second & 1) || second + 1 >= gpr_limit))
                                              : second != 255)));
                for (u32 k = 0; !bad && k < operands; ++k) {
                    u32 base = scalar ? (operands <= 2 ? (k ? b : a) : k < 2 ? a : b) : k < coordinates ? a : b;
                    u32 index = scalar ? (operands <= 2 ? 0 : k < 2 ? k : k - 2) : k < coordinates ? k : 0;
                    bad = base == 255 ? index != 0 : base + index >= gpr_limit; /* RZ is one zero operand */
                    list[k] = base == 255 ? 255 : base + index;
                }
                if (bad) return reject(si, at, op, "texture sample coordinates/descriptor form");
                for (u32 k = 0; k < coordinates; ++k)
                    emit(&l, 0x5c98078000000000ull | pred | (TEMP_A + k) | (u64)list[k] << 20);
                if (has_lod) emit(&l, 0x5c98078000000000ull | pred | 253 | (u64)list[coordinates] << 20);
                if (offsets)
                    emit(&l, 0x5c98078000000000ull | pred | (has_lod ? 254 : 253) |
                                 (u64)list[coordinates + has_lod] << 20);
                if (compare)
                    emit(&l, 0x5c98078000000000ull | pred | (has_lod ? 254 : 253) |
                                 (u64)list[coordinates + has_lod] << 20);
                /* The handle's data flow must also be defined on the predicated-off path. Only the sample is guarded. */
                /* NAK: bits 84-86 are eviction priority, bit 90 is NODEP. Neither changes the sampled values.
                   Non-divergent derivatives are bit 77, not NODEP. */
                emit(&l, 0x4c98078000070000ull | 252 | (u64)offset << 20 | (u64)bank << 34);
                if (gather) /* Maxwell's TLD4.B: the component at 38..39, and offsets (36..37) off */
                    emit(&l, 0xdec0000000000000ull | pred | TEMP_A | (u64)TEMP_A << 8 | 252ull << 20 |
                                 (u64)maxwell_dim[dim] << 28 | (u64)mask << 31 | (u64)(w2 >> 23 & 3) << 38 |
                                 (u64)compare << 50);
                else
                    emit(&l, 0xde80000000000000ull | pred | TEMP_A | (u64)TEMP_A << 8 | 252ull << 20 |
                                 (u64)maxwell_dim[dim] << 28 | (u64)mask << 31 | (u64)lod << 37 | 7ull << 51 |
                                 (u64)(w2 >> 13 & 1) << 49 | (u64)compare << 50 | (u64)offsets << 36);
                if (half) {
                    /* The selected channels are packed consecutively, low half first. F2F clears the high half,
                       including the missing channel of an odd count, before a second channel is added there. */
                    for (u32 i = 0; i < count; ++i)
                        emit(&l, 0x5ca8000000000000ull | pred | (TEMP_A + i) | 1ull << 8 | 2ull << 10 |
                                     (u64)(TEMP_A + i) << 20);
                    for (u32 i = 0; i < count; i += 2) {
                        u32 target = i ? second : d;
                        if (i + 1 < count) {
                            emit(&l, 0x3848000000000000ull | pred | (TEMP_A + i + 1) |
                                         (u64)(TEMP_A + i + 1) << 8 | 16ull << 20);
                            add(&l, pred, target, TEMP_A + i, TEMP_A + i + 1, 0);
                        } else
                            emit(&l, 0x5c98078000000000ull | pred | target | (u64)(TEMP_A + i) << 20);
                    }
                } else {
                    for (u32 i = 0; i < count; ++i)
                        emit(&l, 0x5c98078000000000ull | pred | (i < 2 ? d + i : second + i - 2) |
                                     (u64)(TEMP_A + i) << 20);
                }
                break;
            }
            case 0xb66: { /* TLD.LZ through a direct descriptor, scalar coordinates (60). ARRAY_2D packs the
                             layer and x in a register pair, then takes y from the separate b operand. */
                u32 b = w1 & 255, second = w2 & 255, mask = w2 >> 8 & 15, count = snx_popcount64(mask);
                u32 bank = w1 >> 22 & 31, offset = w1 >> 8 & 0x3fff, dim = w1 >> 29;
                int array = dim == 5;
                if (!gpr(a, gpr_limit) || (dim ? !gpr(b, gpr_limit) : b != 255) || (dim > 1 && !array) ||
                    (array && ((a & 1) || a + 1 >= gpr_limit)) || (w1 & 0x18000000u) != 0x10000000u ||
                    bank >= 18 || offset > 0x1fff || !count ||
                    (w2 & ~0x04700fffu) != 0x008e0000 || d >= gpr_limit || (count > 1 && (d & 1)) ||
                    d + (count > 1 ? 2 : 1) > gpr_limit ||
                    (count > 2 ? second >= gpr_limit || (count == 4 && ((second & 1) || second + 1 >= gpr_limit))
                               : second != 255))
                    return reject(si, at, op, "texel fetch form");
                emit(&l, 0x5c98078000000000ull | pred | TEMP_A | (u64)a << 20);
                if (array) {
                    emit(&l, 0x5c98078000000000ull | pred | TEMP_B | (u64)(a + 1) << 20);
                    emit(&l, 0x5c98078000000000ull | pred | TEMP_C | (u64)b << 20);
                } else if (dim) emit(&l, 0x5c98078000000000ull | pred | TEMP_B | (u64)b << 20);
                emit(&l, 0x4c98078000070000ull | 252 | (u64)offset << 20 | (u64)bank << 34);
                emit(&l, 0xdd00000000000000ull | pred | TEMP_A | (u64)TEMP_A << 8 | 252ull << 20 |
                             (u64)(array ? 3 : dim * 2) << 28 |
                             (u64)mask << 31 | 7ull << 51);
                for (u32 i = 0; i < count; ++i)
                    emit(&l, 0x5c98078000000000ull | pred | (i < 2 ? d + i : second + i - 2) |
                                 (u64)(TEMP_A + i) << 20);
                break;
            }
            case 0x232:
            case 0x832: { /* HMUL2 of packed F16 register or immediate pairs, with packed/H0/H1 selectors. */
                u32 b = op == 0x832 ? TEMP_C : w1 & 255, a_select = w2 >> 10 & 3;
                u32 b_select = op == 0x832 ? 0 : w1 >> 28 & 3;
                if (!gpr(d, gpr_limit) || !gpr(a, gpr_limit) ||
                    (op == 0x232 && (!gpr(b, gpr_limit) || (w1 & ~0x300000ffu))) || (w2 & ~0xc00u) ||
                    a_select == 1 || b_select == 1)
                    return reject(si, at, op, "packed half multiply form");
                if (op == 0x832) mov_imm(&l, pred, b, w1);
                emit(&l, 0x5d08000000000000ull | pred | d | (u64)a << 8 | (u64)b << 20 |
                             (u64)b_select << 28 | (u64)a_select << 47);
                break;
            }
            case 0x23e:
            case 0xa3e: { /* F2FP.PACK_AB or MERGE_C (78), round-to-nearest. MERGE_C keeps C's low half as the
                             high result half and converts only B. The constant operand is B, not A. */
                u32 b = op == 0xa3e ? TEMP_C : w1 & 255, c = w2 & 255;
                int merge = w2 >> 14 & 1;
                if (!gpr(d, gpr_limit) || !gpr(a, gpr_limit) || !gpr(c, gpr_limit) || (w2 & ~0x40ffu) || (!merge && c != 255) ||
                    (op == 0x23e && (!gpr(b, gpr_limit) || (w1 & ~255u))) ||
                    (op == 0xa3e && !constant(&l, pred, b, w1)))
                    return reject(si, at, op, "float pack form");
                if (merge)
                    emit(&l, 0x5c98078000000000ull | pred | TEMP_A | (u64)c << 20);
                else
                    emit(&l, 0x5ca8000000000000ull | pred | TEMP_A | 1ull << 8 | 2ull << 10 | (u64)a << 20);
                emit(&l, 0x5ca8000000000000ull | pred | TEMP_B | 1ull << 8 | 2ull << 10 | (u64)b << 20);
                emit(&l, 0x5c18000000000000ull | pred | d | (u64)TEMP_A << 8 | (u64)TEMP_B << 20 | 16ull << 39);
                break;
            }
            case 0xb97:
            case 0xb9b: { /* Formatted surface load/store, with an explicit descriptor bank */
                int store = op == 0xb9b, strong = store && (w2 & ~0xf00u) == 0x0010a000;
                u32 bank = w1 >> 22 & 31, offset = w1 >> 8 & 0x3fff, dim = w1 >> 29;
                u32 mask = w2 >> 8 & 15, data = store ? w1 & 255 : d, count = snx_popcount64(mask);
                if (bank >= 18 || offset > 0x1fff || (w1 & 0x18000000u) || (!store && (w1 & 255)) ||
                    (store && d) || dim < 3 || dim > 5 || a + (dim == 3 ? 1 : 2) >= gpr_limit ||
                    (mask != 1 && mask != 15 && (store || mask != 3)) || data + count > gpr_limit ||
                    (!store && (data & (count - 1))) ||
                    (w2 & ~0xf00u) != (store ? strong ? 0x0010a000u : 0x00100000u : 0x001e0000u))
                    return reject(si, at, op, "formatted surface mode/registers");
                emit(&l, 0x4c98078000070000ull | TEMP_C | (u64)offset << 20 | (u64)bank << 34);
                if (strong) emit(&l, 0xef98000000000100ull | pred);
                emit(&l, (store ? 0xeb20000000000000ull : 0xeb00000000000000ull) | pred | data |
                             (u64)a << 8 | (u64)mask << 20 | (u64)dim << 33 | (u64)TEMP_C << 39);
                if (strong) emit(&l, 0xef98000000000100ull | pred);
                break;
            }
            case 0x526: { /* IPA indexed by a uniform byte offset plus the instruction's component displacement */
                u32 index = uniform_reg(a, gpr_limit);
                if (!gpr(d, gpr_limit) || !index || w1 != 255 || (w2 & ~0x40ffu) != 0x080e0000)
                    return reject(si, at, op, "uniform indexed interpolation form");
                mov_imm(&l, pred, TEMP_C, (w2 & 255) * 4);
                add(&l, pred, TEMP_A, index, TEMP_C, 0);
                emit(&l, 0xe000000000000000ull | pred | d | (u64)TEMP_A << 8 | (u64)255 << 20 |
                             1ull << 38 | (u64)(w2 >> 14 & 1) << 55);
                break;
            }
            case 0x923:
            case 0x927: { /* ISBE input workgroup ID, output attribute words, primitive count and byte indices.
                             Preserve these as a native-only adapter, never reinterpret a mesh access as a MOV. */
                int write = op == 0x927;
                if (!write && !w1 && !w2) { /* ISBERD with no offset or mode: a geometry vertex's index as its
                                               attribute handle, which Maxwell's ISBERD copies */
                    if (!gpr(d, gpr_limit) || d == 255 || !gpr(a, gpr_limit) || a == 255)
                        return reject(si, at, op, "stage buffer read registers");
                    emit(&l, 0xefd0000000000000ull | pred | d | (u64)a << 8);
                    break;
                }
                u32 region = w2 >> 12 & 3, data = write ? w1 & 255 : d;
                if (!gpr(a, gpr_limit) || !gpr(data, gpr_limit) || (write ? d != 0 : (w1 & 255) != 0) ||
                    (w2 & ~0xf800u) || (region != 0 && region != 3) || (w2 >> 15 & 1) != (u32)write ||
                    (!write && (d == 255 || (w2 & 0x3800u) != 0x3800u)) ||
                    (write && region == 3 && (w2 & 0x3800u) != 0x3800u) ||
                    (write && region == 0 && (w2 & 0x4800) && (a != 255 || w1 >> 8 != 3 || w2 != 0x8800)))
                    return reject(si, at, op, "stage buffer access mode");
                emit(&l, 0xefc0000000000000ull | pred | data | (u64)a << 8 | (u64)(w1 >> 8) << 20 |
                             (u64)(w2 >> 11 & 1) << 44 | (u64)(region == 3) << 45 |
                             (u64)(w2 >> 14 & 1) << 46 | (u64)write << 47 | (u64)write << 48);
                break;
            }
            case 0x326: /* IPA.PASS/CONSTANT at the default or centroid sample location (nvdisasm SM87).
                           Vulkan currently rasterizes one sample, so both locations are the pixel centre. */
                if (!gpr(d, gpr_limit) || a || w1 != 255 || (w2 & ~0x50ffu) != 0x000e0000)
                    return reject(si, at, op, "interpolation form");
                emit(&l, 0xe00000000ff0ff00ull | pred | d | (u64)(w2 & 255) << 30 |
                             (u64)(w2 >> 12 & 1) << 52 | (u64)(w2 >> 14 & 1) << 55);
                break;
            case 0x308:
            case 0xb08:
            case 0xd08: { /* MUFU.F32, or LG2/EX2.F16 with a selected half and a low-half result. */
                u32 operation = w2 >> 10 & 63, source;
                if (op == 0x308 && (w2 & 0x200)) {
                    source = w1 & 255;
                    if (!gpr(d, gpr_limit) || a || !gpr(source, gpr_limit) || (w1 & ~0xd00000ffu) ||
                        (w2 & ~0xfc00u) != 0x200 || (operation != 2 && operation != 3))
                        return reject(si, at, op, "half special float function form");
                    emit(&l, 0x5ca8000000000000ull | pred | TEMP_A | 2ull << 8 | 1ull << 10 |
                                 (u64)source << 20 | (u64)(w1 >> 28 & 1) << 41 |
                                 (u64)(w1 >> 31) << 45 | (u64)(w1 >> 30 & 1) << 49);
                    emit(&l, 0x5080000000000000ull | pred | TEMP_A | (u64)TEMP_A << 8 | (u64)operation << 20);
                    emit(&l, 0x5ca8000000000000ull | pred | d | 1ull << 8 | 2ull << 10 | (u64)TEMP_A << 20);
                    break;
                }
                if (!gpr(d, gpr_limit) || a || (w2 & ~0xfc00u) != (op == 0xd08 ? 0x08000000u : 0) ||
                    operation > 8 || operation == 6 || operation == 7 ||
                    !float_source(&l, pred, w0, w1, w2, 1, &source, gpr_limit))
                    return reject(si, at, op, "special float function form");
                emit(&l, 0x5080000000000000ull | pred | d | (u64)source << 8 | (u64)operation << 20);
                break;
            }
            case 0x387:
            case 0x983: { /* Scalar local spills use an immediate byte offset, private to each invocation. */
                int store = op == 0x387;
                u32 data = store ? w1 & 255 : d, offset = w1 >> 8;
                if (a != 255 || !gpr(data, gpr_limit) || (store ? d != 0 : (w1 & 255) != 0) ||
                    (offset & 3) || offset > 0x7fffff || w2 != (store ? 0x00100800u : 0x00300800u))
                    return reject(si, at, op, "local scalar spill form");
                emit(&l, (store ? 0xef50000000000000ull : 0xef40000000000000ull) | pred |
                             data | 255ull << 8 | (u64)offset << 20 | 4ull << 48);
                break;
            }
            case 0x388:
            case 0x984: { /* STS / LDS [Ra.Xn + offset] of 8 to 128 bits (73..75, Maxwell's sizes): 78..79 scale Ra by 4,
                             8 or 16, LDS with 91 adds a uniform register (32..39). The data in 32..39 for STS. As
                             PokeDinner lowers them */
                u32 width = w2 >> 9 & 7, stride = w2 >> 14 & 3, count = width == 6 ? 4 : width == 5 ? 2 : 1;
                int uniform_base = op == 0x984 && (w2 >> 27 & 1);
                u32 data = op == 0x388 ? w1 & 255 : d, base = uniform_base ? uniform_reg(w1 & 255, gpr_limit) : 255;
                if (width > 6 || (w2 & ~(uniform_base ? 0x08000000u : 0u)) != (width << 9 | stride << 14) ||
                    (op == 0x984 && !uniform_base && (w1 & 255)) || !base || !gpr(a, gpr_limit) ||
                    (data == 255 ? op != 0x388 : data >= gpr_limit || (data & (count - 1)) || data + count > gpr_limit))
                    return reject(si, at, op, "shared memory form");
                u32 address = a;
                if (stride && a != 255) {
                    emit(&l, 0x3848000000000000ull | pred | TEMP_A | (u64)a << 8 | (u64)(stride + 1) << 20);
                    address = TEMP_A;
                }
                if (base != 255) {
                    add(&l, pred, TEMP_A, address, base, 0);
                    address = TEMP_A;
                }
                if (data == 255 && count > 1) {
                    /* TEMP_A may hold the address. The four zero words must not overwrite it or UP_MASK. */
                    data = TEMP_B;
                    for (u32 i = 0; i < count; ++i) mov_imm(&l, pred, data + i, 0);
                }
                emit(&l, (op == 0x388 ? 0xef58000000000000ull : 0xef48000000000000ull) | pred | data |
                             (u64)address << 8 | (u64)(w1 >> 8) << 20 | (u64)width << 48);
                break;
            }
            case 0x381:
            case 0x981: { /* LDG.E: register pair, or uniform base plus a 32/64-bit register offset */
                u32 width = w2 >> 9 & 7, count = width == 6 ? 4 : width == 5 ? 2 : 1;
                u32 address = a, load_pred = w2 & 15;
                /* sizes as Maxwell's (73..75): U8, S8, U16, S16, 32, 64, 128 */
                if (width > 6 || d >= gpr_limit || (d & (count - 1)) || d + count > gpr_limit)
                    return reject(si, at, op, "global load data registers");
                if (op == 0x381) {
                    if (w1 & 255 || (w2 & ~0x2e00u) != 0x001e8100 || a >= gpr_limit || (a & 1))
                        return reject(si, at, op, "global load register address form");
                } else {
                    u32 base = uniform_reg(w1 & 63, gpr_limit);
                    int offset64 = (w2 >> 26 & 1) != 0;
                    if ((w1 & 0xc0) || !base || base == 255 || (base & 1) || !gpr(a, gpr_limit) ||
                        (w2 & ~0x04002e0fu) != 0x081e8100 ||
                        (load_pred && (guard != 7 || !(load_pred & 7))) ||
                        (offset64 && a != 255 && ((a & 1) || a + 1 >= gpr_limit)))
                        return reject(si, at, op, "global load uniform address/predicate form");
                    add(&l, pred, TEMP_A, base, a, 1ull << 47);
                    add(&l, pred, TEMP_B, base + 1, offset64 && a != 255 ? a + 1 : 255, 1ull << 43);
                    address = TEMP_A;
                }
                if (load_pred) {
                    for (u32 i = 0; i < count; ++i) mov_imm(&l, pred, d + i, 0);
                    pred = (u64)((7 - (load_pred & 7)) | (load_pred & 8)) << 16;
                }
                /* CTA-strong loads are conservatively ordered at device scope. */
                int strong = (w2 >> 13 & 15) == 5;
                if (strong) emit(&l, 0xef98000000000100ull | pred);
                emit(&l, 0xeed0000000000000ull | pred | d | (u64)address << 8 |
                             (u64)(w1 >> 8) << 20 | 1ull << 45 | (u64)width << 48);
                if (strong) emit(&l, 0xef98000000000100ull | pred);
                break;
            }
            case 0x386:
            case 0x986: { /* STG.E, 8/16/32/64/128 bits (Maxwell's sizes, 73..75), including a vector of zero words */
                u32 data = w1 & 255, width = w2 >> 9 & 7, address = a;
                if (d || width > 6) return reject(si, at, op, "global store width form");
                if (op == 0x386) {
                    if (a >= gpr_limit || (a & 1) || w2 != (0x0010a100u | width << 9))
                        return reject(si, at, op, "global store register address form");
                } else {
                    u32 base = uniform_reg(w2 & 63, gpr_limit);
                    int offset64 = (w2 >> 26 & 1) != 0;
                    if (!gpr(a, gpr_limit) || !base || base == 255 || (base & 1) ||
                        (w2 & ~(0x04000000u | 63u)) != (0x0810a100u | width << 9) ||
                        (offset64 && a != 255 && ((a & 1) || a + 1 >= gpr_limit)))
                        return reject(si, at, op, "global store uniform address form");
                    /* Preserve the low-word carry when adding an offset to a base above 4 GiB. */
                    add(&l, pred, TEMP_A, base, a, 1ull << 47);
                    add(&l, pred, TEMP_B, base + 1, offset64 && a != 255 ? a + 1 : 255, 1ull << 43);
                    address = TEMP_A;
                }
                u32 count = width < 4 ? 1 : 1u << (width - 4);
                if (data != 255 && (!gpr(data, gpr_limit) || (data & (count - 1)) || data + count > gpr_limit))
                    return reject(si, at, op, "global store data registers");
                if (data == 255 && count > 1) {
                    data = TEMP_C;
                    for (u32 i = 0; i < count; ++i) mov_imm(&l, pred, data + i, 0);
                }
                /* The accepted native form is CTA-strong. Order it conservatively at device scope, like LDG. */
                emit(&l, 0xef98000000000100ull | pred);
                /* The displacement is a signed 24-bit byte offset, as in Maxwell. Keep wide stores together. */
                emit(&l, 0xeed8000000000000ull | pred | data | (u64)address << 8 | (u64)(w1 >> 8) << 20 |
                             1ull << 45 | (u64)width << 48);
                emit(&l, 0xef98000000000100ull | pred);
                break;
            }
            case 0x945: /* BSSY: retain the target as metadata for bounded native jump tables. */
                if (guard != 7 || a || d >= 16 || w2 != 0x03800000 || depth == 16)
                    return reject(si, at, op, "reconvergence scope form");
                for (u32 i = 0; i < depth; ++i)
                    if (tokens[i] == d) return reject(si, at, op, "reconvergence token already active");
                tokens[depth++] = (u8)d;
                emit(&l, 0xe29000000007000full);
                break;
            case 0x941: /* BSYNC */
                if (guard != 7 || a || w1 || w2 != 0x03800000 || !depth || tokens[depth - 1] != d)
                    return reject(si, at, op, "reconvergence token mismatch");
                --depth;
                /* BSYNC waits and falls through. A Maxwell SYNC instead pops and jumps, so a BREAK followed by
                   a branch out of an inner scope would send a later outer BSYNC back into the skipped code. */
                emit(&l, 0x50b0000000070000ull);
                break;
            case 0x947: { /* BRA: the instruction guard, and the condition predicate of 87..90. With a PT guard the
                             condition becomes the guard. With both, the condition goes to the Z flag (SEL, then R2P.CC)
                             and the branch tests NE: the translator takes such a branch as conditional by its guard */
                u32 condition = w2 >> 23 & 15;
                if ((w0 >> 16) || (w2 & ~0x0783ffffu)) return reject(si, at, op, "branch predicate/modifier form");
                if (condition == 15) /* !PT: never taken */
                    emit(&l, 0xe240000000000000ull | pred);
                else if (condition == 7 || guard == 7)
                    emit(&l, 0xe24000000000000full | (condition == 7 ? pred : (u64)condition << 16));
                else {
                    mov_imm(&l, 7ull << 16, TEMP_C, 1);
                    emit(&l, 0x5ca0000000070000ull | TEMP_C | (u64)255 << 8 | (u64)TEMP_C << 20 |
                                 (u64)condition << 39); /* Z set when the condition is false */
                    emit(&l, 0x38f0000000070000ull | (u64)TEMP_C << 8 | 1ull << 20 | 1ull << 40);
                    emit(&l, 0xe240000000000005ull | pred);
                }
                break;
            }
            case 0x949:
            case 0x958: { /* BRX/BRXU: compare native PCs, then branch to their relocated starts. The lowerer's CC is
                             temporary within an instruction, so ordinary predicates stay intact. */
                u32 source = op == 0x958 ? uniform_reg(a, gpr_limit) : a;
                if (guard != 7 || d || (op == 0x958 ? !source : !gpr(source, gpr_limit)) ||
                    (w2 & ~0x3ffffu) != (op == 0x958 ? 0x0b800000u : 0x03800000u) || functions)
                    return reject(si, at, op, "indirect branch form");
                u32 base = (u32)branch_target(code, at);
                for (u32 target = 0; target < bytes; target += 16) {
                    emit(&l, 0x1c10000000070000ull | TEMP_A | (u64)source << 8 | (u64)(base - target) << 20);
                    emit(&l, 0xe240000000070002ull); /* BRA.EQ on the comparison's Z flag */
                }
                /* A PC outside the program ends the invocation, as in the Maxwell indirect dispatcher. */
                emit(&l, 0xe30000000007000full);
                break;
            }
            case 0x944:
                if (loop_break_call(code, at, bytes)) {
                    emit(&l, 0xe24000000000000full | pred);
                } else {
                    /* A call may sit in an open reconvergence scope: the translator keeps calls on a stack of their
                       own, and the callee starts with no open token, so it can neither pop nor leave one of the
                       caller's (its RET requires depth 0). */
                    if (!functions || at >= main_end || (w0 >> 16) || (w2 & ~0x3ffffu) != 0x03c00000)
                        return reject(si, at, op, "unverified native call linkage");
                    emit(&l, 0xe26000000000000full | pred);
                }
                break;
            case 0x950: /* A verified leaf return preserves the native link value, but uses the Maxwell call stack. */
                if (!functions || at < main_end || depth || d || a + 1 >= gpr_limit ||
                    (w2 & ~0x3ffffu) != 0x03c00000)
                    return reject(si, at, op, "unverified native return linkage");
                emit(&l, 0xe32000000000000full | pred);
                if (guard == 7 && at + 16 == bytes) {
                    if (l.full) return reject(si, at, op, "lowered program capacity");
                    return l.size;
                }
                break;
            case 0x95b: /* KILL: fragment discard, with the native instruction's execution predicate. */
                if ((w0 >> 16) || w1 || w2 != 0x03800000) return reject(si, at, op, "fragment discard form");
                emit(&l, 0xe33000000000000full | pred);
                break;
            case 0x94d: { /* EXIT's flow predicate is ANDed with its execution guard, without changing either. */
                u32 condition = w2 >> 23 & 15;
                if ((w0 >> 16) || w1 || (w2 & ~0x07800000u))
                    return reject(si, at, op, "conditional exit form");
                if (condition == 15)
                    emit(&l, 0x50b0000000070000ull);
                else if (condition == 7 || guard == 7)
                    emit(&l, 0xe30000000000000full | (condition == 7 ? pred : (u64)condition << 16));
                else {
                    u32 skip = next_pc(&l);
                    emit(&l, 0xe24000000000000full | (u64)(guard ^ 8) << 16);
                    emit(&l, 0xe30000000000000full | (u64)condition << 16);
                    if (!l.full)
                        snx_st_le64(l.out + skip, snx_ld_le64(l.out + skip) |
                                                  (u64)(next_pc(&l) - skip - 8) << 20);
                }
                if (guard == 7 && condition == 7 && at + 16 == bytes) {
                    if (l.full) return reject(si, at, op, "lowered program capacity");
                    return l.size;
                }
                break;
            }
            case 0x2a5: { /* UIMAD.WIDE.U32 URd:URd+1 = URa * URb + URc:URc+1: IMUL's low and high words, then a 64-bit
                             addition with Maxwell's carry flag (.CC, then .X) */
                u32 low = uniform_reg(d, gpr_limit), high = uniform_reg(d + 1, gpr_limit), x = uniform_reg(a, gpr_limit), y = uniform_reg(w1 & 255, gpr_limit);
                u32 c = w2 & 255, c_low = uniform_reg(c, gpr_limit), c_high = c == 63 ? 255 : uniform_reg(c + 1, gpr_limit);
                if (guard != 7 || (d & 1) || d + 1 >= uniform_count(gpr_limit) || !low || !high || !x || !y || (w1 & ~0x3fu) ||
                    !c_low || !c_high || (c != 63 && (c & 1)) || (w2 & ~0xffu) != 0x0f8e0000)
                    return reject(si, at, op, "uniform wide multiply-add form");
                emit(&l, 0x5c38000000000000ull | pred | TEMP_A | (u64)x << 8 | (u64)y << 20);
                emit(&l, 0x5c38000000000000ull | pred | TEMP_B | (u64)x << 8 | (u64)y << 20 | 1ull << 39);
                add(&l, pred, TEMP_A, TEMP_A, c_low, 1ull << 47);
                add(&l, pred, TEMP_B, TEMP_B, c_high, 1ull << 43);
                emit(&l, 0x5c98078000000000ull | pred | low | (u64)TEMP_A << 20);
                emit(&l, 0x5c98078000000000ull | pred | high | (u64)TEMP_B << 20);
                break;
            }
            case 0x942: { /* BREAK Bn: the threads at it leave the convergence barrier, and the BRA after it moves
                             them. The translation takes reconvergence from the structured control flow, so the BREAK
                             changes no value there. Only an open barrier is taken. */
                int open = 0;
                for (u32 i = 0; i < depth; ++i) open |= tokens[i] == d;
                if (!open || a || w1 || (w2 & ~0x07800000u)) return reject(si, at, op, "convergence barrier break form");
                emit(&l, 0x50b0000000000000ull | pred);
                break;
            }
            case 0x300:
            case 0xd00: { /* FLO of a register or UR, with signedness, input inversion and .SH */
                u32 source = op == 0xd00 ? uniform_reg(w1 & 63, gpr_limit) : w1 & 255;
                if (!gpr(d, gpr_limit) || a || (op == 0xd00 ? !source || (w1 & ~0x8000003fu) : !gpr(source, gpr_limit) || (w1 & ~0x800000ffu)) ||
                    (w2 & ~0x600u) != (op == 0xd00 ? 0x080e0000u : 0x000e0000u))
                    return reject(si, at, op, "find leading one form");
                emit(&l, 0x5c30000000000000ull | pred | d | (u64)source << 20 | (u64)(w1 >> 31) << 40 |
                             (u64)(w2 >> 10 & 1) << 41 | (u64)(w2 >> 9 & 1) << 48);
                break;
            }
            case 0x81a: /* SGXT of the low 0..32 bits, unsigned or sign-extended (73). */
                if (!gpr(d, gpr_limit) || !gpr(a, gpr_limit) || w1 > 32 || (w2 & ~0x200u))
                    return reject(si, at, op, "sign extension form");
                emit(&l, 0x3800000000000000ull | pred | d | (u64)a << 8 | (u64)w1 << 28 |
                             (u64)(w2 >> 9 & 1) << 48);
                break;
            case 0x309:   /* POPC of a register */
            case 0xd09: { /* or of a uniform register */
                u32 source = op == 0xd09 ? uniform_reg(w1, gpr_limit) : w1;
                if (!gpr(d, gpr_limit) || a || (op == 0xd09 ? !source || (w1 & ~0x3fu) : !gpr(w1, gpr_limit)) ||
                    w2 != (op == 0xd09 ? 0x08000000u : 0))
                    return reject(si, at, op, "population count form");
                emit(&l, 0x5c08000000000000ull | pred | d | (u64)source << 20);
                break;
            }
            case 0x9a8: { /* ATOMG on a 32-bit type, at a 64-bit uniform base (bits 64..69, 64-bit at 72) plus an
                             optional register (64-bit at 70) and a 24-bit offset (NAK: data 32..39, type 73..75,
                             operation 87..90, the uniform base used at 91). Maxwell's ATOM.E takes the operation and
                             type in the same numbers, and a 20-bit offset */
                u32 base = uniform_reg(w2 & 63, gpr_limit), type = w2 >> 9 & 7, operation = w2 >> 23 & 15, data = w1 & 255;
                i32 offset = (i32)(w1 & 0xffffff00u) / 256;
                int pair = (w2 >> 6 & 1) && a != 255;
                if (!gpr(d, gpr_limit) || !gpr(a, gpr_limit) || !gpr(data, gpr_limit) || !base || base == 255 || (base & 1) ||
                    (w2 & ~0x07f00e3fu) != 0x080ee140 || operation > 8 || (type != 0 && type != 1 && type != 3) ||
                    (pair && ((a & 1) || a + 1 >= gpr_limit)) || offset < -0x80000 || offset > 0x7ffff)
                    return reject(si, at, op, "global atomic form");
                add(&l, pred, TEMP_A, base, a, 1ull << 47);
                add(&l, pred, TEMP_B, base + 1, pair ? a + 1 : 255, 1ull << 43);
                emit(&l, 0xed00000000000000ull | pred | d | (u64)TEMP_A << 8 | (u64)data << 20 |
                             (u64)((u32)offset & 0xfffff) << 28 | 1ull << 48 | (u64)type << 49 | (u64)operation << 52);
                break;
            }
            case 0x3a8: { /* ATOMG on a 32-bit type at a 64-bit register address (72) plus a 24-bit offset, its type and
                             operation as 0x9a8's. Maxwell's ATOM.E reads the same register pair */
                u32 type = w2 >> 9 & 7, operation = w2 >> 23 & 15, data = w1 & 255;
                i32 offset = (i32)(w1 & 0xffffff00u) / 256;
                if (!gpr(d, gpr_limit) || (a & 1) || a + 1 >= gpr_limit || !gpr(data, gpr_limit) || (w2 & ~0x07f00e00u) != 0x000ee100 ||
                    operation > 8 || (type != 0 && type != 1 && type != 3) || offset < -0x80000 || offset > 0x7ffff)
                    return reject(si, at, op, "global atomic register address form");
                emit(&l, 0xed00000000000000ull | pred | d | (u64)a << 8 | (u64)data << 20 |
                             (u64)((u32)offset & 0xfffff) << 28 | 1ull << 48 | (u64)type << 49 | (u64)operation << 52);
                break;
            }
            case 0x98e: { /* RED: ATOMG with no result, at a register pair or a uniform 64-bit base.
                             Maxwell's RED.E takes the data in 0..7, the type at 20 and the operation at 23. */
                u32 type = w2 >> 9 & 7, operation = w2 >> 23 & 15, data = w1 & 255, address = a;
                i32 offset = (i32)(w1 & 0xffffff00u) / 256;
                if (w2 & (1u << 27)) {
                    address = uniform_reg(w2 & 63, gpr_limit);
                    operation &= 7; /* Bit 90 selects the uniform address mode, not the reduction operation. */
                    if (a != 255 || !address || address == 255 || (address & 1) ||
                        (w2 & ~0x0380003fu) != 0x0c10e100)
                        return reject(si, at, op, "global reduction uniform address form");
                } else if ((a & 1) || a + 1 >= gpr_limit || (w2 & ~0x07f00e00u) != 0x0000e100) {
                    return reject(si, at, op, "global reduction register address form");
                }
                if (d || !gpr(data, gpr_limit) || operation > 7 || (type != 0 && type != 1 && type != 3) ||
                    offset < -0x80000 || offset > 0x7ffff)
                    return reject(si, at, op, "global reduction form");
                emit(&l, 0xebf8000000000000ull | pred | data | (u64)address << 8 | (u64)type << 20 |
                             (u64)operation << 23 | (u64)((u32)offset & 0xfffff) << 28 | 1ull << 48);
                break;
            }
            case 0x827: /* IMAD.HI.U32 with an immediate multiplier and a zero addend */
                if (!gpr(d, gpr_limit) || !gpr(a, gpr_limit) || w2 != 0x078e00ff) return reject(si, at, op, "high multiply form");
                mov_imm(&l, pred, TEMP_C, w1);
                emit(&l, 0x5c38000000000000ull | pred | d | (u64)a << 8 | (u64)TEMP_C << 20 | 1ull << 39);
                break;
            default: return reject(si, at, op, "unsupported instruction");
        }
        if (up & UP_WRITE)
            emit(&l, 0x38e8000000070000ull | UP_MASK | (u64)255 << 8 | (u64)0x7f << 20);
        if (up & UP_SWAP)
            emit(&l, 0x38f0000000070000ull | (u64)UP_SAVE << 8 | (u64)0x7f << 20);
        if (l.full) return reject(si, at, op, "lowered program capacity");
    }
    return reject(si, bytes, 0, "missing terminating exit");
}
/* Most programs keep their existing normalized words. Only a refused program tries the larger vector layout,
   whose smaller uniform bank and absent matrix scratch are checked by the same lowering. */
static u32 lower_layouts(const u8* code, u32 bytes, u8* out, u32 capacity, ShaderInfo* si, u32* positions,
                         const NativeFunction* functions, u32 main_end, int up_live, u32 hook) {
    u32 size = lower_program(code, bytes, out, capacity, si, positions, functions, main_end, up_live, DEFAULT_GPR_LIMIT,
                             hook);
    if (size) return size;
    si->unsupported = 0;
    si->first_unsupported[0] = 0;
    return lower_program(code, bytes, out, capacity, si, positions, functions, main_end, up_live, WIDE_GPR_LIMIT, hook);
}


/* Native branches use byte displacements from the following 16-byte instruction. Maxwell branch destinations
   must instead refer to the emitted instructions, including inserted scheduling words and expanded operations.
   The displacement starts at bit 34 (NAK's set_rel_offset(34..)), in 4-byte words, so the first word holds it in
   bytes with bits 0 and 1 cleared. Bit 0 is BRA's .U, a hint that the condition is uniform across the warp, which
   NVIDIA's compiler sets: it doesn't change where the branch goes. */
static i64 branch_target(const u8* code, u32 at) {
    u32 op = snx_ld_le32(code + at) & 0xfff, low = snx_ld_le32(code + at + 4) & ~3u;
    u64 raw = low;
    u32 sign = 31;
    if (op != 0x945) {
        raw |= (u64)(snx_ld_le32(code + at + 8) & 0x3ffff) << 32;
        sign = 49;
    }
    i64 relative = (i64)(raw & ((1ull << sign) - 1)) - (i64)(raw & (1ull << sign));
    return (i64)at + 16 + relative;
}

static int native_call_link(const u8* code, u32 at, u32* setup, u32* link) {
    for (u32 back = 1; back <= 6 && back <= at / 16; ++back) {
        u32 pc = at - back * 16, w0 = snx_ld_le32(code + pc), op = w0 & 0xfff;
        if ((w0 & 0xff00ffffu) == 0x7802 && snx_ld_le32(code + pc + 4) == at + 16 &&
            snx_ld_le32(code + pc + 8) == 0xf00 && (w0 >> 16 & 255) < WIDE_GPR_LIMIT) {
            *setup = pc;
            *link = w0 >> 16 & 255;
            return 1;
        }
        if (op == 0x941 || op == 0x944 || op == 0x945 || op == 0x947 || op == 0x94d || op == 0x950) break;
    }
    return 0;
}

static u32 native_leaf_end(const u8* code, u32 bytes, u32 start, NativeFunction* functions, ShaderInfo* si,
                           u32* needed) {
    u32 furthest = start;
    for (u32 at = start; at < bytes; at += 16) {
        u32 w0 = snx_ld_le32(code + at), op = w0 & 0xfff;
        if (op == 0x944 && !loop_break_call(code, at, bytes))
            return reject(si, at, op, "nested native calls are not supported");
        if (op == 0x945 || op == 0x947 || op == 0x944) {
            i64 target = branch_target(code, at);
            if (needed && target >= bytes && target <= SHADER_AMPERE_NATIVE_MAX && !(target & 15)) {
                *needed = (u32)target + 16;
                return 0;
            }
            if (target < start || target >= bytes || (target & 15))
                return reject(si, at, op, "branch leaves native leaf function");
            if (functions) functions[target / 16].flags |= NATIVE_TARGET;
            if ((u32)target > furthest) furthest = (u32)target;
        }
        if (op == 0x950) {
            if ((w0 >> 16 & 255) || (w0 >> 24) + 1 >= WIDE_GPR_LIMIT ||
                (snx_ld_le32(code + at + 8) & ~0x3ffffu) != 0x03c00000 || branch_target(code, at) != 0)
                return reject(si, at, op, "native leaf return mode");
            if ((w0 >> 12 & 15) == 7 && at >= furthest) return at + 16;
        }
    }
    if (needed) *needed = bytes + 16;
    return reject(si, start, 0x944, "missing native leaf return");
}

static u32 native_functions(const u8* code, u32 bytes, u32 main_end, NativeFunction* functions,
                            NativeCall* calls, ShaderInfo* si) {
    for (u32 at = 0; at < main_end; at += 16) {
        functions[at / 16].flags |= NATIVE_LIVE;
        u32 op = snx_ld_le32(code + at) & 0xfff;
        if (op != 0x945 && op != 0x947 && op != 0x944) continue;
        i64 target = branch_target(code, at);
        if (target < 0 || target >= bytes || (target & 15))
            return reject(si, at, op, "native call or branch target");
        functions[target / 16].flags |= NATIVE_TARGET;
    }
    u32 extent = main_end, count = 0;
    for (u32 at = 0; at < main_end; at += 16) {
        u32 w0 = snx_ld_le32(code + at);
        if ((w0 & 0xfff) != 0x944 || loop_break_call(code, at, bytes)) continue;
        u32 setup, link;
        if ((w0 >> 16) || (snx_ld_le32(code + at + 8) & ~0x3ffffu) != 0x03c00000 ||
            !native_call_link(code, at, &setup, &link))
            return reject(si, at, 0x944, "native call requires an explicit return address");
        for (u32 pc = setup + 16; pc <= at; pc += 16)
            if (functions[pc / 16].flags & NATIVE_TARGET)
                return reject(si, at, 0x944, "branch bypasses native link setup");
        calls[count++] = (NativeCall){at, setup, link};
        u32 target = (u32)branch_target(code, at);
        NativeFunction* f = &functions[target / 16];
        if (f->end) {
            if (f->link != link) return reject(si, at, 0x944, "inconsistent native leaf linkage");
            continue;
        }
        u32 end = native_leaf_end(code, bytes, target, functions, si, NULL);
        if (!end) return 0;
        for (u32 pc = target; pc < end; pc += 16) {
            if (functions[pc / 16].flags & NATIVE_LIVE)
                return reject(si, at, 0x944, "overlapping native function bodies");
            functions[pc / 16].flags |= NATIVE_LIVE;
        }
        f->end = end;
        f->link = (u8)link;
        if (end > extent) extent = end;
    }
    return extent;
}

static int keeps_register(const u8* out, u32 begin, u32 end, u32 reg) {
    for (u32 pc = begin; pc < end; pc += 8) {
        if (!IDX_OK(pc / 8)) continue;
        u64 word = snx_ld_le64(out + pc);
        if (shader_writes_reg(word, decode(word)->id, reg)) return 0;
    }
    return 1;
}

static int preserves_setup(const u8* out, const u32* positions, const NativeFunction* functions,
                           u32 setup, u32 use, u32 reg) {
    for (u32 at = setup + 16; at <= use; at += 16)
        if (functions[at / 16].flags & NATIVE_TARGET) return 0;
    return keeps_register(out, positions[(setup + 16) / 16], positions[use / 16], reg);
}

static int native_links_valid(const u8* code, u32 extent, const u8* out, const u32* positions,
                              const NativeFunction* functions, const NativeCall* calls, u32 count, ShaderInfo* si) {
    for (u32 i = 0; i < count; ++i)
        if (!preserves_setup(out, positions, functions, calls[i].setup, calls[i].at, calls[i].link))
            return reject(si, calls[i].at, 0x944, "native return address overwritten before call");
    for (u32 start = 0; start < extent; start += 16) {
        const NativeFunction* f = &functions[start / 16];
        if (!f->end) continue;
        if (!keeps_register(out, positions[start / 16], positions[(f->end - 16) / 16] + 8, f->link))
            return reject(si, start, 0x944, "callee overwrites native return link");
        for (u32 at = start; at < f->end; at += 16) {
            if ((snx_ld_le32(code + at) & 0xfff) != 0x950) continue;
            u32 reg = snx_ld_le32(code + at) >> 24, copy = 0, high = 0;
            for (u32 back = 1; back <= (at - start) / 16; ++back) {
                u32 pc = at - back * 16, w0 = snx_ld_le32(code + pc), w1 = snx_ld_le32(code + pc + 4);
                u32 w2 = snx_ld_le32(code + pc + 8);
                int move_link = w0 == (reg << 16 | 0x7202) && w1 == f->link && w2 == 0xf00;
                int imad_link = w0 == (reg << 16 | 0xff007224u) && w1 == 255 &&
                                (w2 & ~0x200u) == (0x078e0000u | f->link);
                int zero = w0 == ((reg + 1) << 16 | 0x7802) && !w1 && w2 == 0xf00;
                int imad_zero = w0 == ((reg + 1) << 16 | 0xff007424u) && !w1 &&
                                (w2 & ~0x200u) == 0x078e00ff;
                if ((move_link || imad_link) && !copy) copy = pc;
                if ((zero || imad_zero) && !high) high = pc;
                if (high && (copy || reg == f->link)) break;
            }
            if ((reg != f->link && (!copy || !preserves_setup(out, positions, functions, copy, at, reg))) ||
                !high || !preserves_setup(out, positions, functions, high, at, reg + 1))
                return reject(si, at, 0x950, "return requires a preserved zero-extended native link");
        }
    }
    return 1;
}

static u32 native_main_end(const u8* code, u32 bytes, ShaderInfo* si, u32* call_count, int* flow, u32* needed) {
    u32 main_end = 0, furthest = 0;
    int indirect = 0;
    for (u32 at = 0; at < bytes; at += 16) {
        u32 w0 = snx_ld_le32(code + at), op = w0 & 0xfff;
        if (indirect && main_end && !w0 && !snx_ld_le32(code + at + 4) && !snx_ld_le32(code + at + 8)) break;
        if (op == 0x949 || op == 0x958) indirect = 1;
        if (op == 0x945 || op == 0x947 || (op == 0x944 && loop_break_call(code, at, bytes))) {
            i64 target = branch_target(code, at);
            if (needed && target >= bytes && target <= SHADER_AMPERE_NATIVE_MAX && !(target & 15)) {
                *needed = (u32)target + 16;
                return 0;
            }
            if (target < 0 || target >= bytes || (target & 15))
                return reject(si, at, op, "branch target outside instruction boundaries");
            if ((u32)target > furthest) furthest = (u32)target;
            *flow = 1;
        } else if (op == 0x941 || op == 0x949 || op == 0x958) *flow = 1;
        if (op == 0x944 && !loop_break_call(code, at, bytes)) {
            ++*call_count;
            *flow = 1;
        }
        if (at >= furthest && w0 == 0x0000794d && !snx_ld_le32(code + at + 4) &&
            snx_ld_le32(code + at + 8) == 0x03800000) {
            main_end = at + 16;
            if (!indirect) break;
        }
    }
    if (!main_end) {
        if (needed) *needed = bytes + 16;
        return reject(si, bytes, 0, "missing terminating exit");
    }
    return main_end;
}

u32 shader_ampere_read_extent(const u8* code, u32 bytes) {
    if (!code || bytes < 16 || (bytes & 15)) return bytes;
    ShaderInfo si = {0};
    u32 calls = 0, needed = 0;
    int flow = 0;
    u32 end = native_main_end(code, bytes, &si, &calls, &flow, &needed);
    if (!end) return needed ? needed : bytes;
    u32 extent = end;
    for (u32 at = 0; calls && at < end; at += 16) {
        if ((snx_ld_le32(code + at) & 0xfff) != 0x944 || loop_break_call(code, at, bytes)) continue;
        i64 target = branch_target(code, at);
        if (target < 0 || target >= SHADER_AMPERE_NATIVE_MAX || (target & 15)) return bytes;
        if (target >= bytes) return (u32)target + 16;
        u32 leaf = native_leaf_end(code, bytes, (u32)target, NULL, &si, &needed);
        if (!leaf) return needed ? needed : bytes;
        if (leaf > extent) extent = leaf;
    }
    /* the body of an exit hook (exit_hook_prologue), which only LEPC's address reaches */
    for (u32 at = 0; at < end; at += 16)
        if ((snx_ld_le32(code + at) & 0xfff) == 0x34e) {
            u32 hook = exit_hook_prologue(code, end, at);
            if (hook && hook + 48 > extent) extent = hook + 48;
            break;
        }
    return extent;
}

u32 shader_ampere_lower(const u8* code, u32 bytes, u8* out, u32 capacity, ShaderInfo* si, u32* consumed) {
    if (consumed) *consumed = 0;
    if (!code || !out || capacity < 80 || bytes < 16 || (bytes & 15)) return reject(si, 0, 0, "invalid program bounds");
    u32 call_count = 0;
    int flow = 0;
    u32 main_end = native_main_end(code, bytes, si, &call_count, &flow, NULL);
    if (!main_end) return 0;
    u32 count = call_count ? bytes / 16 : main_end / 16;
    size_t workspace = (size_t)count * (sizeof(u32) + (call_count ? sizeof(NativeFunction) : 0)) +
                       (size_t)call_count * sizeof(NativeCall);
    u32* positions = !flow ? NULL : call_count ? calloc(1, workspace) : malloc(workspace);
    if (flow && !positions) return reject(si, 0, 0, "branch relocation allocation");
    NativeFunction* functions = call_count ? (NativeFunction*)(positions + count) : NULL;
    NativeCall* calls = call_count ? (NativeCall*)(functions + count) : NULL;
    u32 extent = call_count ? native_functions(code, bytes, main_end, functions, calls, si) : main_end;
    u32 hook = exit_hook_at(code, bytes, main_end);
    u32 size = extent ? lower_layouts(code, extent, out, capacity, si, positions, functions, main_end, 0, hook) : 0;
    if (size && call_count && !native_links_valid(code, extent, out, positions, functions, calls, call_count, si)) size = 0;
    for (u32 at = 0; size && flow && at < extent; at += 16) {
        if (functions && !(functions[at / 16].flags & NATIVE_LIVE)) continue;
        u32 op = snx_ld_le32(code + at) & 0xfff;
        int indirect = op == 0x949 || op == 0x958;
        if (op != 0x945 && op != 0x947 && op != 0x944 && !indirect) continue;
        u32 pc = positions[at / 16], targets = indirect ? extent / 16 : 1;
        for (u32 t = 0; t < targets; ++t) {
            u32 target = indirect ? t * 16 : (u32)branch_target(code, at);
            /* Past the words staging a branch condition: every flow word emitted here has 0xe2 on top. */
            while (pc + 8 <= size && (snx_ld_le64(out + pc) >> 56) != 0xe2) pc += 8;
            i64 relative = (i64)positions[target / 16] - pc - 8;
            if (relative < -0x800000 || relative > 0x7fffff) {
                size = reject(si, at, op, "lowered branch displacement");
                break;
            }
            u64 word = snx_ld_le64(out + pc);
            snx_st_le64(out + pc, word | ((u64)relative & 0xffffff) << 20);
            pc += 8;
        }
    }
    free(positions);
    if (size && consumed) *consumed = hook == ~0u ? extent : snx_max_u32(extent, exit_hook_prologue(code, main_end, hook) + 48);
    return size;
}

u32 shader_ampere_diagnose(const u8* code, u32 bytes,
                           void (*report)(void* ctx, u32 offset, const char* reason), void* ctx) {
    ShaderInfo bounds = {0};
    u32 calls = 0, unchecked = 0;
    int flow = 0, up_live = 0;
    if (!code || bytes < 16 || (bytes & 15)) return ~0u;
    u32 full = bytes;
    bytes = native_main_end(code, bytes, &bounds, &calls, &flow, NULL);
    if (!bytes) return ~0u;
    u32 hook = exit_hook_at(code, full, bytes);
    for (u32 at = 0; at + 16 <= bytes; at += 16)
        up_live |= uniform_predicates(snx_ld_le32(code + at), snx_ld_le32(code + at + 8)) != 0;
    for (u32 at = 0; at + 16 <= bytes; at += 16) {
        u32 op = snx_ld_le32(code + at) & 0xfff;
        if (hook != ~0u && (at == hook || at == hook + 64)) continue; /* checked with the whole program */
        switch (op) {
            case 0x941: case 0x942: case 0x944: case 0x945: case 0x947: case 0x949: case 0x950: case 0x958:
                ++unchecked;
                continue;
        }
        u8 native[32] = {0}, scratch[4096];
        memcpy(native, code + at, 16);
        snx_st_le32(native + 16, 0x794d);
        snx_st_le32(native + 24, 0x03800000);
        ShaderInfo si = {0};
        if (!lower_layouts(native, sizeof native, scratch, sizeof scratch, &si, NULL, NULL, 0, up_live, ~0u)) {
            char reason[sizeof si.first_unsupported];
            const char* why = strchr(si.first_unsupported, ':');
            snprintf(reason, sizeof reason, "Ampere opcode %03x at +%x:%s", op, at, why ? why + 1 : " local check failed");
            report(ctx, at, reason);
        }
    }
    return unchecked;
}
