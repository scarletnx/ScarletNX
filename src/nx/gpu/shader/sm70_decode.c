/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   The operand layout is the inverse of Mesa's NAK encoder (src/nouveau/compiler/nak/sm70_encode.rs):
   Copyright © 2022 Collabora, Ltd.
   SPDX-License-Identifier: MIT */
/* The SM70+ instruction decoder (sm70.h) and the search for programs in shader memory. The opcodes are
   sm70_opcodes.def. Two switches made from it match an instruction: the 12-bit opcodes first, then the 9-bit ALU
   opcodes with the forms each takes. A code that two rows share is a duplicate case, which the compiler refuses. */
#include "nx/gpu/shader/sm70.h"
#include "common/utilities/endian.h"
#include "common/utilities/bits.h"
#include <stdio.h>
#include <stdlib.h>

const Sm70OpInfo sm70_ops[SM70__COUNT] = {
    {0, SM70_K_FIX, 0, "(unknown)"},
#define SM70_OP(name, code, kind, forms, display) {code, kind, forms, display},
#include "nx/gpu/shader/sm70_opcodes.def"
#undef SM70_OP
};

/* the row of a 12-bit opcode, or SM70_NONE */
static int match_fix(u32 op12) {
    switch (op12) {
/* rows of the other kinds get a case past 12 bits, so that each row is one case */
#define SM70_OP(name, code, kind, forms, display)                                                                      \
    case (kind) == SM70_K_FIX ? (code): 0x10000 + SM70_##name : return SM70_##name;
#include "nx/gpu/shader/sm70_opcodes.def"
#undef SM70_OP
        default: return SM70_NONE;
    }
}
/* the row of a 9-bit ALU opcode, or SM70_NONE */
static int match_alu(u32 op9) {
    switch (op9) {
#define SM70_OP(name, code, kind, forms, display)                                                                      \
    case (kind) != SM70_K_FIX ? (code): 0x10000 + SM70_##name : return SM70_##name;
#include "nx/gpu/shader/sm70_opcodes.def"
#undef SM70_OP
        default: return SM70_NONE;
    }
}

static Sm70Src reg_src(const Sm70Insn* in, unsigned at, unsigned neg, unsigned abs, int uniform) {
    Sm70Src s = {0};
    s.kind = uniform ? SM70_SRC_UREG : SM70_SRC_REG;
    s.reg = (u8)sm70_field(in, at, uniform ? 6 : 8);
    s.neg = (u8)sm70_field(in, neg, 1);
    s.abs = (u8)sm70_field(in, abs, 1);
    return s;
}
static Sm70Src imm_src(const Sm70Insn* in) {
    Sm70Src s = {0};
    s.kind = SM70_SRC_IMM;
    s.value = sm70_field(in, 32, 32);
    return s;
}
/* bits 32-58: the offset at 38-53, and the bank at 54-58 or, bindless (bit 91), the uniform register at 32-37 */
static Sm70Src cbuf_src(const Sm70Insn* in) {
    Sm70Src s = {0};
    s.kind = SM70_SRC_CBUF;
    s.value = sm70_field(in, 38, 16);
    s.bindless = (u8)sm70_field(in, 91, 1);
    if (s.bindless)
        s.reg = (u8)sm70_field(in, 32, 6);
    else
        s.bank = (u8)sm70_field(in, 54, 5);
    s.abs = (u8)sm70_field(in, 62, 1);
    s.neg = (u8)sm70_field(in, 63, 1);
    return s;
}

