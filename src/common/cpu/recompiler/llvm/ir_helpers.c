/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "common/cpu/recompiler/llvm/ir.h"
#include "common/cpu/aarch64/cpu.h"
#include <stdio.h>
#include <string.h>

/* ---- cpu.h's inline helpers ----
   Each mirrors its C body. They are internal and always inlined, as SNX_INLINE asks, so a constant argument
   (a condition, a shift type, an access size) folds away. */

typedef struct {
    LLVMBasicBlockRef block;
    LLVMValueRef fn, c, M;
} Saved;

static LLVMValueRef helper_begin(SnxIr* ir, const char* name, LLVMTypeRef ret, LLVMTypeRef* params, unsigned count,
                                 Saved* saved) {
    LLVMValueRef function = LLVMAddFunction(ir->mod, name, LLVMFunctionType(ret, params, count, 0));
    LLVMSetLinkage(function, LLVMInternalLinkage);
    ir_attribute(ir, function, LLVMAttributeFunctionIndex, "alwaysinline");
    ir_attribute(ir, function, LLVMAttributeFunctionIndex, "nounwind");
    saved->block = LLVMGetInsertBlock(ir->b);
    saved->fn = ir->fn;
    saved->c = ir->c;
    saved->M = ir->M;
    ir_state_invalidate(ir);
    ir->fn = function;
    LLVMPositionBuilderAtEnd(ir->b, LLVMAppendBasicBlockInContext(ir->ctx, function, "entry"));
    return function;
}
static void helper_end(SnxIr* ir, Saved* saved) {
    ir_state_invalidate(ir);
    ir->fn = saved->fn;
    ir->c = saved->c;
    ir->M = saved->M;
    if (saved->block) LLVMPositionBuilderAtEnd(ir->b, saved->block);
}
#define P(i) LLVMGetParam(function, i)
#define B    ir->b

static void store_flag(SnxIr* ir, LLVMValueRef value, size_t offset) { ir_cpu_store(ir, ir_u32(ir, value), offset); }

/* adds64/adds32(c,a,b,cin) */
static void build_adds(SnxIr* ir, LLVMValueRef function, LLVMTypeRef t, unsigned bits) {
    LLVMValueRef a = P(1), b = P(2), cin = P(3);
    LLVMValueRef r = LLVMBuildAdd(B, LLVMBuildAdd(B, a, b, ""), ir_cast(ir, cin, t, 0), "");
    store_flag(ir, LLVMBuildLShr(B, r, LLVMConstInt(t, bits - 1, 0), ""), offsetof(CPU, n));
    store_flag(ir, LLVMBuildZExt(B, LLVMBuildICmp(B, LLVMIntEQ, r, LLVMConstInt(t, 0, 0), ""), t, ""),
               offsetof(CPU, z));
    LLVMValueRef cf = LLVMBuildSelect(B, LLVMBuildICmp(B, LLVMIntNE, cin, ir_i32(ir, 0), ""),
                                      LLVMBuildICmp(B, LLVMIntULE, r, a, ""),
                                      LLVMBuildICmp(B, LLVMIntULT, r, a, ""), "");
    ir_cpu_store(ir, ir_bool32(ir, cf), offsetof(CPU, cf));
    LLVMValueRef vf = LLVMBuildAnd(B, LLVMBuildNot(B, LLVMBuildXor(B, a, b, ""), ""), LLVMBuildXor(B, a, r, ""), "");
    store_flag(ir, LLVMBuildLShr(B, vf, LLVMConstInt(t, bits - 1, 0), ""), offsetof(CPU, vf));
    LLVMBuildRet(B, r);
}
/* subs64/subs32(c,a,b) */
static void build_subs(SnxIr* ir, LLVMValueRef function, LLVMTypeRef t, unsigned bits) {
    LLVMValueRef a = P(1), b = P(2);
    LLVMValueRef r = LLVMBuildSub(B, a, b, "");
    store_flag(ir, LLVMBuildLShr(B, r, LLVMConstInt(t, bits - 1, 0), ""), offsetof(CPU, n));
    ir_cpu_store(ir, ir_bool32(ir, LLVMBuildICmp(B, LLVMIntEQ, r, LLVMConstInt(t, 0, 0), "")), offsetof(CPU, z));
    ir_cpu_store(ir, ir_bool32(ir, LLVMBuildICmp(B, LLVMIntUGE, a, b, "")), offsetof(CPU, cf));
    LLVMValueRef vf = LLVMBuildAnd(B, LLVMBuildXor(B, a, b, ""), LLVMBuildXor(B, a, r, ""), "");
    store_flag(ir, LLVMBuildLShr(B, vf, LLVMConstInt(t, bits - 1, 0), ""), offsetof(CPU, vf));
    LLVMBuildRet(B, r);
}
/* logic64/logic32(c,r) */
static void build_logic(SnxIr* ir, LLVMValueRef function, LLVMTypeRef t, unsigned bits) {
    LLVMValueRef r = P(1);
    store_flag(ir, LLVMBuildLShr(B, r, LLVMConstInt(t, bits - 1, 0), ""), offsetof(CPU, n));
    ir_cpu_store(ir, ir_bool32(ir, LLVMBuildICmp(B, LLVMIntEQ, r, LLVMConstInt(t, 0, 0), "")), offsetof(CPU, z));
    ir_cpu_store(ir, ir_i32(ir, 0), offsetof(CPU, cf));
    ir_cpu_store(ir, ir_i32(ir, 0), offsetof(CPU, vf));
    LLVMBuildRetVoid(B);
}
/* cond_ok(c,cond) */
static void build_cond_ok(SnxIr* ir, LLVMValueRef function) {
    LLVMValueRef cond = P(1), zero = ir_i32(ir, 0);
    LLVMBasicBlockRef always = ir_block(ir, "always"), test = ir_block(ir, "test");
    LLVMValueRef n = ir_cpu_load(ir, ir->t_i32, offsetof(CPU, n));
    LLVMValueRef z = ir_cpu_load(ir, ir->t_i32, offsetof(CPU, z));
    LLVMValueRef cf = ir_cpu_load(ir, ir->t_i32, offsetof(CPU, cf));
    LLVMValueRef vf = ir_cpu_load(ir, ir->t_i32, offsetof(CPU, vf));
    LLVMValueRef k = LLVMBuildLShr(B, cond, ir_i32(ir, 1), "");
    LLVMBuildCondBr(B, LLVMBuildICmp(B, LLVMIntEQ, k, ir_i32(ir, 7), ""), always, test);
    LLVMPositionBuilderAtEnd(B, always);
    LLVMBuildRet(B, ir_i32(ir, 1));
    LLVMPositionBuilderAtEnd(B, test);
    LLVMValueRef nv = ir_bool32(ir, LLVMBuildICmp(B, LLVMIntEQ, n, vf, ""));
    /* case 6: !z && n==vf, case 5: n==vf, case 4: cf && !z, cases 3..0: vf, n, cf, z */
    LLVMValueRef r = ir_bool32(
        ir, LLVMBuildAnd(B, LLVMBuildICmp(B, LLVMIntEQ, z, zero, ""), LLVMBuildICmp(B, LLVMIntEQ, n, vf, ""), ""));
    r = LLVMBuildSelect(B, LLVMBuildICmp(B, LLVMIntEQ, k, ir_i32(ir, 5), ""), nv, r, "");
    r = LLVMBuildSelect(B, LLVMBuildICmp(B, LLVMIntEQ, k, ir_i32(ir, 4), ""),
                        ir_bool32(ir, LLVMBuildAnd(B, LLVMBuildICmp(B, LLVMIntNE, cf, zero, ""),
                                                   LLVMBuildICmp(B, LLVMIntEQ, z, zero, ""), "")),
                        r, "");
    r = LLVMBuildSelect(B, LLVMBuildICmp(B, LLVMIntEQ, k, ir_i32(ir, 3), ""), vf, r, "");
    r = LLVMBuildSelect(B, LLVMBuildICmp(B, LLVMIntEQ, k, ir_i32(ir, 2), ""), n, r, "");
    r = LLVMBuildSelect(B, LLVMBuildICmp(B, LLVMIntEQ, k, ir_i32(ir, 1), ""), cf, r, "");
    r = LLVMBuildSelect(B, LLVMBuildICmp(B, LLVMIntEQ, k, ir_i32(ir, 0), ""), z, r, "");
    LLVMValueRef result = LLVMBuildSelect(B, LLVMBuildTrunc(B, cond, ir->t_i1, ""),
                                          ir_bool32(ir, LLVMBuildICmp(B, LLVMIntEQ, r, zero, "")), r, "");
    LLVMBuildRet(B, result);
}
/* set_nzcv(c,f) */
static void build_set_nzcv(SnxIr* ir, LLVMValueRef function) {
    LLVMValueRef f = P(1), one = ir_i32(ir, 1);
    ir_cpu_store(ir, LLVMBuildAnd(B, LLVMBuildLShr(B, f, ir_i32(ir, 3), ""), one, ""), offsetof(CPU, n));
    ir_cpu_store(ir, LLVMBuildAnd(B, LLVMBuildLShr(B, f, ir_i32(ir, 2), ""), one, ""), offsetof(CPU, z));
    ir_cpu_store(ir, LLVMBuildAnd(B, LLVMBuildLShr(B, f, one, ""), one, ""), offsetof(CPU, cf));
    ir_cpu_store(ir, LLVMBuildAnd(B, f, one, ""), offsetof(CPU, vf));
    LLVMBuildRetVoid(B);
}
/* A rotate has no exceptional shift count. fshr defines count modulo width,
   including zero, and exposes the host's native rotate to instruction selection. */
