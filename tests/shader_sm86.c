/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The SM86 frontend (src/nx/gpu/shader/sm86.h): the decoder against instruction words that a small encoder here
   makes by the rules of Mesa's NAK encoder (sm70_encode.rs: set_opcode, encode_alu_base, set_pred, set_instr_deps,
   set_rel_offset) and its SPH (sph.rs), the search for programs in shader memory, and the translation of small
   programs, which spirv-val checks when the test gets its path. No word here comes from a game. */
#include "nx/gpu/shader/sm86.h"
#include "common/utilities/endian.h"
#include "support/spv_eval.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond);                                   \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

/* ---- the encoder ---- */

typedef struct {
    u64 w[2];
} Word;
static void set(Word* x, unsigned lo, unsigned n, u64 v) {
    for (unsigned i = 0; i < n; i++) {
        unsigned bit = lo + i;
        u64 m = 1ull << (bit & 63);
        if ((v >> i) & 1)
            x->w[bit >> 6] |= m;
        else
            x->w[bit >> 6] &= ~m;
    }
}
/* set_pred: PT, set_instr_deps: no barriers (7), and the opcode */
static Word fix(u32 opcode) {
    Word x = {{0, 0}};
    set(&x, 0, 12, opcode);
    set(&x, 12, 3, 7);
    set(&x, 110, 3, 7);
    set(&x, 113, 3, 7);
    return x;
}
static Word alu(u32 opcode9, u32 form, u32 dst, u32 a) {
    Word x = fix(0);
    set(&x, 0, 9, opcode9);
    set(&x, 9, 3, form);
    set(&x, 16, 8, dst);
    set(&x, 24, 8, a);
    return x;
}
static void put(u8* code, u32 index, Word x) {
    for (int k = 0; k < 8; k++) {
        code[16 * index + k] = (u8)(x.w[0] >> (8 * k));
        code[16 * index + 8 + k] = (u8)(x.w[1] >> (8 * k));
    }
}
/* BRA to target from ip (instruction indices): set_rel_offset(34..82), target - ip - 4 in 32-bit words */
static Word bra(u32 ip, u32 target, u32 pred, int negate) {
    Word x = fix(0x947);
    set(&x, 12, 3, pred);
    set(&x, 15, 1, (u64)negate);
    set(&x, 87, 3, 7);
    u64 off = (u64)target * 4 - (u64)ip * 4 - 4;
    set(&x, 34, 48, off);
    return x;
}
static Word exit_insn(void) {
    Word x = fix(0x94d);
    set(&x, 87, 3, 7);
    return x;
}
/* a constant buffer operand in bits 32-58: offset 38-53, bank 54-58 */
static void cbuf(Word* x, u32 bank, u32 off) {
    set(x, 38, 16, off);
    set(x, 54, 5, bank);
}

static Sm86Insn dec(Word x) {
    Sm86Insn in;
    sm86_decode(x.w[0], x.w[1], &in);
    return in;
}

/* ---- the decoder ---- */

static void test_alu_forms(void) {
    /* FFMA R1, R2, b, c in each of the forms that encode_alu_base gives */
    Word x = alu(0x023, 1, 1, 2);
    set(&x, 32, 8, 3); /* b R3 */
    set(&x, 64, 8, 4); /* c R4 */
    set(&x, 72, 1, 1); /* a neg */
    set(&x, 62, 1, 1); /* b abs */
    set(&x, 75, 1, 1); /* c neg */
    Sm86Insn in = dec(x);
    CHECK(in.op == SM86_FFMA && in.form == 1 && in.dst == 1);
    CHECK(in.a.kind == SM86_SRC_REG && in.a.reg == 2 && in.a.neg && !in.a.abs);
    CHECK(in.b.kind == SM86_SRC_REG && in.b.reg == 3 && in.b.abs && !in.b.neg);
    CHECK(in.c.kind == SM86_SRC_REG && in.c.reg == 4 && in.c.neg);
    x = alu(0x023, 2, 1, 2); /* c an immediate, b in 64-71 */
    set(&x, 32, 32, 0x3f800000u);
    set(&x, 64, 8, 5);
    in = dec(x);
    CHECK(in.op == SM86_FFMA && in.b.kind == SM86_SRC_REG && in.b.reg == 5);
    CHECK(in.c.kind == SM86_SRC_IMM && in.c.value == 0x3f800000u);
    x = alu(0x023, 3, 1, 2); /* c a constant buffer */
    cbuf(&x, 3, 0x28c);
    set(&x, 64, 8, 6);
    in = dec(x);
    CHECK(in.c.kind == SM86_SRC_CBUF && in.c.bank == 3 && in.c.value == 0x28c && !in.c.bindless && in.b.reg == 6);
    x = alu(0x023, 5, 1, 2); /* b a constant buffer */
    cbuf(&x, 17, 0xfffc);
    set(&x, 64, 8, 7);
    in = dec(x);
    CHECK(in.b.kind == SM86_SRC_CBUF && in.b.bank == 17 && in.b.value == 0xfffc && in.c.reg == 7);
    x = alu(0x024, 6, 1, 2); /* IMAD, b a uniform register: encode_alu_ureg sets bit 91 */
    set(&x, 32, 6, 4);
    set(&x, 91, 1, 1);
    set(&x, 64, 8, 255);
    in = dec(x);
    CHECK(in.op == SM86_IMAD && in.b.kind == SM86_SRC_UREG && in.b.reg == 4 && in.c.reg == 255);
    x = alu(0x024, 7, 1, 2); /* c a uniform register */
    set(&x, 32, 6, 5);
    set(&x, 63, 1, 1);
    set(&x, 91, 1, 1);
    in = dec(x);
    CHECK(in.c.kind == SM86_SRC_UREG && in.c.reg == 5 && in.c.neg);
    x = alu(0x020, 4, 1, 2); /* FMUL with an immediate */
    set(&x, 32, 32, 0x40000000u);
    in = dec(x);
    CHECK(in.op == SM86_FMUL && in.b.kind == SM86_SRC_IMM && in.b.value == 0x40000000u);
    x = alu(0x020, 2, 1, 2); /* FMUL takes no immediate in c: not an encoding */
    CHECK(!sm86_decode(x.w[0], x.w[1], &in) && in.op == SM86_NONE);
    x = alu(0x002, 5, 9, 255); /* MOV R9, c[0x8][0x1e4] */
    cbuf(&x, 8, 0x1e4);
    in = dec(x);
    CHECK(in.op == SM86_MOV && in.b.kind == SM86_SRC_CBUF && in.b.bank == 8 && in.b.value == 0x1e4);
}

static void test_common_fields(void) {
    Word x = alu(0x021, 1, 200, 100); /* @!P3 FADD with scheduling */
    set(&x, 12, 3, 3);
    set(&x, 15, 1, 1);
    set(&x, 105, 4, 13);
    set(&x, 109, 1, 1);
    set(&x, 110, 3, 2);
    set(&x, 113, 3, 5);
    set(&x, 116, 6, 0x29);
    set(&x, 122, 4, 0xa);
    Sm86Insn in = dec(x);
    CHECK(in.op == SM86_FADD && in.pred == 3 && in.pred_not && in.dst == 200 && in.a.reg == 100);
    CHECK(in.ctrl.stall == 13 && in.ctrl.yield == 1 && in.ctrl.wr_bar == 2 && in.ctrl.rd_bar == 5);
    CHECK(in.ctrl.wait_mask == 0x29 && in.ctrl.reuse == 0xa);
    CHECK(sm86_field(&in, 0, 12) == 0x221 && sm86_field(&in, 60, 8) == 0 && sm86_field(&in, 116, 6) == 0x29);
    /* the uniform ALU: UIADD3 UR4, UR4, 0x28, URZ (form 4), and its uniform destination */
    x = alu(0x090, 4, 4, 4);
    set(&x, 91, 1, 1);
    set(&x, 32, 32, 0x28);
    set(&x, 64, 6, SM86_URZ);
    in = dec(x);
    CHECK(in.op == SM86_UIADD3 && in.dst == 4 && in.a.kind == SM86_SRC_UREG && in.a.reg == 4);
    CHECK(in.b.kind == SM86_SRC_IMM && in.b.value == 0x28 && in.c.kind == SM86_SRC_UREG && in.c.reg == SM86_URZ);
}

static void test_flow(void) {
    Sm86Insn in = dec(bra(10, 10, 7, 0)); /* the BRA to itself after a program's EXIT */
    CHECK(in.op == SM86_BRA && sm86_branch_target(&in, 10) == 10);
    in = dec(bra(10, 3, 0, 1));
    CHECK(in.pred == 0 && in.pred_not && sm86_branch_target(&in, 10) == 3);
    in = dec(bra(10, 40, 7, 0));
    CHECK(sm86_branch_target(&in, 10) == 40);
    in = dec(bra(10, 0, 7, 0));
    CHECK(sm86_branch_target(&in, 10) == 0);
    Word x = fix(0x945); /* BSSY B0 to 7 instructions on: set_rel_offset(34..64) */
    set(&x, 34, 30, 7 * 4);
    in = dec(x);
    CHECK(in.op == SM86_BSSY && sm86_branch_target(&in, 2) == 10);
    in = dec(exit_insn());
    CHECK(in.op == SM86_EXIT && in.pred == SM86_PT);
    in = dec(fix(0x941));
    CHECK(in.op == SM86_BSYNC);
}

static void test_table(void) {
    /* sm86.decode's patterns agree with SM86_OPS: every opcode has a code, an ALU opcode's codes (its forms) share
       their low 9 bits, any other opcode has a single 12-bit code, and the predicate bits change nothing */
    u32 codes[SM86__COUNT] = {0}, low9[SM86__COUNT] = {0};
    for (u32 code = 0; code < 4096; code++) {
        Sm86Insn in = dec(fix(code));
        for (u32 pred = 0; pred < 16; pred++) {
            Word x = fix(code);
            set(&x, 12, 4, pred);
            CHECK(dec(x).op == in.op);
        }
        if (in.op == SM86_NONE) continue;
        if (codes[in.op]++ && sm86_ops[in.op].kind != SM86_K_FIX && low9[in.op] != (code & 0x1ff)) {
            fprintf(stderr, "%s has codes of two ALU opcodes\n", sm86_ops[in.op].name);
            CHECK(0);
        }
        low9[in.op] = code & 0x1ff;
    }
    for (int op = 1; op < SM86__COUNT; op++) {
        int ok = sm86_ops[op].kind == SM86_K_FIX ? codes[op] == 1 : codes[op] >= 1 && codes[op] <= 7;
        if (!ok) fprintf(stderr, "%s has %u codes\n", sm86_ops[op].name, codes[op]);
        CHECK(ok);
    }
    /* a few rows of sm86.decode: FADD in form 1 and 7, FMUL not in form 2, UIADD3 in form 4, FSWZADD */
    CHECK(dec(alu(0x021, 1, 1, 2)).op == SM86_FADD && dec(alu(0x021, 7, 1, 2)).op == SM86_FADD);
    CHECK(dec(alu(0x020, 2, 1, 2)).op == SM86_NONE && dec(alu(0x090, 4, 1, 2)).op == SM86_UIADD3);
    CHECK(dec(fix(0x822)).op == SM86_FSWZADD && dec(fix(0x022)).op == SM86_NONE);
    Sm86Insn in;
    CHECK(!sm86_decode(0x7fff, 0, &in) && in.op == SM86_NONE); /* 0xfff is no opcode */
}

/* SPH word 0 as sph.rs sets it: sph type (bits 0-4), version 4 from SM 7.3 (bits 5-9), shader type (bits 10-13) */
static u32 sph0(u32 sph_type, u32 shader_type) { return sph_type | 4u << 5 | shader_type << 10; }

static void test_sph(void) {
    CHECK(sm86_sph_stage(sph0(1, 1)) == ST_VS);
    CHECK(sm86_sph_stage(sph0(1, 2)) == ST_TCS);
    CHECK(sm86_sph_stage(sph0(1, 3)) == ST_TES);
    CHECK(sm86_sph_stage(sph0(1, 4)) == ST_GS);
    CHECK(sm86_sph_stage(sph0(2, 5)) == ST_FS);
    CHECK(sm86_sph_stage(sph0(2, 1)) == -1);
    CHECK(sm86_sph_stage(sph0(1, 1) & ~(31u << 5)) == -1);
    CHECK(sm86_sph_stage(3u << 5 | 1u | 1u << 10) == ST_VS); /* version 3, before SM 7.3 */
    /* a mesh pipeline's programs take the sets of the stages that it does not have */
    CHECK(shader_vk_set(ST_MS) == shader_vk_set(ST_VS) && shader_vk_set(ST_TS) == shader_vk_set(ST_TCS));
    CHECK(shader_vk_set(ST_TS) != shader_vk_set(ST_FS) && shader_vk_set(ST_MS) != shader_vk_set(ST_FS));
}

/* ---- programs ---- */

/* a compute program: R0 = TID.X; UR4:UR5 = c[0][0x20]; R2 = [R0 + UR4]; R2 = R2 * 2.0; [R0 + UR4 + 0x100] = R2 */
static u32 compute_program(u8* code) {
    u32 n = 0;
    Word x = fix(0x919);
    set(&x, 16, 8, 0);
    set(&x, 72, 8, 0x21);
    put(code, n++, x);
    x = fix(0xab9); /* ULDC.64 UR4 */
    set(&x, 16, 6, 4);
    cbuf(&x, 0, 0x20);
    set(&x, 73, 3, 5);
    put(code, n++, x);
    x = fix(0x981); /* LDG.E R2, [R0.U32 + UR4] */
    set(&x, 16, 8, 2);
    set(&x, 24, 8, 0);
    set(&x, 32, 6, 4);
    set(&x, 72, 1, 1);
    set(&x, 73, 3, 4);
    set(&x, 81, 3, 7);
    set(&x, 91, 1, 1);
    put(code, n++, x);
    x = alu(0x020, 4, 2, 2); /* FMUL R2, R2, 2.0 */
    set(&x, 32, 32, 0x40000000u);
    set(&x, 84, 3, 4);
    put(code, n++, x);
    x = fix(0x986); /* STG.E [R0.U32 + UR4 + 0x100], R2 */
    set(&x, 24, 8, 0);
    set(&x, 32, 8, 2);
    set(&x, 40, 24, 0x100);
    set(&x, 64, 6, 4);
    set(&x, 72, 1, 1);
    set(&x, 73, 3, 4);
    set(&x, 91, 1, 1);
    put(code, n++, x);
    put(code, n, exit_insn());
    n++;
    put(code, n, bra(n, n, 7, 0));
    return n + 1;
}
/* a vertex program: R0-R3 = a[0x80] (generic 0); if (R0 < 5) R1 = 1.0; a[0x70] = R0-R3 (the position) */
static u32 vertex_program(u8* code) {
    u32 n = 0;
    Word x = fix(0x321); /* ALD R0-R3 */
    set(&x, 16, 8, 0);
    set(&x, 24, 8, 255);
    set(&x, 32, 8, 255);
    set(&x, 40, 10, 0x80);
    set(&x, 74, 2, 3);
    put(code, n++, x);
    x = alu(0x00c, 4, 0, 0); /* ISETP.GE.U32.AND P0, PT, R0, 5, PT: the branch skips when R0 >= 5 */
    set(&x, 16, 8, 255);
    set(&x, 32, 32, 5);
    set(&x, 76, 3, 6);
    set(&x, 81, 3, 0);
    set(&x, 84, 3, 7);
    set(&x, 87, 3, 7);
    put(code, n++, x);
    put(code, n, bra(n, n + 2, 0, 0));
    n++;
    x = alu(0x002, 4, 1, 255); /* MOV R1, 1.0 */
    set(&x, 32, 32, 0x3f800000u);
    set(&x, 72, 4, 0xf);
    put(code, n++, x);
    x = fix(0x322); /* AST a[0x70], R0-R3 */
    set(&x, 24, 8, 255);
    set(&x, 32, 8, 0);
    set(&x, 64, 8, 255);
    set(&x, 40, 10, 0x70);
    set(&x, 74, 2, 3);
    put(code, n++, x);
    put(code, n, exit_insn());
    n++;
    put(code, n, bra(n, n, 7, 0));
    return n + 1;
}
/* a fragment program: R0 = a[0x80] interpolated, R1-R3 = 0, render target 0 from R0-R3 */
static u32 fragment_program(u8* code) {
    u32 n = 0;
    Word x = fix(0x326); /* IPA R0, a[0x80] */
    set(&x, 16, 8, 0);
    set(&x, 24, 8, 0);
    set(&x, 32, 8, 255);
    set(&x, 64, 8, 0x80 >> 2);
    set(&x, 81, 3, 7);
    put(code, n++, x);
    for (u32 r = 1; r < 4; r++) {
        x = alu(0x002, 1, r, 255); /* MOV Rr, RZ */
        set(&x, 32, 8, 255);
        put(code, n++, x);
    }
    put(code, n, exit_insn());
    n++;
    put(code, n, bra(n, n, 7, 0));
    return n + 1;
}

/* a module in dir as sm86_<name>.spv, which shader_sm86_test.py hands to spirv-val */
static int keep_module(const char* dir, const char* name, const u32* words, size_t count) {
    char path[1024];
    snprintf(path, sizeof path, "%s/sm86_%s.spv", dir, name);
    FILE* f = fopen(path, "wb");
    if (!f) return 0;
    int ok = fwrite(words, 4, count, f) == count;
    ok &= !fclose(f);
    if (!ok) fprintf(stderr, "sm86: %s could not be written\n", path);
    return ok;
}

static void test_programs(const char* dir) {
    u8* mem = calloc(1, 0x1000);
    if (!mem) abort();
    /* a block of shader memory: the compute program, NOP padding to 128 bytes, then the vertex program with its
       SPH, then zeros */
    u32 cs = compute_program(mem);
    u32 at = (cs * 16 + 127) & ~127u;
    for (u32 i = cs; i < at / 16; i++) put(mem, i, fix(0x918));
    u32 sph[32] = {0};
    sph[0] = sph0(1, 1);
    for (u32 k = 0; k < 32; k++) snx_st_le32(mem + at + 4 * k, sph[k]);
    u32 vs = vertex_program(mem + at + SM86_SPH_BYTES);
    Sm86Program progs[4];
    int found = sm86_find_programs(mem, 0x1000, progs, 4);
    CHECK(found == 2);
    if (found == 2) {
        CHECK(progs[0].stage == ST_CS && progs[0].code == 0 && progs[0].size == cs * 16 && progs[0].complete);
        CHECK(progs[1].stage == ST_VS && progs[1].header == at && progs[1].code == at + SM86_SPH_BYTES);
        CHECK(progs[1].size == vs * 16 && progs[1].complete);
    }
    /* the translations */
    ShaderInfo si = {0};
    size_t count = 0;
    u32* spv = sm86_to_spirv(mem, cs * 16, ST_CS, NULL, &si, &count);
    CHECK(spv != NULL);
    if (!spv) fprintf(stderr, "compute: %s\n", si.first_unsupported);
    CHECK(si.ngbuf == 1 && si.gbuf_cbuf_off[0] == 0x20 && si.gbuf_wmask == 1 && (si.cbuf_mask & 1));
    /* its store logs its bytes, in the write log's first part (SNX_VKB_WPAGES, shader.h's SH_WLOG_*) */
    CHECK(si.gbuf_marks == 1 && si.nmarks == 1 && si.gbuf_mark[0] == 1);
    if (spv) CHECK(keep_module(dir, "cs", spv, count));
    free(spv);
    si = (ShaderInfo){0};
    spv = sm86_to_spirv(mem + at + SM86_SPH_BYTES, vs * 16, ST_VS, sph, &si, &count);
    CHECK(spv != NULL);
    if (!spv) fprintf(stderr, "vertex: %s\n", si.first_unsupported);
    CHECK(si.in_mask == 1);
    if (spv) CHECK(keep_module(dir, "vs", spv, count));
    free(spv);
    /* the fragment program: its SPH enables render target 0 (word 18) and imaps input 0 x as perspective */
    u8* fs = calloc(1, 0x400);
    if (!fs) abort();
    u32 n = fragment_program(fs);
    u32 fsph[32] = {0};
    fsph[0] = sph0(2, 5);
    fsph[6] = 2;
    fsph[18] = 0xf;
    si = (ShaderInfo){0};
    spv = sm86_to_spirv(fs, n * 16, ST_FS, fsph, &si, &count);
    CHECK(spv != NULL);
    if (!spv) fprintf(stderr, "fragment: %s\n", si.first_unsupported);
    if (spv) CHECK(keep_module(dir, "fs", spv, count));
    free(spv);
    /* an unknown opcode refuses with its code */
    put(fs, 0, fix(0xfff));
    si = (ShaderInfo){0};
    spv = sm86_to_spirv(fs, n * 16, ST_FS, fsph, &si, &count);
    CHECK(!spv && si.unsupported && strstr(si.first_unsupported, "0xfff"));
    free(spv);
    free(fs);
    free(mem);
}

/* the forms the game's programs added: TLD, TXQ, IPA of the point sprite's T, UMOV, ULDC indexed by a uniform
   register, IMAD.X, F2I.S16, AST of the shading rate (0x5c), and a program that ends in a loop */
static void test_game_forms(const char* dir) {
    u8* fs = calloc(1, 0x400);
    if (!fs) abort();
    u32 n = 0;
    Word x = fix(0xb66); /* TLD.SCR.LL R0-R3, R4, R6, c[2][0x40], 2D */
    set(&x, 24, 8, 4);
    set(&x, 32, 8, 6);
    set(&x, 40, 14, 0x10);
    set(&x, 54, 5, 2);
    set(&x, 60, 1, 1);
    set(&x, 61, 3, 1);
    set(&x, 64, 8, 255);
    set(&x, 72, 4, 0xf);
    set(&x, 81, 3, 7);
    set(&x, 87, 3, 3);
    put(fs, n++, x);
    x = fix(0xb6f); /* TXQ R8-R11, RZ, c[2][0x40], the dimensions */
    set(&x, 16, 8, 8);
    set(&x, 24, 8, 255);
    set(&x, 40, 14, 0x10);
    set(&x, 54, 5, 2);
    set(&x, 64, 8, 255);
    set(&x, 72, 4, 0xf);
    put(fs, n++, x);
    x = fix(0x326); /* IPA R15, a[0x3fc], the front face */
    set(&x, 16, 8, 15);
    set(&x, 32, 8, 255);
    set(&x, 64, 8, 0x3fc >> 2);
    set(&x, 81, 3, 7);
    put(fs, n++, x);
    x = fix(0x326); /* IPA R12, a[0x2e4] */
    set(&x, 16, 8, 12);
    set(&x, 32, 8, 255);
    set(&x, 64, 8, 0x2e4 >> 2);
    set(&x, 81, 3, 7);
    put(fs, n++, x);
    x = alu(0x082, 4, 8, 0); /* UMOV UR8, 0x40 */
    set(&x, 32, 32, 0x40);
    set(&x, 91, 1, 1);
    put(fs, n++, x);
    x = fix(0xabb); /* ULDC UR9, c[1][UR8 - 4] */
    set(&x, 16, 6, 9);
    set(&x, 24, 6, 8);
    cbuf(&x, 1, 0xfffc);
    set(&x, 73, 3, 4);
    set(&x, 91, 1, 1);
    put(fs, n++, x);
    x = alu(0x024, 4, 13, 12); /* IMAD.X R13, R12, 1, RZ, P0 */
    set(&x, 32, 32, 1);
    set(&x, 64, 8, 255);
    set(&x, 74, 1, 1);
    set(&x, 87, 4, 0);
    put(fs, n++, x);
    x = alu(0x105, 1, 14, 255); /* F2I.S16.TRUNC R14, R12 */
    set(&x, 32, 8, 12);
    set(&x, 72, 1, 1);
    set(&x, 75, 2, 1);
    set(&x, 78, 2, 3);
    set(&x, 84, 2, 2);
    put(fs, n++, x);
    put(fs, n, exit_insn());
    n++;
    put(fs, n, bra(n, n, 7, 0));
    n++;
    u32 fsph[32] = {0};
    fsph[0] = sph0(2, 5);
    fsph[18] = 0xf;
    ShaderInfo si = {0};
    size_t count = 0;
    u32* spv = sm86_to_spirv(fs, n * 16, ST_FS, fsph, &si, &count);
    CHECK(spv != NULL);
    if (!spv) fprintf(stderr, "game forms: %s\n", si.first_unsupported);
    CHECK(si.ntex == 1 && si.tex[0].bl_cbuf == 2 && si.tex[0].cbuf_off == 0x10 && (si.cbuf_mask & 2));
    if (spv) CHECK(keep_module(dir, "fs_game", spv, count));
    free(spv);
    /* a vertex program that stores the shading rate and the position */
    n = 0;
    x = fix(0x322);
    set(&x, 24, 8, 255);
    set(&x, 32, 8, 0);
    set(&x, 64, 8, 255);
    set(&x, 40, 10, 0x5c);
    put(fs, n++, x);
    set(&x, 40, 10, 0x70);
    set(&x, 74, 2, 3);
    put(fs, n++, x);
    put(fs, n, exit_insn());
    n++;
    put(fs, n, bra(n, n, 7, 0));
    n++;
    u32 vsph[32] = {0};
    vsph[0] = sph0(1, 1);
    si = (ShaderInfo){0};
    spv = sm86_to_spirv(fs, n * 16, ST_VS, vsph, &si, &count);
    CHECK(spv != NULL);
    if (!spv) fprintf(stderr, "shading rate: %s\n", si.first_unsupported);
    if (spv) CHECK(keep_module(dir, "vs_rate", spv, count));
    free(spv);
    /* a program that ends in a loop: @P0 BRA back, then the BRA to itself, with no EXIT before it */
    memset(fs, 0, 0x400);
    n = 0;
    x = alu(0x002, 4, 1, 255);
    set(&x, 32, 32, 1);
    put(fs, n++, x);
    put(fs, n, bra(n, 0, 0, 0));
    n++;
    put(fs, n, bra(n, n, 7, 0));
    n++;
    Sm86Program progs[2];
    int found = sm86_find_programs(fs, 0x400, progs, 2);
    CHECK(found == 1 && progs[0].complete && progs[0].size == n * 16);
    /* CALL.REL.NOINC to a function after the main program's end, which RET.REL.NODEC R2 ends (CuAssembler's sm_86
       layout: the offset as BRA's, NOINC and NODEC at bit 86). The search takes the function in, and the
       translation inlines it */
    memset(fs, 0, 0x400);
    n = 0;
    x = fix(0x944);
    set(&x, 34, 48, (u64)(5 - 0 - 1) * 4);
    set(&x, 86, 4, 0xf);
    put(fs, n++, x); /* 0: CALL to 5 */
    for (u32 r = 1; r < 4; r++) {
        x = alu(0x002, 1, r, 255); /* MOV Rr, RZ */
        set(&x, 32, 8, 255);
        put(fs, n++, x);
    }
    put(fs, n, exit_insn()); /* 4 */
    n++;
    put(fs, n, bra(n, n, 7, 0)); /* 5 is the function: the BRA to itself is at 6 */
    n++;
    u32 entry = n;
    x = alu(0x002, 4, 0, 255); /* MOV R0, 1.0 */
    set(&x, 32, 32, 0x3f800000u);
    put(fs, n++, x);
    x = fix(0x950); /* RET.REL.NODEC R2 */
    set(&x, 24, 8, 2);
    set(&x, 86, 4, 0xf);
    put(fs, n++, x);
    put(fs, n, bra(n, n, 7, 0));
    n++;
    /* the CALL's offset: to the function's entry */
    x = fix(0x944);
    set(&x, 34, 48, (u64)(entry - 0 - 1) * 4);
    set(&x, 86, 4, 0xf);
    put(fs, 0, x);
    found = sm86_find_programs(fs, 0x400, progs, 2);
    CHECK(found == 1 && progs[0].complete && progs[0].size == n * 16);
    si = (ShaderInfo){0};
    spv = sm86_to_spirv(fs, n * 16, ST_FS, fsph, &si, &count);
    CHECK(spv != NULL);
    if (!spv) fprintf(stderr, "call: %s\n", si.first_unsupported);
    if (spv) CHECK(keep_module(dir, "fs_call", spv, count));
    free(spv);
    free(fs);
}

/* ---- programs run on the CPU (support/spv_eval.c) ---- */

static u32 f2u(float f) {
    u32 u;
    memcpy(&u, &f, 4);
    return u;
}
static float u2f(u32 u) {
    float f;
    memcpy(&f, &u, 4);
    return f;
}
/* the fragment program code (n instructions, its SPH writing color 0 from R0-R3) translated and run at lane, with
   bank 1 as cb1 (cb1_words of it, NULL for none): its color 0 into out, 1 when it ran */
static int run_fs_at(const u8* code, u32 n, u32 lane, const u32* cb1, u32 cb1_words, u32 out[4]) {
    u32 fsph[32] = {0};
    fsph[0] = sph0(2, 5);
    fsph[18] = 0xf;
    ShaderInfo si = {0};
    size_t count = 0;
    u32* spv = sm86_to_spirv(code, n * 16, ST_FS, fsph, &si, &count);
    if (!spv) {
        fprintf(stderr, "run_fs: %s\n", si.first_unsupported);
        return 0;
    }
    SpvEvalEnv env = {0};
    env.builtin[41][0] = lane;
    u32 bank1[1024] = {0};
    for (u32 k = 0; cb1 && k < cb1_words && k < 1024; k++) bank1[k] = cb1[k];
    env.ubo[SNX_VKB_CB + 1] = bank1;
    env.ubo_words[SNX_VKB_CB + 1] = 1024;
    const char* why = spv_eval(spv, count, &env);
    free(spv);
    if (why) fprintf(stderr, "run_fs: the evaluator stops at %s\n", why);
    for (u32 k = 0; k < 4; k++) out[k] = env.out[0][k];
    return !why && env.out_written[0];
}

/* the jump table of the BRX test: c[1][0x10] on, the targets' byte addresses, then a word that is no target */
static int table_read(u32 idx, u32 off, u32* out) {
    static const u32 table[3] = {2 * 16, 4 * 16, 0xffffffffu};
    if (idx != 1 || off < 0x10 || off >= 0x10 + sizeof table) return 0;
    *out = table[(off - 0x10) / 4];
    return 1;
}
static u32 spv_find(const u32* words, size_t count, u32 op, u32* at, u32 max);
static Word mov_imm(u32 dst, u32 value);
/* LDC R2, c[1][R1 + 0x10]; BRX R2, -(the next instruction's address); two cases that write R0 and exit */
static void test_brx(const char* dir) {
    u8 code[16 * 8] = {0};
    Word x = fix(0xb82);
    set(&x, 16, 8, 2);
    set(&x, 24, 8, 1);
    cbuf(&x, 1, 0x10);
    set(&x, 73, 3, 4);
    put(code, 0, x);
    x = fix(0x949);
    set(&x, 24, 8, 2);
    set(&x, 34, 48, (u64)0 - 8); /* -32 bytes (minus the next instruction's address) in 4-byte units */
    set(&x, 87, 3, 7);
    put(code, 1, x);
    for (u32 k = 0; k < 2; k++) {
        x = alu(0x002, 4, 0, 255); /* MOV R0, 1.0 or 2.0 */
        set(&x, 32, 32, k ? 0x40000000u : 0x3f800000u);
        put(code, 2 + 2 * k, x);
        put(code, 3 + 2 * k, exit_insn());
    }
    put(code, 6, bra(6, 6, 7, 0));
    u32 fsph[32] = {0};
    fsph[0] = sph0(2, 5);
    fsph[18] = 0xf;
    ShaderInfo si = {0};
    size_t count = 0;
    CHECK(!sm86_to_spirv(code, 7 * 16, ST_FS, fsph, &si, &count)); /* no reader: refused */
    g_shader_cb_read = table_read;
    si = (ShaderInfo){0};
    u32* spv = sm86_to_spirv(code, 7 * 16, ST_FS, fsph, &si, &count);
    g_shader_cb_read = NULL;
    CHECK(spv != NULL);
    if (!spv) fprintf(stderr, "brx: %s\n", si.first_unsupported);
    if (spv) CHECK(keep_module(dir, "fs_brx", spv, count));
    /* the switch's cases are the instructions the table's bytes name, 2 and 4 */
    if (spv) {
        u32 at[4];
        CHECK(spv_find(spv, count, 251 /* OpSwitch */, at, 4) == 1);
        const u32* sw = spv + at[0];
        CHECK((sw[0] >> 16) == 7 && ((sw[3] == 2 && sw[5] == 4) || (sw[3] == 4 && sw[5] == 2)));
    }
    free(spv);
}

/* two entries, then a word that names instruction 0 as well: another table's, as one game's two lie in c[1] */
static int table2_read(u32 idx, u32 off, u32* out) {
    static const u32 table[4] = {5 * 16, 7 * 16, 0, 0xffffffffu};
    if (idx != 1 || off < 0x10 || off >= 0x10 + sizeof table) return 0;
    *out = table[(off - 0x10) / 4];
    return 1;
}
/* ptxas's bounded switch: IMNMX.U32 R1, R0, 1 (index 0 or 1); IMAD R2, R1, 4, RZ; LDC R2, c[1][R2 + 0x10]; BRX R2,
   -(the next instruction's address). The table is its two entries, not up to the first word that names no
   instruction: the third would go back to the program's start */
