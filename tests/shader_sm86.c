/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The SM86 frontend (src/nx/gpu/shader/sm86.h): the decoder against instruction words that a small encoder here
   makes by the rules of Mesa's NAK encoder (sm70_encode.rs: set_opcode, encode_alu_base, set_pred, set_instr_deps,
   set_rel_offset) and its SPH (sph.rs), the search for programs in shader memory, and the translation of small
   programs, which spirv-val checks when the test gets its path. No word here comes from a game. */
#include "nx/gpu/shader/sm86.h"
#include "common/utilities/endian.h"
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
    /* each row decodes to itself, in every form it takes, and no 12-bit code is also an ALU form */
    for (int op = 1; op < SM86__COUNT; op++) {
        const Sm86OpInfo* info = &sm86_ops[op];
        for (u32 form = 0; form < 8; form++) {
            if (info->kind == SM86_K_FIX && form) break;
            if (info->kind != SM86_K_FIX && !(info->forms & (1u << form))) continue;
            Word x = info->kind == SM86_K_FIX ? fix(info->code) : alu(info->code, form, 1, 2);
            if (info->kind == SM86_K_UALU) set(&x, 91, 1, 1);
            Sm86Insn in = dec(x);
            if (in.op != op) fprintf(stderr, "%s form %u decodes as %s\n", info->name, form, sm86_ops[in.op].name);
            CHECK(in.op == op);
        }
    }
    for (u32 code = 0; code < 4096; code++) {
        int fixed = 0, as_alu = 0;
        for (int op = 1; op < SM86__COUNT; op++) {
            const Sm86OpInfo* info = &sm86_ops[op];
            if (info->kind == SM86_K_FIX && info->code == code) fixed++;
            if (info->kind != SM86_K_FIX && info->code == (code & 0x1ff) && (info->forms & (1u << (code >> 9))))
                as_alu++;
        }
        if (fixed + as_alu > 1) fprintf(stderr, "code 0x%03x matches %d rows\n", code, fixed + as_alu);
        CHECK(fixed + as_alu <= 1);
    }
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

static int validate(const char* spirv_val, const char* dir, const char* name, const u32* words, size_t count) {
    if (!spirv_val) return 1;
    char path[1024], command[2600];
    snprintf(path, sizeof path, "%s/sm86_%s.spv", dir, name);
    FILE* f = fopen(path, "wb");
    if (!f) return 0;
    int ok = fwrite(words, 4, count, f) == count;
    ok &= !fclose(f);
    snprintf(command, sizeof command, "\"%s\" --target-env vulkan1.3 \"%s\"", spirv_val, path);
    ok &= system(command) == 0;
    if (!ok) fprintf(stderr, "spirv-val fails on %s\n", path);
    return ok;
}

