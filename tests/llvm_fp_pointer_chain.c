/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A vector helper's temporary pointers must not escape through its returned FPSR chain. */
#include "llvm_steps.h"
#include "common/cpu/recompiler/llvm/ir_vec.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int run(unsigned count, int capturing, int bench) {
    LlvmSteps s;
    char error[1024];
    if (llvm_steps_begin_opt(&s, 1, LLVM_STEPS_GENERIC, '1', error, sizeof error)) return snx_error("%s", error);
    SnxIr* ir = s.ir;
    s.fns = snx_alloc(sizeof *s.fns);
    s.count = s.cap = 1;
    s.fns[0] = LLVMAddFunction(ir->mod, "chain", LLVMFunctionType(ir->t_i64, &ir->t_ptr, 1, 0));
    snx_ir_begin(ir, s.fns[0]);
    SnxA64Insn seed;
    u32 seed_word = 0x0ea1d800u; /* Grow an 8-byte unary scratch area into wider binary/FMA inputs. */
    snx_a64_decode_word(seed_word, 0, &seed);
    snx_ir_vec(ir, &seed, seed_word, 0x1000);
    LLVMBasicBlockRef entry = LLVMGetInsertBlock(ir->b), loop = ir_block(ir,"loop"), done = ir_block(ir,"done");
    LLVMBuildBr(ir->b, loop);
    LLVMPositionBuilderAtEnd(ir->b, loop);
    LLVMValueRef iterations = LLVMBuildPhi(ir->b, ir->t_i32,"iterations");
    SnxA64Insn insn;
    u32 word = 0x4e21d400u; /* FADD V0.4S,V0.4S,V1.4S: helper return flags cross guarded joins. */
    snx_a64_decode_word(word, 0, &insn);
    for (unsigned i = 0; i < count; i++) snx_ir_vec(ir, &insn, word, 0x1000);
    LLVMValueRef left = LLVMBuildSub(ir->b, iterations, ir_i32(ir, 1),"");
    LLVMBasicBlockRef back = LLVMGetInsertBlock(ir->b);
    LLVMBuildCondBr(ir->b, LLVMBuildICmp(ir->b, LLVMIntNE, left, ir_i32(ir, 0),""), loop, done);
    LLVMValueRef incoming[] = {ir_i32(ir, 2), left};
    LLVMBasicBlockRef blocks[] = {entry, back};
    LLVMAddIncoming(iterations, incoming, blocks, 2);
    LLVMPositionBuilderAtEnd(ir->b, done);
    SnxA64Insn fma;
    u32 fma_word = 0x4e22cc20u;
    snx_a64_decode_word(fma_word, 0, &fma);
    snx_ir_vec(ir, &fma, fma_word, 0x1000);
    LLVMBuildRet(ir->b, ir_i64(ir, 0x1004));
    unsigned allocas = 0;
    for (LLVMBasicBlockRef block = LLVMGetFirstBasicBlock(s.fns[0]); block; block = LLVMGetNextBasicBlock(block))
        for (LLVMValueRef value = LLVMGetFirstInstruction(block); value; value = LLVMGetNextInstruction(value))
            allocas += LLVMIsAAllocaInst(value) != NULL;
    if (allocas != 1 || !ir->fp_lanes_scratch || LLVMConstIntGetZExtValue(LLVMGetOperand(ir->fp_lanes_scratch, 0)) != 48)
        return snx_error("pointer chain needs %u allocations instead of one bounded 48-byte scratch area", allocas);
    ir_promote_registers(ir, s.fns[0]);
    if (capturing) {
        LLVMValueRef helper = LLVMGetNamedFunction(ir->mod, "snx_fp_host_lanes");
        unsigned kind = LLVMGetEnumAttributeKindForName("captures", 8);
        if (!kind) kind = LLVMGetEnumAttributeKindForName("nocapture", 9);
        for (unsigned i = 1; i <= 4; i++) LLVMRemoveEnumAttributeAtIndex(helper, i, kind);
    }
    double start = snx_seconds();
    if (llvm_steps_load(&s, error, sizeof error)) return snx_error("%s", error);
    double elapsed = snx_seconds() - start;
    CPU input = {0};
    input.pc = 0x1000;
    input.fpsr = 0x08000080u;
    input.v[0] = (V128){.s = {0x3f800000u, 0x7f812345u, 0xbf800000u, 0x7f812345u}};
    input.v[1] = (V128){.s = {0x33800000u, 0, 0x3f800000u, 0x3f800000u}};
    input.v[2] = (V128){.s = {0x3f800000u, 0x3f800000u, 0x3f800000u, 0x3f800000u}};
    CPU actual = input, expected = input;
    for (unsigned lane = 0; lane < 2; lane++) expected.v[0].s[lane] = (u32) snx_fp_unary_c(&expected, expected.v[0].s[lane], 32, SNX_FP_RECPE);
    expected.v[0].d[1] = 0;
    for (unsigned i = 0; i < 2 * count; i++) for (unsigned lane = 0; lane < 4; lane++)
        expected.v[0].s[lane] = (u32) snx_fp_binary_c(&expected, expected.v[0].s[lane], expected.v[1].s[lane], 32, SNX_FP_ADD);
    for (unsigned lane = 0; lane < 4; lane++) expected.v[0].s[lane] = (u32) snx_fp_fma_c(&expected,
        expected.v[0].s[lane], expected.v[1].s[lane], expected.v[2].s[lane], 32);
    int failed = s.steps[0](&actual) != 0x1004 || memcmp(&actual, &expected, sizeof actual);
    if (failed) fprintf(stderr, "Pointer chain %u returned the wrong CPU state\n", count);
    if (bench && !failed) printf("{\"elapsed_ns\":%.0f,\"checksum\":\"%016llx\"}\n", elapsed*1e9,
        (unsigned long long)(actual.v[0].d[0]^actual.v[0].d[1]^actual.fpsr));
    llvm_steps_end(&s);
    return failed;
}
int main(int argc, char** argv) {
    if (argc >= 3 && !strcmp(argv[1], "--bench")) {
        unsigned long n = strtoul(argv[2], NULL, 10);
        if (!n || n > 8192) return 2;
        return run((unsigned)n, argc > 3 && !strcmp(argv[3], "--capturing"), 1);
    }
    if (run(16, 0, 0) || run(16, 1, 0) || run(1024, 0, 0)) return 1;
    puts("Pointer chains: captured-control and non-capture metadata preserve full CPU state");
    return 0;
}
