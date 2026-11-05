/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported from QEMU (target/arm/tcg/translate-a64.c: the trans_* functions of these instructions, trans_SM3SS1,
   trans_XAR; gengvec.c's gen_gvec_rax1, gen_gvec_eor3, gen_gvec_bcax), LGPL-2.1-or-later:
   Copyright (c) 2013 Alexander Graf <agraf@suse.de> */
/* The cryptographic extensions as LLVM IR, keyed on the generated decoder's ops: AES (on the host's AES instructions
   where it has them, x86's AES-NI or AArch64's, else helpers/crypto.c's rounds), SHA-1 and SHA-256 (AArch64's own
   instructions where it has SHA2; SHA-256 through x86's SHA-NI; else helpers/crypto.c), PMULL of doublewords
   (PCLMULQDQ, AArch64's PMULL, else helpers/crypto.c), and FEAT_SHA512, FEAT_SHA3, FEAT_SM3 and FEAT_SM4
   (helpers/vector.c, the XOR and rotate ones inline). */
#include "common/cpu/recompiler/llvm/ir_vec.h"
#include "common/cpu/aarch64/cpu.h"

typedef LLVMValueRef V;
#define B ir->b

static V bytes(SnxIr* ir, unsigned n) { return ir_vec_read(ir, n, 0, 0, 16); }
static V doublewords(SnxIr* ir, unsigned n) { return ir_vec_read(ir, n, 3, 0, 2); }
static V words(SnxIr* ir, unsigned n) { return ir_vec_read(ir, n, 2, 0, 4); }
static V reg_pointer(SnxIr* ir, unsigned n) { return ir_cpu(ir, ir_off_v(n)); }

/* helpers/crypto.c's v_aes(d, k, dec) and v_aesmc(d, s, inv), on the registers in the CPU structure */
static void aes_helper(SnxIr* ir, const char* name, unsigned d, unsigned s, unsigned flag) {
    LLVMTypeRef params[3];
    V args[3];
    params[0] = ir->t_ptr;
    params[1] = ir->t_ptr;
    params[2] = ir->t_i32;
    args[0] = reg_pointer(ir, d);
    args[1] = reg_pointer(ir, s);
    args[2] = ir_i32(ir, flag);
    V fn = ir_extern(ir, name, ir->t_void, params, 3);
    ir_argmem(ir, fn);
    ir_state_invalidate(ir);
    LLVMBuildCall2(B, LLVMFunctionType(ir->t_void, params, 3, 0), fn, args, 3, "");
}

static V x86(SnxIr* ir, const char* name, V a, V b) {
    LLVMTypeRef q = ir_vec_type(ir, 3, 2), b16 = ir_vec_type(ir, 0, 16);
    V args[2];
    args[0] = LLVMBuildBitCast(B, a, q, "");
    args[1] = b ? LLVMBuildBitCast(B, b, q, "") : NULL;
    return LLVMBuildBitCast(B, ir_intrinsic(ir, name, NULL, args, b ? 2 : 1), b16, "");
}

/* AESE, AESD: SubBytes(ShiftRows(d ^ k)) and its inverse, x86's last rounds with a zero key */
static void aes(SnxIr* ir, const SnxA64Args* a, int dec) {
    if (ir->features->aarch64 && ir->features->arm_aes) {
        V args[2];
        args[0] = bytes(ir, (unsigned)a->rd);
        args[1] = bytes(ir, (unsigned)a->rm);
        ir_vec_write(ir, (unsigned)a->rd,
                     ir_intrinsic(ir, dec ? "llvm.aarch64.crypto.aesd" : "llvm.aarch64.crypto.aese", NULL, args, 2));
    } else if (ir->features->x86 && ir->features->aes) {
        V x = LLVMBuildXor(B, bytes(ir, (unsigned)a->rd), bytes(ir, (unsigned)a->rm), "");
        ir_vec_write(ir, (unsigned)a->rd,
                     x86(ir, dec ? "llvm.x86.aesni.aesdeclast" : "llvm.x86.aesni.aesenclast", x,
                         LLVMConstNull(ir_vec_type(ir, 0, 16))));
    } else
        aes_helper(ir, "v_aes", (unsigned)a->rd, (unsigned)a->rm, (unsigned)dec);
}

/* AESMC, AESIMC: MixColumns is x86's full round after an inverse last round, both with zero keys, as SubBytes and
   ShiftRows commute */
static void aesmc(SnxIr* ir, const SnxA64Args* a, int inv) {
    if (ir->features->aarch64 && ir->features->arm_aes) {
        V x = bytes(ir, (unsigned)a->rn);
        ir_vec_write(ir, (unsigned)a->rd,
                     ir_intrinsic(ir, inv ? "llvm.aarch64.crypto.aesimc" : "llvm.aarch64.crypto.aesmc", NULL, &x, 1));
    } else if (ir->features->x86 && ir->features->aes) {
        V x = bytes(ir, (unsigned)a->rn), zero = LLVMConstNull(ir_vec_type(ir, 0, 16));
        if (inv)
            x = x86(ir, "llvm.x86.aesni.aesimc", x, NULL);
        else
            x = x86(ir, "llvm.x86.aesni.aesenc", x86(ir, "llvm.x86.aesni.aesdeclast", x, zero), zero);
        ir_vec_write(ir, (unsigned)a->rd, x);
    } else
        aes_helper(ir, "v_aesmc", (unsigned)a->rd, (unsigned)a->rn, (unsigned)inv);
}

/* the SHA-1 and SHA-256 ops of helpers/crypto.c: v_sha1(c, op, d, n, m), v_sha256 */
static void sha(SnxIr* ir, const SnxA64Args* a, const char* name, unsigned op) {
    V ops[4];
    ops[0] = ir_i32(ir, op);
    ops[1] = ir_i32(ir, (SnxU32)a->rd);
    ops[2] = ir_i32(ir, (SnxU32)a->rn);
    ops[3] = ir_i32(ir, (SnxU32)a->rm);
    ir_vec_call(ir, name, ir->t_void, ops, 4);
}

/* SHA1C, SHA1P, SHA1M on AArch64: Vd's four state words, Vn.S[0] and Vm's message words in one instruction */
static void sha1_round(SnxIr* ir, const SnxA64Args* a, const char* name) {
    V args[3];
    args[0] = words(ir, (unsigned)a->rd);
    args[1] = LLVMBuildExtractElement(B, words(ir, (unsigned)a->rn), ir_i32(ir, 0), "");
    args[2] = words(ir, (unsigned)a->rm);
    ir_vec_write(ir, (unsigned)a->rd, ir_intrinsic(ir, name, NULL, args, 3));
}

/* SHA1H: the 32-bit hash of Vn.S[0], the rest of Vd zeroed */
static void sha1h(SnxIr* ir, const SnxA64Args* a) {
    V args[1], zero = LLVMConstNull(ir_vec_type(ir, 2, 4));
    args[0] = LLVMBuildExtractElement(B, words(ir, (unsigned)a->rn), ir_i32(ir, 0), "");
    V r = ir_intrinsic(ir, "llvm.aarch64.crypto.sha1h", NULL, args, 1);
    ir_vec_write(ir, (unsigned)a->rd, LLVMBuildInsertElement(B, zero, r, ir_i32(ir, 0), ""));
}

/* SHA1SU0, SHA1SU1, SHA256H, SHA256H2, SHA256SU0, SHA256SU1: the instruction's own registers in its own order */
static void sha2_intrinsic(SnxIr* ir, const SnxA64Args* a, const char* name, unsigned count) {
    V args[3];
    args[0] = words(ir, (unsigned)a->rd);
    if (count > 1) args[1] = words(ir, (unsigned)a->rn);
    if (count > 2) args[2] = words(ir, (unsigned)a->rm);
    ir_vec_write(ir, (unsigned)a->rd, ir_intrinsic(ir, name, NULL, args, count));
}

/* SHA256H and SHA256H2 through SHA-NI (dynarmic's mapping): the state's two halves in the order the rounds take
   them, two rounds twice, and the result's halves back in the guest's order */
static V sha256_hash_x86(SnxIr* ir, unsigned d, unsigned n, unsigned m, int part1) {
    static const unsigned cdgh[4] = {3, 2, 7, 6}, abef[4] = {1, 0, 5, 4}, high[4] = {2, 3, 2, 3};
    V lo = words(ir, part1 ? n : d), hi = words(ir, part1 ? d : n), msg = words(ir, m), args[3];
    V first = ir_vec_shuffle(ir, lo, hi, cdgh, 4), second = ir_vec_shuffle(ir, lo, hi, abef, 4);
    args[0] = first;
    args[1] = second;
    args[2] = msg;
    V r1 = ir_intrinsic(ir, "llvm.x86.sha256rnds2", NULL, args, 3), hi_msg = ir_vec_shuffle(ir, msg, msg, high, 4);
    args[0] = second;
    args[1] = r1;
    args[2] = hi_msg;
    V r2 = ir_intrinsic(ir, "llvm.x86.sha256rnds2", NULL, args, 3);
    return ir_vec_shuffle(ir, r2, r1, part1 ? cdgh : abef, 4);
}

/* SHA256SU0 through SHA-NI */
static V sha256_su0_x86(SnxIr* ir, unsigned d, unsigned n) {
    V args[2];
    args[0] = words(ir, d);
    args[1] = words(ir, n);
    return ir_intrinsic(ir, "llvm.x86.sha256msg1", NULL, args, 2);
}

/* SHA256SU1 through SHA-NI: Vd plus Vn's words shifted in at byte 4, then the schedule's second half */
static V sha256_su1_x86(SnxIr* ir, unsigned d, unsigned n, unsigned m) {
    static const unsigned palign[16] = {4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19};
    LLVMTypeRef b16 = ir_vec_type(ir, 0, 16);
    V n8 = LLVMBuildBitCast(B, words(ir, n), b16, ""), m8 = LLVMBuildBitCast(B, words(ir, m), b16, "");
    V shifted = LLVMBuildBitCast(B, ir_vec_shuffle(ir, n8, m8, palign, 16), ir_vec_type(ir, 2, 4), ""), args[2];
    args[0] = LLVMBuildAdd(B, words(ir, d), shifted, "");
    args[1] = words(ir, m);
    return ir_intrinsic(ir, "llvm.x86.sha256msg2", NULL, args, 2);
}

/* PMULL, PMULL2 of doublewords: the 128-bit carry-less product of the low (high) halves */
static void pmull64(SnxIr* ir, const SnxA64Args* a) {
    unsigned hi = (unsigned)a->q;
    if (ir->features->aarch64 && ir->features->arm_aes) {
        V args[2];
        args[0] = LLVMBuildExtractElement(B, doublewords(ir, (unsigned)a->rn), ir_i32(ir, hi), "");
        args[1] = LLVMBuildExtractElement(B, doublewords(ir, (unsigned)a->rm), ir_i32(ir, hi), "");
        ir_vec_write(ir, (unsigned)a->rd, ir_intrinsic(ir, "llvm.aarch64.neon.pmull64", NULL, args, 2));
    } else if (ir->features->x86 && ir->features->pclmul) {
        V args[3];
        args[0] = doublewords(ir, (unsigned)a->rn);
        args[1] = doublewords(ir, (unsigned)a->rm);
        args[2] = LLVMConstInt(ir->t_i8, hi ? 0x11 : 0x00, 0);
        ir_vec_write(ir, (unsigned)a->rd, ir_intrinsic(ir, "llvm.x86.pclmulqdq", NULL, args, 3));
    } else {
        LLVMTypeRef params[4];
        V args[4];
        params[0] = params[1] = params[2] = ir->t_ptr;
        params[3] = ir->t_i32;
        args[0] = reg_pointer(ir, (unsigned)a->rd);
        args[1] = reg_pointer(ir, (unsigned)a->rn);
        args[2] = reg_pointer(ir, (unsigned)a->rm);
        args[3] = ir_i32(ir, hi);
        V fn = ir_extern(ir, "v_pmull", ir->t_void, params, 4);
        ir_argmem(ir, fn);
        ir_state_invalidate(ir);
        LLVMBuildCall2(B, LLVMFunctionType(ir->t_void, params, 4, 0), fn, args, 4, "");
    }
}

static V rotate_right(SnxIr* ir, V x, unsigned s) {
    V args[3];
    args[0] = x;
    args[1] = x;
    args[2] = ir_vec_k(ir, LLVMTypeOf(x), s);
    return ir_intrinsic(ir, "llvm.fshr", LLVMTypeOf(x), args, 3);
}

/* SM3SS1: ROR(ROR(Vn.S[3], 20) + Vm.S[3] + Va.S[3], 25) into Vd.S[3], the rest zeroed */
static void sm3ss1(SnxIr* ir, const SnxA64Args* a) {
    V n = ir_vec_read(ir, (unsigned)a->rn, 2, 3, 1), m = ir_vec_read(ir, (unsigned)a->rm, 2, 3, 1);
    V x = ir_vec_read(ir, (unsigned)a->ra, 2, 3, 1);
    unsigned mask[4] = {1, 1, 1, 0};
    V r = rotate_right(ir, LLVMBuildAdd(B, LLVMBuildAdd(B, rotate_right(ir, n, 20), m, ""), x, ""), 25);
    ir_vec_write(ir, (unsigned)a->rd, ir_vec_shuffle(ir, r, LLVMConstNull(LLVMTypeOf(r)), mask, 4));
}

static void call5(SnxIr* ir, const char* name, unsigned op, unsigned d, unsigned n, unsigned m, int imm) {
    V ops[5];
    unsigned count = 4;
    ops[0] = ir_i32(ir, op);
    ops[1] = ir_i32(ir, d);
    ops[2] = ir_i32(ir, n);
    ops[3] = ir_i32(ir, m);
    if (imm >= 0) ops[count++] = ir_i32(ir, (SnxU32)imm);
    ir_vec_call(ir, name, ir->t_void, ops, count);
}

int ir_vec_crypto_handles(SnxA64Op op) {
    switch (op) {
        case SNX_A64_OP_AESE:
        case SNX_A64_OP_AESD:
        case SNX_A64_OP_AESMC:
        case SNX_A64_OP_AESIMC:
        case SNX_A64_OP_SHA1C:
        case SNX_A64_OP_SHA1P:
        case SNX_A64_OP_SHA1M:
        case SNX_A64_OP_SHA1SU0:
        case SNX_A64_OP_SHA256H:
        case SNX_A64_OP_SHA256H2:
        case SNX_A64_OP_SHA256SU1:
        case SNX_A64_OP_SHA1H:
        case SNX_A64_OP_SHA1SU1:
        case SNX_A64_OP_SHA256SU0:
        case SNX_A64_OP_PMULL_p64:
        case SNX_A64_OP_SHA512H:
        case SNX_A64_OP_SHA512H2:
        case SNX_A64_OP_SHA512SU0:
        case SNX_A64_OP_SHA512SU1:
        case SNX_A64_OP_RAX1:
        case SNX_A64_OP_EOR3:
        case SNX_A64_OP_BCAX:
        case SNX_A64_OP_XAR:
        case SNX_A64_OP_SM3PARTW1:
        case SNX_A64_OP_SM3PARTW2:
        case SNX_A64_OP_SM3SS1:
        case SNX_A64_OP_SM3TT1A:
        case SNX_A64_OP_SM3TT1B:
        case SNX_A64_OP_SM3TT2A:
        case SNX_A64_OP_SM3TT2B:
        case SNX_A64_OP_SM4E:
        case SNX_A64_OP_SM4EKEY: return 1;
        default: return 0;
    }
}

void ir_vec_crypto(SnxIr* ir, const SnxA64Insn* insn) {
    const SnxA64Args* a = &insn->args;
    unsigned rd = (unsigned)a->rd, rn = (unsigned)a->rn, rm = (unsigned)a->rm;
    switch (insn->op) {
        case SNX_A64_OP_AESE: aes(ir, a, 0); break;
        case SNX_A64_OP_AESD: aes(ir, a, 1); break;
        case SNX_A64_OP_AESMC: aesmc(ir, a, 0); break;
        case SNX_A64_OP_AESIMC: aesmc(ir, a, 1); break;
        case SNX_A64_OP_SHA1C:
            if (ir->features->aarch64 && ir->features->arm_sha2)
                sha1_round(ir, a, "llvm.aarch64.crypto.sha1c");
            else
                sha(ir, a, "v_sha1", 0);
            break;
        case SNX_A64_OP_SHA1P:
            if (ir->features->aarch64 && ir->features->arm_sha2)
                sha1_round(ir, a, "llvm.aarch64.crypto.sha1p");
            else
                sha(ir, a, "v_sha1", 1);
            break;
        case SNX_A64_OP_SHA1M:
            if (ir->features->aarch64 && ir->features->arm_sha2)
                sha1_round(ir, a, "llvm.aarch64.crypto.sha1m");
            else
                sha(ir, a, "v_sha1", 2);
            break;
        case SNX_A64_OP_SHA1SU0:
            if (ir->features->aarch64 && ir->features->arm_sha2)
                sha2_intrinsic(ir, a, "llvm.aarch64.crypto.sha1su0", 3);
            else
                sha(ir, a, "v_sha1", 3);
            break;
        case SNX_A64_OP_SHA1H:
            if (ir->features->aarch64 && ir->features->arm_sha2)
                sha1h(ir, a);
            else
                sha(ir, a, "v_sha1", 0x10);
            break;
        case SNX_A64_OP_SHA1SU1:
            if (ir->features->aarch64 && ir->features->arm_sha2)
                sha2_intrinsic(ir, a, "llvm.aarch64.crypto.sha1su1", 2);
            else
                sha(ir, a, "v_sha1", 0x11);
            break;
        case SNX_A64_OP_SHA256H:
            if (ir->features->aarch64 && ir->features->arm_sha2)
                sha2_intrinsic(ir, a, "llvm.aarch64.crypto.sha256h", 3);
            else if (ir->features->x86 && ir->features->sha_ni)
                ir_vec_write(ir, rd, sha256_hash_x86(ir, rd, rn, rm, 1));
            else
                sha(ir, a, "v_sha256", 4);
            break;
        case SNX_A64_OP_SHA256H2:
            if (ir->features->aarch64 && ir->features->arm_sha2)
                sha2_intrinsic(ir, a, "llvm.aarch64.crypto.sha256h2", 3);
            else if (ir->features->x86 && ir->features->sha_ni)
                ir_vec_write(ir, rd, sha256_hash_x86(ir, rd, rn, rm, 0));
            else
                sha(ir, a, "v_sha256", 5);
            break;
        case SNX_A64_OP_SHA256SU1:
            if (ir->features->aarch64 && ir->features->arm_sha2)
                sha2_intrinsic(ir, a, "llvm.aarch64.crypto.sha256su1", 3);
            else if (ir->features->x86 && ir->features->sha_ni)
                ir_vec_write(ir, rd, sha256_su1_x86(ir, rd, rn, rm));
            else
                sha(ir, a, "v_sha256", 6);
            break;
        case SNX_A64_OP_SHA256SU0:
            if (ir->features->aarch64 && ir->features->arm_sha2)
                sha2_intrinsic(ir, a, "llvm.aarch64.crypto.sha256su0", 2);
            else if (ir->features->x86 && ir->features->sha_ni)
                ir_vec_write(ir, rd, sha256_su0_x86(ir, rd, rn));
            else
                sha(ir, a, "v_sha256", 0x12);
            break;
        case SNX_A64_OP_PMULL_p64: pmull64(ir, a); break;
        case SNX_A64_OP_SHA512H: call5(ir, "snx_vec_sha512", 0, rd, rn, rm, -1); break;
        case SNX_A64_OP_SHA512H2: call5(ir, "snx_vec_sha512", 1, rd, rn, rm, -1); break;
        case SNX_A64_OP_SHA512SU0: call5(ir, "snx_vec_sha512", 2, rd, rn, rn, -1); break;
        case SNX_A64_OP_SHA512SU1: call5(ir, "snx_vec_sha512", 3, rd, rn, rm, -1); break;
        case SNX_A64_OP_RAX1: { /* Vn ^ ROL(Vm, 1) */
            V m = doublewords(ir, rm);
            ir_vec_write(ir, rd, LLVMBuildXor(B, doublewords(ir, rn), rotate_right(ir, m, 63), ""));
            break;
        }
        case SNX_A64_OP_EOR3:
            ir_vec_write(ir, rd, LLVMBuildXor(B, LLVMBuildXor(B, doublewords(ir, rn), doublewords(ir, rm), ""),
                                              doublewords(ir, (unsigned)a->ra), ""));
            break;
        case SNX_A64_OP_BCAX: /* Vn ^ (Vm & ~Va) */
            ir_vec_write(ir, rd, LLVMBuildXor(B, doublewords(ir, rn),
                                              LLVMBuildAnd(B, doublewords(ir, rm),
                                                           LLVMBuildNot(B, doublewords(ir, (unsigned)a->ra), ""), ""),
                                              ""));
            break;
        case SNX_A64_OP_XAR:
            ir_vec_write(ir, rd, rotate_right(ir, LLVMBuildXor(B, doublewords(ir, rn), doublewords(ir, rm), ""),
                                              (unsigned)a->imm));
            break;
        case SNX_A64_OP_SM3PARTW1: call5(ir, "snx_vec_sm3", 0, rd, rn, rm, 0); break;
        case SNX_A64_OP_SM3PARTW2: call5(ir, "snx_vec_sm3", 1, rd, rn, rm, 0); break;
        case SNX_A64_OP_SM3TT1A: call5(ir, "snx_vec_sm3", 2, rd, rn, rm, a->imm); break;
        case SNX_A64_OP_SM3TT1B: call5(ir, "snx_vec_sm3", 3, rd, rn, rm, a->imm); break;
        case SNX_A64_OP_SM3TT2A: call5(ir, "snx_vec_sm3", 4, rd, rn, rm, a->imm); break;
        case SNX_A64_OP_SM3TT2B: call5(ir, "snx_vec_sm3", 5, rd, rn, rm, a->imm); break;
        case SNX_A64_OP_SM3SS1: sm3ss1(ir, a); break;
        case SNX_A64_OP_SM4E: call5(ir, "snx_vec_sm4", 0, rd, rm, rm, -1); break; /* Vd's state, Vn's keys */
        case SNX_A64_OP_SM4EKEY: call5(ir, "snx_vec_sm4", 1, rd, rn, rm, -1); break;
        default: break;
    }
}