static void test_brx_bounded(const char* dir) {
    u8 code[16 * 10] = {0};
    Word x = alu(0x017, 4, 1, 0);
    set(&x, 32, 32, 1);
    set(&x, 87, 3, 7);
    put(code, 0, x);
    x = alu(0x024, 4, 2, 1);
    set(&x, 32, 32, 4);
    set(&x, 64, 8, 255);
    put(code, 1, x);
    x = fix(0xb82);
    set(&x, 16, 8, 2);
    set(&x, 24, 8, 2);
    cbuf(&x, 1, 0x10);
    set(&x, 73, 3, 4);
    put(code, 2, x);
    x = fix(0x949);
    set(&x, 24, 8, 2);
    set(&x, 34, 48, (u64)0 - 16); /* -64 bytes in 4-byte units */
    set(&x, 87, 3, 7);
    put(code, 3, x);
    for (u32 k = 0; k < 2; k++) {
        put(code, 5 + 2 * k, mov_imm(0, k ? 0x40000000u : 0x3f800000u));
        put(code, 6 + 2 * k, exit_insn());
    }
    put(code, 4, exit_insn());
    put(code, 9, bra(9, 9, 7, 0));
    u32 fsph[32] = {0};
    fsph[0] = sph0(2, 5);
    fsph[18] = 0xf;
    ShaderInfo si = {0};
    size_t count = 0;
    g_shader_cb_read = table2_read;
    u32* spv = sm86_to_spirv(code, 10 * 16, ST_FS, fsph, &si, &count);
    g_shader_cb_read = NULL;
    CHECK(spv != NULL);
    if (!spv) fprintf(stderr, "bounded brx: %s\n", si.first_unsupported);
    if (spv) {
        u32 at[4];
        CHECK(spv_find(spv, count, 251 /* OpSwitch */, at, 4) == 1);
        const u32* sw = spv + at[0];
        CHECK((sw[0] >> 16) == 7 && ((sw[3] == 5 && sw[5] == 7) || (sw[3] == 7 && sw[5] == 5)));
        CHECK(spv_find(spv, count, 246 /* OpLoopMerge */, at, 4) == 0);
        CHECK(keep_module(dir, "fs_brx_bounded", spv, count));
    }
    free(spv);
}

/* the uniform ALU group and the other forms of the second game run, translated and validated */
static void test_alu_group(const char* dir) {
    u8 code[16 * 16] = {0};
    u32 n = 0;
    Word x = alu(0x099, 4, 4, 5); /* USHF.L.U32 UR4, UR5, 3, URZ */
    set(&x, 91, 1, 1);
    set(&x, 32, 32, 3);
    set(&x, 64, 6, 63);
    set(&x, 73, 2, 3);
    put(code, n++, x);
    x = alu(0x091, 1, 6, 4); /* ULEA UR6, UR4, UR5, 2 */
    set(&x, 91, 1, 1);
    set(&x, 32, 6, 5);
    set(&x, 64, 6, 63);
    set(&x, 75, 5, 2);
    set(&x, 81, 3, 7);
    put(code, n++, x);
    x = alu(0x092, 4, 7, 6); /* ULOP3.LUT UR7, UR6, 0xff, URZ, 0xc0 */
    set(&x, 91, 1, 1);
    set(&x, 32, 32, 0xff);
    set(&x, 64, 6, 63);
    set(&x, 72, 8, 0xc0);
    set(&x, 81, 3, 7);
    set(&x, 87, 4, 0xf);
    put(code, n++, x);
    x = fix(0x3c2); /* R2UR UR8, R1 */
    set(&x, 16, 6, 8);
    set(&x, 24, 8, 1);
    put(code, n++, x);
    x = alu(0x083, 4, 9, 63); /* UP2UR UR9, UPR, URZ, 0x7f */
    set(&x, 91, 1, 1);
    set(&x, 32, 32, 0x7f);
    put(code, n++, x);
    x = alu(0x003, 4, 2, 255); /* P2R R2, PR, RZ, 0x7f */
    set(&x, 32, 32, 0x7f);
    put(code, n++, x);
    x = alu(0x100, 1, 3, 255); /* FLO.U32.SH R3, R1 */
    set(&x, 32, 8, 1);
    set(&x, 74, 1, 1);
    put(code, n++, x);
    x = alu(0x109, 1, 4, 255); /* POPC R4, R1 */
    set(&x, 32, 8, 1);
    put(code, n++, x);
    x = alu(0x106, 1, 5, 255); /* I2F.U8.B1 R5, R1 */
    set(&x, 32, 8, 1);
    set(&x, 60, 2, 1);
    set(&x, 75, 2, 2);
    set(&x, 84, 2, 0);
    put(code, n++, x);
    for (u32 k = 0; k < 2; k++) { /* F2FP.PACK_AB R6, R5, R3 and its BF16 form R7 */
        x = alu(0x03e, 1, 6 + k, 5);
        set(&x, 32, 8, 3);
        set(&x, 64, 8, 255);
        set(&x, 76, 1, k);
        put(code, n++, x);
    }
    x = alu(0x030, 1, 8, 6); /* HADD2 R8, R6, R7.H1_H1 */
    set(&x, 32, 8, 7);
    set(&x, 60, 2, 3);
    put(code, n++, x);
    put(code, n++, fix(0x9ab)); /* ERRBAR */
    put(code, n, exit_insn());
    n++;
    put(code, n, bra(n, n, 7, 0));
    n++;
    u32 fsph[32] = {0};
    fsph[0] = sph0(2, 5);
    fsph[18] = 0xf;
    ShaderInfo si = {0};
    size_t count = 0;
    u32* spv = sm86_to_spirv(code, n * 16, ST_FS, fsph, &si, &count);
    CHECK(spv != NULL);
    if (!spv) fprintf(stderr, "alu group: %s\n", si.first_unsupported);
    if (spv) CHECK(keep_module(dir, "fs_alu", spv, count));
    free(spv);
}

/* the forms of the fourth game run in a compute program: SHFL.BFLY in a segment of 4 (c 0x1c1f), SHFL.IDX with
   the lane in a register, S2R of LTMASK (0x39), MEMBAR, HADD2.F32 and PLOP3 with a uniform predicate */
static void test_compute_forms(const char* dir) {
    u8 code[16 * 12] = {0};
    u32 n = 0;
    Word x = fix(0xf89); /* SHFL.BFLY PT, R1, R2, 0x1, 0x1c1f */
    set(&x, 16, 8, 1);
    set(&x, 24, 8, 2);
    set(&x, 40, 13, 0x1c1f);
    set(&x, 53, 5, 1);
    set(&x, 58, 2, 3);
    set(&x, 81, 3, 7);
    put(code, n++, x);
    x = fix(0x589); /* SHFL.IDX PT, R3, R2, R1, 0x1f */
    set(&x, 16, 8, 3);
    set(&x, 24, 8, 2);
    set(&x, 32, 8, 1);
    set(&x, 40, 13, 0x1f);
    set(&x, 81, 3, 7);
    put(code, n++, x);
    x = fix(0x919); /* S2R R4, SR_LTMASK */
    set(&x, 16, 8, 4);
    set(&x, 72, 8, 0x39);
    put(code, n++, x);
    x = fix(0x992); /* MEMBAR.SC.GPU */
    set(&x, 76, 3, 2);
    put(code, n++, x);
    x = alu(0x030, 1, 5, 255); /* HADD2.F32 R5, -RZ, R3.H0_H0 */
    set(&x, 72, 1, 1);
    set(&x, 32, 8, 3);
    set(&x, 60, 2, 2);
    set(&x, 78, 1, 1);
    put(code, n++, x);
    x = fix(0x81c); /* PLOP3.LUT P0, PT, PT, PT, UP1, 0x80, 0 */
    set(&x, 87, 4, 7);
    set(&x, 77, 4, 7);
    set(&x, 68, 4, 1);
    set(&x, 67, 1, 1);
    set(&x, 64, 3, 0x80 & 7);
    set(&x, 72, 5, 0x80 >> 3);
    set(&x, 81, 3, 0);
    set(&x, 84, 3, 7);
    put(code, n++, x);
    put(code, n, exit_insn());
    n++;
    put(code, n, bra(n, n, 7, 0));
    n++;
    ShaderInfo si = {0};
    si.cs_local[0] = 32, si.cs_local[1] = si.cs_local[2] = 1;
    size_t count = 0;
    u32* spv = sm86_to_spirv(code, n * 16, ST_CS, NULL, &si, &count);
    CHECK(spv != NULL);
    if (!spv) fprintf(stderr, "compute forms: %s\n", si.first_unsupported);
    if (spv) CHECK(keep_module(dir, "cs_forms", spv, count));
    free(spv);
}

/* SUST.P to a 2D image with its handle in c[0][0x30], and SULD.P from it, by the kind of the renderer's key: the
   bindless rule of spirv_translate.c gives 2 (unsigned) without bit 0 of img_int_mask, 0 (float) with it, and 3
   (signed) with bit 0 of img_sint_mask, the masks compute.c and pipeline.c key a program by. rt_int_mask's bit 8,
   where the key was before, changes nothing. ShaderInfo.img[].type is the dimension code, 3 for 2D */
static void test_image_kinds(const char* dir) {
    u8 code[16 * 4] = {0};
    Word x = fix(0xb9b); /* SUST.P.2D.RGBA [R0], R4, c[0][0x30] */
    set(&x, 24, 8, 0);
    set(&x, 32, 8, 4);
    set(&x, 40, 14, 0x30 / 4);
    set(&x, 61, 3, 3);
    set(&x, 72, 4, 0xf);
    put(code, 0, x);
    x = fix(0xb97); /* SULD.P.2D.R R8, [R0], c[0][0x30] */
    set(&x, 16, 8, 8);
    set(&x, 24, 8, 0);
    set(&x, 40, 14, 0x30 / 4);
    set(&x, 61, 3, 3);
    set(&x, 72, 4, 1);
    set(&x, 81, 3, 7);
    put(code, 1, x);
    put(code, 2, exit_insn());
    put(code, 3, bra(3, 3, 7, 0));
    static const char* const names[3] = {"cs_image_u", "cs_image_s", "cs_image_f"};
    static const u8 kinds[3] = {2, 3, 0};
    for (u32 k = 0; k < 3; k++) {
        ShaderInfo si = {0};
        si.cs_local[0] = si.cs_local[1] = si.cs_local[2] = 1;
        si.rt_int_mask = si.rt_sint_mask = 1u << 8;
        if (k == 1) si.img_sint_mask = 1;
        if (k == 2) si.img_int_mask = 1;
        size_t count = 0;
        u32* spv = sm86_to_spirv(code, 4 * 16, ST_CS, NULL, &si, &count);
        CHECK(spv != NULL);
        if (!spv) fprintf(stderr, "image kind: %s\n", si.first_unsupported);
        CHECK(si.nimg == 1 && si.img[0].kind == kinds[k] && si.img[0].type == 3 && si.img[0].cbuf_off == 0x30 / 4);
        if (spv) CHECK(keep_module(dir, names[k], spv, count));
        free(spv);
    }
}

/* A fragment program's KILL says so in has_discard, which the renderer reads before it tests depth ahead of the
   program; one without a KILL leaves it 0 */
static void test_kill_discard(void) {
    for (u32 k = 0; k < 2; k++) {
        u8 code[16 * 4] = {0};
        u32 n = 0, fsph[32] = {0};
        Word x = fix(0x95b); /* KILL */
        set(&x, 87, 3, 7);
        if (k) put(code, n++, x);
        put(code, n++, exit_insn());
        put(code, n, bra(n, n, 7, 0));
        n++;
        fsph[0] = sph0(2, 5);
        fsph[18] = 0xf;
        ShaderInfo si = {0};
        size_t count = 0;
        u32* spv = sm86_to_spirv(code, n * 16, ST_FS, fsph, &si, &count);
        CHECK(spv != NULL && si.has_discard == (int)k);
        if (!spv || si.has_discard != (int)k) fprintf(stderr, "kill discard %u: has_discard %d\n", k, si.has_discard);
        free(spv);
    }
}

/* the root tables, banks 24 to 31, as one uniform buffer: R0 = TID.X; UR4:UR5 = c[0][0x20];
   R2 = c[25][0x10]; [R0 + UR4] = R2. Table 1's word 4 is byte 0x110 of the buffer */
static void test_root_tables(const char* dir) {
    u8 code[16 * 8] = {0};
    u32 n = 0;
    Word x = fix(0x919); /* S2R R0, TID.X */
    set(&x, 16, 8, 0);
    set(&x, 72, 8, 0x21);
    put(code, n++, x);
    x = fix(0xab9); /* ULDC.64 UR4, c[0][0x20] */
    set(&x, 16, 6, 4);
    cbuf(&x, 0, 0x20);
    set(&x, 73, 3, 5);
    put(code, n++, x);
    x = alu(0x002, 5, 2, 255); /* MOV R2, c[25][0x10] */
    cbuf(&x, 25, 0x10);
    put(code, n++, x);
    x = fix(0x986); /* STG.E [R0.U32 + UR4], R2 */
    set(&x, 24, 8, 0);
    set(&x, 32, 8, 2);
    set(&x, 64, 6, 4);
    set(&x, 72, 1, 1);
    set(&x, 73, 3, 4);
    set(&x, 91, 1, 1);
    put(code, n++, x);
    put(code, n, exit_insn());
    n++;
    put(code, n, bra(n, n, 7, 0));
    n++;
    ShaderInfo si = {0};
    size_t count = 0;
    u32* spv = sm86_to_spirv(code, n * 16, ST_CS, NULL, &si, &count);
    CHECK(spv != NULL);
    if (!spv) fprintf(stderr, "root tables: %s\n", si.first_unsupported);
    CHECK(si.root_max == SH_ROOT_TABLE + 0x14 && !(si.cbuf_mask & ~1u));
    if (spv) CHECK(keep_module(dir, "root", spv, count));
    free(spv);
}

/* ---- texel offsets ---- */

/* the SPIR-V instructions of opcode op in words, from the header's end: their first words' indices into at */
static u32 spv_find(const u32* words, size_t count, u32 op, u32* at, u32 max) {
    u32 found = 0;
    for (size_t i = 5; i < count && words[i] >> 16; i += words[i] >> 16)
        if ((words[i] & 0xffff) == op && found < max) at[found++] = (u32)i;
    return found;
}
/* the value of the integer constant id: 1 when it is one */
static int spv_int(const u32* words, size_t count, u32 id, i32* value) {
    u32 at[4096];
    u32 n = spv_find(words, count, 43 /* OpConstant */, at, 4096);
    for (u32 k = 0; k < n; k++)
        if (words[at[k] + 2] == id) {
            *value = (i32)words[at[k] + 3];
            return 1;
        }
    return 0;
}
/* the components of the constant vector id, two of them */
static int spv_ivec2(const u32* words, size_t count, u32 id, i32* x, i32* y) {
    u32 at[4096];
    u32 n = spv_find(words, count, 44 /* OpConstantComposite */, at, 4096);
    for (u32 k = 0; k < n; k++)
        if (words[at[k] + 2] == id && (words[at[k]] >> 16) == 5)
            return spv_int(words, count, words[at[k] + 3], x) && spv_int(words, count, words[at[k] + 4], y);
    return 0;
}
/* TEX.SCR R0-R3, R4, R5 (the coordinates), offset R10, c[2][0x40], 2D, implicit LOD */
static Word tex_offset(void) {
    Word x = fix(0xb60);
    set(&x, 16, 8, 0);
    set(&x, 24, 8, 4);
    set(&x, 32, 8, 10);
    set(&x, 40, 14, 0x10);
    set(&x, 54, 5, 2);
    set(&x, 60, 1, 1);
    set(&x, 61, 3, 1);
    set(&x, 64, 8, 255);
    set(&x, 72, 4, 0xf);
    set(&x, 76, 1, 1);
    set(&x, 81, 3, 7);
    return x;
}
static Word mov_imm(u32 dst, u32 value) {
    Word x = alu(0x002, 4, dst, 255);
    set(&x, 32, 32, value);
    return x;
}
/* the program's sample: its ConstOffset (x, y) and 1, or 0 when it takes none */
static int sample_const_offset(const u32* spv, size_t count, i32* x, i32* y) {
    u32 at[8];
    if (spv_find(spv, count, 87 /* OpImageSampleImplicitLod */, at, 8) != 1) return -1;
    const u32* s = spv + at[0];
    if ((s[0] >> 16) < 7 || !(s[5] & 8)) return 0;
    return spv_ivec2(spv, count, s[6], x, y) ? 1 : -1;
}
static u32* translate_fs(const u8* code, u32 n, ShaderInfo* si, size_t* count) {
    u32 fsph[32] = {0};
    fsph[0] = sph0(2, 5);
    fsph[18] = 0xf;
    *si = (ShaderInfo){0};
    u32* spv = sm86_to_spirv(code, n * 16, ST_FS, fsph, si, count);
    if (!spv) fprintf(stderr, "texel offsets: %s\n", si->first_unsupported);
    return spv;
}
/* A sampler's min/max reduction the program takes itself (ShaderInfo.tex_minmax_mask, tex_minmax_max for the
   greatest): TEX.SCR R0-R3, R4, R5 of a 2D texture becomes four gathers, one a component, and the least or greatest
   of the texels its footprint weighs, which the evaluator runs on a 4x4 texture. At (0.5, 0.5) that is texels 1 and 2
   of both rows; at (0.375, 0.5) x lands on texel 1's centre, and column 2 weighs nothing */
static void test_minmax_samples(const char* dir) {
    /* each component's gather: (i, j + 1), (i + 1, j + 1), (i + 1, j), (i, j) */
    static const float gathered[4][4] = {{5, 1, 2, 7}, {0.25f, 0.5f, 0.75f, 1}, {-1, -2, -3, -4}, {8, 8, 9, 6}};
    static const struct {
        float u, v;
        float least[4], greatest[4];
    } at[] = {{0.5f, 0.5f, {1, 0.25f, -4, 6}, {7, 1, -1, 9}}, {0.375f, 0.5f, {5, 0.25f, -4, 6}, {7, 1, -1, 8}}};
    for (u32 greatest = 0; greatest < 2; greatest++)
        for (u32 c = 0; c < sizeof at / sizeof *at; c++) {
            u8 code[5 * 16] = {0};
            Word tex = tex_offset();
            set(&tex, 32, 8, 255); /* no offset register */
            set(&tex, 76, 1, 0);
            put(code, 0, mov_imm(4, f2u(at[c].u)));
            put(code, 1, mov_imm(5, f2u(at[c].v)));
            put(code, 2, tex);
            put(code, 3, exit_insn());
            put(code, 4, bra(4, 4, 7, 0));
            u32 fsph[32] = {0};
            fsph[0] = sph0(2, 5);
            fsph[18] = 0xf;
            ShaderInfo si = {0};
            si.tex_minmax_mask = 1;
            si.tex_minmax_max = greatest;
            size_t count = 0;
            u32* spv = sm86_to_spirv(code, sizeof code, ST_FS, fsph, &si, &count);
            CHECK(spv != NULL);
            if (!spv) {
                fprintf(stderr, "min/max sample: %s\n", si.first_unsupported);
                return;
            }
            CHECK(si.tex_minmax_mask == 1 && si.tex_minmax_max == greatest); /* for the bind, which reads them */
            u32 found[8];
            CHECK(spv_find(spv, count, 96 /* OpImageGather */, found, 8) == 4);
            CHECK(spv_find(spv, count, 87 /* OpImageSampleImplicitLod */, found, 8) == 0);
            if (!greatest && !c) CHECK(keep_module(dir, "tex_minmax", spv, count));
            SpvEvalEnv env = {0};
            env.image_size[SNX_VKB_TEX][0] = env.image_size[SNX_VKB_TEX][1] = 4;
            for (u32 g = 0; g < 4; g++)
                for (u32 t = 0; t < 4; t++) env.gather[SNX_VKB_TEX][g][t] = f2u(gathered[g][t]);
            const char* why = spv_eval(spv, count, &env);
            const float* want = greatest ? at[c].greatest : at[c].least;
            int same = !why && env.out_written[0];
            for (u32 g = 0; same && g < 4; g++) same = u2f(env.out[0][g]) == want[g];
            if (!same)
                fprintf(stderr, "min/max sample's %s at (%g, %g): %s; read %g %g %g %g, want %g %g %g %g\n",
                        greatest ? "greatest" : "least", (double)at[c].u, (double)at[c].v, why ? why : "ran",
                        (double)u2f(env.out[0][0]), (double)u2f(env.out[0][1]), (double)u2f(env.out[0][2]),
                        (double)u2f(env.out[0][3]), (double)want[0], (double)want[1], (double)want[2], (double)want[3]);
            CHECK(same);
            free(spv);
        }
}
/* TEX, TLD and TLD4 with texel offsets: a register every path sets to one immediate is ConstOffset, as the program
   packs it (four bits a component); a register two paths set apart moves the coordinates instead. TLD adds its
   offset to the coordinates, and TLD4's offset for each texel takes four gathers */
static void test_texel_offsets(const char* dir) {
    u8 code[16 * 16];
    ShaderInfo si;
    size_t count = 0;
    i32 ox = 0, oy = 0;
    u32 n, *spv;

    /* MOV R10, 0xf1; TEX: (1, -1) */
    memset(code, 0, sizeof code);
    n = 0;
    put(code, n++, mov_imm(10, 0xf1));
    put(code, n++, tex_offset());
    put(code, n, exit_insn());
    n++;
    put(code, n, bra(n, n, 7, 0));
    n++;
    spv = translate_fs(code, n, &si, &count);
    CHECK(spv != NULL);
    if (spv) {
        CHECK(sample_const_offset(spv, count, &ox, &oy) == 1 && ox == 1 && oy == -1);
        CHECK(keep_module(dir, "tex_offset", spv, count));
    }
    free(spv);

    /* @P0 BRA L; MOV R10, 0x10; BRA M; L: MOV R10, 0x10; M: TEX: both paths set (0, 1) */
    memset(code, 0, sizeof code);
    put(code, 0, bra(0, 3, 0, 0));
    put(code, 1, mov_imm(10, 0x10));
    put(code, 2, bra(2, 4, 7, 0));
    put(code, 3, mov_imm(10, 0x10));
    put(code, 4, tex_offset());
    put(code, 5, exit_insn());
    put(code, 6, bra(6, 6, 7, 0));
    spv = translate_fs(code, 7, &si, &count);
    CHECK(spv != NULL);
    if (spv) {
        CHECK(sample_const_offset(spv, count, &ox, &oy) == 1 && ox == 0 && oy == 1);
        CHECK(keep_module(dir, "tex_offset_paths", spv, count));
    }
    free(spv);

    /* the same with the paths apart (0x10 and 0x01): no ConstOffset, the coordinates move by the level's size */
    put(code, 3, mov_imm(10, 0x01));
    spv = translate_fs(code, 7, &si, &count);
    CHECK(spv != NULL);
    if (spv) {
        u32 at[4];
        CHECK(sample_const_offset(spv, count, &ox, &oy) == 0);
        CHECK(spv_find(spv, count, 103 /* OpImageQuerySizeLod */, at, 4) == 1);
        CHECK(keep_module(dir, "tex_offset_apart", spv, count));
    }
    free(spv);

    /* a guarded MOV, or none on the path from the start: not a constant either */
    put(code, 3, mov_imm(10, 0x10));
    {
        Word guarded = mov_imm(10, 0x10);
        set(&guarded, 12, 3, 1); /* @P1 */
        put(code, 1, guarded);
    }
    spv = translate_fs(code, 7, &si, &count);
    CHECK(spv && sample_const_offset(spv, count, &ox, &oy) == 0);
    free(spv);
    put(code, 1, mov_imm(11, 0x10)); /* the other path writes another register */
    spv = translate_fs(code, 7, &si, &count);
    CHECK(spv && sample_const_offset(spv, count, &ox, &oy) == 0);
    free(spv);

    /* TLD4.SCR R0-R3, R4, R5, offsets R10 and R11 for each texel: four gathers, each with an Offset */
    memset(code, 0, sizeof code);
    n = 0;
    {
        Word x = fix(0xb63);
        set(&x, 24, 8, 4);
        set(&x, 32, 8, 10);
        set(&x, 40, 14, 0x10);
        set(&x, 54, 5, 2);
        set(&x, 60, 1, 1);
        set(&x, 61, 3, 1);
        set(&x, 64, 8, 255);
        set(&x, 72, 4, 0xf);
        set(&x, 76, 2, 2);
        set(&x, 81, 3, 7);
        put(code, n++, x);
    }
    put(code, n, exit_insn());
    n++;
    put(code, n, bra(n, n, 7, 0));
    n++;
    spv = translate_fs(code, n, &si, &count);
    CHECK(spv != NULL);
    if (spv) {
        u32 at[8];
        u32 gathers = spv_find(spv, count, 96 /* OpImageGather */, at, 8), with = 0;
        for (u32 k = 0; k < gathers; k++) with += (spv[at[k]] >> 16) == 8 && spv[at[k] + 6] == 16;
        CHECK(gathers == 4 && with == 4);
        CHECK(keep_module(dir, "tld4_offsets", spv, count));
    }
    free(spv);

    /* TLD.SCR.LL R0-R3, R4, R6, offset R7 (list1: the level R6, then R7): translated, fetching without Offset */
    memset(code, 0, sizeof code);
    n = 0;
    {
        Word x = fix(0xb66);
        set(&x, 24, 8, 4);
        set(&x, 32, 8, 6);
        set(&x, 40, 14, 0x10);
        set(&x, 54, 5, 2);
        set(&x, 61, 3, 1);
        set(&x, 64, 8, 255);
        set(&x, 72, 4, 0xf);
        set(&x, 76, 1, 1);
        set(&x, 81, 3, 7);
        set(&x, 87, 3, 3);
        put(code, n++, x);
    }
    put(code, n, exit_insn());
    n++;
    put(code, n, bra(n, n, 7, 0));
    n++;
    spv = translate_fs(code, n, &si, &count);
    CHECK(spv != NULL);
    if (spv) {
        u32 at[4];
        CHECK(spv_find(spv, count, 95 /* OpImageFetch */, at, 4) == 1 && !(spv[at[0] + 5] & 0x18));
        CHECK(keep_module(dir, "tld_offset", spv, count));
    }
    free(spv);
}

/* ULDC.64 UR4, c[0][off] */
static Word uldc64(u32 off) {
    Word x = fix(0xab9);
    set(&x, 16, 6, 4);
    cbuf(&x, 0, off);
    set(&x, 73, 3, 5);
    return x;
}
/* An address's base along the flow: UR4 is c[0][0x20] where @P0 BRA jumps to the LDG, and c[0][0x40] on the path
   that exits before it. The LDG's buffer is the first; in program order it was the second. With both paths reaching
   the LDG, they disagree, and the program is refused */
static void test_global_flow(const char* dir) {
    u8 code[16 * 8];
    memset(code, 0, sizeof code);
    put(code, 0, uldc64(0x20));
    put(code, 1, bra(1, 4, 0, 0));
    put(code, 2, uldc64(0x40));
    put(code, 3, exit_insn());
    {
        Word x = fix(0x981); /* LDG.E R2, [R0.U32 + UR4] */
        set(&x, 16, 8, 2);
        set(&x, 24, 8, 0);
        set(&x, 32, 6, 4);
        set(&x, 72, 1, 1);
        set(&x, 73, 3, 4);
        set(&x, 81, 3, 7);
        set(&x, 91, 1, 1);
        put(code, 4, x);
    }
    put(code, 5, exit_insn());
    put(code, 6, bra(6, 6, 7, 0));
    ShaderInfo si = {0};
    size_t count = 0;
    u32* spv = sm86_to_spirv(code, 7 * 16, ST_CS, NULL, &si, &count);
    CHECK(spv != NULL);
    if (!spv) fprintf(stderr, "global flow: %s\n", si.first_unsupported);
    CHECK(si.ngbuf == 1 && si.gbuf_cbuf_off[0] == 0x20);
    if (spv) CHECK(keep_module(dir, "global_flow", spv, count));
    free(spv);
    put(code, 3, mov_imm(5, 1)); /* the second path goes on to the LDG */
    si = (ShaderInfo){0};
    spv = sm86_to_spirv(code, 7 * 16, ST_CS, NULL, &si, &count);
    CHECK(!spv && strstr(si.first_unsupported, "global address whose base"));
    free(spv);
}

/* FSWZADD R0, R1, R2 with swizzle 0x1b: lane 0 (the swizzle's top two bits) adds, lane 1 does b - a, lane 2 a - b
   and lane 3 gives b, as Maxwell's (nouveau's QUADOP). R1 = 1.5, R2 = 4 */
static void test_fswzadd(const char* dir) {
    u8 code[16 * 5] = {0};
    put(code, 0, mov_imm(1, f2u(1.5f)));
    put(code, 1, mov_imm(2, f2u(4.0f)));
    Word x = fix(0x822);
    set(&x, 16, 8, 0);
    set(&x, 24, 8, 1);
    set(&x, 32, 8, 0x1b);
    set(&x, 64, 8, 2);
    put(code, 2, x);
    put(code, 3, exit_insn());
    put(code, 4, bra(4, 4, 7, 0));
    static const float want[4] = {5.5f, 2.5f, -2.5f, 4.0f};
    for (u32 lane = 0; lane < 4; lane++) {
        u32 out[4];
        CHECK(run_fs_at(code, 5, lane, NULL, 0, out));
        CHECK(out[0] == f2u(want[lane]));
        if (out[0] != f2u(want[lane])) fprintf(stderr, "fswzadd: lane %u gives %08x\n", lane, out[0]);
    }
    /* a modifier the translation does not know is refused */
    set(&x, 80, 1, 1);
    put(code, 2, x);
    u32 fsph[32] = {0};
    fsph[0] = sph0(2, 5);
    fsph[18] = 0xf;
    ShaderInfo si = {0};
    size_t count = 0;
    CHECK(!sm86_to_spirv(code, 5 * 16, ST_FS, fsph, &si, &count));
    set(&x, 80, 1, 0);
    put(code, 2, x);
    si = (ShaderInfo){0};
    u32* spv = sm86_to_spirv(code, 5 * 16, ST_FS, fsph, &si, &count);
    CHECK(spv && keep_module(dir, "fs_fswzadd", spv, count));
    free(spv);
}

/* S2R R0, SR_LTMASK in a fragment program: the lanes below the invocation's, as in a compute program. A fragment
   program's warp-wide atomics read it with a VOTE ballot */
static void test_lane_masks(const char* dir) {
    u8 code[16 * 3] = {0};
    Word x = fix(0x919);
    set(&x, 16, 8, 0);
    set(&x, 72, 8, 0x39);
    put(code, 0, x);
    put(code, 1, exit_insn());
    put(code, 2, bra(2, 2, 7, 0));
    u32 out[4];
    CHECK(run_fs_at(code, 3, 5, NULL, 0, out) && out[0] == 0x1fu);
    x = fix(0x919); /* SR_TID.X stays a compute program's */
    set(&x, 72, 8, 0x21);
    put(code, 0, x);
    u32 fsph[32] = {0};
    fsph[0] = sph0(2, 5);
    fsph[18] = 0xf;
    ShaderInfo si = {0};
    size_t count = 0;
    CHECK(!sm86_to_spirv(code, 3 * 16, ST_FS, fsph, &si, &count));
    (void)dir;
}

/* SHFL with its lane operand and c each an immediate or a register (II 0xf89, RI 0x589, IR 0x989, RR 0x389), run at
   every lane of a warp: the lane it reads and whether that lane is in range are PTX's shfl.sync, the same in the
   four forms. c 0x1c03 is a segment of 4 lanes, 0x1807 of 8, 0x1c1f a segment of 4 whose clamp says 32, 0x1f the
   warp. A compute network's upscaler has c in a register */
static Word predicated(Word x, u32 pred);
static void test_shfl_operands(const char* dir) {
    static const u32 forms[4] = {0xf89, 0x589, 0x989, 0x389}, cs[4] = {0x1c03, 0x1807, 0x1c1f, 0x1f};
    static const u32 bs[4] = {0, 1, 3, 30};
    u32 fsph[32] = {0};
    fsph[0] = sph0(2, 5);
    fsph[18] = 0xf;
    int bad = 0;
    for (u32 f = 0; f < 4; f++)
        for (u32 mode = 0; mode < 4; mode++)
            for (u32 ci = 0; ci < 4; ci++)
                for (u32 bi = 0; bi < 4; bi++) {
                    int lane_reg = forms[f] == 0x589 || forms[f] == 0x389,
                        c_reg = forms[f] == 0x989 || forms[f] == 0x389;
                    u32 b = bs[bi], cc = cs[ci];
                    u8 code[16 * 8] = {0};
                    put(code, 0, mov_imm(2, 0x1234));
                    put(code, 1, mov_imm(4, cc | 0xffff0000u)); /* the bits above c's 13 are not its */
                    put(code, 2, mov_imm(5, b));
                    put(code, 3, mov_imm(0, 0));
                    Word x = fix(forms[f]); /* SHFL P0, R6, R2, b, c */
                    set(&x, 16, 8, 6);
                    set(&x, 24, 8, 2);
                    if (lane_reg)
                        set(&x, 32, 8, 5);
                    else
                        set(&x, 53, 5, b);
                    if (c_reg)
                        set(&x, 64, 8, 4);
                    else
                        set(&x, 40, 13, cc);
                    set(&x, 58, 2, mode);
                    set(&x, 81, 3, 0);
                    put(code, 4, x);
                    put(code, 5, predicated(mov_imm(0, 1), 0));
                    put(code, 6, exit_insn());
                    put(code, 7, bra(7, 7, 7, 0));
                    ShaderInfo si = {0};
                    size_t count = 0;
                    u32* spv = sm86_to_spirv(code, 8 * 16, ST_FS, fsph, &si, &count);
                    if (!spv) {
                        if (!bad++) fprintf(stderr, "shfl 0x%x mode %u: %s\n", forms[f], mode, si.first_unsupported);
                        continue;
                    }
                    if (mode == 0 && ci == 0 && bi == 0) {
                        char name[32];
                        snprintf(name, sizeof name, "fs_shfl_%x", forms[f]);
                        CHECK(keep_module(dir, name, spv, count));
                    }
                    for (u32 lane = 0; lane < 32; lane++) {
                        u32 seg = (cc >> 8) & 31, min_lane = lane & seg, max_lane = min_lane | (cc & ~seg & 31);
                        u32 j = mode == 0   ? (min_lane | (b & ~seg & 31))
                                : mode == 1 ? lane - b
                                : mode == 2 ? lane + b
                                            : lane ^ b;
                        u32 ok = mode == 1 ? (i32)j >= (i32)min_lane : j <= max_lane;
                        SpvEvalEnv env = {0};
                        env.builtin[41][0] = lane;
                        env.shuffle_id = ~0u;
                        const char* why = spv_eval(spv, count, &env);
                        if (why || !env.out_written[0] || env.out[0][0] != ok || env.shuffle_id != (j & 31)) {
                            if (!bad++)
                                fprintf(stderr,
                                        "shfl 0x%x mode %u c 0x%x b %u lane %u: reads lane %u in range %u, wants %u "
                                        "and %u%s%s\n",
                                        forms[f], mode, cc, b, lane, env.shuffle_id, env.out[0][0], j & 31, ok,
                                        why ? ", the evaluator stops at " : "", why ? why : "");
                        }
                    }
                    free(spv);
                }
    CHECK(bad == 0);
    if (bad) fprintf(stderr, "shfl: %d cases differ\n", bad);
}

