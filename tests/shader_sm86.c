/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The SM86 frontend (src/nx/gpu/shader/sm86.h): the decoder against instruction words that a small encoder here
   makes by the rules of Mesa's NAK encoder (sm70_encode.rs: set_opcode, encode_alu_base, set_pred, set_instr_deps,
   set_rel_offset) and its SPH (sph.rs), the search for programs in shader memory, and the translation of small
   programs, which spirv-val checks when the test gets its path. No word here comes from a game. */
#include "nx/gpu/shader/sm86.h"
#include "common/utilities/endian.h"
#include "support/spv_eval.h"
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

static int validate(const char* spirv_val, const char* dir, const char* name, const u32* words, size_t count) {
    if (!spirv_val) return 1;
    char path[1024], command[2600];
    snprintf(path, sizeof path, "%s/sm86_%s.spv", dir, name);
    FILE* f = fopen(path, "wb");
    if (!f) return 0;
    int ok = fwrite(words, 4, count, f) == count;
    ok &= !fclose(f);
#ifdef _WIN32 /* cmd.exe drops the first and the last quote of a command that starts with one */
    snprintf(command, sizeof command, "\"\"%s\" --target-env vulkan1.3 \"%s\"\"", spirv_val, path);
#else
    snprintf(command, sizeof command, "\"%s\" --target-env vulkan1.3 \"%s\"", spirv_val, path);
#endif
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
    /* its store logs its bytes, in the write log's first part (SNX_VKB_WPAGES, shader.h's SH_WLOG_*) */
    CHECK(si.gbuf_marks == 1 && si.nmarks == 1 && si.gbuf_mark[0] == 1);
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

/* ---- programs run on the CPU (support/spv_eval.c) ---- */

static u32 f2u(float f) {
    u32 u;
    memcpy(&u, &f, 4);
    return u;
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
    if (spv) CHECK(validate(spirv_val, dir, "fs_brx", spv, count));
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
static void test_brx_bounded(const char* spirv_val, const char* dir) {
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
        CHECK(validate(spirv_val, dir, "fs_brx_bounded", spv, count));
    }
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
/* TEX, TLD and TLD4 with texel offsets: a register every path sets to one immediate is ConstOffset, as the program
   packs it (four bits a component); a register two paths set apart moves the coordinates instead. TLD adds its
   offset to the coordinates, and TLD4's offset for each texel takes four gathers */
static void test_texel_offsets(const char* spirv_val, const char* dir) {
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
        CHECK(validate(spirv_val, dir, "tex_offset", spv, count));
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
        CHECK(validate(spirv_val, dir, "tex_offset_paths", spv, count));
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
        CHECK(validate(spirv_val, dir, "tex_offset_apart", spv, count));
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
        CHECK(validate(spirv_val, dir, "tld4_offsets", spv, count));
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
        CHECK(validate(spirv_val, dir, "tld_offset", spv, count));
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
static void test_global_flow(const char* spirv_val, const char* dir) {
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
    if (spv) CHECK(validate(spirv_val, dir, "global_flow", spv, count));
    free(spv);
    put(code, 3, mov_imm(5, 1)); /* the second path goes on to the LDG */
    si = (ShaderInfo){0};
    spv = sm86_to_spirv(code, 7 * 16, ST_CS, NULL, &si, &count);
    CHECK(!spv && strstr(si.first_unsupported, "global address whose base"));
    free(spv);
}

/* FSWZADD R0, R1, R2 with swizzle 0x1b: lane 0 (the swizzle's top two bits) adds, lane 1 does b - a, lane 2 a - b
   and lane 3 gives b, as Maxwell's (nouveau's QUADOP). R1 = 1.5, R2 = 4 */
static void test_fswzadd(const char* spirv_val, const char* dir) {
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
    CHECK(spv && validate(spirv_val, dir, "fs_fswzadd", spv, count));
    free(spv);
}

/* S2R R0, SR_LTMASK in a fragment program: the lanes below the invocation's, as in a compute program. A fragment
   program's warp-wide atomics read it with a VOTE ballot */
static void test_lane_masks(const char* spirv_val, const char* dir) {
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
    (void)spirv_val, (void)dir;
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
static void test_ldc_small(const char* spirv_val, const char* dir) {
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
    (void)spirv_val, (void)dir;
}

/* BSSY B0, 2; BREAK B0; BSYNC B0; MOV R0, 1.0: BREAK takes the lanes out of the barrier and goes on */
static void test_break(const char* spirv_val, const char* dir) {
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
    (void)spirv_val, (void)dir;
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
static void test_local_memory(const char* spirv_val, const char* dir) {
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
    CHECK(spv && validate(spirv_val, dir, "fs_local", spv, count));
    free(spv);
}

/* LDS takes a uniform register with bit 91 alone: UR0 is 0x40, and LDS R2, [RZ + 0x10] without the bit reads the
   word STS stored at 0x10 */
static void test_shared_no_uniform(const char* spirv_val, const char* dir) {
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
    CHECK(validate(spirv_val, dir, "cs_shared", spv, count));
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
static void test_hsetp2(const char* spirv_val, const char* dir) {
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
    (void)spirv_val, (void)dir;
}

/* BMOV.32.CLEAR RZ, B0 changes nothing the program computes; into a register it is refused */
static void test_bmov(const char* spirv_val, const char* dir) {
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
    (void)spirv_val, (void)dir;
}

/* The prologue of the debug variants of the driver's programs: BMOV.32 B0, 0xffffffff, BMOV.32.CLEAR B1, B0 and
   BMOV.32 B15, 0x0 write only convergence barriers, which change nothing the program computes. HFMA2.MMA R0, -RZ,
   RZ, imm, imm loads its two halves: R0 is (-2.0, 1.0) as its immediate 0x3c00c000, and R1 with +RZ (+0 + -0) is
   +0 in its low half. A BMOV into a state register is refused */
static void test_debug_prologue(const char* spirv_val, const char* dir) {
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
    (void)spirv_val, (void)dir;
}

/* The first texture coordinate (0x300-0x30f) moves to generic 22 (17 + its group, 5) in both stages: AST a[0x304]
   writes output 22's y, and IPA a[0x300] reads input 22's x, with the interpolation the header's texture coordinate
   map gives it (word 15, constant), not the FragCoord.w that a perspective input takes */
static void test_legacy_varyings(const char* spirv_val, const char* dir) {
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
    (void)spirv_val, (void)dir;
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
static void test_brxu(const char* spirv_val, const char* dir) {
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
        CHECK(validate(spirv_val, dir, "fs_brxu", spv, count));
    }
    free(spv);
}

/* IPA R0, a[UR4 + 0x80] with UR4 = 0x10: input 1's x. The header reads inputs 0 and 1 (constant) */
static void test_ipa_uniform(const char* spirv_val, const char* dir) {
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
    (void)spirv_val, (void)dir;
}

/* TXQ R4, R5 of the handle c[2][0x20] before TLD.LZ.MS R0-R3, R4, R5, sample R9 of it, as a game's resolve programs
   do: one multisampled 2D slot (ShTex.flags bit 1) for both, the sample fetched with the Sample operand from R9, and
   the size queried without a level, as a multisampled image has none */
static void test_tld_multisample(const char* spirv_val, const char* dir) {
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
    CHECK(validate(spirv_val, dir, "fs_tld_ms", spv, count));
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
        if (op == 184) (*tests)++;                                   /* OpFOrdLessThan */
        i += count;
    }
}
static void test_ftz_modes(const char* spirv_val, const char* dir) {
    u8* code = calloc(1, 0x1000);
    if (!code) abort();
    u32 n = compute_program(code), fmul = n;
    for (u32 i = 0; i < n; i++)
        if ((code[16 * i] | (u32)(code[16 * i + 1] & 1) << 8) == 0x020) fmul = i;
    CHECK(fmul < n);
    static const struct {
        int ftz, can_flush, mode, tests;
    } cases[] = {{1, SH_FTZ_MODE, 1, 0}, {1, SH_FTZ_EMULATE, 0, 1}, {1, SH_FTZ_NATIVE, 0, 0},
                 {0, SH_FTZ_MODE, 0, 0}, {0, SH_FTZ_EMULATE, 0, 0}};
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
        CHECK(validate(spirv_val, dir, name, spv, count));
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
            CHECK(validate(spirv_val, dir, "ftz_mixed", spv, count));
        }
        free(spv);
    }
    free(code);
}
static void test_tex_f16(const char* spirv_val, const char* dir) {
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
    CHECK(validate(spirv_val, dir, "fs_tex_f16", spv, count));
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
static void test_output_defaults(const char* spirv_val, const char* dir) {
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
    (void)spirv_val, (void)dir;
}

/* A fragment program's depth is in the register after the sample mask's, whether it writes a sample mask or not:
   R1 with no colour (a program that turns a material number into the depth that its lighting passes test EQUAL
   against), R5 after colour 0, and R5 with the sample mask in R4 */
static void test_fragment_depth(const char* spirv_val, const char* dir) {
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
    (void)spirv_val, (void)dir;
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
/* Pointers from memory, as one game's vertex program that clears indirect draws: R2 is the buffer that c[0][0x110]
   holds (0x1_0000_1000), whose words 0-5 are three pointers. The first, base + 0x40, is inside it: the STG through
   it writes word 0x12 and the LDG reads word 0x10. The second, below the base, and the third, base + 0x1_0000_0040
   (the same low word of the difference), are in no buffer: the STG is dropped and the LDG reads 0 */
static void test_global_pointer(const char* spirv_val, const char* dir) {
    u8 code[16 * 16] = {0};
    Word m = alu(0x002, 5, 2, 255);
    cbuf(&m, 0, 0x110);
    put(code, 0, m);
    m = alu(0x002, 5, 3, 255);
    cbuf(&m, 0, 0x114);
    put(code, 1, m);
    put(code, 2, ldg_nou(4, 2, 0, 5));
    put(code, 3, ldg_nou(8, 2, 8, 5));
    put(code, 4, ldg_nou(12, 2, 16, 5));
    put(code, 5, mov_imm(6, 7));
    put(code, 6, mov_imm(7, 9));
    put(code, 7, mov_imm(11, 5));
    put(code, 8, stg_nou(4, 8, 6));
    put(code, 9, stg_nou(12, 0xc, 7));
    put(code, 10, ldg_nou(10, 4, 0, 4));
    put(code, 11, ldg_nou(11, 8, 0, 4));
    put(code, 12, ast(0x70, 10, 2));
    put(code, 13, exit_insn());
    put(code, 14, bra(14, 14, 7, 0));
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
    int ran = run_stage(ST_VS, code, 15, vsph, &env);
    int ok = ran && buffer[0x12] == 7 && buffer[0x13] == 0 && env.position[0] == 0x1234 && env.position[1] == 0;
    CHECK(ok);
    if (!ok)
        fprintf(stderr, "global pointer: words %x %x, position %x %x\n", buffer[0x12], buffer[0x13], env.position[0],
                env.position[1]);
    ShaderInfo si = {0};
    size_t count = 0;
    u32* spv = sm86_to_spirv(code, 15 * 16, ST_VS, vsph, &si, &count);
    CHECK(spv && si.ngbuf == 1 && si.gbuf_cbuf_off[0] == 0x110 && (si.gbuf_wmask & 1));
    if (spv) CHECK(validate(spirv_val, dir, "global_pointer", spv, count));
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
static void test_mesh(const char* spirv_val, const char* dir) {
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
        if (!fix_depth) CHECK(validate(spirv_val, dir, "ms", spv, count));
        static u32 position[40 * 4], generic1[40 * 4], generic0[2 * 4], indices[2 * 3];
        memset(position, 0xee, sizeof position);
        SpvEvalEnv env = {0};
        env.builtin[26][0] = 2; /* WorkgroupId.x, after the draw's first group 5 */
        env.push[0] = 5;
        env.push[1] = fix_depth ? f2u(1.0f) : 0;
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
static void test_task(const char* spirv_val, const char* dir) {
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
    for (u32 first = 0; first < 6; first += 5) {
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
        if (!first) CHECK(validate(spirv_val, dir, "ts", spv, count));
        SpvEvalEnv env = {0};
        env.builtin[26][0] = 2;
        env.push[0] = first;
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
static void test_mesh_after_task(const char* spirv_val, const char* dir) {
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
    CHECK(validate(spirv_val, dir, "ms_task", spv, count));
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
            per_primitive |= d[2] == 5271; /* PerPrimitiveEXT */
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
static void test_fragment_per_primitive(const char* spirv_val, const char* dir) {
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
        if (mask) CHECK(validate(spirv_val, dir, "fs_per_primitive", spv, count));
        free(spv);
    }
}

/* A fragment program's input that no output of the stage before writes (ShaderInfo.in_unfed_mask) reads 0, 0, 0, 1,
   as the Maxwell translator's does, and the module declares no Input for it, which the stage before would not match
   (VUID-RuntimeSpirv-OpEntryPoint-08743: one game's mesh programs write fewer generics than their fragment programs
   read). An IPA at the centroid of such an input decorates nothing: the decoration named id 0 before, a module that
   spirv-val and the validation layer refuse */
static void test_fragment_unfed(const char* spirv_val, const char* dir) {
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
        CHECK(validate(spirv_val, dir, centroid ? "fs_unfed_centroid" : "fs_unfed_default", spv, count));
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
        if (unfed) CHECK(validate(spirv_val, dir, "fs_unfed", spv, count));
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
/* whether a module decorates a variable as the built-in */
static int spv_has_builtin(const u32* words, size_t count, u32 builtin) {
    for (size_t i = 5; i < count && words[i] >> 16; i += words[i] >> 16)
        if ((words[i] & 0xffff) == 71 && words[i] >> 16 == 4 && words[i + 2] == 11 && words[i + 3] == builtin) return 1;
    return 0;
}
/* HMMA (16816 and 1688, F16 and F32), LDSM and MOVM with and without the device's quads in compute and its mixed dot
   product: A's 48 reads are quad broadcasts or shuffles, B's 48, LDSM's 4 and MOVM's 2 stay shuffles, and each pair
   of products is an OpFDot2MixAcc32VALVE (96) or an OpDot. B's registers are OR'd with hmma_zero's zero, which
   compares NumWorkgroups.x with 0 */
static void test_warp_matrix(const char* spirv_val, const char* dir) {
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
    int quad_was = g_shader_quad_compute, mixed_was = g_shader_mixed_dot;
    for (int quad = 0; quad < 2; quad++)
        for (int mixed = 0; mixed < 2; mixed++) {
            g_shader_quad_compute = quad, g_shader_mixed_dot = mixed;
            ShaderInfo si = {0};
            si.cs_local[0] = 32, si.cs_local[1] = si.cs_local[2] = 1;
            si.shared_size = 1024;
            size_t count = 0;
            u32* spv = sm86_to_spirv(code, n * 16, ST_CS, NULL, &si, &count);
            CHECK(spv != NULL);
            if (!spv) continue;
            u32 quads = spv_count(spv, count, 365), shuffles = spv_count(spv, count, 345);
            u32 mixes = spv_count(spv, count, 6916), dots = spv_count(spv, count, 148);
            CHECK(quads == (quad ? 48u : 0u) && shuffles == (quad ? 54u : 102u));
            CHECK(mixes == (mixed ? 96u : 0u) && dots == (mixed ? 0u : 96u));
            CHECK(spv_has_builtin(spv, count, 24) && spv_count(spv, count, 170 /* OpIEqual */) >= 1);
            if (quads != (quad ? 48u : 0u) || mixes != (mixed ? 96u : 0u))
                fprintf(stderr, "warp matrix, quad %d, mixed %d: %u quad broadcasts, %u shuffles, %u mixed dots, %u "
                                "dots\n",
                        quad, mixed, quads, shuffles, mixes, dots);
            char name[32];
            snprintf(name, sizeof name, "warp_matrix%d%d", quad, mixed);
            CHECK(validate(spirv_val, dir, name, spv, count));
            free(spv);
        }
    g_shader_quad_compute = quad_was, g_shader_mixed_dot = mixed_was;
}

int main(int argc, char** argv) {
    const char* spirv_val = argc > 2 ? argv[2] : NULL;
    const char* dir = argc > 1 ? argv[1] : ".";
    test_alu_forms();
    test_common_fields();
    test_flow();
    test_table();
    test_sph();
    test_tcs_empty();
    test_programs(spirv_val, dir);
    test_game_forms(spirv_val, dir);
    test_brx(spirv_val, dir);
    test_brx_bounded(spirv_val, dir);
    test_alu_group(spirv_val, dir);
    test_compute_forms(spirv_val, dir);
    test_image_kinds(spirv_val, dir);
    test_root_tables(spirv_val, dir);
    test_texel_offsets(spirv_val, dir);
    test_global_flow(spirv_val, dir);
    test_fswzadd(spirv_val, dir);
    test_lane_masks(spirv_val, dir);
    test_ldc_small(spirv_val, dir);
    test_break(spirv_val, dir);
    test_local_memory(spirv_val, dir);
    test_shared_no_uniform(spirv_val, dir);
    test_hsetp2(spirv_val, dir);
    test_bmov(spirv_val, dir);
    test_debug_prologue(spirv_val, dir);
    test_legacy_varyings(spirv_val, dir);
    test_brxu(spirv_val, dir);
    test_ipa_uniform(spirv_val, dir);
    test_tld_multisample(spirv_val, dir);
    test_tex_f16(spirv_val, dir);
    test_output_defaults(spirv_val, dir);
    test_fragment_depth(spirv_val, dir);
    test_global_pointer(spirv_val, dir);
    test_mesh(spirv_val, dir);
    test_task(spirv_val, dir);
    test_mesh_after_task(spirv_val, dir);
    test_mesh_output_bytes();
    test_mesh_refused();
    test_fragment_per_primitive(spirv_val, dir);
    test_fragment_unfed(spirv_val, dir);
    test_uses_isbe();
    test_ftz_modes(spirv_val, dir);
    test_warp_matrix(spirv_val, dir);
    if (failures) {
        fprintf(stderr, "%d checks failed\n", failures);
        return 1;
    }
    printf("sm86: all checks pass\n");
    return 0;
}