static void test_programs(const char* spirv_val, const char* dir) {
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
    if (spv) CHECK(validate(spirv_val, dir, "cs", spv, count));
    free(spv);
    si = (ShaderInfo){0};
    spv = sm86_to_spirv(mem + at + SM86_SPH_BYTES, vs * 16, ST_VS, sph, &si, &count);
    CHECK(spv != NULL);
    if (!spv) fprintf(stderr, "vertex: %s\n", si.first_unsupported);
    CHECK(si.in_mask == 1);
    if (spv) CHECK(validate(spirv_val, dir, "vs", spv, count));
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
    if (spv) CHECK(validate(spirv_val, dir, "fs", spv, count));
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
static void test_game_forms(const char* spirv_val, const char* dir) {
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
    if (spv) CHECK(validate(spirv_val, dir, "fs_game", spv, count));
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
    if (spv) CHECK(validate(spirv_val, dir, "vs_rate", spv, count));
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
    if (spv) CHECK(validate(spirv_val, dir, "fs_call", spv, count));
    free(spv);
    free(fs);
}

/* the jump table of the BRX test: c[1][0x10] on, the targets' byte addresses, then a word that is no target */
static int table_read(u32 idx, u32 off, u32* out) {
    static const u32 table[3] = {2 * 16, 4 * 16, 0xffffffffu};
    if (idx != 1 || off < 0x10 || off >= 0x10 + sizeof table) return 0;
    *out = table[(off - 0x10) / 4];
    return 1;
}
/* LDC R2, c[1][R1 + 0x10]; BRX R2, -(the next instruction's address); two cases that write R0 and exit */
static void test_brx(const char* spirv_val, const char* dir) {
    u8 code[16 * 8] = {0};
    Word x = fix(0xb82);
    set(&x, 16, 8, 2);
    set(&x, 24, 8, 1);
    cbuf(&x, 1, 0x10);
    set(&x, 73, 3, 4);
    put(code, 0, x);
    x = fix(0x949);
    set(&x, 24, 8, 2);
    set(&x, 33, 49, (u64)0 - 8); /* -32 bytes in 4-byte units */
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
    if (spv) CHECK(validate(spirv_val, dir, "fs_brx", spv, count));
    free(spv);
}

/* the uniform ALU group and the other forms of the second game run, translated and validated */
static void test_alu_group(const char* spirv_val, const char* dir) {
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
    if (spv) CHECK(validate(spirv_val, dir, "fs_alu", spv, count));
    free(spv);
}

/* the forms of the fourth game run in a compute program: SHFL.BFLY in a segment of 4 (c 0x1c1f), SHFL.IDX with
   the lane in a register, S2R of LTMASK (0x39), MEMBAR, HADD2.F32 and PLOP3 with a uniform predicate */
static void test_compute_forms(const char* spirv_val, const char* dir) {
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
    if (spv) CHECK(validate(spirv_val, dir, "cs_forms", spv, count));
    free(spv);
}

/* SUST.P to a 2D image with its handle in c[0][0x30], and SULD.P from it, by the kind of the renderer's key: the
   bindless rule of spirv_translate.c gives 2 (unsigned) without bit 8 of rt_int_mask, 0 (float) with it, and 3
   (signed) with bit 8 of rt_sint_mask. ShaderInfo.img[].type is the dimension code, 3 for 2D */
static void test_image_kinds(const char* spirv_val, const char* dir) {
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
        if (k == 1) si.rt_sint_mask = 1u << 8;
        if (k == 2) si.rt_int_mask = 1u << 8;
        size_t count = 0;
        u32* spv = sm86_to_spirv(code, 4 * 16, ST_CS, NULL, &si, &count);
        CHECK(spv != NULL);
        if (!spv) fprintf(stderr, "image kind: %s\n", si.first_unsupported);
        CHECK(si.nimg == 1 && si.img[0].kind == kinds[k] && si.img[0].type == 3 && si.img[0].cbuf_off == 0x30 / 4);
        if (spv) CHECK(validate(spirv_val, dir, names[k], spv, count));
        free(spv);
    }
}

/* the root tables, banks 24 to 31, as one uniform buffer: R0 = TID.X; UR4:UR5 = c[0][0x20];
   R2 = c[25][0x10]; [R0 + UR4] = R2. Table 1's word 4 is byte 0x110 of the buffer */
static void test_root_tables(const char* spirv_val, const char* dir) {
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
    if (spv) CHECK(validate(spirv_val, dir, "root", spv, count));
    free(spv);
}

int main(int argc, char** argv) {
    const char* spirv_val = argc > 2 ? argv[2] : NULL;
    const char* dir = argc > 1 ? argv[1] : ".";
    test_alu_forms();
    test_common_fields();
    test_flow();
    test_table();
    test_sph();
    test_programs(spirv_val, dir);
    test_game_forms(spirv_val, dir);
    test_brx(spirv_val, dir);
    test_alu_group(spirv_val, dir);
    test_compute_forms(spirv_val, dir);
    test_image_kinds(spirv_val, dir);
    test_root_tables(spirv_val, dir);
    if (failures) {
        fprintf(stderr, "%d checks failed\n", failures);
        return 1;
    }
    printf("sm86: all checks pass\n");
    return 0;
}