/* LDC c[bank][R + offset] of NAK's MemType U8 (0), I8 (1), U16 (2) and I16 (3): the byte or halfword at the
   address, zero or sign extended */
static Word ldc(u32 dst, u32 index, u32 bank, u32 off, u32 type) {
    Word x = fix(0xb82);
    set(&x, 16, 8, dst);
    set(&x, 24, 8, index);
    cbuf(&x, bank, off);
    set(&x, 73, 3, type);
    return x;
}
static void test_ldc_small(const char* dir) {
    u8 code[16 * 7] = {0};
    put(code, 0, mov_imm(4, 1));
    put(code, 1, ldc(0, 4, 1, 0x4, 0));   /* the byte at 5 */
    put(code, 2, ldc(1, 4, 1, 0x6, 1));   /* the byte at 7, signed */
    put(code, 3, ldc(2, 4, 1, 0x9, 2));   /* the halfword at 10 */
    put(code, 4, ldc(3, 255, 1, 0x6, 3)); /* the halfword at 6, signed */
    put(code, 5, exit_insn());
    put(code, 6, bra(6, 6, 7, 0));
    static const u32 cb1[3] = {0x03020100u, 0x87068504u, 0x8899aabbu};
    u32 out[4];
    CHECK(run_fs_at(code, 7, 0, cb1, 3, out));
    CHECK(out[0] == 0x85u && out[1] == 0xffffff87u && out[2] == 0x8899u && out[3] == 0xffff8706u);
    if (out[0] != 0x85u || out[1] != 0xffffff87u || out[2] != 0x8899u || out[3] != 0xffff8706u)
        fprintf(stderr, "ldc small: %08x %08x %08x %08x\n", out[0], out[1], out[2], out[3]);
    (void)dir;
}

/* BSSY B0, 2; BREAK B0; BSYNC B0; MOV R0, 1.0: BREAK takes the lanes out of the barrier and goes on */
static void test_break(const char* dir) {
    u8 code[16 * 6] = {0};
    Word x = fix(0x945);
    set(&x, 16, 4, 0);
    set(&x, 34, 48, (u64)(2 * 4 - 0 * 4 - 4));
    set(&x, 87, 3, 7);
    put(code, 0, x);
    x = fix(0x942);
    set(&x, 16, 4, 0);
    set(&x, 87, 3, 7);
    put(code, 1, x);
    x = fix(0x941);
    set(&x, 16, 4, 0);
    set(&x, 87, 3, 7);
    put(code, 2, x);
    put(code, 3, mov_imm(0, f2u(1.0f)));
    put(code, 4, exit_insn());
    put(code, 5, bra(5, 5, 7, 0));
    u32 out[4];
    CHECK(run_fs_at(code, 6, 0, NULL, 0, out) && out[0] == f2u(1.0f));
    (void)dir;
}

/* LDL and STL: the address register at 24, the signed offset at 40-63, NAK's MemType at 73, STL's data at 32 */
static Word local_op(u32 opcode, u32 reg, u32 addr, u32 off, u32 type) {
    Word x = fix(opcode);
    set(&x, opcode == 0x983 ? 16 : 32, 8, reg);
    set(&x, 24, 8, addr);
    set(&x, 40, 24, off);
    set(&x, 73, 3, type);
    return x;
}
/* STL.64 of R1:R2 at 8, a byte at 0xd, and LDL back: the words and the byte, sign extended */
static void test_local_memory(const char* dir) {
    u8 code[16 * 11] = {0};
    put(code, 0, mov_imm(1, 0x11111111u));
    put(code, 1, mov_imm(2, 0x22222222u));
    put(code, 2, local_op(0x387, 1, 255, 0x8, 5));
    put(code, 3, mov_imm(5, 0x95));
    put(code, 4, local_op(0x387, 5, 255, 0xd, 0));
    put(code, 5, mov_imm(3, 4));
    put(code, 6, local_op(0x983, 0, 3, 0x4, 4));
    put(code, 7, local_op(0x983, 1, 3, 0x8, 4));
    put(code, 8, local_op(0x983, 2, 255, 0xd, 1));
    put(code, 9, exit_insn());
    put(code, 10, bra(10, 10, 7, 0));
    u32 out[4];
    CHECK(run_fs_at(code, 11, 0, NULL, 0, out));
    CHECK(out[0] == 0x11111111u && out[1] == 0x22229522u && out[2] == 0xffffff95u);
    if (out[0] != 0x11111111u || out[1] != 0x22229522u || out[2] != 0xffffff95u)
        fprintf(stderr, "local memory: %08x %08x %08x\n", out[0], out[1], out[2]);
    u32 fsph[32] = {0};
    fsph[0] = sph0(2, 5);
    fsph[1] = 0x10; /* the SPH's local memory: 16 bytes */
    fsph[18] = 0xf;
    ShaderInfo si = {0};
    size_t count = 0;
    u32* spv = sm86_to_spirv(code, 11 * 16, ST_FS, fsph, &si, &count);
    CHECK(spv && keep_module(dir, "fs_local", spv, count));
    free(spv);
}

/* LDS takes a uniform register with bit 91 alone: UR0 is 0x40, and LDS R2, [RZ + 0x10] without the bit reads the
   word STS stored at 0x10 */
static void test_shared_no_uniform(const char* dir) {
    u8 code[16 * 7] = {0};
    Word x = alu(0x082, 4, 0, 0); /* UMOV UR0, 0x40 */
    set(&x, 32, 32, 0x40);
    set(&x, 91, 1, 1);
    put(code, 0, x);
    put(code, 1, mov_imm(1, 7));
    x = fix(0x388); /* STS [RZ + 0x10], R1 */
    set(&x, 24, 8, 255);
    set(&x, 32, 8, 1);
    set(&x, 40, 24, 0x10);
    set(&x, 73, 3, 4);
    put(code, 2, x);
    x = fix(0x984); /* LDS R2, [RZ + 0x10] */
    set(&x, 16, 8, 2);
    set(&x, 24, 8, 255);
    set(&x, 40, 24, 0x10);
    set(&x, 73, 3, 4);
    put(code, 3, x);
    x = fix(0x388); /* STS [RZ + 0x20], R2 */
    set(&x, 24, 8, 255);
    set(&x, 32, 8, 2);
    set(&x, 40, 24, 0x20);
    set(&x, 73, 3, 4);
    put(code, 4, x);
    put(code, 5, exit_insn());
    put(code, 6, bra(6, 6, 7, 0));
    ShaderInfo si = {0};
    si.cs_local[0] = si.cs_local[1] = si.cs_local[2] = 1;
    si.shared_size = 256;
    size_t count = 0;
    u32* spv = sm86_to_spirv(code, 7 * 16, ST_CS, NULL, &si, &count);
    CHECK(spv != NULL);
    if (!spv) return;
    SpvEvalEnv env = {0};
    const char* why = spv_eval(spv, count, &env);
    if (why) fprintf(stderr, "shared: the evaluator stops at %s\n", why);
    CHECK(!why && env.shared_words >= 9 && env.shared[8] == 7);
    CHECK(keep_module(dir, "cs_shared", spv, count));
    free(spv);
}

/* the program code (n instructions) of stage, with header sph, translated and run once in env: 1 when it ran */
static int run_stage(int stage, const u8* code, u32 n, const u32* sph, SpvEvalEnv* env) {
    ShaderInfo si = {0};
    size_t count = 0;
    u32* spv = sm86_to_spirv(code, n * 16, stage, sph, &si, &count);
    if (!spv) {
        fprintf(stderr, "run_stage: %s\n", si.first_unsupported);
        return 0;
    }
    const char* why = spv_eval(spv, count, env);
    free(spv);
    if (why) fprintf(stderr, "run_stage: the evaluator stops at %s\n", why);
    return !why;
}
static Word predicated(Word x, u32 pred) {
    set(&x, 12, 3, pred);
    return x;
}

/* HSETP2 compares the halves pair by pair: R1 is (low 2.0, high 1.0). Form 2, GT against two halves of 1.5: P0 the
   low pair's, true, and P1 the high pair's, false. Form 3, LT of R1.H0_H0 against c[1][0].H1_H1 (3.0): P2 true */
static void test_hsetp2(const char* dir) {
    u8 code[16 * 8] = {0};
    put(code, 0, mov_imm(1, 0x3c004000u));
    Word x = alu(0x034, 2, 0, 1);
    set(&x, 32, 32, 0x3e003e00u);
    set(&x, 76, 4, 4);
    set(&x, 81, 3, 0);
    set(&x, 84, 3, 1);
    set(&x, 87, 3, 7);
    put(code, 1, x);
    x = alu(0x034, 3, 0, 1);
    cbuf(&x, 1, 0x0);
    set(&x, 60, 2, 3);
    set(&x, 74, 2, 2);
    set(&x, 76, 4, 1);
    set(&x, 81, 3, 2);
    set(&x, 84, 3, 7);
    set(&x, 87, 3, 7);
    put(code, 2, x);
    put(code, 3, predicated(mov_imm(0, f2u(1.0f)), 2));
    put(code, 4, predicated(mov_imm(2, f2u(1.0f)), 0));
    put(code, 5, predicated(mov_imm(3, f2u(1.0f)), 1));
    put(code, 6, exit_insn());
    put(code, 7, bra(7, 7, 7, 0));
    static const u32 cb1[1] = {0x42000000u}; /* the high half 3.0 */
    u32 out[4];
    CHECK(run_fs_at(code, 8, 0, cb1, 1, out));
    CHECK(out[0] == f2u(1.0f) && out[2] == f2u(1.0f) && out[3] == 0);
    if (out[0] != f2u(1.0f) || out[2] != f2u(1.0f) || out[3] != 0)
        fprintf(stderr, "hsetp2: %08x %08x %08x\n", out[0], out[2], out[3]);
    (void)dir;
}

/* BMOV.32.CLEAR RZ, B0 changes nothing the program computes; into a register it is refused */
static void test_bmov(const char* dir) {
    u8 code[16 * 4] = {0};
    Word x = fix(0x355);
    set(&x, 16, 8, 255);
    set(&x, 84, 1, 1);
    put(code, 0, x);
    put(code, 1, mov_imm(0, f2u(1.0f)));
    put(code, 2, exit_insn());
    put(code, 3, bra(3, 3, 7, 0));
    u32 out[4];
    CHECK(run_fs_at(code, 4, 0, NULL, 0, out) && out[0] == f2u(1.0f));
    set(&x, 16, 8, 5);
    put(code, 0, x);
    u32 fsph[32] = {0};
    fsph[0] = sph0(2, 5);
    fsph[18] = 0xf;
    ShaderInfo si = {0};
    size_t count = 0;
    CHECK(!sm86_to_spirv(code, 4 * 16, ST_FS, fsph, &si, &count));
    (void)dir;
}

/* The prologue of the debug variants of the driver's programs: BMOV.32 B0, 0xffffffff, BMOV.32.CLEAR B1, B0 and
   BMOV.32 B15, 0x0 write only convergence barriers, which change nothing the program computes. HFMA2.MMA R0, -RZ,
   RZ, imm, imm loads its two halves: R0 is (-2.0, 1.0) as its immediate 0x3c00c000, and R1 with +RZ (+0 + -0) is
   +0 in its low half. A BMOV into a state register is refused */
static void test_debug_prologue(const char* dir) {
    u8 code[16 * 9] = {0};
    Word bi = fix(0x956);
    set(&bi, 32, 32, 0xffffffffu);
    put(code, 0, bi);
    Word bl = fix(0xf55);
    set(&bl, 16, 8, 1);
    set(&bl, 24, 8, 0);
    set(&bl, 84, 1, 1);
    put(code, 1, bl);
    Word b15 = fix(0x956);
    set(&b15, 24, 8, 15);
    put(code, 2, b15);
    Word h = alu(0x035, 2, 0, 255);
    set(&h, 32, 32, 0x3c00c000u);
    set(&h, 64, 8, 255);
    set(&h, 72, 1, 1);
    put(code, 3, h);
    Word h2 = alu(0x035, 2, 1, 255);
    set(&h2, 32, 32, 0x00008000u);
    set(&h2, 64, 8, 255);
    put(code, 4, h2);
    put(code, 5, mov_imm(2, 0));
    put(code, 6, mov_imm(3, 0));
    put(code, 7, exit_insn());
    put(code, 8, bra(8, 8, 7, 0));
    u32 out[4];
    int ran = run_fs_at(code, 9, 0, NULL, 0, out);
    CHECK(ran && out[0] == 0x3c00c000u && out[1] == 0);
    if (ran && (out[0] != 0x3c00c000u || out[1] != 0)) fprintf(stderr, "debug prologue: %08x %08x\n", out[0], out[1]);
    u32 fsph[32] = {0};
    fsph[0] = sph0(2, 5);
    fsph[18] = 0xf;
    ShaderInfo si = {0};
    size_t count = 0;
    set(&bi, 24, 8, 0x1a); /* MACTIVE */
    put(code, 0, bi);
    CHECK(!sm86_to_spirv(code, 9 * 16, ST_FS, fsph, &si, &count));
    put(code, 0, fix(0x918));
    set(&bl, 16, 8, 0x1a);
    put(code, 1, bl);
    CHECK(!sm86_to_spirv(code, 9 * 16, ST_FS, fsph, &si, &count));
    (void)dir;
}

/* The first texture coordinate (0x300-0x30f) moves to generic 22 (17 + its group, 5) in both stages: AST a[0x304]
   writes output 22's y, and IPA a[0x300] reads input 22's x, with the interpolation the header's texture coordinate
   map gives it (word 15, constant), not the FragCoord.w that a perspective input takes */
static void test_legacy_varyings(const char* dir) {
    u8 code[16 * 4] = {0};
    put(code, 0, mov_imm(1, 0x12345678u));
    Word x = fix(0x322);
    set(&x, 24, 8, 255);
    set(&x, 32, 8, 1);
    set(&x, 40, 10, 0x304);
    set(&x, 64, 8, 255);
    put(code, 1, x);
    put(code, 2, exit_insn());
    put(code, 3, bra(3, 3, 7, 0));
    u32 vsph[32] = {0};
    vsph[0] = sph0(1, 1);
    SpvEvalEnv env = {0};
    CHECK(run_stage(ST_VS, code, 4, vsph, &env) && env.out_written[22] && env.out[22][1] == 0x12345678u);
    x = fix(0x326);
    set(&x, 16, 8, 0);
    set(&x, 32, 8, 255);
    set(&x, 64, 8, 0x300 >> 2);
    put(code, 0, x);
    put(code, 1, exit_insn());
    put(code, 2, bra(2, 2, 7, 0));
    u32 fsph[32] = {0};
    fsph[0] = sph0(2, 5);
    fsph[15] = 0x55; /* texture coordinate 0: constant */
    fsph[18] = 0xf;
    env = (SpvEvalEnv){0};
    env.in[22][0] = f2u(3.0f);
    env.builtin[15][3] = f2u(2.0f); /* FragCoord.w */
    CHECK(run_stage(ST_FS, code, 3, fsph, &env) && env.out[0][0] == f2u(3.0f));
    /* a program that reads texture coordinate 0 and generic 22 itself is refused */
    x = fix(0x326);
    set(&x, 16, 8, 1);
    set(&x, 32, 8, 255);
    set(&x, 64, 8, (0x80 + 22 * 16) >> 2);
    put(code, 1, x);
    put(code, 2, exit_insn());
    put(code, 3, bra(3, 3, 7, 0));
    ShaderInfo si = {0};
    size_t count = 0;
    CHECK(!sm86_to_spirv(code, 4 * 16, ST_FS, fsph, &si, &count));
    (void)dir;
}

/* BRXU UR3 with the table that ULDC.IDX reads at c[1][UR3 + 0x10], its index bounded on the uniform datapath:
   UISETP.LT.U32 UP0, UR1, 1; USEL UR2, UR1, 1, UP0; USHF.L.U32 UR3, UR2, 2. Two entries, then a word that names
   instruction 0, which the bound leaves out. UR1 is 1: the second case, R0 = 2.0 */
static Word ualu(u32 op9, u32 dst, u32 a, u32 imm) {
    Word x = alu(op9, 4, dst, a);
    set(&x, 32, 32, imm);
    set(&x, 64, 6, 63);
    set(&x, 91, 1, 1);
    return x;
}
static int table3_read(u32 idx, u32 off, u32* out) {
    static const u32 table[3] = {7 * 16, 9 * 16, 0};
    if (idx != 1 || off < 0x10 || off >= 0x10 + sizeof table) return 0;
    *out = table[(off - 0x10) / 4];
    return 1;
}
static void test_brxu(const char* dir) {
    u8 code[16 * 12] = {0};
    put(code, 0, ualu(0x082, 1, 0, 1)); /* UMOV UR1, 1 */
    Word x = ualu(0x08c, 0, 1, 1);      /* UISETP.LT.U32.AND UP0, PT, UR1, 1, PT */
    set(&x, 76, 3, 1);
    set(&x, 81, 3, 0);
    set(&x, 84, 3, 7);
    set(&x, 87, 3, 7);
    put(code, 1, x);
    x = ualu(0x087, 2, 1, 1); /* USEL UR2, UR1, 1, UP0 */
    set(&x, 87, 3, 0);
    put(code, 2, x);
    x = ualu(0x099, 3, 2, 2); /* USHF.L.U32 UR3, UR2, 2, URZ */
    set(&x, 73, 2, 3);
    put(code, 3, x);
    x = fix(0xabb); /* ULDC.IDX UR3, c[1][UR3 + 0x10] */
    set(&x, 16, 6, 3);
    set(&x, 24, 6, 3);
    cbuf(&x, 1, 0x10);
    set(&x, 73, 3, 4);
    put(code, 4, x);
    x = fix(0x958); /* BRXU UR3, -(the next instruction's address) */
    set(&x, 24, 6, 3);
    set(&x, 34, 48, (u64)0 - 24);
    set(&x, 87, 3, 7);
    put(code, 5, x);
    put(code, 6, exit_insn());
    put(code, 7, mov_imm(0, f2u(1.0f)));
    put(code, 8, exit_insn());
    put(code, 9, mov_imm(0, f2u(2.0f)));
    put(code, 10, exit_insn());
    put(code, 11, bra(11, 11, 7, 0));
    static const u32 cb1[7] = {0, 0, 0, 0, 7 * 16, 9 * 16, 0};
    g_shader_cb_read = table3_read;
    u32 out[4];
    CHECK(run_fs_at(code, 12, 0, cb1, 7, out) && out[0] == f2u(2.0f));
    u32 fsph[32] = {0};
    fsph[0] = sph0(2, 5);
    fsph[18] = 0xf;
    ShaderInfo si = {0};
    size_t count = 0;
    u32* spv = sm86_to_spirv(code, 12 * 16, ST_FS, fsph, &si, &count);
    g_shader_cb_read = NULL;
    CHECK(spv != NULL);
    if (spv) {
        u32 at[4];
        CHECK(spv_find(spv, count, 251 /* OpSwitch */, at, 4) == 1);
        CHECK((spv[at[0]] >> 16) == 7); /* two cases: the bound's */
        CHECK(keep_module(dir, "fs_brxu", spv, count));
    }
    free(spv);
}

/* IPA R0, a[UR4 + 0x80] with UR4 = 0x10: input 1's x. The header reads inputs 0 and 1 (constant) */
static void test_ipa_uniform(const char* dir) {
    u8 code[16 * 4] = {0};
    put(code, 0, ualu(0x082, 4, 0, 0x10)); /* UMOV UR4, 0x10 */
    Word x = fix(0x526);
    set(&x, 16, 8, 0);
    set(&x, 24, 8, 4);
    set(&x, 32, 8, 255);
    set(&x, 64, 8, 0x80 >> 2);
    set(&x, 91, 1, 1);
    put(code, 1, x);
    put(code, 2, exit_insn());
    put(code, 3, bra(3, 3, 7, 0));
    u32 fsph[32] = {0};
    fsph[0] = sph0(2, 5);
    fsph[6] = 0x0101;
    fsph[18] = 0xf;
    SpvEvalEnv env = {0};
    env.in[0][0] = f2u(7.0f);
    env.in[1][0] = f2u(5.0f);
    CHECK(run_stage(ST_FS, code, 4, fsph, &env) && env.out[0][0] == f2u(5.0f));
    (void)dir;
}

/* A fragment program at a resolution scale (SNX_VKB_RS: 1 / the scale twice, then the scale). Its position is
   FragCoord over the scale, the guest's pixels. TLD.LZ of c[2][0x40] at texel p fetches p where the push constants do
   not mark the slot as bound to a scaled target. Where they do, and mark the target as one of the size drawn to, the
   pixel of texel p that lies where the fragment lies in its guest pixel: at 2x texel (5, 3) is (10, 6) for the
   fragment at (6, 4) and (11, 7) for the one at (7, 5); at 1.5x texel 4 is pixel 6 alone, where the fragment's place
   would give 7; at 0.5x texel (5, 3) is (2, 1). A scaled target of another size (a table) gives every fragment the
   pixel at the texel's centre, (11, 7) at 2x. TXQ answers a scaled target's 32x16 image over the scale, and another
   texture's as it is. A compute program fetches the texel as it is and takes no push constant: the renderer gives it
   scaled targets at the guest's size */
static Word scale_fetch(void) {
    Word x = fix(0xb66); /* TLD.LZ R8-R11, R4, R5, c[2][0x40], 2D */
    set(&x, 16, 8, 8);
    set(&x, 24, 8, 4);
    set(&x, 32, 8, 255);
    set(&x, 40, 19, 2u << 14 | 0x40 >> 2);
    set(&x, 61, 3, 1);
    set(&x, 64, 8, 255);
    set(&x, 72, 4, 0xf);
    set(&x, 87, 3, 1);
    return x;
}
static void test_resolution_scale(const char* dir) {
    static const struct {
        float scale, fx, fy; /* the fragment's FragCoord */
        /* the texel fetched, whether its slot is bound to a scaled target, and to one of the size drawn to */
        u32 px, py, scaled, own;
        u32 x, y, w, h; /* the coordinates fetched and the size answered */
    } cases[] = {
        {2.0f, 7.5f, 5.5f, 5, 3, 1, 1, 11, 7, 16, 8}, {2.0f, 6.5f, 4.5f, 5, 3, 1, 1, 10, 6, 16, 8},
        {2.0f, 7.5f, 5.5f, 5, 3, 0, 0, 5, 3, 32, 16}, {1.5f, 5.5f, 3.5f, 4, 3, 1, 1, 6, 5, 21, 11},
        {0.5f, 2.5f, 1.5f, 5, 3, 1, 1, 2, 1, 64, 32}, {2.0f, 7.5f, 5.5f, 5, 3, 1, 0, 11, 7, 16, 8},
        {2.0f, 6.5f, 4.5f, 5, 3, 1, 0, 11, 7, 16, 8}, {2.0f, 6.5f, 4.5f, 5, 3, 0, 1, 5, 3, 32, 16},
        {1.5f, 4.5f, 3.5f, 5, 3, 1, 0, 8, 5, 21, 11}, {3.0f, 0.5f, 0.5f, 5, 3, 1, 0, 16, 10, 11, 5},
        {0.5f, 2.5f, 1.5f, 5, 3, 1, 0, 2, 1, 64, 32},
    };
    u32 fsph[32] = {0};
    fsph[0] = sph0(2, 5);
    fsph[18] = 0xf;
    for (u32 k = 0; k < sizeof cases / sizeof *cases; k++) {
        u8 code[16 * 12] = {0};
        u32 n = 0;
        for (u32 i = 0; i < 2; i++) {
            Word x = fix(0x326); /* IPA R0, a[0x70] and IPA R1, a[0x74] */
            set(&x, 16, 8, i);
            set(&x, 32, 8, 255);
            set(&x, 64, 8, (0x70 + 4 * i) >> 2);
            set(&x, 81, 3, 7);
            put(code, n++, x);
        }
        put(code, n++, mov_imm(4, cases[k].px));
        put(code, n++, mov_imm(5, cases[k].py));
        put(code, n++, scale_fetch());
        Word x = fix(0xb6f); /* TXQ R2, R3, RZ, c[2][0x40]: the width and the height */
        set(&x, 16, 8, 2);
        set(&x, 24, 8, 255);
        set(&x, 40, 19, 2u << 14 | 0x40 >> 2);
        set(&x, 64, 8, 255);
        set(&x, 72, 4, 3);
        put(code, n++, x);
        put(code, n, exit_insn());
        n++;
        put(code, n, bra(n, n, 7, 0));
        n++;
        ShaderInfo si = {0};
        size_t count = 0;
        u32* spv = sm86_to_spirv(code, n * 16, ST_FS, fsph, &si, &count);
        CHECK(spv != NULL);
        if (!spv) {
            fprintf(stderr, "resolution scale: %s\n", si.first_unsupported);
            return;
        }
        if (!k) CHECK(keep_module(dir, "fs_resolution_scale", spv, count));
        CHECK(si.ntex == 1);
        float s = cases[k].scale;
        u32 scale[4] = {f2u(1.0f / s), f2u(1.0f / s), f2u(s), 0};
        SpvEvalEnv env = {0};
        env.builtin[15][0] = f2u(cases[k].fx); /* FragCoord */
        env.builtin[15][1] = f2u(cases[k].fy);
        env.ubo[SNX_VKB_RS] = scale;
        env.ubo_words[SNX_VKB_RS] = 4;
        env.push[4] = cases[k].scaled; /* the uvec4 after the fragment stage's vec4: slot 0's bit */
        env.push[8] = cases[k].own;    /* and the one after it: slot 0's target has the size drawn to */
        env.image_size[SNX_VKB_TEX][0] = 32;
        env.image_size[SNX_VKB_TEX][1] = 16;
        const char* why = spv_eval(spv, count, &env);
        if (why) fprintf(stderr, "resolution scale: the evaluator stops at %s\n", why);
        CHECK(!why && env.out_written[0]);
        free(spv);
        const char* bound = !cases[k].scaled ? "that is not"
                            : cases[k].own   ? "bound to, of the size drawn to,"
                                             : "bound to, of another size than the one drawn to,";
        float gx = cases[k].fx / s, gy = cases[k].fy / s;
        float dx = u2f(env.out[0][0]) - gx, dy = u2f(env.out[0][1]) - gy;
        int near = dx > -1e-5f && dx < 1e-5f && dy > -1e-5f && dy < 1e-5f;
        if (!near)
            fprintf(stderr,
                    "resolution scale: the fragment at (%g, %g) of a target at %gx reads its position as "
                    "(%g, %g), not the guest's (%g, %g)\n",
                    (double)cases[k].fx, (double)cases[k].fy, (double)s, (double)u2f(env.out[0][0]),
                    (double)u2f(env.out[0][1]), (double)gx, (double)gy);
        CHECK(near);
        int fetched = env.fetched[SNX_VKB_TEX][0] == cases[k].x && env.fetched[SNX_VKB_TEX][1] == cases[k].y;
        if (!fetched)
            fprintf(stderr,
                    "resolution scale: at %gx the fragment at (%g, %g) fetches texel (%u, %u) of a texture %s "
                    "a scaled target at (%u, %u), not (%u, %u)\n",
                    (double)s, (double)cases[k].fx, (double)cases[k].fy, cases[k].px, cases[k].py, bound,
                    env.fetched[SNX_VKB_TEX][0], env.fetched[SNX_VKB_TEX][1], cases[k].x, cases[k].y);
        CHECK(fetched);
        if (env.out[0][2] != cases[k].w || env.out[0][3] != cases[k].h)
            fprintf(stderr,
                    "resolution scale: at %gx the 32x16 image of a texture %s a scaled target has the size "
                    "%ux%u, not %ux%u\n",
                    (double)s, bound, env.out[0][2], env.out[0][3], cases[k].w, cases[k].h);
        CHECK(env.out[0][2] == cases[k].w && env.out[0][3] == cases[k].h);
    }
    /* the same fetch in a compute program: the texel as it is, and no push constant block */
    u8 compute[16 * 5] = {0};
    put(compute, 0, mov_imm(4, 5));
    put(compute, 1, mov_imm(5, 3));
    put(compute, 2, scale_fetch());
    put(compute, 3, exit_insn());
    put(compute, 4, bra(4, 4, 7, 0));
    ShaderInfo si = {0};
    size_t count = 0;
    u32* spv = sm86_to_spirv(compute, 5 * 16, ST_CS, NULL, &si, &count);
    CHECK(spv != NULL);
    if (!spv) {
        fprintf(stderr, "resolution scale, compute: %s\n", si.first_unsupported);
        return;
    }
    u32 vars[64], nv = spv_find(spv, count, 59 /* OpVariable */, vars, 64), pushed = 0;
    for (u32 k = 0; k < nv && k < 64; k++) pushed += spv[vars[k] + 3] == 9; /* PushConstant */
    CHECK(!pushed);
    SpvEvalEnv env = {0};
    const char* why = spv_eval(spv, count, &env);
    if (why) fprintf(stderr, "resolution scale, compute: the evaluator stops at %s\n", why);
    CHECK(!why && env.fetched[SNX_VKB_TEX][0] == 5 && env.fetched[SNX_VKB_TEX][1] == 3);
    free(spv);
}

/* TXQ R4, R5 of the handle c[2][0x20] before TLD.LZ.MS R0-R3, R4, R5, sample R9 of it, as a game's resolve programs
   do: one multisampled 2D slot (ShTex.flags bit 1) for both, the sample fetched with the Sample operand from R9, and
   the size queried without a level, as a multisampled image has none */
static void test_tld_multisample(const char* dir) {
    u8 code[16 * 5] = {0};
    Word x = fix(0xb6f);
    set(&x, 16, 8, 4);
    set(&x, 24, 8, 6);
    set(&x, 40, 19, 2u << 14 | 0x20 >> 2);
    set(&x, 64, 8, 255);
    set(&x, 72, 4, 3);
    put(code, 0, x);
    put(code, 1, mov_imm(9, 1));
    x = fix(0xb66);
    set(&x, 16, 8, 0);
    set(&x, 24, 8, 4);
    set(&x, 32, 8, 9);
    set(&x, 40, 19, 2u << 14 | 0x20 >> 2);
    set(&x, 61, 3, 1);
    set(&x, 64, 8, 255);
    set(&x, 72, 4, 0xf);
    set(&x, 78, 1, 1);
    set(&x, 87, 3, 1);
    put(code, 2, x);
    put(code, 3, exit_insn());
    put(code, 4, bra(4, 4, 7, 0));
    u32 fsph[32] = {0};
    fsph[0] = sph0(2, 5);
    fsph[18] = 0xf;
    ShaderInfo si = {0};
    size_t count = 0;
    u32* spv = sm86_to_spirv(code, 5 * 16, ST_FS, fsph, &si, &count);
    CHECK(spv != NULL);
    if (!spv) {
        fprintf(stderr, "tld multisample: %s\n", si.first_unsupported);
        return;
    }
    CHECK(si.ntex == 1 && (si.tex[0].flags & 2) && si.tex[0].type == 1 && si.tex[0].cbuf_off == 0x20 / 4);
    u32 at[4];
    CHECK(spv_find(spv, count, 25 /* OpTypeImage */, at, 4) == 1 && spv[at[0] + 6] == 1); /* MS */
    CHECK(spv_find(spv, count, 104 /* OpImageQuerySize */, at, 4) == 1);
    CHECK(spv_find(spv, count, 103 /* OpImageQuerySizeLod */, at, 4) == 0);
    /* the fetch's Sample operand (0x40) is R9 as an int: a bitcast of a load of R9's variable */
    CHECK(spv_find(spv, count, 95 /* OpImageFetch */, at, 4) == 1);
    const u32* f = spv + at[0];
    u32 r9 = 0;
    {
        u32 vars[64], nv = spv_find(spv, count, 59 /* OpVariable */, vars, 64), k = 0;
        for (u32 j = 0; j < nv; j++)
            if (spv[vars[j] + 3] == 7 /* Function */ && k++ == 9) r9 = spv[vars[j] + 2];
    }
    int sample_r9 = 0;
    if ((f[0] >> 16) == 7 && f[5] == 0x40) {
        u32 casts[64], nc = spv_find(spv, count, 124 /* OpBitcast */, casts, 64), loads[256],
                       nl = spv_find(spv, count, 61 /* OpLoad */, loads, 256);
        for (u32 j = 0; j < nc; j++)
            if (spv[casts[j] + 2] == f[6])
                for (u32 l = 0; l < nl; l++)
                    if (spv[loads[l] + 2] == spv[casts[j] + 3] && spv[loads[l] + 3] == r9) sample_r9 = 1;
    }
    CHECK(sample_r9);
    CHECK(keep_module(dir, "fs_tld_ms", spv, count));
    free(spv);
}