static void build_ror(SnxIr* ir, LLVMValueRef function, LLVMTypeRef t, unsigned bits) {
    LLVMValueRef x = P(0), count = ir_cast(ir, P(1), t, 0);
    LLVMValueRef args[3] = {x, x, count};
    (void)bits;
    LLVMBuildRet(B, ir_intrinsic(ir, "llvm.fshr", t, args, 3));
}
/* shf64/shf32(x,t,s) */
static void build_shf(SnxIr* ir, LLVMValueRef function, LLVMTypeRef t, int ror) {
    LLVMValueRef x = P(0), type = P(1), s = ir_cast(ir, P(2), t, 0), args[2];
    args[0] = x;
    args[1] = P(2);
    LLVMValueRef r = ir_call(ir, ir_helper(ir, ror), args, 2);
    r = LLVMBuildSelect(B, LLVMBuildICmp(B, LLVMIntEQ, type, ir_i32(ir, 2), ""), LLVMBuildAShr(B, x, s, ""), r, "");
    r = LLVMBuildSelect(B, LLVMBuildICmp(B, LLVMIntEQ, type, ir_i32(ir, 1), ""), LLVMBuildLShr(B, x, s, ""), r, "");
    r = LLVMBuildSelect(B, LLVMBuildICmp(B, LLVMIntEQ, type, ir_i32(ir, 0), ""), LLVMBuildShl(B, x, s, ""), r, "");
    LLVMBuildRet(B, r);
}
/* ext64(x,opt,sh) */
static void build_ext64(SnxIr* ir, LLVMValueRef function) {
    LLVMValueRef x = P(0), opt = P(1), r = x;
    static const struct {
        unsigned opt, bits;
        int is_signed;
    } kinds[] = {{6, 32, 1}, {5, 16, 1}, {4, 8, 1}, {2, 32, 0}, {1, 16, 0}, {0, 8, 0}};
    for (size_t i = 0; i < sizeof(kinds) / sizeof(kinds[0]); ++i) {
        LLVMValueRef narrow = LLVMBuildTrunc(B, x, LLVMIntTypeInContext(ir->ctx, kinds[i].bits), "");
        r = LLVMBuildSelect(B, LLVMBuildICmp(B, LLVMIntEQ, opt, ir_i32(ir, kinds[i].opt), ""),
                            ir_cast(ir, narrow, ir->t_i64, kinds[i].is_signed), r, "");
    }
    LLVMBuildRet(B, LLVMBuildShl(B, r, ir_u64(ir, P(2)), ""));
}
/* clz64/clz32(x): x ? __builtin_clz(x) : width, which is ctlz defined at zero */
static void build_clz(SnxIr* ir, LLVMValueRef function, LLVMTypeRef t) {
    LLVMValueRef args[2];
    args[0] = P(0);
    args[1] = LLVMConstInt(ir->t_i1, 0, 0);
    LLVMBuildRet(B, ir_u32(ir, ir_intrinsic(ir, "llvm.ctlz", t, args, 2)));
}
/* cls64/cls32(x): clz(x ^ (x >> 1 arithmetic)) - 1 */
static void build_cls(SnxIr* ir, LLVMValueRef function, LLVMTypeRef t, int clz) {
    LLVMValueRef x = P(0);
    LLVMValueRef arg = LLVMBuildXor(B, x, LLVMBuildAShr(B, x, LLVMConstInt(t, 1, 0), ""), "");
    LLVMBuildRet(B, LLVMBuildSub(B, ir_call(ir, ir_helper(ir, clz), &arg, 1), ir_i32(ir, 1), ""));
}
static LLVMValueRef swap_pairs(SnxIr* ir, LLVMValueRef x, unsigned shift, SnxU64 mask) {
    LLVMValueRef m = ir_i64(ir, mask), s = ir_i64(ir, shift);
    return LLVMBuildOr(B, LLVMBuildAnd(B, LLVMBuildLShr(B, x, s, ""), m, ""),
                       LLVMBuildShl(B, LLVMBuildAnd(B, x, m, ""), s, ""), "");
}
/* rbit64(x): cpu.h swaps bits, pairs and nibbles, then bswap64 - a bit reversal, which LLVM lowers to the host's
   best sequence (GF2P8AFFINEQB and BSWAP on x86 with GFNI, RBIT on AArch64) */