int sm70_decode(u64 lo, u64 hi, Sm70Insn* out) {
    Sm70Insn in = {0};
    in.lo = lo;
    in.hi = hi;
    in.form = (u8)sm70_field(&in, 9, 3);
    in.pred = (u8)sm70_field(&in, 12, 3);
    in.pred_not = (u8)sm70_field(&in, 15, 1);
    in.dst = (u8)sm70_field(&in, 16, 8);
    in.ctrl.stall = (u8)sm70_field(&in, 105, 4);
    in.ctrl.yield = (u8)sm70_field(&in, 109, 1);
    in.ctrl.wr_bar = (u8)sm70_field(&in, 110, 3);
    in.ctrl.rd_bar = (u8)sm70_field(&in, 113, 3);
    in.ctrl.wait_mask = (u8)sm70_field(&in, 116, 6);
    in.ctrl.reuse = (u8)sm70_field(&in, 122, 4);
    int op = match_fix(sm70_field(&in, 0, 12));
    if (op == SM70_NONE) {
        op = match_alu(sm70_field(&in, 0, 9));
        if (op != SM70_NONE && !(sm70_ops[op].forms & (1u << in.form))) op = SM70_NONE;
    }
    in.op = (u16)op;
    if (op != SM70_NONE && sm70_ops[op].kind == SM70_K_UALU) {
        /* uniform registers throughout: a at 24, b and c at 32 or 64 */
        in.dst = (u8)sm70_field(&in, 16, 6);
        in.a = reg_src(&in, 24, 72, 73, 1);
        switch (in.form) {
            case 1:
                in.b = reg_src(&in, 32, 63, 62, 1);
                in.c = reg_src(&in, 64, 75, 74, 1);
                break;
            case 2:
                in.b = reg_src(&in, 64, 75, 74, 1);
                in.c = imm_src(&in);
                break;
            case 6: /* UMOV of a uniform register */
                in.b = reg_src(&in, 32, 63, 62, 1);
                in.c = reg_src(&in, 64, 75, 74, 1);
                break;
            default: /* 4 */
                in.b = imm_src(&in);
                in.c = reg_src(&in, 64, 75, 74, 1);
                break;
        }
    } else if (op != SM70_NONE && sm70_ops[op].kind == SM70_K_ALU) {
        in.a = reg_src(&in, 24, 72, 73, 0);
        Sm70Src r64 = reg_src(&in, 64, 75, 74, 0);
        switch (in.form) {
            case 1:
                in.b = reg_src(&in, 32, 63, 62, 0);
                in.c = r64;
                break;
            case 2:
                in.b = r64;
                in.c = imm_src(&in);
                break;
            case 3:
                in.b = r64;
                in.c = cbuf_src(&in);
                break;
            case 4:
                in.b = imm_src(&in);
                in.c = r64;
                break;
            case 5:
                in.b = cbuf_src(&in);
                in.c = r64;
                break;
            case 6:
                in.b = reg_src(&in, 32, 63, 62, 1);
                in.c = r64;
                break;
            default: /* 7 */
                in.b = r64;
                in.c = reg_src(&in, 32, 63, 62, 1);
                break;
        }
    }
    *out = in;
    return op != SM70_NONE;
}

u32 sm70_branch_target(const Sm70Insn* in, u32 idx) {
    /* BRA's offset is bits 34-81 and BSSY's bits 34-63, in units of 4 bytes, signed */
    int op = in->op;
    unsigned bits = op == SM70_BRA || op == SM70_UBRA ? 48 : 30;
    u64 v = (in->lo >> 34) & ((1ull << 30) - 1);
    if (bits == 48) v |= (in->hi & 0x3ffffull) << 30;
    u64 sign = 1ull << (bits - 1);
    u64 next = (u64)idx + 1;
    u64 target;
    if (v & sign) {
        u64 back = ((~v + 1) & ((sign << 1) - 1)) / 4; /* instructions back */
        if (back > next) return 0xffffffffu;
        target = next - back;
    } else
        target = next + v / 4;
    return target >= 0xffffffffull ? 0xffffffffu : (u32)target;
}

void sm70_load(const u8* code, u32 count, u64* words) {
    for (u32 i = 0; i < 2 * count; i++) words[i] = snx_ld_le64(code + 8 * (size_t)i);
}

int sm70_sph_stage(u32 sph0) {
    u32 type = sph0 & 31, version = (sph0 >> 5) & 31, shader = (sph0 >> 10) & 15;
    if (version != 3 && version != 4) return -1;
    if (type == 2) return shader == 5 ? ST_FS : -1;
    if (type != 1) return -1;
    switch (shader) {
        case 1: return ST_VS;
        case 2: return ST_TCS;
        case 3: return ST_TES;
        case 4: return ST_GS;
        default: return -1;
    }
}

/* padding between programs: an all-zero instruction or a NOP on PT */
static int is_padding(u64 lo, u64 hi) { return (!lo && !hi) || (lo & 0xffffu) == 0x7918u; }