/* TEX.F16 (bit 79) packs its components two halves a register, as one game's text programs read them with
   HADD2.F32 R, -RZ, R.H0_H0: the texel (0.25, 0.5, 0.75, 0.625). Its alpha alone goes to R4's low half, with a zero
   high half; all four to R8 (r low, g high) and R10 (b low, a high), dst1 at 64-71. Color 0 is R4.H0 + R8.H0 (0.875),
   R8.H1 (0.5), R10.H0 (0.75), R4.H1 + R10.H1 (0.625) */
static Word tex_f16(u32 dst, u32 dst1, u32 mask) {
    Word x = fix(0xb60);
    set(&x, 16, 8, dst);
    set(&x, 24, 8, 0); /* x and y in R0 and R1 */
    set(&x, 32, 8, 255);
    set(&x, 40, 19, 2u << 14 | 0x20 >> 2);
    set(&x, 61, 3, 1); /* 2D */
    set(&x, 64, 8, dst1);
    set(&x, 72, 4, mask);
    set(&x, 79, 1, 1);
    set(&x, 81, 3, 7);
    return x;
}
/* HADD2.F32 dst, a.(swizzle sa), b.(swizzle sb): NAK's codes, 2 the low half twice, 3 the high one */
static Word hadd2_f32(u32 dst, u32 a, u32 sa, u32 b, u32 sb) {
    Word x = alu(0x030, 1, dst, a);
    set(&x, 32, 8, b);
    set(&x, 60, 2, sb);
    set(&x, 74, 2, sa);
    set(&x, 78, 1, 1);
    return x;
}
/* FTZ by the device's mode (sm86_to_spirv_ftz), as shader_to_spirv decides it. With every FADD, FMUL and FFMA FTZ
   (bit 80): the DenormFlushToZero execution mode and no flush of its own under SH_FTZ_MODE, a flush of each operand
   and result (FAbs and an FOrdLessThan) under SH_FTZ_EMULATE, neither under SH_FTZ_NATIVE. With one that keeps
   subnormals, never the mode, which is the whole module's */
static void spv_ftz_counts(const u32* w, size_t n, int* mode, int* tests) {
    *mode = *tests = 0;
    for (size_t i = 5; i < n;) {
        u32 count = w[i] >> 16, op = w[i] & 0xffffu;
        if (!count) break;
        if (op == 16 && count >= 3 && w[i + 2] == 4460) *mode = 1; /* OpExecutionMode DenormFlushToZero */
        if (op == 184) (*tests)++;                                 /* OpFOrdLessThan */
        i += count;
    }
}
static void test_ftz_modes(const char* dir) {
    u8* code = calloc(1, 0x1000);
    if (!code) abort();
    u32 n = compute_program(code), fmul = n;
    for (u32 i = 0; i < n; i++)
        if ((code[16 * i] | (u32)(code[16 * i + 1] & 1) << 8) == 0x020) fmul = i;
    CHECK(fmul < n);
    static const struct {
        int ftz, can_flush, mode, tests;
    } cases[] = {{1, SH_FTZ_MODE, 1, 0},
                 {1, SH_FTZ_EMULATE, 0, 1},
                 {1, SH_FTZ_NATIVE, 0, 0},
                 {0, SH_FTZ_MODE, 0, 0},
                 {0, SH_FTZ_EMULATE, 0, 0}};
    for (u32 k = 0; k < sizeof cases / sizeof cases[0] && fmul < n; k++) {
        if (cases[k].ftz)
            code[16 * fmul + 10] |= 1; /* bit 80 */
        else
            code[16 * fmul + 10] &= (u8)~1u;
        ShaderInfo si = {0};
        size_t count = 0;
        int mode, tests;
        u32* spv = sm86_to_spirv_ftz(code, n * 16, ST_CS, NULL, &si, &count, cases[k].can_flush);
        CHECK(spv != NULL);
        if (!spv) continue;
        spv_ftz_counts(spv, count, &mode, &tests);
        CHECK(mode == cases[k].mode && (tests > 0) == cases[k].tests);
        if (mode != cases[k].mode || (tests > 0) != cases[k].tests)
            fprintf(stderr, "ftz %d, can_flush %d: mode %d, %d tests\n", cases[k].ftz, cases[k].can_flush, mode, tests);
        char name[32];
        snprintf(name, sizeof name, "ftz%u", k);
        CHECK(keep_module(dir, name, spv, count));
        free(spv);
    }
    /* mixed: the FMUL with FTZ, and a copy of it without before the EXIT. The copy keeps subnormals, so the module
       takes no mode and the FTZ one flushes its own operands, where the device has the mode */
    if (fmul < n) {
        Word copy;
        memcpy(copy.w, code + 16 * fmul, 8);
        memcpy(copy.w + 1, code + 16 * fmul + 8, 8);
        code[16 * fmul + 10] |= 1;
        set(&copy, 80, 1, 0);
        put(code, n - 2, copy);
        put(code, n - 1, exit_insn());
        put(code, n, bra(n, n, 7, 0));
        ShaderInfo si = {0};
        size_t count = 0;
        int mode, tests;
        u32* spv = sm86_to_spirv_ftz(code, (n + 1) * 16, ST_CS, NULL, &si, &count, SH_FTZ_MODE);
        CHECK(spv != NULL);
        if (spv) {
            spv_ftz_counts(spv, count, &mode, &tests);
            CHECK(!mode && tests > 0);
            CHECK(keep_module(dir, "ftz_mixed", spv, count));
        }
        free(spv);
    }
    free(code);
}
static void test_tex_f16(const char* dir) {
    u8 code[16 * 8] = {0};
    put(code, 0, tex_f16(4, 255, 0x8));
    put(code, 1, tex_f16(8, 10, 0xf));
    put(code, 2, hadd2_f32(0, 4, 2, 8, 2));
    put(code, 3, hadd2_f32(1, 255, 0, 8, 3));
    put(code, 4, hadd2_f32(2, 255, 0, 10, 2));
    put(code, 5, hadd2_f32(3, 4, 3, 10, 3));
    put(code, 6, exit_insn());
    put(code, 7, bra(7, 7, 7, 0));
    u32 fsph[32] = {0};
    fsph[0] = sph0(2, 5);
    fsph[18] = 0xf;
    ShaderInfo si = {0};
    size_t count = 0;
    u32* spv = sm86_to_spirv(code, 8 * 16, ST_FS, fsph, &si, &count);
    CHECK(spv != NULL);
    if (!spv) {
        fprintf(stderr, "tex f16: %s\n", si.first_unsupported);
        return;
    }
    CHECK(si.ntex == 1 && si.tex[0].cbuf_off == 0x20 / 4);
    SpvEvalEnv env = {0};
    const u32 texel[4] = {f2u(0.25f), f2u(0.5f), f2u(0.75f), f2u(0.625f)};
    memcpy(env.texel[SNX_VKB_TEX], texel, sizeof texel);
    const char* why = spv_eval(spv, count, &env);
    if (why) fprintf(stderr, "tex f16: the evaluator stops at %s\n", why);
    const u32* o = env.out[0];
    int ok = !why && env.out_written[0] && o[0] == f2u(0.875f) && o[1] == f2u(0.5f) && o[2] == f2u(0.75f) &&
             o[3] == f2u(0.625f);
    CHECK(ok);
    if (!ok) fprintf(stderr, "tex f16: %08x %08x %08x %08x\n", o[0], o[1], o[2], o[3]);
    CHECK(keep_module(dir, "fs_tex_f16", spv, count));
    free(spv);
}

/* A vertex program's outputs start as (0, 0, 0, 1), as the console gives the components a program doesn't write: one
   that writes the position's x and y (0.5, 0.25) and generic 0's x (0.5) gets a w of 1 for both, which one game's
   full screen passes leave to it */
static Word ast(u32 addr, u32 data, u32 count) {
    Word x = fix(0x322);
    set(&x, 24, 8, 255);
    set(&x, 32, 8, data);
    set(&x, 40, 10, addr);
    set(&x, 64, 8, 255);
    set(&x, 74, 2, count - 1);
    return x;
}
static void test_output_defaults(const char* dir) {
    u8 code[16 * 6] = {0};
    put(code, 0, mov_imm(1, f2u(0.5f)));
    put(code, 1, mov_imm(2, f2u(0.25f)));
    put(code, 2, ast(0x70, 1, 2));
    put(code, 3, ast(0x80, 1, 1));
    put(code, 4, exit_insn());
    put(code, 5, bra(5, 5, 7, 0));
    u32 vsph[32] = {0};
    vsph[0] = sph0(1, 1);
    SpvEvalEnv env = {0};
    int ran = run_stage(ST_VS, code, 6, vsph, &env);
    const u32 *p = env.position, *g = env.out[0];
    int ok = ran && p[0] == f2u(0.5f) && p[1] == f2u(0.25f) && p[2] == 0 && p[3] == f2u(1.0f) && env.out_written[0] &&
             g[0] == f2u(0.5f) && g[1] == 0 && g[2] == 0 && g[3] == f2u(1.0f);
    CHECK(ok);
    if (!ok)
        fprintf(stderr, "output defaults: position %08x %08x %08x %08x, generic 0 %08x %08x %08x %08x\n", p[0], p[1],
                p[2], p[3], g[0], g[1], g[2], g[3]);
    (void)dir;
}

/* A fragment program's depth is in the register after the sample mask's, whether it writes a sample mask or not:
   R1 with no colour (a program that turns a material number into the depth that its lighting passes test EQUAL
   against), R5 after colour 0, and R5 with the sample mask in R4 */
static void test_fragment_depth(const char* dir) {
    static const struct {
        u32 targets, outputs, depth_reg;
    } cases[3] = {{0, 2, 1}, {0xf, 2, 5}, {0xf, 3, 5}};
    for (u32 i = 0; i < 3; i++) {
        u8 code[16 * 8] = {0};
        u32 n = 0;
        for (u32 r = 0; r < 6; r++) put(code, n++, mov_imm(r, f2u(0.0625f * (float)(r + 1))));
        put(code, n++, exit_insn());
        put(code, n, bra(n, n, 7, 0));
        n++;
        u32 fsph[32] = {0};
        fsph[0] = sph0(2, 5);
        fsph[18] = cases[i].targets;
        fsph[19] = cases[i].outputs;
        SpvEvalEnv env = {0};
        int ran = run_stage(ST_FS, code, n, fsph, &env);
        u32 want = f2u(0.0625f * (float)(cases[i].depth_reg + 1));
        CHECK(ran && env.frag_depth == want);
        if (!ran || env.frag_depth != want)
            fprintf(stderr, "fragment depth, targets %#x, outputs %u: %08x, R%u's %08x expected\n", cases[i].targets,
                    cases[i].outputs, env.frag_depth, cases[i].depth_reg, want);
    }
    (void)dir;
}

/* ISETP.GE.U32.AND Pd, PT, RZ, imm, PT: Pd true for imm 0, false for 1 */
static Word setp_const(u32 pd, u32 imm) {
    Word x = alu(0x00c, 4, 255, 255);
    set(&x, 32, 32, imm);
    set(&x, 76, 3, 6);
    set(&x, 81, 3, pd);
    set(&x, 84, 3, 7);
    set(&x, 87, 3, 7);
    return x;
}
/* A BRA's or an EXIT's predicate operand (bits 87-89, 90 negates it) holds with the guard: the instruction takes effect
   only where both do. One game's interface programs end @P0 EXIT P1, P0 their KILL's switch and P1 its test, before
   the MOVs to the colour's registers: with the operand left out every lane with P0 exited with R0-R3 unwritten, and
   the title screen drew nothing. Colour 0 is R0-R3, 0.25 before the instruction and 0.75 after it when it falls
   through. Cases: the guard (P0, true), the operand (P1, 7 for none), its negation, P1's value, and whether it's a BRA
   over the MOVs instead of an EXIT */
static void test_flow_operand(const char* dir) {
    static const struct {
        u32 guard, operand, negate, p1, bra, exits;
    } cases[] = {
        {0, 1, 0, 0, 0, 0}, /* @P0 EXIT P1, P1 false: no exit */
        {0, 1, 0, 1, 0, 1}, /* @P0 EXIT P1, P1 true */
        {0, 1, 1, 0, 0, 1}, /* @P0 EXIT !P1, P1 false */
        {7, 1, 0, 0, 0, 0}, /* EXIT P1, P1 false: the block goes on */
        {7, 1, 0, 1, 0, 1}, /* EXIT P1, P1 true */
        {0, 7, 0, 0, 0, 1}, /* @P0 EXIT */
        {0, 1, 0, 0, 1, 0}, /* @P0 BRA P1, P1 false: not taken */
        {0, 1, 0, 1, 1, 1}, /* @P0 BRA P1, P1 true */
        {7, 1, 1, 1, 1, 0}, /* BRA !P1, P1 true: not taken */
    };
    for (u32 i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        u8 code[16 * 16] = {0};
        u32 n = 0;
        for (u32 r = 0; r < 4; r++) put(code, n++, mov_imm(r, f2u(0.25f)));
        put(code, n++, setp_const(0, 0));
        put(code, n++, setp_const(1, cases[i].p1 ? 0 : 1));
        Word x = cases[i].bra ? bra(n, n + 5, cases[i].guard, 0) : exit_insn();
        set(&x, 12, 3, cases[i].guard);
        set(&x, 87, 3, cases[i].operand);
        set(&x, 90, 1, cases[i].negate);
        put(code, n++, x);
        for (u32 r = 0; r < 4; r++) put(code, n++, mov_imm(r, f2u(0.75f)));
        put(code, n++, exit_insn());
        put(code, n, bra(n, n, 7, 0));
        n++;
        u32 fsph[32] = {0};
        fsph[0] = sph0(2, 5);
        fsph[18] = 0xf;
        SpvEvalEnv env = {0};
        int ran = run_stage(ST_FS, code, n, fsph, &env);
        u32 want = f2u(cases[i].exits ? 0.25f : 0.75f);
        CHECK(ran && env.out[0][0] == want && env.out[0][3] == want);
        if (!ran || env.out[0][0] != want)
            fprintf(stderr, "flow operand, case %u: colour %08x, %08x expected\n", i, env.out[0][0], want);
    }
    (void)dir;
}

/* F2IP dst, a, b, c with the flags of bits 72-79 (0x04 .U8.F32.NTZ, 0x14 .S8, 0x1c .S8.RELU), CuAssembler's sm_86
   records' encoding (0x243) */
static Word f2ip(u32 dst, u32 a, u32 b, u32 c, u32 flags) {
    Word x = alu(0x043, 1, dst, a);
    set(&x, 32, 8, b);
    set(&x, 64, 8, c);
    set(&x, 72, 8, flags);
    return x;
}
/* F2IP packs two floats as bytes, rounded to the nearest even, saturated, NaN 0, with c's low half above them (PTX's
   cvt.pack: d = c << 16 | a << 8 | b). R0: .U8 of 1.5 (2) and 300 (255) under c 0xabcd1234; R1: .U8 of 2.5 (2, a
   tie) and -3 (0); R2: .S8 of -200 (-128) and NaN (0); R3: .S8.RELU of -5 (0) and 126.6 (127) */
static void test_f2ip(const char* dir) {
    u8 code[16 * 20] = {0};
    u32 n = 0;
    static const float in[8] = {1.5f, 300.0f, 2.5f, -3.0f, -200.0f, 0.0f, -5.0f, 126.6f};
    for (u32 k = 0; k < 8; k++) put(code, n++, mov_imm(4 + k, k == 5 ? 0x7fc00000u : f2u(in[k])));
    put(code, n++, mov_imm(12, 0xabcd1234u));
    put(code, n++, f2ip(0, 4, 5, 12, 0x04));
    put(code, n++, f2ip(1, 6, 7, 255, 0x04));
    put(code, n++, f2ip(2, 8, 9, 255, 0x14));
    put(code, n++, f2ip(3, 10, 11, 255, 0x1c));
    put(code, n++, exit_insn());
    put(code, n, bra(n, n, 7, 0));
    n++;
    Sm86Insn d = dec(f2ip(0, 4, 5, 12, 0x04));
    CHECK(d.op == SM86_F2IP && d.form == 1);
    u32 out[4];
    int ran = run_fs_at(code, n, 0, NULL, 0, out);
    int ok = ran && out[0] == 0x123402ffu && out[1] == 0x200u && out[2] == 0x8000u && out[3] == 0x7fu;
    CHECK(ok);
    if (!ok) fprintf(stderr, "f2ip: %08x %08x %08x %08x\n", out[0], out[1], out[2], out[3]);
    /* bits 78-79 round as F2I's: R0 .U8 toward zero (0xc4, as one game's mesh programs have it) of 2.9 (2) and 254.7
       (254); R1 .U8 down (0x44) of 2.5 (2) and -0.5 (-1, saturated to 0); R2 .U8 up (0x84) of 2.1 (3) and 254.2
       (255); R3 .S8 toward zero (0xd4) of -3.7 (-3) and 126.9 (126) */
    {
        u8 rcode[16 * 20] = {0};
        u32 m = 0;
        static const float rin[8] = {2.9f, 254.7f, 2.5f, -0.5f, 2.1f, 254.2f, -3.7f, 126.9f};
        static const u32 rflags[4] = {0xc4, 0x44, 0x84, 0xd4};
        for (u32 k = 0; k < 8; k++) put(rcode, m++, mov_imm(4 + k, f2u(rin[k])));
        for (u32 k = 0; k < 4; k++) put(rcode, m++, f2ip(k, 4 + 2 * k, 5 + 2 * k, 255, rflags[k]));
        put(rcode, m++, exit_insn());
        put(rcode, m, bra(m, m, 7, 0));
        m++;
        ran = run_fs_at(rcode, m, 0, NULL, 0, out);
        ok = ran && out[0] == 0x2feu && out[1] == 0x200u && out[2] == 0x3ffu && out[3] == 0xfd7eu;
        CHECK(ok);
        if (!ok) fprintf(stderr, "f2ip rounding: %08x %08x %08x %08x\n", out[0], out[1], out[2], out[3]);
    }
    /* a flag it doesn't know (bit 73) is refused */
    put(code, 13, f2ip(3, 10, 11, 255, 0x06));
    u32 fsph[32] = {0};
    fsph[0] = sph0(2, 5);
    fsph[18] = 0xf;
    ShaderInfo si = {0};
    size_t count = 0;
    u32* spv = sm86_to_spirv(code, n * 16, ST_FS, fsph, &si, &count);
    CHECK(!spv && strstr(si.first_unsupported, "F2IP with flags 0x6"));
    free(spv);
    put(code, 13, f2ip(3, 10, 11, 255, 0x1c));
    si = (ShaderInfo){0};
    spv = sm86_to_spirv(code, n * 16, ST_FS, fsph, &si, &count);
    if (spv) CHECK(keep_module(dir, "fs_f2ip", spv, count));
    free(spv);
}

/* ULDC of a byte or a halfword (types 0-3, NAK's MemType as LDC's), indexed by a uniform register or not, into UR, then
   MOV R, UR. Bank 1's words 4 and 5 are 0x80f1e2d3 and 0xc4b5a697. R0: ULDC.U8 c[1][0x11] (0xe2); R1: ULDC.IDX.S8
   c[1][UR4 + 0x12] with UR4 = 1 (0x80 sign extended); R2: ULDC.IDX.U16 c[1][UR4 - 1 + 0x16] (0xc4b5, the signed
   offset of 0xffff); R3: ULDC.S16 c[1][0x14] (0xa697 sign extended), as one game's compute program walks a byte code */
static Word uldc_small(u32 ud, u32 bank, u32 off, u32 type, int indexed, u32 ur) {
    Word x = fix(indexed ? 0xabb : 0xab9);
    set(&x, 16, 6, ud);
    if (indexed) set(&x, 24, 6, ur);
    cbuf(&x, bank, off);
    set(&x, 73, 3, type);
    return x;
}
static Word mov_ur(u32 dst, u32 ur) {
    Word x = alu(0x002, 6, dst, 255);
    set(&x, 32, 6, ur);
    set(&x, 91, 1, 1);
    return x;
}
static void test_uldc_small(const char* dir) {
    u8 code[16 * 14] = {0};
    u32 n = 0;
    put(code, n++, ualu(0x082, 4, 0, 1)); /* UMOV UR4, 1 */
    put(code, n++, uldc_small(5, 1, 0x11, 0, 0, 63));
    put(code, n++, uldc_small(6, 1, 0x12, 1, 1, 4));
    put(code, n++, uldc_small(7, 1, 0x16 + 0xffffu, 2, 1, 4));
    put(code, n++, uldc_small(8, 1, 0x14, 3, 0, 63));
    for (u32 k = 0; k < 4; k++) put(code, n++, mov_ur(k, 5 + k));
    put(code, n++, exit_insn());
    put(code, n, bra(n, n, 7, 0));
    n++;
    static const u32 cb1[6] = {0, 0, 0, 0, 0x80f1e2d3u, 0xc4b5a697u};
    u32 out[4];
    int ran = run_fs_at(code, n, 0, cb1, 6, out);
    int ok = ran && out[0] == 0xe2 && out[1] == 0xffffff80u && out[2] == 0xc4b5 && out[3] == 0xffffa697u;
    CHECK(ok);
    if (!ok) fprintf(stderr, "uldc small: %08x %08x %08x %08x\n", out[0], out[1], out[2], out[3]);
    u32 fsph[32] = {0};
    fsph[0] = sph0(2, 5);
    fsph[18] = 0xf;
    ShaderInfo si = {0};
    size_t count = 0;
    u32* spv = sm86_to_spirv(code, n * 16, ST_FS, fsph, &si, &count);
    if (spv) CHECK(keep_module(dir, "fs_uldc_small", spv, count));
    free(spv);
}

/* SHFL with c in a register (0x989 the lane an immediate, 0x389 a register, the register at 64): c's bits 8-12 split
   the lanes into segments and bits 0-4 clamp the lane, as the immediate's. The evaluator runs one lane, so a shuffle
   that holds reads its own value: what is checked is the predicate, which says whether the source lane is inside,
   R0 1.0 where it is. Cases: SHFL.UP by 1 with c 0x1800 (segments of 8) at lane 8, whose lane 7 is the segment
   before (false), and at lane 9 (true); SHFL.IDX of R6 = 5 with c 0x1f03 (every lane a segment of its own) at lane 2,
   which reads lane 2 (true); SHFL.UP by 1 with c RZ at lane 0 (false); SHFL.IDX of R6 = 30 with c 4 (lanes 0 to 4)
   at lane 20 (false); SHFL.DOWN by 2 with c 7 at lane 3, lane 5 (true) */
static Word shfl_creg(u32 dst, u32 pd, u32 value, int lane_reg, u32 lane, u32 creg, u32 mode) {
    Word x = fix(lane_reg ? 0x389 : 0x989);
    set(&x, 16, 8, dst);
    set(&x, 24, 8, value);
    if (lane_reg)
        set(&x, 32, 8, lane);
    else
        set(&x, 53, 5, lane);
    set(&x, 58, 2, mode);
    set(&x, 64, 8, creg);
    set(&x, 81, 3, pd);
    return x;
}
static Word sel_reg(u32 dst, u32 a, u32 pred) { /* SEL dst, a, RZ, pred: a where pred holds */
    Word x = alu(0x007, 1, dst, a);
    set(&x, 32, 8, 255);
    set(&x, 87, 3, pred);
    return x;
}
static void test_shfl_creg(const char* dir) {
    static const struct {
        u32 lane, lane_reg, lane_value, creg_value, mode, want;
    } cases[] = {
        {8, 0, 1, 0x1800, 1, 0}, {9, 0, 1, 0x1800, 1, 1},   {2, 1, 5, 0x1f03, 0, 1},
        {0, 0, 1, 0, 1, 0},      {20, 1, 30, 0x0004, 0, 0}, /* IDX lane 30 past the clamp 4 of one segment of 32 */
        {3, 0, 2, 0x0007, 2, 1},                            /* DOWN by 2 from lane 3: 5 <= 7 */
    };
    for (u32 i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        u8 code[16 * 12] = {0};
        u32 n = 0;
        put(code, n++, mov_imm(4, 0x1234));
        put(code, n++, mov_imm(5, cases[i].creg_value));
        put(code, n++, mov_imm(6, cases[i].lane_value));
        put(code, n++, mov_imm(9, f2u(1.0f)));
        put(code, n++,
            shfl_creg(8, 0, 4, (int)cases[i].lane_reg, cases[i].lane_reg ? 6 : cases[i].lane_value,
                      cases[i].creg_value ? 5 : 255, cases[i].mode));
        put(code, n++, sel_reg(0, 9, 0));
        put(code, n++, exit_insn());
        put(code, n, bra(n, n, 7, 0));
        n++;
        u32 out[4];
        int ran = run_fs_at(code, n, cases[i].lane, NULL, 0, out);
        u32 want = cases[i].want ? f2u(1.0f) : 0;
        CHECK(ran && out[0] == want);
        if (!ran || out[0] != want) fprintf(stderr, "shfl c register, case %u: %08x, %08x expected\n", i, out[0], want);
        if (i == 0) {
            u32 fsph[32] = {0};
            fsph[0] = sph0(2, 5);
            fsph[18] = 0xf;
            ShaderInfo si = {0};
            size_t count = 0;
            u32* spv = sm86_to_spirv(code, n * 16, ST_FS, fsph, &si, &count);
            if (spv) CHECK(keep_module(dir, "fs_shfl_creg", spv, count));
            free(spv);
        }
    }
}

/* S2R R0, SR 0x84 (NAK's NAK_SV_VARIABLE_RATE): a fragment's width in bits 8-15 and height in bits 16-23, 1 and 1 as
   the runtime shades every pixel, which the game's PRMT R, R, 0x7771, RZ takes the width of. A vertex program's is
   refused */
static void test_shading_rate(const char* dir) {
    u8 code[16 * 4] = {0};
    Word x = fix(0x919);
    set(&x, 16, 8, 0);
    set(&x, 72, 8, 0x84);
    put(code, 0, x);
    put(code, 1, exit_insn());
    put(code, 2, bra(2, 2, 7, 0));
    u32 out[4];
    CHECK(run_fs_at(code, 3, 0, NULL, 0, out) && out[0] == 0x10100u);
    u32 vsph[32] = {0};
    vsph[0] = sph0(1, 1);
    ShaderInfo si = {0};
    size_t count = 0;
    u32* spv = sm86_to_spirv(code, 3 * 16, ST_VS, vsph, &si, &count);
    CHECK(!spv && strstr(si.first_unsupported, "0x84 outside a fragment program"));
    free(spv);
    (void)dir;
}
/* FMUL's scale (bits 84-86, 2^(field - 4): nvdisasm's .D4 at 2, .M4 at 6) is part of the product whatever the device
   rounds in: 3.0 * 2.0 is 1.5 with .D4 and 24.0 with .M4, with doubles and without them. And it is applied before the
   one rounding: 2^100 * 2^28 .D4 is 2^126, though the product alone is past a float's range, and a .D2 whose product
   is subnormal rounds once (0x223d8f), not on the product and again on its half (0x223d8e). The evaluator runs on the
   host's floats, which keep subnormals, as an FMUL without .FTZ does */
static void test_float64_fallback_scale(const char* dir) {
    static const struct {
        u32 field, x, y;
        float product;
        const char* name;
    } scales[] = {
        {2, 0x40400000u, 0x40000000u, 1.5f, "D4"},           {6, 0x40400000u, 0x40000000u, 24.0f, "M4"},
        {2, 0x71800000u, 0x4d800000u, 0x1p126f, "D4"},       {2, 0x4d800000u, 0x71800000u, 0x1p126f, "D4"},
        {3, 0x1fabdf83u, 0x1fcc002du, 0x223d8fp-149f, "D2"}, {3, 0x1fcc002du, 0x1fabdf83u, 0x223d8fp-149f, "D2"},
    };
    int had = g_shader_float64;
    for (int f64 = 1; f64 >= 0; f64--)
        for (size_t i = 0; i < sizeof scales / sizeof *scales; i++) {
            u8 code[16 * 8] = {0};
            u32 n = 0, out[4] = {0};
            put(code, n++, mov_imm(0, scales[i].x));
            Word x = alu(0x020, 4, 0, 0); /* FMUL.<scale> R0, R0, y */
            set(&x, 32, 32, scales[i].y);
            set(&x, 84, 3, scales[i].field);
            put(code, n++, x);
            put(code, n++, exit_insn());
            put(code, n, bra(n, n, 7, 0));
            n++;
            g_shader_float64 = f64;
            CHECK(run_fs_at(code, n, 0, NULL, 0, out));
            if (out[0] != f2u(scales[i].product))
                fprintf(stderr, "FMUL.%s %#x, %#x with g_shader_float64=%d: %#x, not %g (%#x)\n", scales[i].name,
                        scales[i].x, scales[i].y, f64, out[0], (double)scales[i].product, f2u(scales[i].product));
            CHECK(out[0] == f2u(scales[i].product));
        }
    g_shader_float64 = had;
    (void)dir;
}

/* LDG.E (no uniform) of type 4 (32 bits) or 5 (64 bits) into dst from [addr + imm], and STG.E of data */
static Word ldg_nou(u32 dst, u32 addr, u32 imm, u32 type) {
    Word x = fix(0x381);
    set(&x, 16, 8, dst);
    set(&x, 24, 8, addr);
    set(&x, 40, 24, imm);
    set(&x, 72, 1, 1);
    set(&x, 73, 3, type);
    return x;
}
static Word stg_nou(u32 addr, u32 imm, u32 data) {
    Word x = fix(0x386);
    set(&x, 24, 8, addr);
    set(&x, 32, 8, data);
    set(&x, 40, 24, imm);
    set(&x, 72, 1, 1);
    set(&x, 73, 3, 4);
    return x;
}
/* IADD3 dst, a, b, RZ with no carries (form 1) */
static Word iadd3(u32 dst, u32 a, u32 b) {
    Word x = alu(0x010, 1, dst, a);
    set(&x, 32, 8, b);
    set(&x, 64, 8, 255);
    set(&x, 81, 3, 7);
    set(&x, 84, 3, 7);
    set(&x, 87, 4, 7);
    set(&x, 77, 4, 7);
    return x;
}
/* Pointers from memory, as one game's vertex program that clears indirect draws: R2 is the buffer that c[0][0x110]
   holds (0x1_0000_1000), whose words 0-5 are three pointers. The first, base + 0x40, is inside it: the STG through
   it writes word 0x12 and the LDG reads word 0x10. The second, below the base, and the third, base + 0x1_0000_0040
   (the same low word of the difference), are in no buffer: the STG is dropped and the LDG reads 0. The pointers are
   read at R14 = R2 + R15 (0), which isn't the base itself: an address the program computes, which global_dynamic
   compares with its buffers (an LDG.64 at the base itself plus an offset names a buffer of its own:
   test_global_indirect) */
static void test_global_pointer(const char* dir) {
    u8 code[16 * 18] = {0};
    Word m = alu(0x002, 5, 2, 255);
    cbuf(&m, 0, 0x110);
    put(code, 0, m);
    m = alu(0x002, 5, 3, 255);
    cbuf(&m, 0, 0x114);
    put(code, 1, m);
    put(code, 2, mov_imm(15, 0));
    put(code, 3, iadd3(14, 2, 15));
    m = alu(0x002, 1, 15, 255); /* MOV R15, R3 */
    set(&m, 32, 8, 3);
    put(code, 4, m);
    put(code, 5, ldg_nou(4, 14, 0, 5));
    put(code, 6, ldg_nou(8, 14, 8, 5));
    put(code, 7, ldg_nou(12, 14, 16, 5));
    put(code, 8, mov_imm(6, 7));
    put(code, 9, mov_imm(7, 9));
    put(code, 10, mov_imm(11, 5));
    put(code, 11, stg_nou(4, 8, 6));
    put(code, 12, stg_nou(12, 0xc, 7));
    put(code, 13, ldg_nou(10, 4, 0, 4));
    put(code, 14, ldg_nou(11, 8, 0, 4));
    put(code, 15, ast(0x70, 10, 2));
    put(code, 16, exit_insn());
    put(code, 17, bra(17, 17, 7, 0));
    u32 vsph[32] = {0};
    vsph[0] = sph0(1, 1);
    static u32 bank0[1024], buffer[64];
    bank0[0x110 / 4] = 0x1000;
    bank0[0x114 / 4] = 1;
    buffer[0] = 0x1040, buffer[1] = 1;
    buffer[2] = 0x0f00, buffer[3] = 1;
    buffer[4] = 0x1040, buffer[5] = 2;
    buffer[0x10] = 0x1234;
    SpvEvalEnv env = {0};
    env.ubo[SNX_VKB_CB] = bank0;
    env.ubo_words[SNX_VKB_CB] = 1024;
    env.ubo[SNX_VKB_SSBO] = buffer;
    env.ubo_words[SNX_VKB_SSBO] = 64;
    /* a draw's page table of its buffers (SNX_VKB_SMAP): each one bound whole, with no atlas */
    static u32 smap[48];
    for (u32 k = 0; k < 16; k++) smap[k] = ~0u;
    env.ubo[SNX_VKB_SMAP] = smap;
    env.ubo_words[SNX_VKB_SMAP] = 48;
    int ran = run_stage(ST_VS, code, 18, vsph, &env);
    int ok = ran && buffer[0x12] == 7 && buffer[0x13] == 0 && env.position[0] == 0x1234 && env.position[1] == 0;
    CHECK(ok);
    if (!ok)
        fprintf(stderr, "global pointer: words %x %x, position %x %x\n", buffer[0x12], buffer[0x13], env.position[0],
                env.position[1]);
    ShaderInfo si = {0};
    size_t count = 0;
    u32* spv = sm86_to_spirv(code, 18 * 16, ST_VS, vsph, &si, &count);
    CHECK(spv && si.ngbuf == 1 && si.gbuf_cbuf_off[0] == 0x110 && (si.gbuf_wmask & 1));
    if (spv) CHECK(keep_module(dir, "global_pointer", spv, count));
    free(spv);
}