static void build_rbit64(SnxIr* ir, LLVMValueRef function) {
    LLVMValueRef x = P(0);
    if (ir->features->x86 && ir->features->gfni) {
        /* wunkolo, "gf2p8affineqb: Bit reversal": the matrix 0x8040201008040201 reverses each byte's bits, BSWAP the
           bytes. Explicit, whatever the LLVM version lowers llvm.bitreverse to */
        LLVMTypeRef v2 = LLVMVectorType(ir->t_i64, 2), v16 = LLVMVectorType(ir->t_i8, 16);
        LLVMValueRef bytes[16], args[3];
        static const unsigned char matrix[8] = {0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80};
        for (unsigned i = 0; i < 16; ++i) bytes[i] = ir_i8(ir, matrix[i & 7]);
        LLVMValueRef v = LLVMBuildInsertElement(B, LLVMGetUndef(v2), x, ir_i32(ir, 0), "");
        args[0] = LLVMBuildBitCast(B, v, v16, "");
        args[1] = LLVMConstVector(bytes, 16);
        args[2] = ir_i8(ir, 0);
        v = LLVMBuildBitCast(B, ir_intrinsic(ir, "llvm.x86.vgf2p8affineqb.128", NULL, args, 3), v2, "");
        x = LLVMBuildExtractElement(B, v, ir_i32(ir, 0), "");
        LLVMBuildRet(B, ir_intrinsic(ir, "llvm.bswap", ir->t_i64, &x, 1));
        return;
    }
    LLVMBuildRet(B, ir_intrinsic(ir, "llvm.bitreverse", ir->t_i64, &x, 1));
}
/* rbit32(x): (u32)(rbit64(x) >> 32) */
static void build_rbit32(SnxIr* ir, LLVMValueRef function) {
    LLVMValueRef x = ir_u64(ir, P(0));
    x = ir_call(ir, ir_helper(ir, SNX_IR_RBIT64), &x, 1);
    LLVMBuildRet(B, ir_u32(ir, LLVMBuildLShr(B, x, ir_i64(ir, 32), "")));
}
static void build_rev16_64(SnxIr* ir, LLVMValueRef function) {
    LLVMBuildRet(B, swap_pairs(ir, P(0), 8, 0x00FF00FF00FF00FFull));
}
static void build_rev32_64(SnxIr* ir, LLVMValueRef function) {
    LLVMValueRef x = P(0), y = ir_call(ir, ir_helper(ir, SNX_IR_BSWAP64), &x, 1);
    LLVMBuildRet(B,
                 LLVMBuildOr(B, LLVMBuildLShr(B, y, ir_i64(ir, 32), ""), LLVMBuildShl(B, y, ir_i64(ir, 32), ""), ""));
}
static void build_bswap(SnxIr* ir, LLVMValueRef function, LLVMTypeRef t) {
    LLVMValueRef x = P(0);
    LLVMBuildRet(B, ir_intrinsic(ir, "llvm.bswap", t, &x, 1));
}
/* udiv(a,b): b ? a/b : 0 */
static void build_udiv(SnxIr* ir, LLVMValueRef function, LLVMTypeRef t) {
    LLVMValueRef a = P(0), b = P(1), zero = LLVMConstInt(t, 0, 0);
    LLVMBasicBlockRef divide = ir_block(ir, "divide"), none = ir_block(ir, "zero");
    LLVMBuildCondBr(B, LLVMBuildICmp(B, LLVMIntNE, b, zero, ""), divide, none);
    LLVMPositionBuilderAtEnd(B, divide);
    LLVMBuildRet(B, LLVMBuildUDiv(B, a, b, ""));
    LLVMPositionBuilderAtEnd(B, none);
    LLVMBuildRet(B, zero);
}
/* sdiv(a,b): !b -> 0, b==-1 -> 0-a (no INT_MIN/-1 trap), else a/b */
static void build_sdiv(SnxIr* ir, LLVMValueRef function, LLVMTypeRef t) {
    LLVMValueRef a = P(0), b = P(1), zero = LLVMConstInt(t, 0, 0);
    LLVMBasicBlockRef none = ir_block(ir, "zero"), test = ir_block(ir, "test"), negate = ir_block(ir, "negate"),
                      divide = ir_block(ir, "divide");
    LLVMBuildCondBr(B, LLVMBuildICmp(B, LLVMIntEQ, b, zero, ""), none, test);
    LLVMPositionBuilderAtEnd(B, none);
    LLVMBuildRet(B, zero);
    LLVMPositionBuilderAtEnd(B, test);
    LLVMBuildCondBr(B, LLVMBuildICmp(B, LLVMIntEQ, b, LLVMConstAllOnes(t), ""), negate, divide);
    LLVMPositionBuilderAtEnd(B, negate);
    LLVMBuildRet(B, LLVMBuildSub(B, zero, a, ""));
    LLVMPositionBuilderAtEnd(B, divide);
    LLVMBuildRet(B, LLVMBuildSDiv(B, a, b, ""));
}
/* umulh64(a,b): (u64)(((u128)a * b) >> 64) */
static void build_umulh64(SnxIr* ir, LLVMValueRef function) {
    LLVMValueRef a = ir_cast(ir, P(0), ir->t_i128, 0), b = ir_cast(ir, P(1), ir->t_i128, 0);
    LLVMValueRef r = LLVMBuildLShr(B, LLVMBuildMul(B, a, b, ""), LLVMConstInt(ir->t_i128, 64, 0), "");
    LLVMBuildRet(B, LLVMBuildTrunc(B, r, ir->t_i64, ""));
}
/* smulh64(a,b): umulh64, minus b when a < 0, minus a when b < 0 */
static void build_smulh64(SnxIr* ir, LLVMValueRef function) {
    LLVMValueRef a = P(0), b = P(1), args[2], zero = ir_i64(ir, 0);
    args[0] = a;
    args[1] = b;
    LLVMValueRef r = ir_call(ir, ir_helper(ir, SNX_IR_UMULH64), args, 2);
    r = LLVMBuildSelect(B, LLVMBuildICmp(B, LLVMIntSLT, a, zero, ""), LLVMBuildSub(B, r, b, ""), r, "");
    r = LLVMBuildSelect(B, LLVMBuildICmp(B, LLVMIntSLT, b, zero, ""), LLVMBuildSub(B, r, a, ""), r, "");
    LLVMBuildRet(B, r);
}
/* bmask(n): n >= 64 ? ~0 : (1<<n)-1 */
static void build_bmask(SnxIr* ir, LLVMValueRef function) {
    LLVMValueRef n = P(0);
    LLVMValueRef mask = LLVMBuildSub(B, LLVMBuildShl(B, ir_i64(ir, 1), ir_u64(ir, n), ""), ir_i64(ir, 1), "");
    LLVMBuildRet(
        B, LLVMBuildSelect(B, LLVMBuildICmp(B, LLVMIntUGE, n, ir_i32(ir, 64), ""), ir_i64(ir, ~(SnxU64)0), mask, ""));
}
static LLVMValueRef call_bmask(SnxIr* ir, LLVMValueRef n) { return ir_call(ir, ir_helper(ir, SNX_IR_BMASK), &n, 1); }
/* ubfm_(src,immr,imms,ds) */
static void build_ubfm(SnxIr* ir, LLVMValueRef function) {
    LLVMValueRef src = P(0), immr = P(1), imms = P(2), ds = P(3), one = ir_i32(ir, 1);
    LLVMBasicBlockRef low = ir_block(ir, "low"), high = ir_block(ir, "high");
    LLVMBuildCondBr(B, LLVMBuildICmp(B, LLVMIntUGE, imms, immr, ""), low, high);
    LLVMPositionBuilderAtEnd(B, low);
    LLVMBuildRet(B, LLVMBuildAnd(B, LLVMBuildLShr(B, src, ir_u64(ir, immr), ""),
                                 call_bmask(ir, LLVMBuildAdd(B, LLVMBuildSub(B, imms, immr, ""), one, "")), ""));
    LLVMPositionBuilderAtEnd(B, high);
    LLVMBuildRet(B, LLVMBuildShl(B, LLVMBuildAnd(B, src, call_bmask(ir, LLVMBuildAdd(B, imms, one, "")), ""),
                                 ir_u64(ir, LLVMBuildSub(B, ds, immr, "")), ""));
}
/* sbfm_(src,immr,imms,ds) */
static void build_sbfm(SnxIr* ir, LLVMValueRef function) {
    LLVMValueRef src = P(0), immr = P(1), imms = P(2), ds = P(3), one = ir_i32(ir, 1), phis[2];
    LLVMValueRef values[2];
    LLVMBasicBlockRef low = ir_block(ir, "low"), high = ir_block(ir, "high"), join = ir_block(ir, "join"),
                      sign = ir_block(ir, "sign"), done = ir_block(ir, "done"), blocks[2];
    LLVMBuildCondBr(B, LLVMBuildICmp(B, LLVMIntUGE, imms, immr, ""), low, high);
    LLVMPositionBuilderAtEnd(B, low);
    LLVMValueRef r1 = LLVMBuildAnd(B, LLVMBuildLShr(B, src, ir_u64(ir, immr), ""),
                                   call_bmask(ir, LLVMBuildAdd(B, LLVMBuildSub(B, imms, immr, ""), one, "")), "");
    LLVMValueRef top1 = LLVMBuildSub(B, imms, immr, "");
    LLVMBuildBr(B, join);
    LLVMPositionBuilderAtEnd(B, high);
    LLVMValueRef r2 = LLVMBuildShl(B, LLVMBuildAnd(B, src, call_bmask(ir, LLVMBuildAdd(B, imms, one, "")), ""),
                                   ir_u64(ir, LLVMBuildSub(B, ds, immr, "")), "");
    LLVMValueRef top2 = LLVMBuildAdd(B, LLVMBuildSub(B, ds, immr, ""), imms, "");
    LLVMBuildBr(B, join);
    LLVMPositionBuilderAtEnd(B, join);
    blocks[0] = low;
    blocks[1] = high;
    LLVMValueRef r = LLVMBuildPhi(B, ir->t_i64, "");
    values[0] = r1;
    values[1] = r2;
    LLVMAddIncoming(r, values, blocks, 2);
    LLVMValueRef top = LLVMBuildPhi(B, ir->t_i32, "");
    values[0] = top1;
    values[1] = top2;
    LLVMAddIncoming(top, values, blocks, 2);
    LLVMBuildCondBr(B, LLVMBuildTrunc(B, LLVMBuildLShr(B, r, ir_u64(ir, top), ""), ir->t_i1, ""), sign, done);
    LLVMPositionBuilderAtEnd(B, sign);
    r1 = LLVMBuildOr(B, r, LLVMBuildNot(B, call_bmask(ir, LLVMBuildAdd(B, top, one, "")), ""), "");
    LLVMBuildBr(B, done);
    LLVMPositionBuilderAtEnd(B, done);
    phis[0] = LLVMBuildPhi(B, ir->t_i64, "");
    values[0] = r;
    values[1] = r1;
    blocks[0] = join;
    blocks[1] = sign;
    LLVMAddIncoming(phis[0], values, blocks, 2);
    LLVMBuildRet(B, LLVMBuildSelect(B, LLVMBuildICmp(B, LLVMIntEQ, ds, ir_i32(ir, 32), ""),
                                    ir_u64(ir, ir_u32(ir, phis[0])), phis[0], ""));
}
/* bfm_(dst,src,immr,imms,ds) */
static void build_bfm(SnxIr* ir, LLVMValueRef function) {
    LLVMValueRef dst = P(0), src = P(1), immr = P(2), imms = P(3), ds = P(4), one = ir_i32(ir, 1), m, r1, r2, r,
                 values[2];
    LLVMBasicBlockRef low = ir_block(ir, "low"), high = ir_block(ir, "high"), join = ir_block(ir, "join"), blocks[2];
    LLVMBuildCondBr(B, LLVMBuildICmp(B, LLVMIntUGE, imms, immr, ""), low, high);
    LLVMPositionBuilderAtEnd(B, low);
    m = call_bmask(ir, LLVMBuildAdd(B, LLVMBuildSub(B, imms, immr, ""), one, ""));
    r1 = LLVMBuildOr(B, LLVMBuildAnd(B, dst, LLVMBuildNot(B, m, ""), ""),
                     LLVMBuildAnd(B, LLVMBuildLShr(B, src, ir_u64(ir, immr), ""), m, ""), "");
    LLVMBuildBr(B, join);
    LLVMPositionBuilderAtEnd(B, high);
    {
        LLVMValueRef p = ir_u64(ir, LLVMBuildSub(B, ds, immr, ""));
        m = LLVMBuildShl(B, call_bmask(ir, LLVMBuildAdd(B, imms, one, "")), p, "");
        r2 = LLVMBuildOr(B, LLVMBuildAnd(B, dst, LLVMBuildNot(B, m, ""), ""),
                         LLVMBuildAnd(B, LLVMBuildShl(B, src, p, ""), m, ""), "");
    }
    LLVMBuildBr(B, join);
    LLVMPositionBuilderAtEnd(B, join);
    r = LLVMBuildPhi(B, ir->t_i64, "");
    values[0] = r1;
    values[1] = r2;
    blocks[0] = low;
    blocks[1] = high;
    LLVMAddIncoming(r, values, blocks, 2);
    LLVMBuildRet(
        B, LLVMBuildSelect(B, LLVMBuildICmp(B, LLVMIntEQ, ds, ir_i32(ir, 32), ""), ir_u64(ir, ir_u32(ir, r)), r, ""));
}