int sm70_find_programs(const u8* data, u32 size, Sm70Program* out, int max) {
    u32 n = size / 16, i = 0;
    int found = 0;
    while (i < n && found < max) {
        while (i < n && is_padding(snx_ld_le64(data + 16 * (size_t)i), snx_ld_le64(data + 16 * (size_t)i + 8))) i++;
        if (i >= n) break;
        Sm70Program p = {0};
        p.header = p.code = i * 16;
        p.stage = ST_CS;
        Sm70Insn first;
        int stage = sm70_sph_stage(snx_ld_le32(data + 16 * (size_t)i));
        /* an SPH starts on 128 bytes, and its first word is no instruction the decoder knows */
        if (stage >= 0 && !(i % 8) && !sm70_decode(snx_ld_le64(data + 16 * (size_t)i), 0, &first)) {
            p.stage = stage;
            i += SM70_SPH_BYTES / 16;
            p.code = i * 16;
        }
        u32 j = i, last_call = 0;
        for (; j + 1 < n; j++) {
            /* the BRA to itself after the program's last instruction (an EXIT, or a BRA back when the program
               ends in a loop), past every CALL's target */
            Sm70Insn a, b;
            sm70_decode(snx_ld_le64(data + 16 * (size_t)j), snx_ld_le64(data + 16 * (size_t)j + 8), &a);
            if (a.op == SM70_CALL) {
                u32 t = sm70_branch_target(&a, j);
                if (t != 0xffffffffu && t > last_call) last_call = t;
            }
            sm70_decode(snx_ld_le64(data + 16 * (size_t)j + 16), snx_ld_le64(data + 16 * (size_t)j + 24), &b);
            if (b.op == SM70_BRA && b.pred == SM70_PT && !b.pred_not && sm70_branch_target(&b, j + 1) == j + 1 &&
                j + 1 >= last_call)
                break;
        }
        if (j + 1 >= n) {
            /* no BRA to itself: the program ends before 8 instructions of padding, or before the next header. It is
               complete when its last instruction is an EXIT or a BRA that always runs */
            u32 k = i, last = i;
            while (k < n) {
                u32 run = 0;
                while (k + run < n && is_padding(snx_ld_le64(data + 16 * (size_t)(k + run)),
                                                 snx_ld_le64(data + 16 * (size_t)(k + run) + 8)))
                    run++;
                if (run >= 8 || k + run >= n) break;
                k += run;
                Sm70Insn e;
                int known = sm70_decode(snx_ld_le64(data + 16 * (size_t)k), snx_ld_le64(data + 16 * (size_t)k + 8), &e);
                if (k > i && run && !(k % 8) && !known && sm70_sph_stage(snx_ld_le32(data + 16 * (size_t)k)) >= 0)
                    break;
                last = k++;
            }
            Sm70Insn e;
            sm70_decode(snx_ld_le64(data + 16 * (size_t)last), snx_ld_le64(data + 16 * (size_t)last + 8), &e);
            p.complete = (e.op == SM70_EXIT || e.op == SM70_BRA) && e.pred == SM70_PT && !e.pred_not;
            p.size = (last + 1 - i) * 16;
            out[found++] = p;
            i = last + 1;
            continue;
        }
        p.complete = 1;
        p.size = (j + 2 - i) * 16;
        out[found++] = p;
        i = j + 2;
    }
    int complete = 0;
    for (int k = 0; k < found; k++) complete += out[k].complete;
    if (!complete) sm70_debug_dump("block", data, size, NULL, "no complete program");
    return found;
}

void sm70_debug_dump(const char* kind, const u8* bytes, u32 size, const u32* sph, const char* why) {
    const char* dir = getenv("SNX_SM70_DUMP");
    if (!dir || !*dir || !bytes || !size) return;
    unsigned long long hash = snx_fnv1a(SNX_FNV_BASIS, bytes, size);
    char path[1024];
    snprintf(path, sizeof path, "%s/sm70_%s_%016llx.bin", dir, kind, hash);
    FILE* f = fopen(path, "wb");
    if (!f) return;
    fwrite(bytes, 1, size, f);
    fclose(f);
    if (sph) {
        u8 words[SM70_SPH_BYTES];
        for (u32 k = 0; k < SM70_SPH_BYTES / 4; k++) snx_st_le32(words + 4 * k, sph[k]);
        snprintf(path, sizeof path, "%s/sm70_%s_%016llx.sph", dir, kind, hash);
        f = fopen(path, "wb");
        if (f) {
            fwrite(words, 1, sizeof words, f);
            fclose(f);
        }
    }
    snprintf(path, sizeof path, "%s/sm70_%s_%016llx.txt", dir, kind, hash);
    f = fopen(path, "w");
    if (f) {
        fprintf(f, "%s\n", why ? why : "");
        fclose(f);
    }
}