/* The pieces of a 64-bit division by a reciprocal, as one game's exposure program has it (the quotient of
   4239360 / 20480 came out 728513 with any of these read otherwise, and 207 with all three):
   - IADD3.X with a negated source adds its complement, ~x (the + 1 of the negation is the low word's carry);
   - IMAD.HI is the high word of a * b + c, and its carry out goes to the predicate at 81-83;
   - IMAD.X with a negated addend adds its complement.
   R10:R11 = -(R1:R2) by IADD3 RZ, -R1, RZ (carry P0) and IADD3.X RZ, ~R2, RZ, P0; R12 = IMAD.HI R3, R4, R5 with
   the carry in P1; R13 = IMAD.X RZ, RZ, ~R6, P1 */
static void test_wide_carries(const char* dir) {
    u8 code[16 * 14] = {0};
    put(code, 0, mov_imm(1, 0));
    put(code, 1, mov_imm(2, 0xfffffdc0));
    put(code, 2, mov_imm(3, 0xffffffff));
    put(code, 3, mov_imm(4, 0xffffffff));
    put(code, 4, mov_imm(5, 0xffffffff));
    put(code, 5, mov_imm(6, 0x00000010));
    Word m = alu(0x010, 1, 10, 255); /* IADD3 R10, RZ, -R1, RZ, P0 */
    set(&m, 32, 8, 1), set(&m, 63, 1, 1), set(&m, 64, 8, 255);
    set(&m, 81, 3, 0), set(&m, 84, 3, 7), set(&m, 87, 4, 7), set(&m, 77, 4, 7);
    put(code, 6, m);
    m = alu(0x010, 1, 11, 255); /* IADD3.X R11, RZ, ~R2, RZ, P0, !PT */
    set(&m, 32, 8, 2), set(&m, 63, 1, 1), set(&m, 64, 8, 255), set(&m, 74, 1, 1);
    set(&m, 81, 3, 7), set(&m, 84, 3, 7), set(&m, 87, 4, 0), set(&m, 77, 4, 0xf);
    put(code, 7, m);
    m = alu(0x027, 1, 12, 3); /* IMAD.HI.U32 R12, R3, R4, R5, P1 */
    set(&m, 32, 8, 4), set(&m, 64, 8, 5), set(&m, 81, 3, 1), set(&m, 84, 3, 7), set(&m, 87, 4, 7);
    put(code, 8, m);
    m = alu(0x024, 1, 13, 255); /* IMAD.X R13, RZ, RZ, ~R6, P1 */
    set(&m, 32, 8, 255), set(&m, 64, 8, 6), set(&m, 74, 1, 1), set(&m, 75, 1, 1);
    set(&m, 81, 3, 7), set(&m, 84, 3, 7), set(&m, 87, 4, 1);
    put(code, 9, m);
    put(code, 10, ast(0x70, 10, 4));
    put(code, 11, exit_insn());
    put(code, 12, bra(12, 12, 7, 0));
    u32 vsph[32] = {0};
    vsph[0] = sph0(1, 1);
    SpvEvalEnv env = {0};
    int ran = run_stage(ST_VS, code, 13, vsph, &env);
    /* -(0xfffffdc0:0) = 0x240:0; 0xffffffff^2 + 0xffffffff = 0xffffffff:0, no carry; ~0x10 + 0 */
    int ok = ran && env.position[0] == 0 && env.position[1] == 0x240 && env.position[2] == 0xffffffffu &&
             env.position[3] == 0xffffffefu;
    CHECK(ok);
    if (!ok)
        fprintf(stderr, "wide carries: ran %d, %x %x %x %x\n", ran, env.position[0], env.position[1], env.position[2],
                env.position[3]);
    ShaderInfo si = {0};
    size_t count = 0;
    u32* spv = sm86_to_spirv(code, 13 * 16, ST_VS, vsph, &si, &count);
    CHECK(spv != NULL);
    if (spv) CHECK(keep_module(dir, "wide_carries", spv, count));
    free(spv);
}

/* A vertex program's instance ID (ALD of a[0x2f8]) counts from 0 whatever the draw's base instance: with draw
   parameters (the offline default) the module takes BaseInstance beside InstanceIndex, InstanceIndex - (BaseInstance -
   push.x), as the Maxwell translator does. With InstanceIndex less the push constant alone, a direct draw's instances
   counted from its base instance (one game's instanced scenery took other instances' transforms) */
static void test_instance_id(const char* dir) {
    u8 code[16 * 4] = {0};
    Word x = fix(0x321); /* ALD R0 = a[0x2f8] */
    set(&x, 16, 8, 0), set(&x, 24, 8, 255), set(&x, 32, 8, 255), set(&x, 40, 10, 0x2f8), set(&x, 74, 2, 0);
    put(code, 0, x);
    put(code, 1, ast(0x70, 0, 1));
    put(code, 2, exit_insn());
    put(code, 3, bra(3, 3, 7, 0));
    u32 vsph[32] = {0};
    vsph[0] = sph0(1, 1);
    ShaderInfo si = {0};
    size_t count = 0;
    u32* spv = sm86_to_spirv(code, 4 * 16, ST_VS, vsph, &si, &count);
    CHECK(spv != NULL);
    int instance_index = 0, base_instance = 0;
    for (size_t at = 5; spv && at < count && (spv[at] >> 16);) { /* OpDecorate x BuiltIn n */
        if ((spv[at] & 0xffff) == 71 && (spv[at] >> 16) == 4 && spv[at + 2] == 11) {
            if (spv[at + 3] == 43) instance_index = 1;
            if (spv[at + 3] == 4425) base_instance = 1;
        }
        at += spv[at] >> 16;
    }
    CHECK(instance_index && base_instance);
    if (spv) CHECK(keep_module(dir, "instance_id", spv, count));
    free(spv);
}

/* F2I.U64.F32 into the pair dst, dst + 1, and I2F.F32.U64 from the pair src, src + 1, with a rounding mode (78-79) */
static Word f2i_u64(u32 dst, u32 src, u32 mode) {
    Word x = alu(0x111, 1, dst, 255);
    set(&x, 32, 8, src), set(&x, 75, 2, 3), set(&x, 78, 2, mode), set(&x, 84, 2, 2);
    return x;
}
static Word i2f_u64(u32 dst, u32 src, u32 mode) {
    Word x = alu(0x112, 1, dst, 255);
    set(&x, 32, 8, src), set(&x, 75, 2, 2), set(&x, 78, 2, mode), set(&x, 84, 2, 3);
    return x;
}
/* a vertex program that sets R1 to R4 to v, runs ops and writes R10 to R13 as the position's four words, into out */
static int run_words(const u32 v[4], const Word* ops, u32 nops, u32 out[4]) {
    u8 code[16 * 12] = {0};
    u32 n = 0;
    for (u32 k = 0; k < 4; k++) put(code, n++, mov_imm(1 + k, v[k]));
    for (u32 k = 0; k < nops; k++) put(code, n++, ops[k]);
    put(code, n++, ast(0x70, 10, 4));
    put(code, n++, exit_insn());
    put(code, n, bra(n, n, 7, 0));
    u32 vsph[32] = {0};
    vsph[0] = sph0(1, 1);
    SpvEvalEnv env = {0};
    int ran = run_stage(ST_VS, code, n + 1, vsph, &env);
    memcpy(out, env.position, sizeof env.position);
    return ran;
}
/* F2I.U64.F32.TRUNC saturates, a NaN and what is under 0 give 0; I2F.F32.U64 rounds to the nearest (0), down (1), up
   (2) and toward zero (3). The edges: 2^64 - 2^40, the largest float under 2^64, which the conversion is clamped to;
   2^64 and up; 2^24 + 1 and 2^24 + 3, whose nearest floats are under and over them; 2^64 - 1, whose nearest is 2^64 */
static void test_u64_conversions(void) {
    static const struct {
        u32 in[4], out[4];
    } f2i[] = {
        {{0x5f7fffffu, 0x7fc00000u}, {0, 0xffffff00u, 0, 0}},           /* 2^64 - 2^40, NaN */
        {{0x5f800000u, 0xbfc00000u}, {0xffffffffu, 0xffffffffu, 0, 0}}, /* 2^64, -1.5 */
        {{0x7f800000u, 0xff800000u}, {0xffffffffu, 0xffffffffu, 0, 0}}, /* +inf, -inf */
        {{0x4f800001u, 0x4079999au}, {0x200, 1, 3, 0}},                 /* 2^32 + 512, 3.9 */
    };
    static const struct {
        u32 in[4], out[4];
    } i2f[] = {
        {{0x01000001u, 0}, {0x4b800000u, 0x4b800000u, 0x4b800001u, 0x4b800000u}},           /* 2^24 + 1 */
        {{0x01000003u, 0}, {0x4b800002u, 0x4b800001u, 0x4b800002u, 0x4b800001u}},           /* 2^24 + 3 */
        {{0xffffffffu, 0xffffffffu}, {0x5f800000u, 0x5f7fffffu, 0x5f800000u, 0x5f7fffffu}}, /* 2^64 - 1 */
        {{0, 0}, {0, 0, 0, 0}},
    };
    const Word f2i_ops[2] = {f2i_u64(10, 1, 3), f2i_u64(12, 2, 3)};
    const Word i2f_ops[4] = {i2f_u64(10, 1, 0), i2f_u64(11, 1, 1), i2f_u64(12, 1, 2), i2f_u64(13, 1, 3)};
    for (u32 i = 0; i < sizeof f2i / sizeof f2i[0]; i++) {
        u32 out[4] = {0};
        int ok = run_words(f2i[i].in, f2i_ops, 2, out) && !memcmp(out, f2i[i].out, sizeof out);
        CHECK(ok);
        if (!ok)
            fprintf(stderr, "F2I.U64 of %08x, %08x: %08x:%08x, %08x:%08x\n", f2i[i].in[0], f2i[i].in[1], out[1], out[0],
                    out[3], out[2]);
    }
    for (u32 i = 0; i < sizeof i2f / sizeof i2f[0]; i++) {
        u32 out[4] = {0};
        int ok = run_words(i2f[i].in, i2f_ops, 4, out) && !memcmp(out, i2f[i].out, sizeof out);
        CHECK(ok);
        if (!ok)
            fprintf(stderr, "I2F.U64 of %08x%08x: %08x %08x %08x %08x\n", i2f[i].in[1], i2f[i].in[0], out[0], out[1],
                    out[2], out[3]);
    }
    /* the signed form is refused by name */
    Word f2s = f2i_u64(10, 1, 3);
    set(&f2s, 72, 1, 1);
    u8 code[16 * 3] = {0};
    put(code, 0, f2s);
    put(code, 1, exit_insn());
    put(code, 2, bra(2, 2, 7, 0));
    u32 vsph[32] = {0};
    vsph[0] = sph0(1, 1);
    ShaderInfo si = {0};
    size_t count = 0;
    u32* spv = sm86_to_spirv(code, 3 * 16, ST_VS, vsph, &si, &count);
    CHECK(!spv && strstr(si.first_unsupported, "F2I.64 of sizes 2, 3, signed 1"));
    free(spv);
}

/* LDG.E.64 (the uniform form) into dst from [UR + imm], RZ the register */
static Word ldg_uniform64(u32 dst, u32 ur, u32 imm) {
    Word x = fix(0x981);
    set(&x, 16, 8, dst);
    set(&x, 24, 8, 255);
    set(&x, 32, 6, ur);
    set(&x, 40, 24, imm);
    set(&x, 72, 1, 1);
    set(&x, 73, 3, 5);
    set(&x, 81, 3, 7);
    set(&x, 91, 1, 1);
    return x;
}
/* MOV dst, src under the predicate (negated with negate) */
static Word mov_if(u32 dst, u32 src, u32 pred, int negate) {
    Word x = alu(0x002, 1, dst, 255);
    set(&x, 32, 8, src);
    set(&x, 12, 3, pred);
    set(&x, 15, 1, (u64)negate);
    return x;
}
/* Pointers loaded from a table, as one game's material and compute programs do: c[0][0x40] holds the table's address
   (0x1_0000_0000), whose words 2-3 and 4-5 hold A (0x3_2000_0000) and B (0x5_0000_0000), far from it. An LDG.64 at the
   table's address itself plus 8 or 16 names a buffer of its own whose address is in memory (SH_GBUF_INDIRECT, the
   renderer binds the memory mapped from the pointer). R4 = A and R6 = B: R14 = A[0x20] and the stores to B + 4 and
   B + 8 reach them by their own buffers. P0 picks R8:R9 = P0 ? B : A, then IMAD.WIDE R10 = 3 * 16 + R8:R9 and the
   LDG of [R10 + 4]: either pointer, so the address is compared with each buffer when it runs (global_dynamic), the
   two indirect ones among them. Word 13 of A holds 0xabc and of B 0xdef: B's word 1 gets the one P0 picks */
static void test_global_indirect(const char* dir) {
    for (u32 pick_b = 0; pick_b < 2; pick_b++) {
        u8 code[16 * 18] = {0};
        u32 n = 0;
        put(code, n++, uldc64(0x40));
        put(code, n++, ldg_uniform64(4, 4, 8));
        put(code, n++, ldg_uniform64(6, 4, 16));
        put(code, n++, mov_imm(12, 3));
        put(code, n++, setp_const(0, pick_b ? 0 : 1));
        put(code, n++, mov_if(8, 4, 0, 1));
        put(code, n++, mov_if(9, 5, 0, 1));
        put(code, n++, mov_if(8, 6, 0, 0));
        put(code, n++, mov_if(9, 7, 0, 0));
        Word w = alu(0x025, 4, 10, 12); /* IMAD.WIDE.U32 R10, R12, 16, R8 */
        set(&w, 32, 32, 16);
        set(&w, 64, 8, 8);
        put(code, n++, w);
        put(code, n++, ldg_nou(13, 10, 4, 4));
        put(code, n++, ldg_nou(14, 4, 0x20, 4));
        Word st = stg_nou(6, 4, 13);
        put(code, n++, st);
        st = stg_nou(6, 8, 14);
        put(code, n++, st);
        put(code, n++, exit_insn());
        put(code, n, bra(n, n, 7, 0));
        n++;
        ShaderInfo si = {0};
        size_t count = 0;
        u32* spv = sm86_to_spirv(code, n * 16, ST_CS, NULL, &si, &count);
        CHECK(spv != NULL);
        if (!spv) {
            fprintf(stderr, "global indirect: %s\n", si.first_unsupported);
            continue;
        }
        u32 want[3] = {0x40, 0x40u | 1u << 31 | 1u << 21, 0x40u | 1u << 31 | 2u << 21};
        CHECK(si.ngbuf == 3 && si.gbuf_wmask == 4);
        for (int k = 0; k < 3 && k < si.ngbuf; k++) CHECK(si.gbuf_cbuf_off[k] == want[k]);
        CHECK(SH_GBUF_INDIRECT(si.gbuf_cbuf_off[2]) && SH_GBUF_CELL(si.gbuf_cbuf_off[2]) == 16 &&
              SH_GBUF_PARENT(si.gbuf_cbuf_off[2]) == 0x40);
        static u32 bank0[1024], table[64], a[64], b[64], log[2 * SH_WLOG_PART];
        memset(table, 0, sizeof table);
        memset(a, 0, sizeof a);
        memset(b, 0, sizeof b);
        bank0[0x40 / 4] = 0, bank0[0x44 / 4] = 1;
        table[2] = 0x20000000, table[3] = 3;
        table[4] = 0, table[5] = 5;
        a[13] = 0xabc, a[8] = 0x777;
        b[13] = 0xdef;
        SpvEvalEnv env = {0};
        env.ubo[SNX_VKB_CB] = bank0;
        env.ubo_words[SNX_VKB_CB] = 1024;
        env.ubo[SNX_VKB_SSBO] = table;
        env.ubo_words[SNX_VKB_SSBO] = 64;
        env.ubo[SNX_VKB_SSBO + 1] = a;
        env.ubo_words[SNX_VKB_SSBO + 1] = 64;
        env.ubo[SNX_VKB_SSBO + 2] = b;
        env.ubo_words[SNX_VKB_SSBO + 2] = 64;
        env.ubo[SNX_VKB_WPAGES] = log;
        env.ubo_words[SNX_VKB_WPAGES] = 2 * SH_WLOG_PART;
        const char* why = spv_eval(spv, count, &env);
        int ok = !why && b[1] == (pick_b ? 0xdefu : 0xabcu) && b[2] == 0x777 && a[1] == 0;
        CHECK(ok);
        if (!ok)
            fprintf(stderr, "global indirect (%s): %s, B words %x %x\n", pick_b ? "B" : "A", why ? why : "ran", b[1],
                    b[2]);
        CHECK(keep_module(dir, pick_b ? "global_indirect_b" : "global_indirect_a", spv, count));
        free(spv);
    }
}

/* ATOMG.E.ADD (no uniform, 0x3a8) of 32-bit data into dst, at [addr + imm], as one game's culling programs encode it */
static Word atomg_add(u32 dst, u32 addr, u32 imm, u32 data) {
    Word x = fix(0x3a8);
    set(&x, 16, 8, dst);
    set(&x, 24, 8, addr);
    set(&x, 32, 8, data);
    set(&x, 40, 24, imm);
    set(&x, 72, 16, 0x1ee1);
    return x;
}
/* A pointer that a compute program loads from a constant buffer at an offset it computes, as one game's culling
   programs do: LDC.64 R2, c[4][R0] with R0 = 0x20, where c[4] holds 0x5_0000_1100. The address may be in any window of
   the table (SH_GBUF_TABLE): the program has the 16 buffer slots, windows 0 to 15 of bank 4, each base and length in
   SNX_VKB_MBASE. Window 0, from 0x5_0000_1000, 0x400 bytes, holds it; window 1, 0x7_0000_9000, doesn't. The LDG of
   [R2 + 4] reads word 0x41 (0xabc), the STG puts it at word 0x42, the ATOMG adds 3 to word 0x43 (10) and the STG
   after it puts the 10 it returns at word 0x44. A vertex program with the same LDC is refused, as before */
static void test_global_table(const char* dir) {
    u8 code[16 * 10] = {0};
    u32 n = 0;
    put(code, n++, mov_imm(0, 0x20));
    put(code, n++, ldc(2, 0, 4, 0, 5));
    put(code, n++, ldg_nou(4, 2, 4, 4));
    put(code, n++, stg_nou(2, 8, 4));
    put(code, n++, mov_imm(5, 3));
    put(code, n++, atomg_add(6, 2, 0xc, 5));
    put(code, n++, stg_nou(2, 0x10, 6));
    put(code, n++, exit_insn());
    put(code, n, bra(n, n, 7, 0));
    n++;
    ShaderInfo si = {0};
    size_t count = 0;
    u32* spv = sm86_to_spirv(code, n * 16, ST_CS, NULL, &si, &count);
    CHECK(spv != NULL);
    if (!spv) {
        fprintf(stderr, "global table: %s\n", si.first_unsupported);
        return;
    }
    CHECK(si.ngbuf == 16);
    for (int k = 0; k < si.ngbuf; k++)
        CHECK(SH_GBUF_TABLE(si.gbuf_cbuf_off[k]) && SH_GBUF_BANK(si.gbuf_cbuf_off[k]) == 4 &&
              SH_GBUF_TABLE_WINDOW(si.gbuf_cbuf_off[k]) == (u32)k && !SH_GBUF_INDIRECT(si.gbuf_cbuf_off[k]));
    static u32 bank4[1024], windows[64], a[256], b[64], log[16 * SH_WLOG_PART];
    bank4[0x20 / 4] = 0x1100, bank4[0x24 / 4] = 5;
    windows[0] = 0x1000, windows[1] = 5, windows[2] = 0x400;
    windows[4] = 0x9000, windows[5] = 7, windows[6] = 0x100;
    a[0x41] = 0xabc, a[0x43] = 10;
    SpvEvalEnv env = {0};
    env.ubo[SNX_VKB_CB + 4] = bank4;
    env.ubo_words[SNX_VKB_CB + 4] = 1024;
    env.ubo[SNX_VKB_MBASE] = windows;
    env.ubo_words[SNX_VKB_MBASE] = 64;
    env.ubo[SNX_VKB_SSBO] = a;
    env.ubo_words[SNX_VKB_SSBO] = 256;
    env.ubo[SNX_VKB_SSBO + 1] = b;
    env.ubo_words[SNX_VKB_SSBO + 1] = 64;
    env.ubo[SNX_VKB_WPAGES] = log;
    env.ubo_words[SNX_VKB_WPAGES] = 16 * SH_WLOG_PART;
    const char* why = spv_eval(spv, count, &env);
    int ok = !why && a[0x42] == 0xabc && a[0x43] == 13 && a[0x44] == 10;
    for (u32 k = 0; k < 64; k++) ok &= !b[k];
    CHECK(ok);
    if (!ok) fprintf(stderr, "global table: %s, words %x %x %x\n", why ? why : "ran", a[0x42], a[0x43], a[0x44]);
    CHECK(keep_module(dir, "global_table", spv, count));
    free(spv);
    /* the ATOMG's data and destination in one register (R5), as a count's increment that takes the old count has
       them: it still adds the 3, and the STG after it puts the 10 it returns at word 0x44 */
    put(code, 5, atomg_add(5, 2, 0xc, 5));
    put(code, 6, stg_nou(2, 0x10, 5));
    si = (ShaderInfo){0};
    spv = sm86_to_spirv(code, n * 16, ST_CS, NULL, &si, &count);
    CHECK(spv != NULL);
    if (spv) {
        memset(log, 0, sizeof log);
        a[0x43] = 10, a[0x44] = 0;
        why = spv_eval(spv, count, &env);
        ok = !why && a[0x43] == 13 && a[0x44] == 10;
        CHECK(ok);
        if (!ok)
            fprintf(stderr,
                    "global table, an ATOMG into its data's register: %s, the words hold %u and %u (13 and 10 "
                    "wanted)\n",
                    why ? why : "ran", a[0x43], a[0x44]);
        free(spv);
    }
    u32 vsph[32] = {0};
    vsph[0] = sph0(1, 1);
    si = (ShaderInfo){0};
    spv = sm86_to_spirv(code, n * 16, ST_VS, vsph, &si, &count);
    CHECK(!spv && strstr(si.first_unsupported, "global address whose base no constant buffer holds (from LDC"));
    free(spv);
}

/* A pointer in the memory a table's word points to, as one game's culling programs reach the instances of a kind of
   plant: LDC.64 R2, c[4][R0] (0x5_0000_1100), MOV R8, R2 and R9, R3, LDG.64 R4, [R8] (cell 0 of that record:
   0x7_0000_9010), LDG R6, [R4 + 4], STG [R2 + 8], R6. The program names cell 0 (ShaderInfo.table_cells), and with the
   record in window 0 (0x5_0000_1000) and the instances in window 1 (0x7_0000_9000) the STG puts the word at
   0x7_0000_9014 (0xdef) at 0x5_0000_1108. A pointer loaded at the word plus 0x10, an offset the IADD3 adds, names no
   cell: only the record's own cells are read as the dispatch binds */
static Word iadd3_imm(u32 dst, u32 a, u32 imm);
static void test_global_table_cells(const char* dir) {
    u8 code[16 * 10] = {0};
    u32 n = 0;
    put(code, n++, mov_imm(0, 0x20));
    put(code, n++, ldc(2, 0, 4, 0, 5));
    put(code, n++, mov_if(8, 2, 7, 0));
    put(code, n++, mov_if(9, 3, 7, 0));
    put(code, n++, ldg_nou(4, 8, 0, 5));
    put(code, n++, ldg_nou(6, 4, 4, 4));
    put(code, n++, stg_nou(2, 8, 6));
    put(code, n++, exit_insn());
    put(code, n, bra(n, n, 7, 0));
    n++;
    ShaderInfo si = {0};
    size_t count = 0;
    u32* spv = sm86_to_spirv(code, n * 16, ST_CS, NULL, &si, &count);
    CHECK(spv != NULL);
    if (!spv) {
        fprintf(stderr, "global table cells: %s\n", si.first_unsupported);
        return;
    }
    CHECK(si.table_cells == 1);
    static u32 bank4[1024], windows[64], a[256], b[64], log[16 * SH_WLOG_PART];
    bank4[0x20 / 4] = 0x1100, bank4[0x24 / 4] = 5;
    windows[0] = 0x1000, windows[1] = 5, windows[2] = 0x400;
    windows[4] = 0x9000, windows[5] = 7, windows[6] = 0x100;
    a[0x40] = 0x9010, a[0x41] = 7;
    b[5] = 0xdef;
    SpvEvalEnv env = {0};
    env.ubo[SNX_VKB_CB + 4] = bank4;
    env.ubo_words[SNX_VKB_CB + 4] = 1024;
    env.ubo[SNX_VKB_MBASE] = windows;
    env.ubo_words[SNX_VKB_MBASE] = 64;
    env.ubo[SNX_VKB_SSBO] = a;
    env.ubo_words[SNX_VKB_SSBO] = 256;
    env.ubo[SNX_VKB_SSBO + 1] = b;
    env.ubo_words[SNX_VKB_SSBO + 1] = 64;
    env.ubo[SNX_VKB_WPAGES] = log;
    env.ubo_words[SNX_VKB_WPAGES] = 16 * SH_WLOG_PART;
    const char* why = spv_eval(spv, count, &env);
    CHECK(!why && a[0x42] == 0xdef);
    if (why || a[0x42] != 0xdef) fprintf(stderr, "global table cells: %s, word %x\n", why ? why : "ran", a[0x42]);
    CHECK(keep_module(dir, "global_table_cells", spv, count));
    free(spv);
    put(code, 2, iadd3_imm(8, 2, 0x10));
    si = (ShaderInfo){0};
    spv = sm86_to_spirv(code, n * 16, ST_CS, NULL, &si, &count);
    CHECK(spv != NULL && si.table_cells == 0);
    free(spv);
}

/* TEX, TLD, TLD4 and TXQ with the handle in a uniform register (0xf60, 0xf66, 0xf63, 0xf6f), as CuAssembler's sm_86
   records encode "TEX.SCR.LL.DC R34, R8, R2, R6, UR4, 2D", "TLD.SCR.LZ R4, R14, R6, UR4, 2D", "TLD4.SCR.A
   R16, R2, R48, UR4, 2D" and "TXQ R35, R20, UR12, TEX_HEADER_DIMENSION, 0x2": the register at 40-45. A handle that
   a program loads from memory (LDG.64, R2UR, UMOV) names no slot: refused with where it comes from */
/* ---- texture handles from memory (sm86_eval) ---- */

/* the constant buffers and the memory of the handle tests: c[0][0x520] = 0x2_0000_1000 (the table), c[4][0x88] =
   0xc0000003 and c[5][0x2ec] = 4, so the record (3 + 4) * 32 + 0x18 = 0xf8 holds 0x12300abc; [0x2_0000_1010] holds
   0x00402345. The loads it was asked for, in order */
static u64 g_mh_loads[8];
static u32 g_mh_nloads;
static u32 mh_cbuf(void* user, u32 bank, u32 off) {
    (void)user;
    return bank == 0 && off == 0x520   ? 0x00001000u
           : bank == 0 && off == 0x524 ? 0x2u
           : bank == 4 && off == 0x88  ? 0xc0000003u
           : bank == 5 && off == 0x2ec ? 4u
           : bank == 1                 ? 0x11111111u * (off / 4 + 1)
                                       : 0;
}
static int mh_mem(void* user, u64 address, u32* words, u32 count) {
    (void)user;
    if (g_mh_nloads < 8) g_mh_loads[g_mh_nloads++] = address;
    for (u32 k = 0; k < count; k++)
        words[k] = address + 4 * k == 0x2000010f8ull   ? 0x12300abcu
                   : address + 4 * k == 0x200001010ull ? 0x00402345u
                                                       : 0xdeadbeefu;
    return 1;
}
/* LDG.E.64 dst, [reg.U32 + UR.64 + imm] (the uniform form, the register 32-bit) */
static Word ldg_reg_ur64(u32 dst, u32 reg, u32 ur, u32 imm) {
    Word x = ldg_uniform64(dst, ur, imm);
    set(&x, 24, 8, reg);
    return x;
}
static Word r2ur(u32 ud, u32 r) {
    Word x = fix(0x3c2);
    set(&x, 16, 6, ud);
    set(&x, 24, 8, r);
    return x;
}
/* MOV dst, c[bank][off] */
static Word mov_cbuf(u32 dst, u32 bank, u32 off) {
    Word x = alu(0x002, 5, dst, 255);
    cbuf(&x, bank, off);
    return x;
}
/* LOP3 dst, a, imm, c with the truth table lut (no predicate out) */
static Word lop3_imm(u32 dst, u32 a, u32 imm, u32 c, u32 lut) {
    Word x = alu(0x012, 4, dst, a);
    set(&x, 32, 32, imm);
    set(&x, 64, 8, c);
    set(&x, 72, 8, lut);
    set(&x, 81, 3, 7);
    set(&x, 87, 4, 0xf);
    return x;
}
/* LOP3 dst, a, b, c (registers) */
static Word lop3_reg(u32 dst, u32 a, u32 b, u32 c, u32 lut) {
    Word x = lop3_imm(dst, a, 0, c, lut);
    set(&x, 9, 3, 1);
    set(&x, 32, 32, b);
    return x;
}
/* SHF dst, a, imm, c: dtype (73-74: 0 S64, 1 U64, 2 S32, 3 U32), right (76), wrap (75), high (80) */
static Word shf_imm(u32 dst, u32 a, u32 shift, u32 c, u32 dtype, int right, int wrap, int high) {
    Word x = alu(0x019, 4, dst, a);
    set(&x, 32, 32, shift);
    set(&x, 64, 8, c);
    set(&x, 73, 2, dtype);
    set(&x, 75, 1, (u64)wrap);
    set(&x, 76, 1, (u64)right);
    set(&x, 80, 1, (u64)high);
    return x;
}
/* IADD3 dst, a, imm, RZ */
static Word iadd3_imm(u32 dst, u32 a, u32 imm) {
    Word x = alu(0x010, 4, dst, a);
    set(&x, 32, 32, imm);
    set(&x, 64, 8, 255);
    set(&x, 81, 3, 7);
    set(&x, 84, 3, 7);
    set(&x, 87, 4, 7);
    set(&x, 77, 4, 7);
    return x;
}
/* USHF.R.U32.HI ud, URZ, imm, uc: uc >> imm, as the game's handle programs take the sampler's index */
static Word ushf_r(u32 ud, u32 uc, u32 shift) {
    Word x = ualu(0x099, ud, 63, shift);
    set(&x, 64, 6, uc);
    set(&x, 73, 2, 3);
    set(&x, 76, 1, 1);
    set(&x, 80, 1, 1);
    return x;
}
/* the handle of a TEX whose handle is in a uniform register pair (TEX_U) and of a TEX.B, loaded from memory as one
   game's material programs do: the record's address (c[0][0x520] + ((c[4][0x88] & 0xfffff) + c[5][0x2ec]) * 32 +
   0x18) made of constant buffer words, LDG.E.64 R4, R2UR UR8 = R4, UR9 = UR8 >> 20. The translation keeps those
   instructions (ShaderInfo.memh), and sm86_eval runs them as the renderer does: it loads the record at 0x2_0000_10f8
   and gives the texture's word and the sampler's index. The TEX.B takes its handle register R6 from an LDG.64 at
   [UR4 + 0x10]. A handle whose address depends on the invocation (S2R of the lane), or whose register has another
   writer on a path (a guarded MOV), is no texture (SH_PROBE_NONE), and the program translates */