/* a switch over an access size 0..3 (u8..u64): one block per size, joined with a phi of i64 results */
typedef struct {
    LLVMBasicBlockRef blocks[4], join;
    LLVMValueRef values[4];
} SizeSwitch;
static void size_switch(SnxIr* ir, LLVMValueRef size, SizeSwitch* s) {
    for (unsigned i = 0; i < 4; ++i) s->blocks[i] = ir_block(ir, "size");
    s->join = ir_block(ir, "join");
    LLVMValueRef sw = LLVMBuildSwitch(B, size, s->blocks[3], 3); /* default: the 64-bit case */
    for (unsigned i = 0; i < 3; ++i) LLVMAddCase(sw, ir_i32(ir, i), s->blocks[i]);
}
static LLVMValueRef size_join(SnxIr* ir, SizeSwitch* s, LLVMTypeRef type) {
    LLVMPositionBuilderAtEnd(B, s->join);
    LLVMValueRef phi = LLVMBuildPhi(B, type, "");
    LLVMAddIncoming(phi, s->values, s->blocks, 4);
    return phi;
}
/* ldx(c,M,a,sz) */
static void build_ldx(SnxIr* ir, LLVMValueRef function) {
    LLVMValueRef a = P(2), sz = P(3);
    SizeSwitch s;
    ir->c = P(0);
    ir->M = P(1);
    size_switch(ir, sz, &s);
    for (unsigned i = 0; i < 4; ++i) {
        LLVMPositionBuilderAtEnd(B, s.blocks[i]);
        s.values[i] = ir_u64(ir, ir_ldacq(ir, a, 8u << i));
        LLVMBuildBr(B, s.join);
    }
    LLVMValueRef v = size_join(ir, &s, ir->t_i64);
    ir_cpu_store(ir, a, offsetof(CPU, excl_addr));
    ir_cpu_store(ir, v, offsetof(CPU, excl_val));
    ir_cpu_store(ir, sz, offsetof(CPU, excl_size));
    ir_cpu_store(ir, ir_i32(ir, 1), offsetof(CPU, excl_on));
    LLVMBuildRet(B, v);
}
/* snx_cas8..64(p,o,n): __atomic_compare_exchange_n, strong, seq_cst */
static LLVMValueRef cas(SnxIr* ir, LLVMValueRef address, unsigned bits, LLVMValueRef expected, LLVMValueRef value) {
    LLVMTypeRef t = LLVMIntTypeInContext(ir->ctx, bits);
    /* the values are numbers, memory holds the guest's little-endian bytes (ir_ldacq swapped excl_val too) */
    LLVMValueRef x = LLVMBuildAtomicCmpXchg(B, ir_mp(ir, address), ir_guest_order(ir, ir_cast(ir, expected, t, 0)),
                                            ir_guest_order(ir, ir_cast(ir, value, t, 0)),
                                            LLVMAtomicOrderingSequentiallyConsistent,
                                            LLVMAtomicOrderingSequentiallyConsistent, 0);
    LLVMSetAlignment(x, bits / 8);
    return ir_bool32(ir, LLVMBuildExtractValue(B, x, 1, ""));
}
static LLVMValueRef exclusive_match(SnxIr* ir, LLVMValueRef a, LLVMValueRef size) {
    /* The monitor is armed for an address, and a store-exclusive succeeds when that address is the same and the
       monitor's range covers the store's: the console allows a store-exclusive at a smaller size than the load that
       armed it, which games use to change part of a wider counter in place. Requiring the sizes to be equal made
       such a store never land, so the game's retry loop spun on it for good and it never reached its own game. */
    LLVMValueRef on = ir_cpu_load(ir, ir->t_i32, offsetof(CPU, excl_on));
    LLVMValueRef address = ir_cpu_load(ir, ir->t_i64, offsetof(CPU, excl_addr));
    LLVMValueRef have = ir_cpu_load(ir, ir->t_i32, offsetof(CPU, excl_size));
    /* the two size bits and the pair's flag: a pair's load sets the flag and its store names it too, so both sides
       mask it off before the sizes are compared (a pair, 0x13, covers a single's 0x03 and so succeeds) */
    LLVMValueRef mask = ir_i32(ir, 0x13);
    LLVMValueRef armed_size =
        LLVMBuildShl(B, ir_i64(ir, 8), LLVMBuildSExt(B, LLVMBuildAnd(B, have, mask, ""), ir->t_i64, ""), "");
    LLVMValueRef this_size =
        LLVMBuildShl(B, ir_i64(ir, 8), LLVMBuildSExt(B, LLVMBuildAnd(B, size, mask, ""), ir->t_i64, ""), "");
    return LLVMBuildAnd(B,
                        LLVMBuildAnd(B, LLVMBuildICmp(B, LLVMIntNE, on, ir_i32(ir, 0), ""),
                                     LLVMBuildICmp(B, LLVMIntEQ, address, a, ""), ""),
                        LLVMBuildICmp(B, LLVMIntULE, this_size, armed_size, ""), "");
}
/* stx(c,M,a,sz,v) */
static void build_stx(SnxIr* ir, LLVMValueRef function) {
    LLVMValueRef a = P(2), sz = P(3), v = P(4), values[2], expected;
    LLVMBasicBlockRef attempt = NULL, blocks[2];
    SizeSwitch s;
    ir->c = P(0);
    ir->M = P(1);
    LLVMBasicBlockRef start = LLVMGetInsertBlock(B);
    attempt = ir_block(ir, "attempt");
    LLVMBasicBlockRef finish = ir_block(ir, "finish");
    LLVMBuildCondBr(B, exclusive_match(ir, a, sz), attempt, finish);
    LLVMPositionBuilderAtEnd(B, attempt);
    size_switch(ir, sz, &s);
    for (unsigned i = 0; i < 4; ++i) {
        LLVMPositionBuilderAtEnd(B, s.blocks[i]);
        expected = ir_cpu_load(ir, ir->t_i64, offsetof(CPU, excl_val));
        s.values[i] = cas(ir, a, 8u << i, expected, v);
        LLVMBuildBr(B, s.join);
    }
    LLVMValueRef ok = size_join(ir, &s, ir->t_i32);
    LLVMBuildBr(B, finish);
    LLVMPositionBuilderAtEnd(B, finish);
    values[0] = ir_i32(ir, 0);
    values[1] = ok;
    blocks[0] = start;
    blocks[1] = s.join;
    ok = LLVMBuildPhi(B, ir->t_i32, "");
    LLVMAddIncoming(ok, values, blocks, 2);
    ir_cpu_store(ir, ir_i32(ir, 0), offsetof(CPU, excl_on));
    LLVMBuildRet(
        B, LLVMBuildSelect(B, LLVMBuildICmp(B, LLVMIntNE, ok, ir_i32(ir, 0), ""), ir_i32(ir, 0), ir_i32(ir, 1), ""));
}
/* ldxp(c,M,a,sz,&v1,&v2) */
static void build_ldxp(SnxIr* ir, LLVMValueRef function) {
    LLVMValueRef a = P(2), sz = P(3), v1 = P(4), v2 = P(5);
    LLVMBasicBlockRef pair32 = ir_block(ir, "pair32"), pair64 = ir_block(ir, "pair64"), done = ir_block(ir, "done");
    ir->c = P(0);
    ir->M = P(1);
    LLVMBuildCondBr(B, LLVMBuildICmp(B, LLVMIntEQ, sz, ir_i32(ir, 2), ""), pair32, pair64);
    LLVMPositionBuilderAtEnd(B, pair32);
    LLVMValueRef t = ir_ldacq(ir, a, 64);
    ir_store(ir, ir_u64(ir, ir_u32(ir, t)), v1);
    ir_store(ir, LLVMBuildLShr(B, t, ir_i64(ir, 32), ""), v2);
    ir_cpu_store(ir, t, offsetof(CPU, excl_val));
    ir_cpu_store(ir, ir_i64(ir, 0), offsetof(CPU, excl_val2));
    LLVMBuildBr(B, done);
    LLVMPositionBuilderAtEnd(B, pair64);
    LLVMValueRef first = ir_ldacq(ir, a, 64);
    ir_store(ir, first, v1);
    LLVMValueRef second = ir_ldacq(ir, LLVMBuildAdd(B, a, ir_i64(ir, 8), ""), 64);
    ir_store(ir, second, v2);
    ir_cpu_store(ir, ir_load(ir, ir->t_i64, v1), offsetof(CPU, excl_val));
    ir_cpu_store(ir, ir_load(ir, ir->t_i64, v2), offsetof(CPU, excl_val2));
    LLVMBuildBr(B, done);
    LLVMPositionBuilderAtEnd(B, done);
    ir_cpu_store(ir, a, offsetof(CPU, excl_addr));
    ir_cpu_store(ir, LLVMBuildOr(B, sz, ir_i32(ir, 0x10), ""), offsetof(CPU, excl_size));
    ir_cpu_store(ir, ir_i32(ir, 1), offsetof(CPU, excl_on));
    LLVMBuildRetVoid(B);
}
/* stxp(c,M,a,sz,v1,v2) */
static void build_stxp(SnxIr* ir, LLVMValueRef function) {
    LLVMValueRef a = P(2), sz = P(3), v1 = P(4), v2 = P(5), values[3], args[6];
    LLVMTypeRef params[6];
    LLVMBasicBlockRef start, attempt = ir_block(ir, "attempt"), pair32 = ir_block(ir, "pair32"),
                             pair64 = ir_block(ir, "pair64"), finish = ir_block(ir, "finish"), blocks[3];
    ir->c = P(0);
    ir->M = P(1);
    start = LLVMGetInsertBlock(B);
    LLVMBuildCondBr(B, exclusive_match(ir, a, LLVMBuildOr(B, sz, ir_i32(ir, 0x10), "")), attempt, finish);
    LLVMPositionBuilderAtEnd(B, attempt);
    LLVMBuildCondBr(B, LLVMBuildICmp(B, LLVMIntEQ, sz, ir_i32(ir, 2), ""), pair32, pair64);
    LLVMPositionBuilderAtEnd(B, pair32);
    LLVMValueRef packed = LLVMBuildOr(B, ir_u64(ir, ir_u32(ir, v1)), LLVMBuildShl(B, v2, ir_i64(ir, 32), ""), "");
    LLVMValueRef ok32 = cas(ir, a, 64, ir_cpu_load(ir, ir->t_i64, offsetof(CPU, excl_val)), packed);
    LLVMBuildBr(B, finish);
    LLVMPositionBuilderAtEnd(B, pair64);
    params[0] = params[1] = ir->t_ptr;
    params[2] = params[3] = params[4] = params[5] = ir->t_i64;
    args[0] = ir->c; /* RISC-V, LoongArch and MIPS64 reach the runtime's compare-and-swap through it (cpu.h) */
    args[1] = ir_mp(ir, a);
    args[2] = ir_cpu_load(ir, ir->t_i64, offsetof(CPU, excl_val));
    args[3] = ir_cpu_load(ir, ir->t_i64, offsetof(CPU, excl_val2));
    args[4] = v1;
    args[5] = v2;
    LLVMValueRef ok128 = ir_call(ir, ir_extern(ir, "snx_cas128", ir->t_i32, params, 6), args, 6);
    LLVMBuildBr(B, finish);
    LLVMPositionBuilderAtEnd(B, finish);
    LLVMValueRef ok = LLVMBuildPhi(B, ir->t_i32, "");
    values[0] = ir_i32(ir, 0);
    values[1] = ok32;
    values[2] = ok128;
    blocks[0] = start;
    blocks[1] = pair32;
    blocks[2] = pair64;
    LLVMAddIncoming(ok, values, blocks, 3);
    ir_cpu_store(ir, ir_i32(ir, 0), offsetof(CPU, excl_on));
    LLVMBuildRet(
        B, LLVMBuildSelect(B, LLVMBuildICmp(B, LLVMIntNE, ok, ir_i32(ir, 0), ""), ir_i32(ir, 0), ir_i32(ir, 1), ""));
}
/* snx_lookup(c,pc) */
static void build_lookup(SnxIr* ir, LLVMValueRef function) {
    LLVMValueRef pc = P(1);
    LLVMBasicBlockRef table = ir_block(ir, "table"), found = ir_block(ir, "found"), none = ir_block(ir, "none");
    ir->c = P(0);
    LLVMValueRef lo = ir_cpu_load(ir, ir->t_i64, offsetof(CPU, code_lo));
    LLVMValueRef hi = ir_cpu_load(ir, ir->t_i64, offsetof(CPU, code_hi));
    LLVMValueRef o = LLVMBuildSub(B, pc, lo, "");
    LLVMValueRef inside = LLVMBuildAnd(B, LLVMBuildICmp(B, LLVMIntULT, o, LLVMBuildSub(B, hi, lo, ""), ""),
                                       LLVMBuildICmp(B, LLVMIntEQ, LLVMBuildAnd(B, pc, ir_i64(ir, 3), ""),
                                                     ir_i64(ir, 0), ""),
                                       "");
    ir_likely(ir, LLVMBuildCondBr(B, inside, table, none));
    LLVMPositionBuilderAtEnd(B, table);
    LLVMValueRef index = LLVMBuildLShr(B, o, ir_i64(ir, 2), "");
    /* __atomic_load_n(&c->disp[o>>2],__ATOMIC_RELAXED): the runtime JIT fills slots while this runs (cpu.h) */
    LLVMValueRef slot = LLVMBuildGEP2(B, ir->t_ptr, ir_cpu_load(ir, ir->t_ptr, offsetof(CPU, disp)), &index, 1, "");
    LLVMValueRef f = LLVMBuildLoad2(B, ir->t_ptr, slot, "");
    LLVMSetOrdering(f, LLVMAtomicOrderingMonotonic);
    LLVMSetAlignment(f, sizeof(void*));
    ir_likely(ir, LLVMBuildCondBr(B, LLVMBuildIsNotNull(B, f, ""), found, none));
    LLVMPositionBuilderAtEnd(B, found);
    LLVMBuildRet(B, f);
    LLVMPositionBuilderAtEnd(B, none);
    /* c->rt->miss */
    index = ir_i64(ir, ir_rt_miss());
    LLVMBuildRet(
        B, ir_load(ir, ir->t_ptr,
                   LLVMBuildInBoundsGEP2(B, ir->t_i8, ir_cpu_load(ir, ir->t_ptr, offsetof(CPU, rt)), &index, 1, "")));
}
/* dc_zva(M,a): memset(MP(a & ~63), 0, 64) */
static void build_dc_zva(SnxIr* ir, LLVMValueRef function) {
    ir->M = P(0);
    {
        LLVMValueRef a = LLVMBuildAnd(B, P(1), ir_i64(ir, ~(SnxU64)63), ""),
                     zero = LLVMConstNull(LLVMVectorType(ir->t_i8, 16));
        for (unsigned i = 0; i < 4; ++i) ir_wr_bytes(ir, LLVMBuildAdd(B, a, ir_i64(ir, 16 * i), ""), zero);
    }
    LLVMBuildRetVoid(B);
}
/* vfp_imm32(i) / vfp_imm64(i) */
static void build_vfp_imm(SnxIr* ir, LLVMValueRef function, int wide) {
    LLVMTypeRef t = wide ? ir->t_i64 : ir->t_i32;
    LLVMValueRef i = ir_cast(ir, P(0), t, 0);
#define K(v) LLVMConstInt(t, v, 0)
    LLVMValueRef b6 = LLVMBuildAnd(B, LLVMBuildLShr(B, i, K(6), ""), K(1), "");
    LLVMValueRef e = LLVMBuildOr(B, LLVMBuildShl(B, LLVMBuildXor(B, b6, K(1), ""), K(wide ? 10 : 7), ""),
                                 LLVMBuildOr(B,
                                             LLVMBuildShl(B,
                                                          LLVMBuildSelect(B, LLVMBuildICmp(B, LLVMIntNE, b6, K(0), ""),
                                                                          K(wide ? 0xFF : 0x1F), K(0), ""),
                                                          K(2), ""),
                                             LLVMBuildAnd(B, LLVMBuildLShr(B, i, K(4), ""), K(3), ""), ""),
                                 "");
    LLVMValueRef r = LLVMBuildOr(B, LLVMBuildShl(B, LLVMBuildLShr(B, i, K(7), ""), K(wide ? 63 : 31), ""),
                                 LLVMBuildOr(B, LLVMBuildShl(B, e, K(wide ? 52 : 23), ""),
                                             LLVMBuildShl(B, LLVMBuildAnd(B, i, K(15), ""), K(wide ? 48 : 19), ""), ""),
                                 "");
#undef K
    LLVMBuildRet(B, r);
}