static void test_uniform_handle(const char* dir) {
    static const struct {
        u64 hi, lo;
        u32 op, ur;
    } forms[] = {{0x00000000099e4f08ull, 0x3000040602227f60ull, SM86_TEX_U, 4},
                 {0x00000000089e0f04ull, 0x30000406060e7f66ull, SM86_TLD_U, 4},
                 {0x00000000099e0f10ull, 0x3000043030027f63ull, SM86_TLD4_U, 4},
                 {0x00000000080002ffull, 0x00000c0014237f6full, SM86_TXQ_U, 12}};
    for (u32 i = 0; i < sizeof forms / sizeof forms[0]; i++) {
        Sm86Insn in;
        CHECK(sm86_decode(forms[i].lo, forms[i].hi, &in) && in.op == forms[i].op &&
              sm86_field(&in, 40, 6) == forms[i].ur && sm86_field(&in, 91, 1));
    }
    u32 fsph[32] = {0};
    fsph[0] = sph0(2, 5);
    fsph[18] = 0xf;
    for (u32 variant = 0; variant < 4; variant++) {
        u8 code[16 * 20] = {0};
        u32 n = 0;
        put(code, n++, uldc64(0x520));
        put(code, n++, mov_cbuf(2, 4, 0x88));
        put(code, n++, lop3_imm(2, 2, 0xfffff, 255, 0xc0)); /* a & b */
        put(code, n++, mov_cbuf(3, 5, 0x2ec));
        put(code, n++, iadd3(2, 2, 3));
        if (variant == 2) { /* the lane: the address differs from invocation to invocation */
            Word x = fix(0x919);
            set(&x, 16, 8, 3);
            set(&x, 72, 8, 0);
            put(code, n++, x);
            put(code, n++, iadd3(2, 2, 3));
        }
        if (variant == 3) put(code, n++, mov_if(2, 3, 0, 0)); /* @P0 MOV R2, R3: two writers reach the LDG */
        put(code, n++, shf_imm(2, 2, 5, 255, 3, 0, 0, 0)); /* SHF.L.U32 R2, R2, 5, RZ */
        put(code, n++, iadd3_imm(2, 2, 0x18));
        put(code, n++, ldg_reg_ur64(4, 2, 4, 0));
        u32 first = n;
        put(code, n++, r2ur(8, 4));
        put(code, n++, ushf_r(9, 8, 20));
        Word t = {{forms[0].lo, forms[0].hi}};
        set(&t, 40, 6, 8);
        put(code, n++, t);
        if (variant == 1) { /* a TEX.B, its handle R6 from [UR4 + 0x10] */
            put(code, n++, ldg_uniform64(6, 4, 0x10));
            Word b = fix(0x361);
            set(&b, 16, 8, 0);
            set(&b, 24, 8, 8);
            set(&b, 32, 8, 6);
            set(&b, 61, 3, 1);
            set(&b, 64, 8, 255);
            set(&b, 72, 4, 0xf);
            set(&b, 81, 3, 7);
            put(code, n++, b);
        }
        put(code, n++, exit_insn());
        put(code, n, bra(n, n, 7, 0));
        n++;
        ShaderInfo si = {0};
        size_t count = 0;
        u32* spv = sm86_to_spirv(code, n * 16, ST_FS, fsph, &si, &count);
        if (variant >= 2) { /* no texture (SH_PROBE_NONE), the draw going on, and nothing kept to run */
            const char* want = variant == 2 ? "made with S2R" : "has no one writer";
            int ok = spv && si.ntex == 1 && (si.tex[0].flags & SNX_TEX_MEMORY) && si.tex[0].cbuf_off == SH_PROBE_NONE &&
                     si.tex[0].bl2_off == SH_PROBE_NONE && si.memh_none == 1 && strstr(si.memh_why, want) &&
                     si.nmemh == 0;
            CHECK(ok);
            if (!ok)
                fprintf(stderr, "memory handle, variant %u: %s, %s\n", variant, si.first_unsupported, si.memh_why);
            if (spv) CHECK(keep_module(dir, "fs_memory_handle_none", spv, count));
            free(spv);
            continue;
        }
        CHECK(spv != NULL);
        if (!spv) {
            fprintf(stderr, "memory handle: %s\n", si.first_unsupported);
            continue;
        }
        CHECK(keep_module(dir, variant ? "fs_memory_handle_b" : "fs_memory_handle", spv, count));
        free(spv);
        /* the slot names UR8 and UR9 as the R2UR and the USHF leave them, and memh is the instructions up to them */
        u32 want_slots = variant ? 2 : 1;
        CHECK(si.ntex == (int)want_slots && (si.tex[0].flags & SNX_TEX_MEMORY) && si.tex[0].bl2 &&
              si.tex[0].shadow && si.nmemh == first + 2 + variant);
        if (si.ntex < 1) continue;
        CHECK(SH_PROBE_UNIFORM(si.tex[0].cbuf_off) && SH_PROBE_REG(si.tex[0].cbuf_off) == 8 &&
              SH_PROBE_INSN(si.tex[0].cbuf_off) == first && SH_PROBE_REG(si.tex[0].bl2_off) == 9 &&
              SH_PROBE_INSN(si.tex[0].bl2_off) == first + 1);
        u32 probes[3] = {si.tex[0].cbuf_off, si.tex[0].bl2_off, variant ? si.tex[1].cbuf_off : si.tex[0].cbuf_off};
        u32 values[3] = {0, 0, 0};
        Sm86EvalIo io = {NULL, mh_cbuf, mh_mem};
        g_mh_nloads = 0;
        int ran = sm86_eval((const u64(*)[2])si.memh, si.nmemh, &io, probes, 3, values);
        int ok = ran && values[0] == 0x12300abcu && values[1] == 0x123u && g_mh_loads[0] == 0x2000010f8ull &&
                 (!variant || (values[2] == 0x00402345u && !SH_PROBE_UNIFORM(si.tex[1].cbuf_off) &&
                               SH_PROBE_REG(si.tex[1].cbuf_off) == 6 && !si.tex[1].bl2));
        CHECK(ok);
        if (!ok)
            fprintf(stderr, "memory handle %u: ran %d, %08x %08x %08x, load %llx\n", variant, ran, values[0], values[1],
                    values[2], (unsigned long long)g_mh_loads[0]);
    }
}
/* An address's base through a ULDC of one word to the register before it: UR4 = c[0][0x530] (64 bits), UR7 = UR4 +
   0x30, then a ULDC UR6 of one word of bank 3, UMOV UR4 = UR7, and the LDG at [UR4 + 0x5c]. UR7 keeps its key: the
   word's ULDC took the key of UR7 as a 64-bit one's high word, and one game's material program was refused for "a
   global address whose base no constant buffer holds (from UMOV)" */
static void test_uldc_word_key(const char* dir) {
    u8 code[16 * 8] = {0};
    u32 n = 0;
    put(code, n++, uldc64(0x530));
    put(code, n++, ualu(0x090, 7, 4, 0x30)); /* UIADD3 UR7, UR4, 0x30, URZ */
    put(code, n++, uldc_small(6, 3, 0x958, 4, 0, 63));
    Word x = ualu(0x082, 4, 0, 0); /* UMOV UR4, UR7 (form 6) */
    set(&x, 9, 3, 6);
    set(&x, 32, 6, 7);
    set(&x, 64, 6, 63);
    put(code, n++, x);
    put(code, n++, ldg_uniform64(2, 4, 0x5c));
    put(code, n++, exit_insn());
    put(code, n, bra(n, n, 7, 0));
    n++;
    ShaderInfo si = {0};
    size_t count = 0;
    u32* spv = sm86_to_spirv(code, n * 16, ST_CS, NULL, &si, &count);
    CHECK(spv != NULL && si.ngbuf == 1 && si.gbuf_cbuf_off[0] == 0x530);
    if (!spv) fprintf(stderr, "uldc word key: %s\n", si.first_unsupported);
    if (spv) CHECK(keep_module(dir, "uldc_word_key", spv, count));
    free(spv);
}
/* bank 1 of mh_cross: the four words user points to */
static u32 mh_cbuf1(void* user, u32 bank, u32 off) { return bank == 1 && off < 16 ? ((const u32*)user)[off / 4] : 0; }
/* MOV R4-R7 from bank 1 (its words 0-3), the instructions, then R0-R3 out: the translation run on the CPU
   (support/spv_eval.c) and sm86_eval take the same values, so that what the renderer computes for a handle is what
   the program computes */
static void mh_cross(const char* name, const Word* insns, u32 count, const u32* cb1) {
    u8 code[16 * 32] = {0};
    u64 words[32][2];
    u32 n = 0;
    for (u32 k = 0; k < 4; k++) put(code, n++, mov_cbuf(4 + k, 1, 4 * k));
    for (u32 k = 0; k < count; k++) put(code, n++, insns[k]);
    for (u32 k = 0; k < n; k++) words[k][0] = snx_ld_le64(code + 16 * k), words[k][1] = snx_ld_le64(code + 16 * k + 8);
    u32 last = n - 1, gpu[4] = {0}, cpu[4] = {0}, probes[4];
    put(code, n++, exit_insn());
    put(code, n, bra(n, n, 7, 0));
    n++;
    for (u32 k = 0; k < 4; k++) probes[k] = SH_PROBE(last, 0, k);
    int ran = run_fs_at(code, n, 0, cb1, 4, gpu);
    Sm86EvalIo io = {(void*)cb1, mh_cbuf1, mh_mem};
    int evaluated = sm86_eval((const u64(*)[2])words, last + 1, &io, probes, 4, cpu);
    int same = ran && evaluated && !memcmp(gpu, cpu, sizeof gpu);
    CHECK(same);
    if (!same)
        fprintf(stderr, "%s: ran %d %d, the program %08x %08x %08x %08x, sm86_eval %08x %08x %08x %08x\n", name, ran,
                evaluated, gpu[0], gpu[1], gpu[2], gpu[3], cpu[0], cpu[1], cpu[2], cpu[3]);
}
/* IADD3 dst, a, b, c (registers) with its carries out p and q, .X with carries in p_in (87-90) and q_in (77-80) */
static Word iadd3_x(u32 dst, u32 a, u32 b, u32 c, u32 p, u32 q, int x, u32 p_in, u32 q_in) {
    Word w = alu(0x010, 1, dst, a);
    set(&w, 32, 8, b);
    set(&w, 64, 8, c);
    set(&w, 74, 1, (u64)x);
    set(&w, 81, 3, p);
    set(&w, 84, 3, q);
    set(&w, 87, 4, p_in);
    set(&w, 77, 4, q_in);
    return w;
}
/* LEA dst, a, b, c (registers): (a << shift, or with high the high word of c:a << shift) + b */
static Word lea(u32 dst, u32 a, u32 b, u32 c, u32 shift, int high) {
    Word w = alu(0x011, 1, dst, a);
    set(&w, 32, 8, b);
    set(&w, 64, 8, c);
    set(&w, 75, 5, shift);
    set(&w, 80, 1, (u64)high);
    set(&w, 81, 3, 7);
    set(&w, 87, 4, 7);
    return w;
}
/* ISETP p, q, a, b (registers) by cmp (76-78), signed (73), .EX (72) with low (68-71), and acc (87-90) by set_op */
static Word isetp(u32 p, u32 q, u32 a, u32 b, u32 cmp, int is_signed, int ex, u32 low, u32 set_op, u32 acc) {
    Word w = alu(0x00c, 1, 0, a);
    set(&w, 32, 8, b);
    set(&w, 64, 8, 255);
    set(&w, 68, 4, low);
    set(&w, 72, 1, (u64)ex);
    set(&w, 73, 1, (u64)is_signed);
    set(&w, 74, 2, set_op);
    set(&w, 76, 3, cmp);
    set(&w, 81, 3, p);
    set(&w, 84, 3, q);
    set(&w, 87, 4, acc);
    return w;
}
/* SEL dst, a, b (registers) by the predicate (87-90) */
static Word sel(u32 dst, u32 a, u32 b, u32 pred) {
    Word w = alu(0x007, 1, dst, a);
    set(&w, 32, 8, b);
    set(&w, 87, 4, pred);
    return w;
}
/* sm86_eval against the translation, for the instructions it runs: a 64-bit sum by IADD3's carries, LEA and its high
   word, SHF's types, sizes and wraps, IMAD and IMAD.WIDE (signed), PRMT's sign spread, ISETP (signed, unsigned,
   .EX of a 64-bit compare, its accumulator), SEL, IMNMX, PLOP3 and LOP3 */
static void test_memory_eval(void) {
    static const u32 cb1[4] = {0xfffffff0u, 0x20u, 0x80000005u, 0x7fffffffu};
    /* R5:R4 + R7:R6 by IADD3 then IADD3.X with the carry; LEA R4 << 3 + R5; LEA.HI of R6:R4 << 4 plus R5 */
    Word add[] = {iadd3_x(0, 4, 6, 255, 0, 7, 0, 7, 7), iadd3_x(1, 5, 7, 255, 7, 7, 1, 0, 0xf), lea(2, 4, 5, 255, 3, 0),
                  lea(3, 4, 5, 6, 4, 1)};
    mh_cross("sm86_eval: IADD3 and LEA", add, 4, cb1);
    /* SHF.R.U64 R5:R4 >> 7, SHF.R.S32.HI of R6:R4 >> 3, SHF.L.U32 R4 << 33 (clamped to 32), SHF.L.W R4 << 33 */
    Word shf[] = {shf_imm(0, 4, 7, 5, 1, 1, 0, 0), shf_imm(1, 4, 3, 6, 2, 1, 0, 1), shf_imm(2, 4, 33, 255, 3, 0, 0, 0),
                  shf_imm(3, 4, 33, 255, 3, 0, 1, 0)};
    mh_cross("sm86_eval: SHF", shf, 4, cb1);
    /* IMAD.WIDE R0:R1 = R4 * R6 (signed) + R7:R5, PRMT R2 = R4 bytes by 0x9c40, IMAD R3 = R4 * R5 + R6 */
    Word wide = alu(0x025, 1, 0, 4);
    set(&wide, 32, 8, 6);
    set(&wide, 64, 8, 5);
    set(&wide, 73, 1, 1);
    Word prmt = alu(0x016, 4, 2, 4);
    set(&prmt, 32, 32, 0x9c40);
    set(&prmt, 64, 8, 6);
    Word imad = alu(0x024, 1, 3, 4);
    set(&imad, 32, 8, 5);
    set(&imad, 64, 8, 6);
    Word mul[] = {wide, prmt, imad};
    mh_cross("sm86_eval: IMAD and PRMT", mul, 3, cb1);
    /* P0 = R4 < R5 (unsigned), P1 = R4 > R6 (signed) and P0; R0 = P0 ? R4 : R5, R1 = signed min of R4 and R6, P2 =
       P0 ^ P1 (PLOP3), R2 = P2 ? R5 : R6, R3 = R4 ^ R5 ^ R6 */
    Word mnmx = alu(0x017, 1, 1, 4);
    set(&mnmx, 32, 8, 6);
    set(&mnmx, 73, 1, 1);
    set(&mnmx, 87, 4, 7);
    Word plop = fix(0x81c);
    set(&plop, 87, 4, 0);
    set(&plop, 77, 4, 1);
    set(&plop, 68, 4, 7);
    set(&plop, 64, 3, 0x3c & 7); /* lut 0x3c: x ^ y */
    set(&plop, 72, 5, 0x3c >> 3);
    set(&plop, 16, 8, 0);
    set(&plop, 81, 3, 2);
    set(&plop, 84, 3, 7);
    Word cmp[] = {isetp(0, 7, 4, 5, 1, 0, 0, 7, 0, 7), isetp(1, 7, 4, 6, 4, 1, 0, 7, 0, 0), sel(0, 4, 5, 0), mnmx, plop,
                  sel(2, 5, 6, 2), lop3_reg(3, 4, 5, 6, 0x96)};
    mh_cross("sm86_eval: ISETP, SEL, IMNMX, PLOP3, LOP3", cmp, 7, cb1);
    /* R5:R4 < R7:R6 unsigned, by the low words' ISETP then the high words' .EX: R0 = P0 ? R5 : R6 */
    Word ex[] = {isetp(0, 7, 4, 6, 1, 0, 0, 7, 0, 7), isetp(0, 7, 5, 7, 1, 0, 1, 0, 0, 7), sel(0, 5, 6, 0)};
    mh_cross("sm86_eval: ISETP.EX", ex, 3, cb1);
}
/* a global access of the uniform form at [UR + imm] (RZ the register, UR a pair: bit 72): RED (0x98e), ATOMG (0x9a8)
   or STG (0x986) of the register data, of type (bits 73-75) and operation (87-90) */
static Word global_uniform(u32 opcode, u32 dst, u32 ur, u32 imm, u32 data, u32 type, u32 how) {
    Word x = fix(opcode);
    set(&x, 16, 8, dst);
    set(&x, 24, 8, 255);
    set(&x, 32, 8, data);
    set(&x, 40, 24, imm);
    set(&x, 64, 6, ur);
    set(&x, 72, 1, 1);
    set(&x, 73, 3, type);
    set(&x, 87, opcode == 0x98e ? 3 : 4, how);
    set(&x, 91, 1, 1);
    return x;
}
/* RED.E.ADD.F32.FTZ.RN [UR4 + 4], R2 and ATOMG.E.ADD.F32.FTZ.RN R5, [UR4 + 8], R3, then STG [UR4 + 0xc], R5 (NAK's
   AtomType F32 is 3), as one game's compute programs sum positions: words 1 and 2 of the buffer go from 0.5 and 1.0
   to 2.0 and 3.25, and word 3 gets 1.0, the word ATOMG found. With the device's float atomics an OpAtomicFAddEXT,
   else a loop of compare and exchange (one lane alone here: the GPU test races many) */
static void test_atomic_fadd(const char* dir) {
    for (int native = 0; native < 2; native++) {
        u8 code[16 * 8] = {0};
        u32 n = 0;
        put(code, n++, uldc64(0x20));
        put(code, n++, mov_imm(2, f2u(1.5f)));
        put(code, n++, mov_imm(3, f2u(2.25f)));
        put(code, n++, global_uniform(0x98e, 0, 4, 4, 2, 3, 0));
        put(code, n++, global_uniform(0x9a8, 5, 4, 8, 3, 3, 0));
        put(code, n++, global_uniform(0x986, 0, 4, 0xc, 5, 4, 0));
        put(code, n++, exit_insn());
        put(code, n, bra(n, n, 7, 0));
        n++;
        g_shader_float_atomic_add = native;
        ShaderInfo si = {0};
        size_t count = 0;
        u32* spv = sm86_to_spirv(code, n * 16, ST_CS, NULL, &si, &count);
        g_shader_float_atomic_add = 0;
        CHECK(spv != NULL);
        if (!spv) {
            fprintf(stderr, "atomic fadd: %s\n", si.first_unsupported);
            continue;
        }
        u32 at[4];
        CHECK(spv_find(spv, count, 6035 /* OpAtomicFAddEXT */, at, 4) == (native ? 2u : 0u));
        CHECK(spv_find(spv, count, 230 /* OpAtomicCompareExchange */, at, 4) == (native ? 0u : 2u));
        CHECK(si.ngbuf == 1 && si.gbuf_cbuf_off[0] == 0x20 && si.gbuf_wmask == 1 && si.gbuf_rmask == 1);
        static u32 bank0[1024], buffer[16], log[SH_WLOG_PART];
        memset(buffer, 0, sizeof buffer);
        bank0[0x20 / 4] = 0x1000, bank0[0x24 / 4] = 2;
        buffer[1] = f2u(0.5f), buffer[2] = f2u(1.0f);
        SpvEvalEnv env = {0};
        env.ubo[SNX_VKB_CB] = bank0;
        env.ubo_words[SNX_VKB_CB] = 1024;
        env.ubo[SNX_VKB_SSBO] = buffer;
        env.ubo_words[SNX_VKB_SSBO] = 16;
        env.ubo[SNX_VKB_WPAGES] = log;
        env.ubo_words[SNX_VKB_WPAGES] = SH_WLOG_PART;
        const char* why = spv_eval(spv, count, &env);
        int ok = !why && buffer[1] == f2u(2.0f) && buffer[2] == f2u(3.25f) && buffer[3] == f2u(1.0f);
        CHECK(ok);
        if (!ok)
            fprintf(stderr, "atomic fadd (%s): %s, words %08x %08x %08x\n", native ? "native" : "loop",
                    why ? why : "ran", buffer[1], buffer[2], buffer[3]);
        CHECK(keep_module(dir, native ? "cs_fadd_native" : "cs_fadd_loop", spv, count));
        free(spv);
    }
    /* another operation of an F32 atomic is refused by name */
    u8 code[16 * 4] = {0};
    put(code, 0, uldc64(0x20));
    put(code, 1, global_uniform(0x98e, 0, 4, 4, 2, 3, 1));
    put(code, 2, exit_insn());
    put(code, 3, bra(3, 3, 7, 0));
    ShaderInfo si = {0};
    size_t count = 0;
    u32* spv = sm86_to_spirv(code, 4 * 16, ST_CS, NULL, &si, &count);
    CHECK(!spv && strstr(si.first_unsupported, "RED of type 3, operation 1"));
    free(spv);
}
/* ULDC.64 ud, c[bank][off] */
static Word uldc64_to(u32 ud, u32 bank, u32 off) {
    Word x = uldc64(0);
    set(&x, 16, 6, ud);
    cbuf(&x, bank, off);
    return x;
}
/* An address at the sum of two constant buffer pairs, as one game's programs that sum a texture's light into a
   buffer form it: UMOV UR6, 1, ULDC.64 UR8, c[0][0x20] (the buffer, 0x1000), ULDC.64 UR4, c[1][0] (an offset, 0),
   UIMAD.WIDE.U32 UR4, UR6, UR8, UR4, then RED.E.ADD.F32.FTZ.RN [UR4 + 0x10], R2. Each of the two may be the base: the
   program binds both, and the add lands in the one that holds the address, word 4 of c[0][0x20]'s buffer, 1.0 + 2.5.
   With the offset 0x40000 the sum is in neither, and nothing changes */
static void test_global_two_bases(const char* dir) {
    u8 code[16 * 9] = {0};
    u32 n = 0;
    put(code, n++, ualu(0x082, 6, 0, 1));
    put(code, n++, uldc64_to(8, 0, 0x20));
    put(code, n++, uldc64_to(4, 1, 0));
    Word imad = fix(0x2a5); /* UIMAD.WIDE.U32 UR4, UR6, UR8, UR4 */
    set(&imad, 16, 6, 4);
    set(&imad, 24, 6, 6);
    set(&imad, 32, 6, 8);
    set(&imad, 64, 6, 4);
    set(&imad, 81, 3, 7);
    set(&imad, 87, 1, 1);
    set(&imad, 88, 4, 0xf);
    put(code, n++, imad);
    put(code, n++, mov_imm(2, f2u(2.5f)));
    put(code, n++, global_uniform(0x98e, 0, 4, 0x10, 2, 3, 0));
    put(code, n++, exit_insn());
    put(code, n, bra(n, n, 7, 0));
    n++;
    ShaderInfo si = {0};
    size_t count = 0;
    u32* spv = sm86_to_spirv(code, n * 16, ST_CS, NULL, &si, &count);
    CHECK(spv != NULL);
    if (!spv) {
        fprintf(stderr, "global two bases: %s\n", si.first_unsupported);
        return;
    }
    int at_c0 = -1, at_c1 = -1;
    for (int k = 0; k < si.ngbuf; k++) {
        if (si.gbuf_cbuf_off[k] == 0x20) at_c0 = k;
        if (si.gbuf_cbuf_off[k] == 1u << 16) at_c1 = k;
    }
    CHECK(si.ngbuf == 2 && at_c0 >= 0 && at_c1 >= 0);
    if (at_c0 < 0 || at_c1 < 0) {
        free(spv);
        return;
    }
    for (u32 offset = 0; offset <= 0x40000; offset += 0x40000) {
        static u32 bank0[1024], bank1[64], buffer[64], other[4], log[2 * SH_WLOG_PART];
        memset(buffer, 0, sizeof buffer);
        memset(other, 0, sizeof other);
        bank0[0x20 / 4] = 0x1000, bank0[0x28 / 4] = 0x100;
        bank1[0] = offset;
        buffer[4] = f2u(1.0f);
        SpvEvalEnv env = {0};
        env.ubo[SNX_VKB_CB] = bank0;
        env.ubo_words[SNX_VKB_CB] = 1024;
        env.ubo[SNX_VKB_CB + 1] = bank1;
        env.ubo_words[SNX_VKB_CB + 1] = 64;
        env.ubo[SNX_VKB_SSBO + at_c0] = buffer;
        env.ubo_words[SNX_VKB_SSBO + at_c0] = 64;
        env.ubo[SNX_VKB_SSBO + at_c1] = other;
        env.ubo_words[SNX_VKB_SSBO + at_c1] = 4;
        env.ubo[SNX_VKB_WPAGES] = log;
        env.ubo_words[SNX_VKB_WPAGES] = 2 * SH_WLOG_PART;
        const char* why = spv_eval(spv, count, &env);
        int ok = !why && buffer[4] == f2u(offset ? 1.0f : 3.5f) && !other[0] && !other[1] && !other[2] && !other[3];
        CHECK(ok);
        if (!ok) fprintf(stderr, "global two bases (offset %x): %s, word %08x\n", offset, why ? why : "ran", buffer[4]);
    }
    CHECK(keep_module(dir, "global_two_bases", spv, count));
    free(spv);
}
/* An atomic whose operand and destination are one register, at an address that adds two bases (global_dynamic), as
   one game's placement programs take a warp's first slot of a list: ATOMG.E.ADD R2, [UR4 + 0x10], R2 then STG
   [UR4 + 0x14], R2. Word 4 of c[0][0x20]'s buffer goes from 5 to 8 (R2 is 3), and word 5 gets 5, what was there */
static void test_global_atomic_same_register(const char* dir) {
    u8 code[16 * 10] = {0};
    u32 n = 0;
    put(code, n++, ualu(0x082, 6, 0, 1));
    put(code, n++, uldc64_to(8, 0, 0x20));
    put(code, n++, uldc64_to(4, 1, 0));
    Word imad = fix(0x2a5); /* UIMAD.WIDE.U32 UR4, UR6, UR8, UR4 */
    set(&imad, 16, 6, 4);
    set(&imad, 24, 6, 6);
    set(&imad, 32, 6, 8);
    set(&imad, 64, 6, 4);
    set(&imad, 81, 3, 7);
    set(&imad, 87, 1, 1);
    set(&imad, 88, 4, 0xf);
    put(code, n++, imad);
    put(code, n++, mov_imm(2, 3));
    put(code, n++, global_uniform(0x9a8, 2, 4, 0x10, 2, 0, 0));
    put(code, n++, global_uniform(0x986, 0, 4, 0x14, 2, 4, 0));
    put(code, n++, exit_insn());
    put(code, n, bra(n, n, 7, 0));
    n++;
    ShaderInfo si = {0};
    size_t count = 0;
    u32* spv = sm86_to_spirv(code, n * 16, ST_CS, NULL, &si, &count);
    CHECK(spv != NULL);
    if (!spv) {
        fprintf(stderr, "global atomic, one register: %s\n", si.first_unsupported);
        return;
    }
    int at_c0 = -1, at_c1 = -1;
    for (int k = 0; k < si.ngbuf; k++) {
        if (si.gbuf_cbuf_off[k] == 0x20) at_c0 = k;
        if (si.gbuf_cbuf_off[k] == 1u << 16) at_c1 = k;
    }
    CHECK(si.ngbuf == 2 && at_c0 >= 0 && at_c1 >= 0);
    if (at_c0 < 0 || at_c1 < 0) {
        free(spv);
        return;
    }
    static u32 bank0[1024], bank1[64], buffer[64], other[4], log[2 * SH_WLOG_PART];
    bank0[0x20 / 4] = 0x1000, bank0[0x28 / 4] = 0x100;
    buffer[4] = 5;
    SpvEvalEnv env = {0};
    env.ubo[SNX_VKB_CB] = bank0;
    env.ubo_words[SNX_VKB_CB] = 1024;
    env.ubo[SNX_VKB_CB + 1] = bank1;
    env.ubo_words[SNX_VKB_CB + 1] = 64;
    env.ubo[SNX_VKB_SSBO + at_c0] = buffer;
    env.ubo_words[SNX_VKB_SSBO + at_c0] = 64;
    env.ubo[SNX_VKB_SSBO + at_c1] = other;
    env.ubo_words[SNX_VKB_SSBO + at_c1] = 4;
    env.ubo[SNX_VKB_WPAGES] = log;
    env.ubo_words[SNX_VKB_WPAGES] = 2 * SH_WLOG_PART;
    const char* why = spv_eval(spv, count, &env);
    int ok = !why && buffer[4] == 8 && buffer[5] == 5 && !other[0];
    CHECK(ok);
    if (!ok)
        fprintf(stderr, "global atomic, one register: %s, words %u %u\n", why ? why : "ran", buffer[4], buffer[5]);
    CHECK(keep_module(dir, "global_atomic_same_register", spv, count));
    free(spv);
}

/* The driver's tessellation control programs: a header with 6 TessFactor words, no outputs and 0 threads for each
   input primitive, and an EXIT. It runs no invocation: draws take the passthrough in its place, and the translator
   gives no module for it. One with 3 threads is a program of its own, which the translator does not take yet */
/* ---- task and mesh programs ---- */

/* ISBERD (0x923) into reg, or ISBEWR (0x927) of reg, at [base + imm]: the size at 74-75 (U8 0, B32 2), the access
   at 76-77 (MAP 0, PATCH 1, ATTR 3), SKEW at 78 and the output bit at 79, as NAK's sm70_encode.rs encodes them */
enum { ISBE_MAP = 0, ISBE_PATCH = 1, ISBE_ATTR = 3, ISBE_U8 = 0, ISBE_B32 = 2 };
static Word isbe_insn(int write, u32 reg, u32 base, u32 imm, u32 size, u32 access, u32 skew, u32 output) {
    Word x = fix(write ? 0x927 : 0x923);
    set(&x, write ? 32 : 16, 8, reg);
    set(&x, 24, 8, base);
    set(&x, 40, 16, imm);
    set(&x, 74, 2, size);
    set(&x, 76, 2, access);
    set(&x, 78, 1, skew);
    set(&x, 79, 1, output);
    return x;
}
/* ISETP.GE.U32.AND P0, PT, R0, imm, PT */
static Word setp_ge(u32 imm) {
    Word x = alu(0x00c, 4, 255, 0);
    set(&x, 32, 32, imm);
    set(&x, 76, 3, 6);
    set(&x, 81, 3, 0);
    set(&x, 84, 3, 7);
    set(&x, 87, 3, 7);
    return x;
}
/* whether the module decorates an id with decoration, and declares capability */
static int spv_decorates(const u32* words, size_t count, u32 decoration) {
    u32 at[512];
    u32 n = spv_find(words, count, 71 /* OpDecorate */, at, 512);
    for (u32 k = 0; k < n; k++)
        if (words[at[k] + 2] == decoration) return 1;
    return 0;
}
static int spv_declares(const u32* words, size_t count, u32 capability) {
    u32 at[64];
    u32 n = spv_find(words, count, 17 /* OpCapability */, at, 64);
    for (u32 k = 0; k < n; k++)
        if (words[at[k] + 1] == capability) return 1;
    return 0;
}

/* A mesh program alone, one invocation, 40 vertices of the position and generic 1.y (5 words: a vertex group of
   G = 0x280 bytes) and 2 primitives of generic 0.x (after the two vertex groups, at 0x500). It writes vertex 0 and
   vertex 33 (the second group), generic 1.y of vertex 0 as its workgroup index, primitive 1's generic, the indices of
   two triangles and the count 2 */
static u32 mesh_program(u8* code) {
    u32 n = 0;
    put(code, n++, isbe_insn(0, 0, 255, 0, ISBE_B32, ISBE_ATTR, 1, 0)); /* ISBERD.ATTR.SKEW R0, [RZ+0x0] */
    put(code, n++, mov_imm(1, f2u(0.5f)));
    put(code, n++, mov_imm(2, f2u(0.25f)));
    put(code, n++, mov_imm(3, f2u(2.0f)));
    put(code, n++, mov_imm(9, 0x280));
    put(code, n++, isbe_insn(1, 1, 255, 0x000, ISBE_B32, ISBE_ATTR, 1, 1)); /* vertex 0: x */
    put(code, n++, isbe_insn(1, 2, 255, 0x080, ISBE_B32, ISBE_ATTR, 1, 1)); /* y */
    put(code, n++, isbe_insn(1, 3, 255, 0x180, ISBE_B32, ISBE_ATTR, 1, 1)); /* w */
    put(code, n++, isbe_insn(1, 0, 255, 0x200, ISBE_B32, ISBE_ATTR, 1, 1)); /* generic 1.y */
    put(code, n++, isbe_insn(1, 2, 9, 0x004, ISBE_B32, ISBE_ATTR, 1, 1));   /* vertex 33: x, at [R9 + 4] */
    put(code, n++, isbe_insn(1, 1, 255, 0x384, ISBE_B32, ISBE_ATTR, 1, 1)); /* z */
    put(code, n++, isbe_insn(1, 3, 255, 0x404, ISBE_B32, ISBE_ATTR, 1, 1)); /* w */
    put(code, n++, mov_imm(4, 7));
    put(code, n++, mov_imm(5, 9));
    put(code, n++, mov_imm(6, 33));
    put(code, n++, mov_imm(7, 0x105));
    put(code, n++, mov_imm(8, 2));
    static const u8 index_regs[6] = {255, 6, 4, 4, 6, 7}; /* (0, 33, 7) and (7, 33, 0x105 as a byte) */
    for (u32 k = 0; k < 6; k++) put(code, n++, isbe_insn(1, index_regs[k], 255, 4 + k, ISBE_U8, ISBE_MAP, 0, 1));
    put(code, n++, isbe_insn(1, 5, 255, 0x504, ISBE_B32, ISBE_ATTR, 1, 1)); /* primitive 1: generic 0.x */
    put(code, n++, isbe_insn(1, 8, 255, 3, ISBE_B32, ISBE_MAP, 0, 1));      /* the primitive count */
    put(code, n++, exit_insn());
    put(code, n, bra(n, n, 7, 0));
    return n + 1;
}
static ShaderInfo mesh_info(u32 sph[32]) {
    ShaderInfo si = {0};
    sph[0] = sph0(1, 1);
    sph[13] = 0xfu << 12 | 1u << 21; /* the output map: the position (bits 428-431) and generic 1.y (bit 437) */
    si.mesh = (ShMesh){1, 0x80, 40, 2, 0, 3, 0, {0}};
    si.mesh.prim_sph[0] = 0x010c1081u; /* a geometry SPH with passthrough on, generic 0.x (bit 432) */
    si.mesh.prim_sph[13] = 1u << 16;
    return si;
}
static void test_mesh(const char* dir) {
    u8 code[16 * 40] = {0};
    u32 n = mesh_program(code), sph[32] = {0};
    for (int fix_depth = 0; fix_depth < 2; fix_depth++) {
        ShaderInfo si = mesh_info(sph);
        size_t count = 0;
        u32* spv = sm86_to_spirv(code, n * 16, ST_MS, sph, &si, &count);
        CHECK(spv != NULL);
        if (!spv) {
            fprintf(stderr, "mesh: %s\n", si.first_unsupported);
            return;
        }
        CHECK(si.out_mask == 3 && si.prim_mask == 1 && sm86_mesh_primitive_mask(&si.mesh) == 1);
        CHECK(spv_declares(spv, count, 5283 /* MeshShadingEXT */) && spv_decorates(spv, count, 5271));
        if (!fix_depth) CHECK(keep_module(dir, "ms", spv, count));
        static u32 position[40 * 4], generic1[40 * 4], generic0[2 * 4], indices[2 * 3];
        memset(position, 0xee, sizeof position);
        SpvEvalEnv env = {0};
        static u32 first_ubo[4]; /* SNX_VKB_RS, which only an indirect draw's flag reads (test_task) */
        env.builtin[26][0] = 2; /* WorkgroupId.x, after the draw's first group 5 */
        env.push[0] = 5;
        env.push[1] = fix_depth ? f2u(1.0f) : 0;
        env.ubo[SNX_VKB_RS] = first_ubo, env.ubo_words[SNX_VKB_RS] = 4;
        env.arrays[32] = position, env.array_words[32] = 40 * 4;
        env.arrays[1] = generic1, env.array_words[1] = 40 * 4;
        env.arrays[0] = generic0, env.array_words[0] = 2 * 4;
        env.arrays[33] = indices, env.array_words[33] = 2 * 3;
        const char* why = spv_eval(spv, count, &env);
        free(spv);
        if (why) fprintf(stderr, "mesh: the evaluator stops at %s\n", why);
        CHECK(!why && env.set_outputs == 1 && env.mesh_outputs[0] == 40 && env.mesh_outputs[1] == 2);
        const u32 *p0 = position, *p33 = position + 4 * 33, *g = generic1, *q = generic0 + 4;
        u32 z0 = fix_depth ? f2u(1.0f) : 0, z33 = fix_depth ? f2u(1.25f) : f2u(0.5f); /* (z + w) / 2 */
        int ok = p0[0] == f2u(0.5f) && p0[1] == f2u(0.25f) && p0[2] == z0 && p0[3] == f2u(2.0f) &&
                 p33[0] == f2u(0.25f) && p33[1] == 0 && p33[2] == z33 && p33[3] == f2u(2.0f) && position[4] == 0;
        CHECK(ok);
        if (!ok)
            fprintf(stderr, "mesh: vertex 0 %08x %08x %08x %08x, vertex 33 %08x %08x %08x %08x\n", p0[0], p0[1], p0[2],
                    p0[3], p33[0], p33[1], p33[2], p33[3]);
        /* generic 1: x, z and w as a vertex program's outputs start, y the workgroup index 2 + 5 */
        CHECK(g[0] == 0 && g[1] == 7 && g[2] == 0 && g[3] == f2u(1.0f));
        CHECK(q[0] == 9 && q[1] == 0 && q[3] == f2u(1.0f) && generic0[0] == 0);
        CHECK(indices[0] == 0 && indices[1] == 33 && indices[2] == 7 && indices[3] == 7 && indices[4] == 33 &&
              indices[5] == 5);
    }
}

/* A task program, one invocation and a payload of 0x20 bytes: R0 the workgroup index, into the payload at 0x10; then
   an EXIT when R0 >= 5, and else 3 mesh groups to launch. The count starts at 0, whatever the payload held */
static void test_task(const char* dir) {
    u8 code[16 * 8] = {0};
    u32 n = 0;
    put(code, n++, isbe_insn(0, 0, 255, 0, ISBE_B32, ISBE_ATTR, 1, 0));
    put(code, n++, isbe_insn(1, 0, 255, 0x10, ISBE_B32, ISBE_ATTR, 0, 1));
    put(code, n++, setp_ge(5));
    put(code, n++, predicated(exit_insn(), 0));
    put(code, n++, mov_imm(1, 3));
    put(code, n++, isbe_insn(1, 1, 255, 4, ISBE_B32, ISBE_ATTR, 0, 1));
    put(code, n++, exit_insn());
    put(code, n, bra(n, n, 7, 0));
    n++;
    u32 sph[32] = {0};
    sph[0] = sph0(1, 1);
    /* the draw's first group 0 and 5 in the push constant's x, then 5 as an indirect draw's: from the uniform buffer at
       SNX_VKB_RS, where the push constant's z says so (its x is then not read) */
    for (u32 variant = 0; variant < 3; variant++) {
        u32 first = variant ? 5 : 0;
        ShaderInfo si = {0};
        si.mesh.threads = 1;
        si.mesh.payload = 0x20;
        size_t count = 0;
        u32* spv = sm86_to_spirv(code, n * 16, ST_TS, sph, &si, &count);
        CHECK(spv != NULL);
        if (!spv) {
            fprintf(stderr, "task: %s\n", si.first_unsupported);
            return;
        }
        if (!first) CHECK(keep_module(dir, "ts", spv, count));
        SpvEvalEnv env = {0};
        u32 first_ubo[4] = {variant == 2 ? first : 0x777, 0, 0, 0};
        env.builtin[26][0] = 2;
        env.push[0] = variant == 2 ? 99 : first;
        env.push[2] = variant == 2 ? SH_PUSH_GPU_MESH_FIRST : 0;
        env.ubo[SNX_VKB_RS] = first_ubo, env.ubo_words[SNX_VKB_RS] = 4;
        env.payload[1] = 77;
        const char* why = spv_eval(spv, count, &env);
        free(spv);
        if (why) fprintf(stderr, "task: the evaluator stops at %s\n", why);
        /* index 2: 3 groups; index 7: the EXIT leaves the count at 0 */
        u32 groups = first ? 0 : 3;
        CHECK(!why && env.emitted_tasks == 1 && env.mesh_tasks[0] == groups && env.mesh_tasks[1] == 1 &&
              env.mesh_tasks[2] == 1);
        CHECK(env.payload[4] == 2 + first && env.payload[1] == groups);
    }
}

/* A mesh program after a task program: its workgroup index is ISBERD.ATTR [RZ+0x0] without the draw's first group,
   and it reads the task's payload from 0x10 on: its one vertex's position.x from payload byte 0x14, its generic 0.x
   the index. Its count of 9 primitives is more than its most (1), which the outputs keep to */
static void test_mesh_after_task(const char* dir) {
    u8 code[16 * 10] = {0};
    u32 n = 0;
    put(code, n++, isbe_insn(0, 0, 255, 0, ISBE_B32, ISBE_ATTR, 0, 0));
    put(code, n++, isbe_insn(0, 1, 255, 0x14, ISBE_B32, ISBE_ATTR, 0, 0));
    put(code, n++, isbe_insn(1, 1, 255, 0, ISBE_B32, ISBE_ATTR, 1, 1));
    put(code, n++, isbe_insn(1, 0, 255, 0x80, ISBE_B32, ISBE_ATTR, 1, 1));
    put(code, n++, mov_imm(2, 9));
    put(code, n++, isbe_insn(1, 2, 255, 3, ISBE_B32, ISBE_MAP, 0, 1));
    put(code, n++, exit_insn());
    put(code, n, bra(n, n, 7, 0));
    n++;
    u32 sph[32] = {0};
    sph[0] = 0x020c0c81u;          /* a tessellation evaluation SPH, as the hardware runs it */
    sph[13] = 1u << 12 | 1u << 16; /* position.x and generic 0.x */
    ShaderInfo si = {0};
    si.mesh = (ShMesh){1, 0x20, 1, 1, 1, 3, 0, {0}};
    size_t count = 0;
    u32* spv = sm86_to_spirv(code, n * 16, ST_MS, sph, &si, &count);
    CHECK(spv != NULL);
    if (!spv) {
        fprintf(stderr, "mesh after a task: %s\n", si.first_unsupported);
        return;
    }
    CHECK(keep_module(dir, "ms_task", spv, count));
    static u32 position[4], generic0[4];
    SpvEvalEnv env = {0};
    env.builtin[26][0] = 2;
    env.push[0] = 5;
    env.payload[5] = f2u(3.0f);
    env.arrays[32] = position, env.array_words[32] = 4;
    env.arrays[0] = generic0, env.array_words[0] = 4;
    const char* why = spv_eval(spv, count, &env);
    free(spv);
    if (why) fprintf(stderr, "mesh after a task: the evaluator stops at %s\n", why);
    CHECK(!why && env.set_outputs == 1 && env.mesh_outputs[0] == 1 && env.mesh_outputs[1] == 1);
    CHECK(position[0] == f2u(3.0f) && position[1] == 0 && position[2] == 0 && position[3] == f2u(1.0f));
    CHECK(generic0[0] == 2);
}

/* sm86_mesh_output_bytes against the outputs that the translation declares: four scalars for each Output location,
   the per-primitive ones (PerPrimitiveEXT) in granules of primitives, the others in granules of vertices, the
   triangle indices not counted ("Mesh Shader Output" of the Vulkan spec). mesh_info's program: the position and
   generic 1 for 40 vertices, generic 0 for 2 primitives */
static void test_mesh_output_bytes(void) {
    u8 code[16 * 40] = {0};
    u32 n = mesh_program(code), sph[32] = {0};
    ShaderInfo si = mesh_info(sph);
    size_t count = 0;
    u32* spv = sm86_to_spirv(code, n * 16, ST_MS, sph, &si, &count);
    CHECK(spv != NULL);
    if (!spv) return;
    u32 vars[64], decorations[512], vertex = 0, primitive = 0;
    u32 nv = spv_find(spv, count, 59 /* OpVariable */, vars, 64);
    u32 nd = spv_find(spv, count, 71 /* OpDecorate */, decorations, 512);
    for (u32 i = 0; i < nv; i++) {
        if (spv[vars[i] + 3] != 3 /* Output */) continue;
        u32 id = spv[vars[i] + 2];
        int per_primitive = 0, indices = 0;
        for (u32 k = 0; k < nd; k++) {
            const u32* d = spv + decorations[k];
            if (d[1] != id) continue;
            per_primitive |= d[2] == 5271;                       /* PerPrimitiveEXT */
            indices |= d[2] == 11 /* BuiltIn */ && d[3] == 5296; /* PrimitiveTriangleIndicesEXT */
        }
        if (!indices) *(per_primitive ? &primitive : &vertex) += 1;
    }
    free(spv);
    u64 declared = 16ull * (vertex * 64 + primitive * 32); /* 40 vertices in granules of 32, 2 primitives */
    u64 bytes = sm86_mesh_output_bytes(sph, &si.mesh, 32, 32);
    CHECK(vertex == 2 && primitive == 1 && bytes == declared && bytes == 2560);
    if (bytes != declared)
        fprintf(stderr, "mesh output bytes: %llu, the module declares %llu (%u and %u locations)\n",
                (unsigned long long)bytes, (unsigned long long)declared, vertex, primitive);
    CHECK(sm86_mesh_output_bytes(sph, &si.mesh, 1, 1) == 16u * (2 * 40 + 1 * 2));
}

/* the forms and keys that the facts of the game's programs do not cover are refused, with their reason */
static void refused(const u8* code, u32 n, int stage, const u32* sph, ShaderInfo si, const char* reason) {
    size_t count = 0;
    u32* spv = sm86_to_spirv(code, n * 16, stage, sph, &si, &count);
    int ok = !spv && si.unsupported && strstr(si.first_unsupported, reason);
    CHECK(ok);
    if (!ok) fprintf(stderr, "refused: \"%s\", not \"%s\"\n", si.first_unsupported, reason);
    free(spv);
}
static void test_mesh_refused(void) {
    u8 code[16 * 3] = {0};
    u32 sph[32] = {0};
    ShaderInfo si = mesh_info(sph);
    put(code, 0, isbe_insn(1, 1, 255, 0x40, ISBE_B32, ISBE_PATCH, 1, 1));
    put(code, 1, exit_insn());
    put(code, 2, bra(2, 2, 7, 0));
    refused(code, 3, ST_MS, sph, si, "ISBEWR.PATCH.SKEW.O.B32 [RZ+0x40] in a mesh program");
    put(code, 0, isbe_insn(0, 1, 255, 0, ISBE_B32, ISBE_ATTR, 0, 0)); /* the index after a task, in a mesh alone */
    refused(code, 3, ST_MS, sph, si, "ISBERD.ATTR.B32 [RZ+0x0] in a mesh program");
    refused(code, 3, ST_VS, sph, (ShaderInfo){0}, "ISBERD outside a task or mesh program");
    put(code, 0, isbe_insn(1, 1, 255, 0x6, ISBE_B32, ISBE_ATTR, 1, 1));
    refused(code, 3, ST_MS, sph, si, "at the byte offset 0x6");
    put(code, 0, fix(0x918)); /* NOP */
    ShaderInfo k = si;
    k.mesh.threads = 64;
    refused(code, 3, ST_MS, sph, k, "workgroup of 64 invocations");
    k = si;
    k.mesh.primitive_vertices = 2;
    refused(code, 3, ST_MS, sph, k, "primitives of 2 vertices");
    k = si;
    k.mesh.max_vertices = 257;
    refused(code, 3, ST_MS, sph, k, "257 vertices");
    k = si;
    k.mesh.prim_sph[13] |= 1u << 21; /* generic 1.y per primitive too */
    refused(code, 3, ST_MS, sph, k, "both per vertex and per primitive");
    k = si;
    k.mesh.prim_sph[13] |= 1u << 12; /* the position, per primitive */
    refused(code, 3, ST_MS, sph, k, "per-primitive output of SPH output map bit 428");
    u32 clip[32];
    memcpy(clip, sph, sizeof clip);
    clip[18] = 1; /* clip distance 0 (bit 576) */
    refused(code, 3, ST_MS, clip, si, "per-vertex output of SPH output map bit 576");
    k = (ShaderInfo){0};
    k.mesh.threads = 32;
    k.mesh.payload = 4;
    refused(code, 3, ST_TS, sph, k, "task payload of 0x4 bytes");
}

/* A fragment program's inputs that its draw's mesh program writes per primitive (ShaderInfo.prim_mask) are
   PerPrimitiveEXT, and the others are not */
static void test_fragment_per_primitive(const char* dir) {
    u8 code[16 * 8] = {0};
    u32 n = fragment_program(code), fsph[32] = {0};
    fsph[0] = sph0(2, 5);
    fsph[6] = 2;
    fsph[18] = 0xf;
    for (u32 mask = 0; mask < 4; mask += 3) {
        ShaderInfo si = {0};
        si.prim_mask = mask; /* generic 0 (its input), and generic 1, which it does not read */
        size_t count = 0;
        u32* spv = sm86_to_spirv(code, n * 16, ST_FS, fsph, &si, &count);
        CHECK(spv != NULL);
        if (!spv) return;
        CHECK(spv_decorates(spv, count, 5271) == (mask != 0) && spv_declares(spv, count, 5283) == (mask != 0));
        if (mask) CHECK(keep_module(dir, "fs_per_primitive", spv, count));
        free(spv);
    }
}

/* A fragment program's input that no output of the stage before writes (ShaderInfo.in_unfed_mask) reads 0, 0, 0, 1,
   as the Maxwell translator's does, and the module declares no Input for it, which the stage before would not match
   (VUID-RuntimeSpirv-OpEntryPoint-08743: one game's mesh programs write fewer generics than their fragment programs
   read). An IPA at the centroid of such an input decorates nothing: the decoration named id 0 before, a module that
   spirv-val and the validation layer refuse */
static void test_fragment_unfed(const char* dir) {
    u8 code[16 * 8] = {0};
    u32 n = fragment_program(code), fsph[32] = {0};
    fsph[0] = sph0(2, 5);
    fsph[6] = 2;
    fsph[18] = 0xf;
    u32 inputs_of[2] = {0};
    for (u32 centroid = 0; centroid < 2; centroid++) {
        code[9] = (u8)((code[9] & ~0x30u) | centroid << 4); /* the IPA's bits 76-77, its InterpLoc: 1 the centroid */
        ShaderInfo si = {0};
        si.in_unfed_mask = 1;
        size_t count = 0;
        u32* spv = sm86_to_spirv(code, n * 16, ST_FS, fsph, &si, &count);
        CHECK(spv != NULL);
        if (!spv) return;
        u32 at[64], found = spv_find(spv, count, 71 /* OpDecorate */, at, 64), zero = 0;
        for (u32 k = 0; k < found; k++) zero += spv[at[k] + 1] == 0;
        CHECK(found < 64 && zero == 0);
        CHECK(keep_module(dir, centroid ? "fs_unfed_centroid" : "fs_unfed_default", spv, count));
        free(spv);
    }
    code[9] &= (u8)~0x30u;
    for (u32 unfed = 0; unfed < 2; unfed++) {
        ShaderInfo si = {0};
        si.in_unfed_mask = unfed;
        size_t count = 0;
        u32* spv = sm86_to_spirv(code, n * 16, ST_FS, fsph, &si, &count);
        CHECK(spv != NULL);
        if (!spv) return;
        u32 at[64], inputs = 0, found = spv_find(spv, count, 59 /* OpVariable */, at, 64);
        for (u32 k = 0; k < found; k++) inputs += spv[at[k] + 3] == 1 /* Input */;
        inputs_of[unfed] = inputs;
        CHECK(si.in_mask == 1);
        if (unfed) CHECK(keep_module(dir, "fs_unfed", spv, count));
        SpvEvalEnv env = {0};
        env.in[0][0] = f2u(0.75f);
        env.builtin[15][3] = f2u(1.0f); /* FragCoord.w, which IPA's perspective divides by */
        const char* why = spv_eval(spv, count, &env);
        free(spv);
        if (why) fprintf(stderr, "fragment unfed: the evaluator stops at %s\n", why);
        CHECK(!why && env.out[0][0] == (unfed ? 0 : f2u(0.75f)));
    }
    CHECK(inputs_of[0] == inputs_of[1] + 1); /* the generic's Input, beside FragCoord */
}

/* the pass over the shipped programs leaves a task or mesh program to its draw: it finds their ISBE accesses */
static void test_uses_isbe(void) {
    u8 code[16 * 40] = {0};
    u32 n = mesh_program(code);
    CHECK(sm86_uses_isbe(code, n * 16));
    CHECK(!sm86_uses_isbe(code + 16, 16 * 4)); /* the MOVs after its ISBERD */
    n = vertex_program(code);
    CHECK(!sm86_uses_isbe(code, n * 16));
}

static void test_tcs_empty(void) {
    u8 code[16 * 2] = {0};
    put(code, 0, exit_insn());
    put(code, 1, bra(1, 1, 7, 0));
    u32 sph[32] = {0x000c0881u, 0x06000000u, 0, 0x60000000u, 0x000ff000u};
    CHECK(sm86_sph_stage(sph[0]) == ST_TCS && sm86_tcs_empty(sph[2]));
    ShaderInfo si = {0};
    size_t count = 0;
    CHECK(!sm86_to_spirv(code, 2 * 16, ST_TCS, sph, &si, &count) && strstr(si.first_unsupported, "no invocation"));
    sph[2] = 3u << 24;
    CHECK(!sm86_tcs_empty(sph[2]));
    si = (ShaderInfo){0};
    CHECK(!sm86_to_spirv(code, 2 * 16, ST_TCS, sph, &si, &count) && !strstr(si.first_unsupported, "no invocation"));
}

/* the SPIR-V instructions of opcode op in a module */
static u32 spv_count(const u32* words, size_t count, u32 op) {
    u32 found = 0;
    for (size_t i = 5; i < count && words[i] >> 16; i += words[i] >> 16) found += (words[i] & 0xffff) == op;
    return found;
}
/* how many of the module's GLSL extended instructions are the one numbered inst */
static u32 spv_count_glsl(const u32* words, size_t count, u32 inst) {
    u32 found = 0;
    for (size_t i = 5; i < count && words[i] >> 16; i += words[i] >> 16)
        found += (words[i] & 0xffff) == 12 && words[i] >> 16 >= 4 && words[i + 4] == inst;
    return found;
}
/* whether a module decorates a variable as the built-in */
static int spv_has_builtin(const u32* words, size_t count, u32 builtin) {
    for (size_t i = 5; i < count && words[i] >> 16; i += words[i] >> 16)
        if ((words[i] & 0xffff) == 71 && words[i] >> 16 == 4 && words[i + 2] == 11 && words[i + 3] == builtin) return 1;
    return 0;
}
/* HMMA (16816 and 1688, F16 and F32), LDSM and MOVM with and without the device's quads in compute and its mixed dot
   product: A's 48 reads are quad broadcasts or shuffles, B's 48, LDSM's 4 and MOVM's 2 stay shuffles, and each pair
   of products is an OpFDot2MixAcc32VALVE (96) or an OpDot. B's registers are OR'd with hmma_zero's zero, which
   compares NumWorkgroups.x with 0. With g_shader_hmma_pairs B's reads are 24 shuffles and their 24 xors of 4, and A's
   96 (two quad lanes, one picked, for each of the 48). With g_shader_hmma_f16 and no mixed dot product, the OpDots
   are f16 OpFmas (96) */
static void test_warp_matrix(const char* dir) {
    u8 code[16 * 8] = {0};
    static const u32 hmma[4][4] = {{16, 14, 0x08}, {20, 24, 0x18}, {28, 255, 0x00}, {32, 36, 0x10}};
    u32 n = 0;
    for (u32 k = 0; k < 4; k++) { /* HMMA dst, R8, R12, C */
        Word x = fix(0x23c);
        set(&x, 16, 8, hmma[k][0]);
        set(&x, 24, 8, 8);
        set(&x, 32, 8, 12);
        set(&x, 64, 8, hmma[k][1]);
        set(&x, 72, 19, hmma[k][2]);
        put(code, n++, x);
    }
    Word x = fix(0x83b); /* LDSM.16.M88.4 R40, [R7 + 0x40] */
    set(&x, 16, 8, 40);
    set(&x, 24, 8, 7);
    set(&x, 40, 24, 0x40);
    set(&x, 72, 2, 2);
    put(code, n++, x);
    x = fix(0x23a); /* MOVM.16.MT88 R44, R8 */
    set(&x, 16, 8, 44);
    set(&x, 24, 8, 8);
    put(code, n++, x);
    put(code, n++, exit_insn());
    put(code, n, bra(n, n, 7, 0));
    n++;
    int quad_was = g_shader_quad_compute, mixed_was = g_shader_mixed_dot, pairs_was = g_shader_hmma_pairs,
        f16_was = g_shader_hmma_f16;
    int coop_was = g_shader_hmma_coop, tile_was = g_shader_hmma_coop_tile;
    for (int pairs = 0; pairs < 2; pairs++)
        for (int quad = 0; quad < 2; quad++)
            for (int mixed = 0; mixed < 2; mixed++)
                for (int f16 = 0; f16 < 2; f16++) {
                    /* the halves' products and sums (the hmma_f16 setting) are the f32 dots' place, two products an
                       OpFma, where the mixed dot product is not */
                    g_shader_quad_compute = quad, g_shader_mixed_dot = mixed, g_shader_hmma_pairs = pairs;
                    g_shader_hmma_f16 = f16;
                    int packed = !mixed && f16 && g_shader_float16;
                    ShaderInfo si = {0};
                    si.cs_local[0] = 32, si.cs_local[1] = si.cs_local[2] = 1;
                    si.shared_size = 1024;
                    size_t count = 0;
                    u32* spv = sm86_to_spirv(code, n * 16, ST_CS, NULL, &si, &count);
                    CHECK(spv != NULL);
                    if (!spv) continue;
                    u32 quads = spv_count(spv, count, 365), shuffles = spv_count(spv, count, 345);
                    u32 xors = spv_count(spv, count, 346), mixes = spv_count(spv, count, 6916);
                    u32 dots = spv_count(spv, count, 148), fmas = spv_count_glsl(spv, count, 50 /* Fma */);
                    u32 a_reads = pairs ? 96u : 48u, b_reads = pairs ? 24u : 48u;
                    u32 want_quads = quad ? a_reads : 0, want_shuffles = (quad ? 0 : a_reads) + b_reads + 6;
                    CHECK(quads == want_quads && shuffles == want_shuffles && xors == (pairs ? 24u : 0u));
                    CHECK(fmas == (packed ? 96u : 0u));
                    CHECK(mixes == (mixed ? 96u : 0u) && dots == (mixed || packed ? 0u : 96u));
                    CHECK(spv_has_builtin(spv, count, 24) && spv_count(spv, count, 170 /* OpIEqual */) >= 1);
                    if (quads != want_quads || shuffles != want_shuffles || mixes != (mixed ? 96u : 0u) ||
                        fmas != (packed ? 96u : 0u))
                        fprintf(stderr,
                                "warp matrix, pairs %d, quad %d, mixed %d, f16 %d: %u quad broadcasts (%u wanted), "
                                "%u shuffles (%u), %u xors, %u mixed dots, %u dots, %u fmas\n",
                                pairs, quad, mixed, f16, quads, want_quads, shuffles, want_shuffles, xors, mixes, dots,
                                fmas);
                    char name[32];
                    snprintf(name, sizeof name, "warp_matrix%d%d%d%d", pairs, quad, mixed, f16);
                    CHECK(keep_module(dir, name, spv, count));
                    free(spv);
                }
    g_shader_quad_compute = quad_was, g_shader_mixed_dot = mixed_was, g_shader_hmma_pairs = pairs_was;
    g_shader_hmma_f16 = f16_was;
    /* staged (g_shader_hmma_stage): A and B through the warp's slot of shared memory, so the only shuffles are LDSM's
       4 and MOVM's 2, no quad broadcast even where the stage has them, and two subgroup barriers an HMMA */
    int stage_was = g_shader_hmma_stage;
    for (int quad = 0; quad < 2; quad++) {
        g_shader_quad_compute = quad, g_shader_mixed_dot = 0, g_shader_hmma_pairs = 0, g_shader_hmma_stage = 65536;
        ShaderInfo si = {0};
        si.cs_local[0] = 32, si.cs_local[1] = si.cs_local[2] = 1;
        si.shared_size = 1024;
        size_t count = 0;
        u32* spv = sm86_to_spirv(code, n * 16, ST_CS, NULL, &si, &count);
        CHECK(spv != NULL);
        if (!spv) continue;
        u32 quads = spv_count(spv, count, 365), shuffles = spv_count(spv, count, 345);
        u32 barriers = spv_count(spv, count, 224 /* OpControlBarrier */);
        CHECK(quads == 0 && shuffles == 6 && spv_count(spv, count, 346) == 0 && barriers == 8);
        if (quads || shuffles != 6 || barriers != 8)
            fprintf(stderr, "warp matrix staged, quad %d: %u quad broadcasts, %u shuffles, %u barriers\n", quad, quads,
                    shuffles, barriers);
        char name[32];
        snprintf(name, sizeof name, "warp_matrix_stage%d", quad);
        CHECK(keep_module(dir, name, spv, count));
        free(spv);
    }
    g_shader_quad_compute = quad_was, g_shader_mixed_dot = mixed_was, g_shader_hmma_pairs = pairs_was;
    g_shader_hmma_stage = stage_was;
    /* The matrix unit's tile: one OpCooperativeMatrixMulAddKHR an HMMA where the driver offers 16x16x16 (RADV), four
       where its tile is 8x8x8 (Metal's simdgroup_matrix, which a MoltenVK with VK_KHR_cooperative_matrix reports):
       the two M halves, each the sum of the two K halves, of which a .1688 has one: 12 for the 4 HMMAs above, two of
       them .1688 */
    for (int tile = 0; tile < 2; tile++) {
        g_shader_hmma_coop = 65536, g_shader_hmma_coop_tile = tile ? 8 : 16;
        g_shader_quad_compute = 1, g_shader_mixed_dot = 0, g_shader_hmma_pairs = 0;
        ShaderInfo si = {0};
        si.cs_local[0] = 32, si.cs_local[1] = si.cs_local[2] = 1;
        si.shared_size = 1024;
        size_t count = 0;
        u32* spv = sm86_to_spirv(code, n * 16, ST_CS, NULL, &si, &count);
        u32 muladds = spv ? spv_count(spv, count, 4459) : 0;
        CHECK(muladds == (tile ? 12u : 4u));
        if (muladds != (tile ? 12u : 4u))
            fprintf(stderr, "matrix tile %d: %u products for 4 HMMAs, expected %u\n", tile ? 8 : 16, muladds,
                    tile ? 12u : 4u);
        char name[32];
        snprintf(name, sizeof name, "warp_matrix_coop%d", tile ? 8 : 16);
        CHECK(spv && keep_module(dir, name, spv, count));
        free(spv);
    }
    g_shader_quad_compute = quad_was, g_shader_mixed_dot = mixed_was, g_shader_hmma_pairs = pairs_was;
    g_shader_hmma_coop = coop_was, g_shader_hmma_coop_tile = tile_was;
}

/* ---- HMMA's values on the evaluator's subgroup of 32 lanes (spv_eval_subgroup) ---- */

/* the eight HMMAs of hmma_program: destination, C (255: RZ), the variant at bits 72-90 (8 16816.F16, 0x18 16816.F32,
   0 1688.F16, 0x10 1688.F32), all of A at R8, B at R12. R14-R15 is an f16 C; R20 and R40 are earlier HMMAs' D */
static const u32 hmma_cases[8][3] = {{16, 255, 0x08}, {18, 14, 0x08}, {20, 255, 0x18}, {36, 20, 0x18},
                                     {40, 255, 0x10}, {44, 14, 0x00}, {46, 255, 0x00}, {48, 40, 0x10}};
#define HMMA_OUT 24u /* each lane's output words: the cases' D registers in order */
/* S2R R0, TID.X; R1 = 32 R0; R8-R15 from c[1][R1 + 4k] (A, B, C as PTX's mma.sync lays them out); the eight HMMAs;
   their D registers stored at shared [96 R0 + 4k] */
static u32 hmma_program(u8* code) {
    u32 n = 0;
    Word x = fix(0x919); /* S2R R0, TID.X */
    set(&x, 72, 8, 0x21);
    put(code, n++, x);
    x = alu(0x024, 4, 1, 0); /* IMAD R1, R0, 32, RZ */
    set(&x, 32, 32, 32);
    set(&x, 64, 8, 255);
    put(code, n++, x);
    x = alu(0x024, 4, 2, 0); /* IMAD R2, R0, 96, RZ */
    set(&x, 32, 32, HMMA_OUT * 4);
    set(&x, 64, 8, 255);
    put(code, n++, x);
    /* STS [R2], RZ: the program's shared memory is its first Workgroup variable, the one the evaluator keeps, before
       a staged HMMA's slots (g_shader_hmma_stage) */
    x = fix(0x388);
    set(&x, 24, 8, 2);
    set(&x, 32, 8, 255);
    set(&x, 73, 3, 4);
    put(code, n++, x);
    for (u32 k = 0; k < 8; k++) put(code, n++, ldc(8 + k, 1, 1, 4 * k, 4));
    for (u32 k = 0; k < 8; k++) {
        x = fix(0x23c);
        set(&x, 16, 8, hmma_cases[k][0]);
        set(&x, 24, 8, 8);
        set(&x, 32, 8, 12);
        set(&x, 64, 8, hmma_cases[k][1]);
        set(&x, 72, 19, hmma_cases[k][2]);
        put(code, n++, x);
    }
    u32 out = 0;
    for (u32 k = 0; k < 8; k++)
        for (u32 r = 0; r < (hmma_cases[k][2] & 0x10 ? 4u : 2u); r++) {
            x = fix(0x388); /* STS [R2 + 4 out], D + r */
            set(&x, 24, 8, 2);
            set(&x, 32, 8, hmma_cases[k][0] + r);
            set(&x, 40, 24, 4 * out++);
            set(&x, 73, 3, 4);
            put(code, n++, x);
        }
    put(code, n++, exit_insn());
    put(code, n, bra(n, n, 7, 0));
    return n + 1;
}
/* an f16's value, subnormals, infinities and NaNs too */
static double hmma_half(u32 h) {
    double m = (double)(h & 0x3ffu);
    u32 e = h >> 10 & 31;
    double v = e == 31 ? (m ? NAN : INFINITY) : e ? ldexp(1024.0 + m, (int)e - 25) : ldexp(m, -24);
    return h & 0x8000u ? -v : v;
}
/* an element: the exact set's n / 4 for n in -6..6 (every product and sum exact in f16 and f32); the rounded set's
   full mantissas, row 3 of A subnormal; the special set's signed zeros, subnormals, the largest finite, infinities
   and NaNs among ordinary values. The zeros set (hmma_inputs) is products of zero only, of either sign */
enum { HM_EXACT, HM_ROUNDED, HM_SPECIAL, HM_ZEROS };
static u32 hmma_element(u32* seed, int set, int tiny) {
    *seed = *seed * 1664525u + 1013904223u;
    u32 r = *seed, sign = r >> 31 << 15;
    if (set == HM_EXACT) {
        int n = (int)(r >> 24) % 13 - 6;
        u32 h = 0, a = (u32)(n < 0 ? -n : n); /* a / 4 as an f16 */
        if (a) {
            u32 e = a >= 4 ? 2 : a >= 2 ? 1 : 0; /* a / 4 = 2^(e - 2) (a / 2^e) */
            h = (13 + e) << 10 | ((a << (10 - e)) & 0x3ffu);
        }
        return (n < 0 ? 0x8000u : 0) | h;
    }
    u32 m = r >> 8 & 0x3ffu;
    if (set == HM_SPECIAL) {
        static const u16 odd[8] = {0x0000, 0x0001, 0x03ff, 0x7bff, 0x7c00, 0x7e00, 0x7d01, 0x0000};
        if ((r >> 4 & 7) == 0) return sign | odd[r >> 12 & 7]; /* one in 8 */
    }
    if (tiny) return sign | (m | 1); /* a subnormal: m 2^-24 */
    return sign | (15u - (r >> 20 & 1)) << 10 | m;
}
/* The lanes' inputs for one warp (in: 8 words each) and, by output word and half, the terms its sum adds (the 16 or 8
   products, and C where it is the input's) */