LLVMValueRef ir_helper(SnxIr* ir, int id) {
    LLVMTypeRef i32 = ir->t_i32, i64 = ir->t_i64, ptr = ir->t_ptr, p[6];
    LLVMValueRef function;
    Saved saved;
    if (ir->helpers[id]) return ir->helpers[id];
    switch (id) {
#define BEGIN(name, ret, n) function = helper_begin(ir, name, ret, p, n, &saved)
        case SNX_IR_ADDS64:
            p[0] = ptr, p[1] = i64, p[2] = i64, p[3] = i32;
            BEGIN("adds64", i64, 4);
            ir->c = P(0);
            build_adds(ir, function, i64, 64);
            break;
        case SNX_IR_ADDS32:
            p[0] = ptr, p[1] = i32, p[2] = i32, p[3] = i32;
            BEGIN("adds32", i32, 4);
            ir->c = P(0);
            build_adds(ir, function, i32, 32);
            break;
        case SNX_IR_SUBS64:
            p[0] = ptr, p[1] = i64, p[2] = i64;
            BEGIN("subs64", i64, 3);
            ir->c = P(0);
            build_subs(ir, function, i64, 64);
            break;
        case SNX_IR_SUBS32:
            p[0] = ptr, p[1] = i32, p[2] = i32;
            BEGIN("subs32", i32, 3);
            ir->c = P(0);
            build_subs(ir, function, i32, 32);
            break;
        case SNX_IR_LOGIC64:
            p[0] = ptr, p[1] = i64;
            BEGIN("logic64", ir->t_void, 2);
            ir->c = P(0);
            build_logic(ir, function, i64, 64);
            break;
        case SNX_IR_LOGIC32:
            p[0] = ptr, p[1] = i32;
            BEGIN("logic32", ir->t_void, 2);
            ir->c = P(0);
            build_logic(ir, function, i32, 32);
            break;
        case SNX_IR_COND_OK:
            p[0] = ptr, p[1] = i32;
            BEGIN("cond_ok", i32, 2);
            ir->c = P(0);
            build_cond_ok(ir, function);
            break;
        case SNX_IR_SET_NZCV:
            p[0] = ptr, p[1] = i32;
            BEGIN("set_nzcv", ir->t_void, 2);
            ir->c = P(0);
            build_set_nzcv(ir, function);
            break;
        case SNX_IR_ROR64:
            p[0] = i64, p[1] = i32;
            BEGIN("ror64", i64, 2);
            build_ror(ir, function, i64, 64);
            break;
        case SNX_IR_ROR32:
            p[0] = i32, p[1] = i32;
            BEGIN("ror32", i32, 2);
            build_ror(ir, function, i32, 32);
            break;
        case SNX_IR_SHF64:
            p[0] = i64, p[1] = i32, p[2] = i32;
            BEGIN("shf64", i64, 3);
            build_shf(ir, function, i64, SNX_IR_ROR64);
            break;
        case SNX_IR_SHF32:
            p[0] = i32, p[1] = i32, p[2] = i32;
            BEGIN("shf32", i32, 3);
            build_shf(ir, function, i32, SNX_IR_ROR32);
            break;
        case SNX_IR_EXT64:
            p[0] = i64, p[1] = i32, p[2] = i32;
            BEGIN("ext64", i64, 3);
            build_ext64(ir, function);
            break;
        case SNX_IR_CLZ64:
            p[0] = i64;
            BEGIN("clz64", i32, 1);
            build_clz(ir, function, i64);
            break;
        case SNX_IR_CLZ32:
            p[0] = i32;
            BEGIN("clz32", i32, 1);
            build_clz(ir, function, i32);
            break;
        case SNX_IR_CLS64:
            p[0] = i64;
            BEGIN("cls64", i32, 1);
            build_cls(ir, function, i64, SNX_IR_CLZ64);
            break;
        case SNX_IR_CLS32:
            p[0] = i32;
            BEGIN("cls32", i32, 1);
            build_cls(ir, function, i32, SNX_IR_CLZ32);
            break;
        case SNX_IR_RBIT64:
            p[0] = i64;
            BEGIN("rbit64", i64, 1);
            build_rbit64(ir, function);
            break;
        case SNX_IR_RBIT32:
            p[0] = i32;
            BEGIN("rbit32", i32, 1);
            build_rbit32(ir, function);
            break;
        case SNX_IR_REV16_64:
            p[0] = i64;
            BEGIN("rev16_64", i64, 1);
            build_rev16_64(ir, function);
            break;
        case SNX_IR_REV32_64:
            p[0] = i64;
            BEGIN("rev32_64", i64, 1);
            build_rev32_64(ir, function);
            break;
        case SNX_IR_BSWAP32:
            p[0] = i32;
            BEGIN("bswap32", i32, 1);
            build_bswap(ir, function, i32);
            break;
        case SNX_IR_BSWAP64:
            p[0] = i64;
            BEGIN("bswap64", i64, 1);
            build_bswap(ir, function, i64);
            break;
        case SNX_IR_UDIV64:
            p[0] = i64, p[1] = i64;
            BEGIN("udiv64", i64, 2);
            build_udiv(ir, function, i64);
            break;
        case SNX_IR_UDIV32:
            p[0] = i32, p[1] = i32;
            BEGIN("udiv32", i32, 2);
            build_udiv(ir, function, i32);
            break;
        case SNX_IR_SDIV64:
            p[0] = i64, p[1] = i64;
            BEGIN("sdiv64", i64, 2);
            build_sdiv(ir, function, i64);
            break;
        case SNX_IR_SDIV32:
            p[0] = i32, p[1] = i32;
            BEGIN("sdiv32", i32, 2);
            build_sdiv(ir, function, i32);
            break;
        case SNX_IR_UMULH64:
            p[0] = i64, p[1] = i64;
            BEGIN("umulh64", i64, 2);
            build_umulh64(ir, function);
            break;
        case SNX_IR_SMULH64:
            p[0] = i64, p[1] = i64;
            BEGIN("smulh64", i64, 2);
            build_smulh64(ir, function);
            break;
        case SNX_IR_BMASK:
            p[0] = i32;
            BEGIN("bmask", i64, 1);
            build_bmask(ir, function);
            break;
        case SNX_IR_UBFM:
            p[0] = i64, p[1] = i32, p[2] = i32, p[3] = i32;
            BEGIN("ubfm_", i64, 4);
            build_ubfm(ir, function);
            break;
        case SNX_IR_SBFM:
            p[0] = i64, p[1] = i32, p[2] = i32, p[3] = i32;
            BEGIN("sbfm_", i64, 4);
            build_sbfm(ir, function);
            break;
        case SNX_IR_BFM:
            p[0] = i64, p[1] = i64, p[2] = i32, p[3] = i32, p[4] = i32;
            BEGIN("bfm_", i64, 5);
            build_bfm(ir, function);
            break;
        case SNX_IR_LDX:
            p[0] = ptr, p[1] = ptr, p[2] = i64, p[3] = i32;
            BEGIN("ldx", i64, 4);
            ir_noregs(ir, function); /* the exclusive monitor alone */
            build_ldx(ir, function);
            break;
        case SNX_IR_STX:
            p[0] = ptr, p[1] = ptr, p[2] = i64, p[3] = i32, p[4] = i64;
            BEGIN("stx", i32, 5);
            ir_noregs(ir, function); /* the exclusive monitor alone */
            build_stx(ir, function);
            break;
        case SNX_IR_LDXP:
            p[0] = ptr, p[1] = ptr, p[2] = i64, p[3] = i32, p[4] = ptr, p[5] = ptr;
            BEGIN("ldxp", ir->t_void, 6);
            ir_noregs(ir, function); /* the exclusive monitor alone */
            build_ldxp(ir, function);
            break;
        case SNX_IR_STXP:
            p[0] = ptr, p[1] = ptr, p[2] = i64, p[3] = i32, p[4] = i64, p[5] = i64;
            BEGIN("stxp", i32, 6);
            ir_noregs(ir, function); /* the exclusive monitor alone */
            build_stxp(ir, function);
            break;
        case SNX_IR_LOOKUP:
            p[0] = ptr, p[1] = i64;
            BEGIN("snx_lookup", ptr, 2);
            build_lookup(ir, function);
            break;
        case SNX_IR_DC_ZVA:
            p[0] = ptr, p[1] = i64;
            BEGIN("dc_zva", ir->t_void, 2);
            ir_nocpu(ir, function); /* M and the address: no register */
            build_dc_zva(ir, function);
            break;
        case SNX_IR_VFP_IMM32:
            p[0] = i32;
            BEGIN("vfp_imm32", i32, 1);
            build_vfp_imm(ir, function, 0);
            break;
        case SNX_IR_VFP_IMM64:
            p[0] = i32;
            BEGIN("vfp_imm64", i64, 1);
            build_vfp_imm(ir, function, 1);
            break;
        default: return NULL;
#undef BEGIN
    }
    helper_end(ir, &saved);
    ir->helpers[id] = function;
    return function;
}

LLVMValueRef ir_call_helper(SnxIr* ir, int id, LLVMValueRef* args, unsigned count) {
    return ir_call(ir, ir_helper(ir, id), args, count);
}