typedef struct {
    u16 a[16][16], b[16][8], c[16][8];
} HmmaMats;
static void hmma_inputs(HmmaMats* m, u32* in, int set) {
    u32 seed = set == HM_EXACT ? 0x2545f491u : set == HM_ROUNDED ? 0x9e3779b9u : 0x85ebca6bu;
    for (u32 r = 0; r < 16; r++)
        for (u32 k = 0; k < 16; k++) m->a[r][k] = (u16)hmma_element(&seed, set, set == HM_ROUNDED && r == 3);
    for (u32 k = 0; k < 16; k++)
        for (u32 n = 0; n < 8; n++) m->b[k][n] = (u16)hmma_element(&seed, set, 0);
    for (u32 r = 0; r < 16; r++)
        for (u32 n = 0; n < 8; n++) m->c[r][n] = (u16)hmma_element(&seed, set, 0);
    if (set == HM_ZEROS) /* A's zeros a sign a row, B's ordinary values a sign every two columns, C's zeros of both:
                            each output's products are all +0 or all -0, with a C of either sign */
        for (u32 r = 0; r < 16; r++)
            for (u32 k = 0; k < 16; k++) {
                m->a[r][k] = (u16)((r & 1) << 15);
                if (r < 8) m->b[k][r] = (u16)((r >> 1 & 1) << 15 | 0x3c00u | k);
                m->c[r][k & 7] = (u16)((((r >> 1) ^ k) & 1) << 15);
            }
    for (u32 l = 0; l < 32; l++) {
        u32 g = l / 4, t = l % 4, *q = in + l * 8;
        q[0] = m->a[g][2 * t] | (u32)m->a[g][2 * t + 1] << 16;
        q[1] = m->a[g + 8][2 * t] | (u32)m->a[g + 8][2 * t + 1] << 16;
        q[2] = m->a[g][2 * t + 8] | (u32)m->a[g][2 * t + 9] << 16;
        q[3] = m->a[g + 8][2 * t + 8] | (u32)m->a[g + 8][2 * t + 9] << 16;
        q[4] = m->b[2 * t][g] | (u32)m->b[2 * t + 1][g] << 16;
        q[5] = m->b[2 * t + 8][g] | (u32)m->b[2 * t + 9][g] << 16;
        q[6] = m->c[g][2 * t] | (u32)m->c[g][2 * t + 1] << 16;
        q[7] = m->c[g + 8][2 * t] | (u32)m->c[g + 8][2 * t + 1] << 16;
    }
}
/* the program's 32 lanes on the evaluator (out: HMMA_OUT words a lane), translated as the flags are, its
   PackHalf2x16 flushing f16 subnormals where flush is set: 0 when the translation or the evaluator fails */
static int hmma_run(const u8* code, u32 n, const u32* in, u32* out, const char* dir, const char* keep, int flush) {
    ShaderInfo si = {0};
    si.cs_local[0] = 32, si.cs_local[1] = si.cs_local[2] = 1;
    si.shared_size = (int)(32 * HMMA_OUT * 4);
    size_t count = 0;
    u32* spv = sm86_to_spirv(code, n * 16, ST_CS, NULL, &si, &count);
    if (!spv) {
        fprintf(stderr, "warp matrix values: not translated: %s\n", si.first_unsupported);
        return 0;
    }
    if (keep) CHECK(keep_module(dir, keep, spv, count));
    static u32 bank1[256];
    memcpy(bank1, in, 32 * 8 * 4);
    static SpvEvalEnv env[32];
    memset(env, 0, sizeof env);
    for (u32 l = 0; l < 32; l++) {
        env[l].ubo[SNX_VKB_CB + 1] = bank1;
        env[l].ubo_words[SNX_VKB_CB + 1] = 256;
        env[l].builtin[27][0] = env[l].builtin[29][0] = env[l].builtin[41][0] = l; /* LocalInvocationId.x, Index */
        env[l].builtin[24][0] = env[l].builtin[24][1] = env[l].builtin[24][2] = 1; /* NumWorkgroups */
        env[l].half_flush = flush;
    }
    const char* why = spv_eval_subgroup(spv, count, env, 32);
    free(spv);
    if (why || env[0].shared_words < 32 * HMMA_OUT) {
        fprintf(stderr, "warp matrix values: the evaluator stops at %s (%u shared words)\n", why ? why : "nothing",
                env[0].shared_words);
        return 0;
    }
    memcpy(out, env[0].shared, 32 * HMMA_OUT * 4);
    return 1;
}
/* What output word w's half j (0 for an f32) of lane l should be: the sum of its products and C (where C is the
   input's, or twice the sum where C is the device's own D of the same products), in double. *tol is how far an f32
   sum of n terms may be, n + 1 roundings of their magnitudes (and an f16 D's own rounding); *kind 0 a finite value, 1
   +inf, 2 -inf, 3 NaN, by IEEE's rules for a sum of those terms in any order; *zero_sign the sign of an all-zero sum
   (-0 only where every term is -0), else -1. With packed (the hmma_f16 setting's two half sums of the even and the
   odd k, each rounded in f16 once a product), *tol adds n / 2 roundings of 2^-11 of the products' magnitudes and
   2^-25 each below the normals, and *overflow says whether a half's magnitudes reach 65504, where a product or a
   partial sum can be an infinity */
static double hmma_expect(const HmmaMats* m, u32 l, u32 w, u32 j, int packed, double* tol, int* kind, int* zero_sign,
                          int* overflow) {
    u32 at = 0, k = 0;
    for (; k < 8; k++) {
        u32 words = hmma_cases[k][2] & 0x10 ? 4u : 2u;
        if (w < at + words) break;
        at += words;
    }
    u32 v = hmma_cases[k][2], f32 = v >> 4 & 1, kk = v & 8 ? 16 : 8, e = w - at, g = l / 4, t = l % 4;
    u32 row = f32 ? g + 8 * (e >> 1) : g + 8 * e, col = 2 * t + (f32 ? e & 1 : j);
    int twice = hmma_cases[k][1] != 255 && hmma_cases[k][1] != 14, has_c = hmma_cases[k][1] == 14;
    double sum = 0, mag = 0, half_mag[2] = {0, 0};
    int pos_inf = 0, neg_inf = 0, nan = 0, all_neg_zero = 1, all_zero = 1;
    for (u32 i = 0; i <= kk; i++) {
        double term;
        if (i < kk)
            term = hmma_half(m->a[row][i]) * hmma_half(m->b[i][col]);
        else if (has_c)
            term = hmma_half(m->c[row][col]);
        else
            term = 0; /* RZ: +0 */
        if (i < kk && isfinite(term)) half_mag[i & 1] += fabs(term);
        if (twice && i < kk) term *= 2;
        nan |= isnan(term), pos_inf |= term == INFINITY, neg_inf |= term == -INFINITY;
        all_zero &= term == 0, all_neg_zero &= term == 0 && signbit(term);
        if (isfinite(term)) sum += term, mag += fabs(term);
    }
    *kind = nan || (pos_inf && neg_inf) ? 3 : pos_inf ? 1 : neg_inf ? 2 : 0;
    *zero_sign = all_zero ? all_neg_zero : -1;
    *tol = ldexp((double)kk + 2, -24) * mag * (twice ? 3 : 1) + 1e-30;
    *overflow = 0;
    if (packed) {
        double half = ldexp((double)kk / 2, -11) * (half_mag[0] + half_mag[1]) + ldexp((double)kk, -25);
        *tol += half * (twice ? 3 : 1);
        *overflow = (half_mag[0] > half_mag[1] ? half_mag[0] : half_mag[1]) * (1 + ldexp(1, -8)) >= 65504.0;
    }
    if (!f32) *tol += ldexp(fabs(sum), -10) + ldexp(1, -24);
    return sum;
}
/* whether half j of word got (an f32 word: j 0, its bits) is what hmma_expect says: anything not finite too where its
   half sums can overflow */
static int hmma_match(u32 got, int f32, u32 j, double want, double tol, int kind, int zero_sign, int bits,
                      int overflow) {
    double v;
    if (f32)
        v = (double)u2f(got);
    else
        v = hmma_half(got >> 16 * j & 0xffffu);
    if (overflow && !isfinite(v)) return 1;
    if (kind == 3) return isnan(v);
    if (kind) return v == (kind == 1 ? INFINITY : -INFINITY);
    if (!f32 && fabs(want) >= 65520.0 - tol) return isinf(v) || fabs(v - want) <= tol; /* an f16 D overflows */
    if (zero_sign >= 0) return v == 0 && (int)(signbit(v) != 0) == zero_sign;
    if (bits) return v == want;
    return fabs(v - want) <= tol;
}
/* HMMA (16816 and 1688, F16 and F32 D, C from RZ, an f16 input or an earlier D) on the evaluator's 32 lanes, as each
   lane gathers B, as pairs of lanes share it (g_shader_hmma_pairs) and through a warp's slot of shared memory
   (g_shader_hmma_stage), with and without the quads and the mixed dot product, and without it in half sums
   (g_shader_hmma_f16): on the exact set every output is the sum's bits; on the rounded one within an f32 sum's error,
   or the half sums' (hmma_expect); on the special and zeros ones infinities, NaNs and the sign of zero follow IEEE's
   sum of the terms in any order, but where the half sums can overflow. And the forms agree with each other: bit for
   bit on the exact set, the same kinds on the others */
static void test_warp_matrix_values(const char* dir) {
    u8 code[16 * 48] = {0};
    u32 n = hmma_program(code);
    int quad_was = g_shader_quad_compute, mixed_was = g_shader_mixed_dot, f16_was = g_shader_float16,
        pairs_was = g_shader_hmma_pairs, stage_was = g_shader_hmma_stage, half_was = g_shader_hmma_f16;
    static HmmaMats mats;
    static u32 in[32 * 8], out[3][32 * HMMA_OUT];
    for (int set = HM_EXACT; set <= HM_ZEROS; set++) {
        hmma_inputs(&mats, in, set);
        for (int quad = 0; quad < 2; quad++)
            for (int mixed = 0; mixed < 2; mixed++)
                for (int packed = 0; packed < 2 - mixed; packed++) { /* the mixed dot product takes no half sums */
                    /* the forms: each lane's gather, pairs of lanes, staged */
                    for (int form = 0; form < 3; form++) {
                        g_shader_quad_compute = quad, g_shader_mixed_dot = mixed, g_shader_float16 = 1;
                        g_shader_hmma_pairs = form == 1, g_shader_hmma_stage = form == 2 ? 65536 : 0;
                        g_shader_hmma_f16 = packed;
                        char keep[32];
                        snprintf(keep, sizeof keep, "warp_values%d%d%d%d", quad, mixed, form, packed);
                        int ran = hmma_run(code, n, in, out[form], dir, set == HM_EXACT ? keep : NULL, 0);
                        g_shader_quad_compute = quad_was, g_shader_mixed_dot = mixed_was, g_shader_float16 = f16_was;
                        g_shader_hmma_pairs = pairs_was, g_shader_hmma_stage = stage_was;
                        g_shader_hmma_f16 = half_was;
                        CHECK(ran);
                        if (!ran) return;
                        u32 bad = 0;
                        for (u32 l = 0; l < 32; l++)
                            for (u32 w = 0; w < HMMA_OUT; w++) {
                                u32 got = out[form][l * HMMA_OUT + w], at = 0, k = 0;
                                for (; k < 8 && w >= at + (hmma_cases[k][2] & 0x10 ? 4u : 2u); k++)
                                    at += hmma_cases[k][2] & 0x10 ? 4u : 2u;
                                int f32 = hmma_cases[k][2] >> 4 & 1;
                                for (u32 j = 0; j < (f32 ? 1u : 2u); j++) {
                                    double tol;
                                    int kind, zero_sign, overflow;
                                    double want =
                                        hmma_expect(&mats, l, w, j, packed, &tol, &kind, &zero_sign, &overflow);
                                    if (hmma_match(got, f32, j, want, tol, kind, zero_sign, set == HM_EXACT, overflow))
                                        continue;
                                    if (bad++ < 8)
                                        fprintf(stderr,
                                                "warp matrix values (set %d, quad %d, mixed %d, form %d, half sums "
                                                "%d): lane %u word %u half %u is 0x%08x, expected %.9g within %.3g "
                                                "(kind %d, zero sign %d)\n",
                                                set, quad, mixed, form, packed, l, w, j, got, want, tol, kind,
                                                zero_sign);
                                }
                            }
                        CHECK(bad == 0);
                    }
                    /* the three forms: the same bits where the sums are exact, else the same kinds and values within
                       their bounds (hmma_match above) */
                    u32 differ = 0;
                    for (u32 i = 0; i < 32 * HMMA_OUT; i++)
                        differ += (out[0][i] != out[1][i]) + (out[0][i] != out[2][i]);
                    if (set == HM_EXACT) CHECK(differ == 0);
                    if (set == HM_EXACT && differ)
                        fprintf(stderr,
                                "warp matrix values (quad %d, mixed %d, half sums %d): %u words differ between the "
                                "forms\n",
                                quad, mixed, packed, differ);
                }
    }
}

/* HMMA's f16 D where the device flushes f16 subnormals (the evaluator's half_flush, as Turnip on an Adreno 610 does in
   PackHalf2x16): with g_shader_half_flush the halves below 2^-14 are packed by hand and every output is what
   hmma_expect says; packed by PackHalf2x16 alone, some of the rounded set's are lost, which shows the set reaches them */
static void test_warp_matrix_flush(const char* dir) {
    u8 code[16 * 48] = {0};
    u32 n = hmma_program(code);
    int flush_was = g_shader_half_flush, f16_was = g_shader_float16;
    static HmmaMats mats;
    static u32 in[32 * 8], out[32 * HMMA_OUT];
    hmma_inputs(&mats, in, HM_ROUNDED);
    for (int exact = 0; exact < 2; exact++) {
        g_shader_half_flush = exact, g_shader_float16 = 1;
        int ran = hmma_run(code, n, in, out, dir, exact ? "warp_flush" : NULL, 1);
        g_shader_half_flush = flush_was, g_shader_float16 = f16_was;
        CHECK(ran);
        if (!ran) return;
        u32 bad = 0, small = 0;
        for (u32 l = 0; l < 32; l++)
            for (u32 w = 0; w < HMMA_OUT; w++) {
                u32 got = out[l * HMMA_OUT + w], at = 0, k = 0;
                for (; k < 8 && w >= at + (hmma_cases[k][2] & 0x10 ? 4u : 2u); k++)
                    at += hmma_cases[k][2] & 0x10 ? 4u : 2u;
                int f32 = hmma_cases[k][2] >> 4 & 1;
                for (u32 j = 0; j < (f32 ? 1u : 2u); j++) {
                    double tol;
                    int kind, zero_sign, overflow;
                    double want = hmma_expect(&mats, l, w, j, 0, &tol, &kind, &zero_sign, &overflow);
                    small += !f32 && kind == 0 && want != 0 && fabs(want) < 0x1p-14;
                    if (hmma_match(got, f32, j, want, tol, kind, zero_sign, 0, overflow)) continue;
                    if (exact && bad < 8)
                        fprintf(stderr,
                                "warp matrix, f16 subnormals flushed: lane %u word %u half %u is 0x%08x, "
                                "expected %.9g within %.3g\n",
                                l, w, j, got, want, tol);
                    bad++;
                }
            }
        if (!exact && !bad)
            fprintf(stderr,
                    "warp matrix, f16 subnormals flushed: PackHalf2x16 alone lost none of %u halves below "
                    "2^-14\n",
                    small);
        CHECK(exact ? bad == 0 : bad > 0);
    }
}

/* the way the log says a device's compute HMMAs take (shader_hmma_way), from the flags device.c sets */
static void test_hmma_way(void) {
    int quad_was = g_shader_quad_compute, mixed_was = g_shader_mixed_dot, f16_was = g_shader_float16,
        pairs_was = g_shader_hmma_pairs, stage_was = g_shader_hmma_stage, coop_was = g_shader_hmma_coop,
        half_was = g_shader_hmma_f16;
    static const struct {
        int quad, mixed, pairs, stage, coop, half;
        const char* want;
    } cases[] = {
        {1, 1, 1, 0, 0, 0,
         "A by quad broadcasts, B shared by pairs of lanes, the products by the mixed dot product into f32"},
        {1, 0, 0, 0, 0, 0, "A by quad broadcasts, B by shuffles, the products unpacked to f32"},
        {0, 0, 0, 65536, 0, 0, "A and B through a warp's slot of shared memory, the products unpacked to f32"},
        {1, 1, 0, 0, 65536, 0,
         "the device's cooperative matrices where a program's blocks are whole warps and their "
         "fragments fit in shared memory, else A by quad broadcasts, B by shuffles, the products by the "
         "mixed dot product into f32"},
        /* hmma_f16's half sums, where the mixed dot product does not take the products first */
        {0, 0, 0, 65536, 0, 1,
         "A and B through a warp's slot of shared memory, the products summed in half floats (hmma_f16), then in f32"},
        {1, 1, 1, 0, 0, 1,
         "A by quad broadcasts, B shared by pairs of lanes, the products by the mixed dot product into f32"},
    };
    for (size_t k = 0; k < sizeof cases / sizeof *cases; k++) {
        g_shader_quad_compute = cases[k].quad, g_shader_mixed_dot = cases[k].mixed, g_shader_float16 = 1;
        g_shader_hmma_pairs = cases[k].pairs, g_shader_hmma_stage = cases[k].stage, g_shader_hmma_coop = cases[k].coop;
        g_shader_hmma_f16 = cases[k].half;
        char way[256];
        shader_hmma_way(way, sizeof way);
        if (strcmp(way, cases[k].want)) fprintf(stderr, "HMMA way %zu: \"%s\", wanted \"%s\"\n", k, way, cases[k].want);
        CHECK(!strcmp(way, cases[k].want));
    }
    g_shader_quad_compute = quad_was, g_shader_mixed_dot = mixed_was, g_shader_float16 = f16_was;
    g_shader_hmma_pairs = pairs_was, g_shader_hmma_stage = stage_was, g_shader_hmma_coop = coop_was;
    g_shader_hmma_f16 = half_was;
}

/* the HMMA forms vk/device.c chooses (shader_hmma_choose) for the devices measured: the Adreno 610 on Turnip, whose
   shuffles at a lane's own index loop (the OpGroupNonUniformShuffle line of extension_bans.txt holds there, as
   tests/vk_extension_bans.c checks), stages A and B; the Adreno 830 on its own driver keeps each lane's shuffles;
   RADV with subgroups of 32 shares B by pairs of lanes, its quads in compute or not: missing quads are no reason to
   stage, as RADV staged ran 2.2 times slower */
static void test_hmma_choose(void) {
    int quad_was = g_shader_quad_compute, mixed_was = g_shader_mixed_dot, f16_was = g_shader_float16,
        pairs_was = g_shader_hmma_pairs, stage_was = g_shader_hmma_stage, coop_was = g_shader_hmma_coop;
    static const struct {
        const char* device;
        int quad, mixed, radv_warp32, shuffle_loops;
        const char* want;
    } cases[] = {
        {"Adreno 610, Turnip", 0, 0, 0, 1,
         "A and B through a warp's slot of shared memory, the products unpacked to f32"},
        {"Adreno 830, Qualcomm's driver", 1, 0, 0, 0,
         "A by quad broadcasts, B by shuffles, the products unpacked to f32"},
        {"RX 6700 XT, RADV", 1, 1, 1, 0,
         "A by quad broadcasts, B shared by pairs of lanes, the products by the mixed dot product into f32"},
        {"RX 6700 XT, RADV, no quads in compute", 0, 1, 1, 0,
         "A by shuffles, B shared by pairs of lanes, the products by the mixed dot product into f32"},
    };
    for (size_t k = 0; k < sizeof cases / sizeof *cases; k++) {
        g_shader_quad_compute = cases[k].quad, g_shader_mixed_dot = cases[k].mixed, g_shader_float16 = 1;
        g_shader_hmma_coop = 0;
        shader_hmma_choose(1, cases[k].radv_warp32, cases[k].shuffle_loops, 65536);
        char way[256];
        shader_hmma_way(way, sizeof way);
        if (strcmp(way, cases[k].want))
            fprintf(stderr, "HMMA on the %s: \"%s\", wanted \"%s\"\n", cases[k].device, way, cases[k].want);
        CHECK(!strcmp(way, cases[k].want));
        CHECK(g_shader_hmma_stage == (cases[k].shuffle_loops ? 65536 : 0));
    }
    shader_hmma_choose(0, 1, 1, 65536); /* no subgroup operations in compute: no form of hmma's runs */
    CHECK(!g_shader_hmma_stage && !g_shader_hmma_pairs);
    g_shader_quad_compute = quad_was, g_shader_mixed_dot = mixed_was, g_shader_float16 = f16_was;
    g_shader_hmma_pairs = pairs_was, g_shader_hmma_stage = stage_was, g_shader_hmma_coop = coop_was;
}

/* hmma_f16 asked for where the device keeps HMMA's f32 sum: the reason the log gives, which names what the device has
   or lacks, and none where the half sums are taken or not asked for */
static void test_hmma_f16_unused(void) {
    int half_was = g_shader_hmma_f16, float16_was = g_shader_float16, mixed_was = g_shader_mixed_dot;
    g_shader_hmma_f16 = 0, g_shader_float16 = 1, g_shader_mixed_dot = 1;
    CHECK(shader_hmma_f16_unused() == NULL);
    g_shader_hmma_f16 = 1;
    const char* why = shader_hmma_f16_unused();
    CHECK(why && strstr(why, "VK_VALVE_shader_mixed_float_dot_product"));
    g_shader_mixed_dot = 0;
    CHECK(shader_hmma_f16_unused() == NULL);
    g_shader_float16 = 0;
    why = shader_hmma_f16_unused();
    CHECK(why && strstr(why, "shaderFloat16"));
    g_shader_hmma_f16 = half_was, g_shader_float16 = float16_was, g_shader_mixed_dot = mixed_was;
}

/* the memory model a module declares (OpMemoryModel's: 1 GLSL450, 3 Vulkan), whether it declares a capability, and
   how many OpDecorates of a decoration it has */
static u32 spv_memory_model(const u32* words, size_t count) {
    for (size_t i = 5; i < count && words[i] >> 16; i += words[i] >> 16)
        if ((words[i] & 0xffff) == 14 && words[i] >> 16 == 3) return words[i + 2];
    return 0;
}
static int spv_has_capability(const u32* words, size_t count, u32 capability) {
    for (size_t i = 5; i < count && words[i] >> 16; i += words[i] >> 16)
        if ((words[i] & 0xffff) == 17 && words[i + 1] == capability) return 1;
    return 0;
}
static u32 spv_decorations(const u32* words, size_t count, u32 decoration) {
    u32 found = 0;
    for (size_t i = 5; i < count && words[i] >> 16; i += words[i] >> 16)
        found += (words[i] & 0xffff) == 71 && words[i] >> 16 >= 3 && words[i + 2] == decoration;
    return found;
}
/* HMMA on the device's cooperative matrices (g_shader_hmma_coop), in a program that also reads and writes global
   memory (LDG, STG), stores to shared memory (STS, a byte's by two atomics), adds there (ATOMS), waits (MEMBAR.GPU,
   BAR.SYNC) and reads it (LDSM), whose barriers and atomics the Vulkan memory model takes too: each HMMA one
   OpCooperativeMatrixMulAddKHR of two matrices loaded and one stored, with no dot products and no B gathers (LDSM's
   4 shuffles and MOVM's 2 stay), in a module of the Vulkan memory model, whose global buffer is not Coherent but read
   and written by atomics. With the flag off, or a block of 16 lanes, too little shared memory left for the slots, no
   shaderFloat16, the program is hmma's as before; and a program without HMMA stays GLSL450's with the flag on */
static void test_warp_matrix_coop(const char* dir) {
    u8 code[16 * 20] = {0};
    u32 n = 0;
    Word x = fix(0x919); /* S2R R0, TID.X */
    set(&x, 72, 8, 0x21);
    put(code, n++, x);
    put(code, n++, uldc64(0x20));
    x = fix(0x981); /* LDG.E R8, [R0.U32 + UR4] */
    set(&x, 16, 8, 8);
    set(&x, 24, 8, 0);
    set(&x, 32, 6, 4);
    set(&x, 72, 1, 1);
    set(&x, 73, 3, 4);
    set(&x, 81, 3, 7);
    set(&x, 91, 1, 1);
    put(code, n++, x);
    x = fix(0x388); /* STS [R0 + 0x40], R8 */
    set(&x, 24, 8, 0);
    set(&x, 32, 8, 8);
    set(&x, 40, 24, 0x40);
    set(&x, 73, 3, 4);
    put(code, n++, x);
    x = fix(0x388); /* STS.U8 [R0 + 0x60], R8 */
    set(&x, 24, 8, 0);
    set(&x, 32, 8, 8);
    set(&x, 40, 24, 0x60);
    put(code, n++, x);
    x = fix(0x38c); /* ATOMS.ADD R50, [R0 + 0x80], R8 */
    set(&x, 16, 8, 50);
    set(&x, 24, 8, 0);
    set(&x, 32, 8, 8);
    set(&x, 40, 24, 0x80);
    put(code, n++, x);
    x = fix(0x992); /* MEMBAR.SC.GPU */
    set(&x, 76, 3, 2);
    put(code, n++, x);
    put(code, n++, fix(0xb1d)); /* BAR.SYNC 0 */
    static const u32 hmma[4][3] = {{16, 14, 0x08}, {20, 24, 0x18}, {28, 255, 0x00}, {32, 36, 0x10}};
    for (u32 k = 0; k < 4; k++) { /* HMMA dst, R8, R12, C */
        x = fix(0x23c);
        set(&x, 16, 8, hmma[k][0]);
        set(&x, 24, 8, 8);
        set(&x, 32, 8, 12);
        set(&x, 64, 8, hmma[k][1]);
        set(&x, 72, 19, hmma[k][2]);
        put(code, n++, x);
    }
    x = fix(0x83b); /* LDSM.16.M88.4 R40, [R7 + 0x40] */
    set(&x, 16, 8, 40);
    set(&x, 24, 8, 7);
    set(&x, 40, 24, 0x40);
    set(&x, 72, 2, 2);
    put(code, n++, x);
    x = fix(0x23a); /* MOVM.16.MT88 R44, R8 */
    set(&x, 16, 8, 44);
    set(&x, 24, 8, 8);
    put(code, n++, x);
    x = fix(0x986); /* STG.E [R0.U32 + UR4 + 0x100], R16 */
    set(&x, 24, 8, 0);
    set(&x, 32, 8, 16);
    set(&x, 40, 24, 0x100);
    set(&x, 64, 6, 4);
    set(&x, 72, 1, 1);
    set(&x, 73, 3, 4);
    set(&x, 91, 1, 1);
    put(code, n++, x);
    put(code, n++, exit_insn());
    put(code, n, bra(n, n, 7, 0));
    n++;
    int quad_was = g_shader_quad_compute, mixed_was = g_shader_mixed_dot, coop_was = g_shader_hmma_coop,
        f16_was = g_shader_float16;
    /* the translation as each flag and block: 1 when it took the cooperative matrices, 0 when hmma's, -1 failed */
    for (int coop = 0; coop < 2; coop++)
        for (int quad = 0; quad < 2; quad++)
            for (int mixed = 0; mixed < 2; mixed++) {
                g_shader_quad_compute = quad, g_shader_mixed_dot = mixed, g_shader_hmma_coop = coop ? 65536 : 0;
                ShaderInfo si = {0};
                si.cs_local[0] = 32, si.cs_local[1] = 2, si.cs_local[2] = 1;
                si.shared_size = 49152;
                size_t count = 0;
                u32* spv = sm86_to_spirv(code, n * 16, ST_CS, NULL, &si, &count);
                CHECK(spv != NULL);
                if (!spv) {
                    fprintf(stderr, "warp matrix coop: %s\n", si.first_unsupported);
                    continue;
                }
                u32 products = spv_count(spv, count, 4459), loads = spv_count(spv, count, 4457),
                    stores = spv_count(spv, count, 4458), shuffles = spv_count(spv, count, 345);
                u32 dots = spv_count(spv, count, 6916) + spv_count(spv, count, 148);
                u32 model = spv_memory_model(spv, count), coherent = spv_decorations(spv, count, 23);
                u32 atomic_rw = spv_count(spv, count, 227) + spv_count(spv, count, 228);
                if (coop) {
                    CHECK(products == 4 && loads == 8 && stores == 4 && dots == 0 && shuffles == 6);
                    CHECK(model == 3 && coherent == 0 && atomic_rw == 2);
                    CHECK(spv_has_capability(spv, count, 6022) && spv_has_capability(spv, count, 5345) &&
                          spv_has_capability(spv, count, 5346));
                } else {
                    CHECK(products == 0 && loads == 0 && dots == 96 && model == 1 && coherent == 1 && atomic_rw == 0);
                }
                if (coop ? products != 4 || model != 3 || coherent || atomic_rw != 2 : products || model != 1)
                    fprintf(stderr,
                            "warp matrix coop %d, quad %d, mixed %d: %u products, %u loads, %u stores, %u "
                            "dots, %u shuffles, memory model %u, %u Coherent, %u atomic loads and stores\n",
                            coop, quad, mixed, products, loads, stores, dots, shuffles, model, coherent, atomic_rw);
                char name[32];
                snprintf(name, sizeof name, "warp_matrix_coop%d%d%d", coop, quad, mixed);
                CHECK(keep_module(dir, name, spv, count));
                free(spv);
            }
    /* where the cooperative matrices don't fit: a block of 16 lanes, 64 KiB of shared memory but 1 KiB, which the
       two slots' 3.5 KiB don't fit in, and no shaderFloat16 */
    g_shader_hmma_coop = 65536, g_shader_quad_compute = 1, g_shader_mixed_dot = 0;
    for (int k = 0; k < 3; k++) {
        ShaderInfo si = {0};
        si.cs_local[0] = k == 0 ? 16 : 32, si.cs_local[1] = k == 0 ? 4 : 2, si.cs_local[2] = 1;
        si.shared_size = k == 1 ? 65536 - 1024 : 49152;
        g_shader_float16 = k != 2;
        size_t count = 0;
        u32* spv = sm86_to_spirv(code, n * 16, ST_CS, NULL, &si, &count);
        CHECK(spv && spv_count(spv, count, 4459) == 0 && spv_memory_model(spv, count) == 1 &&
              spv_decorations(spv, count, 23) == 1);
        free(spv);
    }
    g_shader_float16 = f16_was;
    {
        u8 plain[16 * 8] = {0};
        u32 m = compute_program(plain);
        ShaderInfo si = {0};
        si.cs_local[0] = 32, si.cs_local[1] = si.cs_local[2] = 1;
        size_t count = 0;
        u32* spv = sm86_to_spirv(plain, m * 16, ST_CS, NULL, &si, &count);
        CHECK(spv && spv_memory_model(spv, count) == 1 && !spv_has_capability(spv, count, 5345));
        free(spv);
    }
    g_shader_quad_compute = quad_was, g_shader_mixed_dot = mixed_was, g_shader_hmma_coop = coop_was;
}

int main(int argc, char** argv) {
    const char* dir = argc > 1 ? argv[1] : ".";
    test_alu_forms();
    test_common_fields();
    test_flow();
    test_table();
    test_sph();
    test_tcs_empty();
    test_programs(dir);
    test_game_forms(dir);
    test_brx(dir);
    test_brx_bounded(dir);
    test_alu_group(dir);
    test_compute_forms(dir);
    test_image_kinds(dir);
    test_kill_discard();
    test_root_tables(dir);
    test_texel_offsets(dir);
    test_minmax_samples(dir);
    test_global_flow(dir);
    test_fswzadd(dir);
    test_lane_masks(dir);
    test_shfl_operands(dir);
    test_ldc_small(dir);
    test_break(dir);
    test_local_memory(dir);
    test_shared_no_uniform(dir);
    test_hsetp2(dir);
    test_bmov(dir);
    test_debug_prologue(dir);
    test_legacy_varyings(dir);
    test_brxu(dir);
    test_ipa_uniform(dir);
    test_tld_multisample(dir);
    test_resolution_scale(dir);
    test_tex_f16(dir);
    test_output_defaults(dir);
    test_fragment_depth(dir);
    test_float64_fallback_scale(dir);
    test_flow_operand(dir);
    test_global_pointer(dir);
    test_wide_carries(dir);
    test_instance_id(dir);
    test_u64_conversions();
    test_global_indirect(dir);
    test_global_table(dir);
    test_global_table_cells(dir);
    test_f2ip(dir);
    test_uldc_small(dir);
    test_shfl_creg(dir);
    test_shading_rate(dir);
    test_atomic_fadd(dir);
    test_global_two_bases(dir);
    test_global_atomic_same_register(dir);
    test_uniform_handle(dir);
    test_memory_eval();
    test_uldc_word_key(dir);
    test_mesh(dir);
    test_task(dir);
    test_mesh_after_task(dir);
    test_mesh_output_bytes();
    test_mesh_refused();
    test_fragment_per_primitive(dir);
    test_fragment_unfed(dir);
    test_uses_isbe();
    test_warp_matrix(dir);
    test_warp_matrix_values(dir);
    test_warp_matrix_flush(dir);
    test_hmma_way();
    test_hmma_choose();
    test_hmma_f16_unused();
    test_warp_matrix_coop(dir);
    test_ftz_modes(dir);
    if (failures) {
        fprintf(stderr, "%d checks failed\n", failures);
        return 1;
    }
    printf("sm86: all checks pass\n");
    return 0;
}
