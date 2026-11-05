/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Shared vector fixups must preserve the complete CPU, return flags by value, and keep unrelated registers local.
   --dump N PREFIX [--reference] saves the actual object for code-size comparisons against an older backend. */
#include "llvm_steps.h"
#include "common/cpu/recompiler/llvm/ir_vec.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int cold_name(LLVMValueRef function) {
    size_t size;
    const char* name = LLVMGetValueName2(function, &size);
    return size >= 12 && !memcmp(name, "snx_fp_cold_", 12);
}
static int run(int jit, char opt, unsigned count, const char* dump, int reference) {
    LlvmSteps s;
    char error[1024];
    if (llvm_steps_begin_opt(&s, jit, LLVM_STEPS_HOST, opt, error, sizeof error)) return snx_error("%s", error);
    SnxIr* ir = s.ir;
    if (ir->big_endian || (!ir->features->x86 && !ir->features->aarch64)) {
        llvm_steps_end(&s);
        return 77;
    }
    s.fns = snx_alloc(sizeof *s.fns);
    s.count = s.cap = 1;
    s.fns[0] = LLVMAddFunction(ir->mod, "cold_chain", LLVMFunctionType(ir->t_i64, &ir->t_ptr, 1, 0));
    snx_ir_begin(ir, s.fns[0]);
    ir_set_x(ir, 4, ir_i64(ir, 0xfedcba9876543210ull));
    const u32 words[] = {0x4ea2c400u, 0x6e219800u}; /* FMINNM V0.4S,V0.4S,V2.4S; FRINTX V0.4S,V0.4S */
    for (unsigned i = 0; i < count; ++i)
        for (unsigned j = 0; j < 2; ++j) {
            SnxA64Insn insn;
            snx_a64_decode_word(words[j], 0, &insn);
            snx_ir_vec(ir, &insn, words[j], 0x1000);
        }
    ir_set_x(ir, 5, ir_x(ir, 4));
    LLVMBuildRet(ir->b, ir_i64(ir, 0x1004));
    unsigned definitions = 0, calls = 0;
    for (LLVMValueRef fn = LLVMGetFirstFunction(ir->mod); fn; fn = LLVMGetNextFunction(fn))
        if (cold_name(fn) && !LLVMIsDeclaration(fn)) {
            ++definitions;
            unsigned memory = LLVMGetEnumAttributeKindForName("memory", 6);
            LLVMAttributeRef attribute = LLVMGetEnumAttributeAtIndex(fn, LLVMAttributeFunctionIndex, memory);
            if (!attribute || LLVMGetEnumAttributeValue(attribute) || LLVMGetFunctionCallConv(fn) != LLVMFastCallConv)
                return snx_error("cold vector fixup is not a pure internal call");
            for (unsigned i = 0; i < LLVMCountParams(fn); ++i)
                if (LLVMGetTypeKind(LLVMTypeOf(LLVMGetParam(fn, i))) == LLVMPointerTypeKind)
                    return snx_error("cold vector fixup takes an external memory pointer");
        }
    for (LLVMBasicBlockRef block = LLVMGetFirstBasicBlock(s.fns[0]); block; block = LLVMGetNextBasicBlock(block))
        for (LLVMValueRef value = LLVMGetFirstInstruction(block); value; value = LLVMGetNextInstruction(value))
            if (LLVMIsACallInst(value) && LLVMIsAFunction(LLVMGetCalledValue(value)) && cold_name(LLVMGetCalledValue(value)))
                ++calls;
    unsigned expected = reference ? 0 : 1 + (ir->features->aarch64 || ir->features->sse42);
    if (definitions != expected || calls != count * expected)
        return snx_error("cold fixups have %u definitions and %u calls, expected %u and %u",
                         definitions, calls, expected, count * expected);
    ir_promote_registers(ir, s.fns[0]);
    if (dump) {
        int failed = llvm_steps_dump(&s, dump, error, sizeof error);
        if (failed) fprintf(stderr, "%s\n", error);
        llvm_steps_end(&s);
        return failed != 0;
    }
    if (llvm_steps_load(&s, error, sizeof error)) return snx_error("%s", error);
    const u32 modes[] = {0, 1u << 24 | 1u << 25, 1u << 22, 3u << 22};
    int failed = 0;
    for (unsigned mode = 0; mode < sizeof modes / sizeof *modes; ++mode) {
        CPU input = {0};
        input.pc = 0x1000;
        input.fpcr = modes[mode];
        input.fpsr = 0x08000080u;
        input.x[7] = 0x1234;
        input.v[0] = (V128){.s = {0x7f812345u, 1, 0x80000000u, 0x3f99999au}};
        input.v[2] = (V128){.s = {0x3f800000u, 0, 0, 0x40133333u}};
        CPU actual = input, expected_cpu = input;
        expected_cpu.x[4] = expected_cpu.x[5] = 0xfedcba9876543210ull;
        for (unsigned i = 0; i < count; ++i)
            for (unsigned lane = 0; lane < 4; ++lane) {
                SnxFpReply r = snx_fp_minmax(expected_cpu.v[0].s[lane], expected_cpu.v[2].s[lane], 32, 1, 1, modes[mode]);
                expected_cpu.v[0].s[lane] = (u32)r.value;
                expected_cpu.fpsr |= r.flags;
                r = snx_fp_round(expected_cpu.v[0].s[lane], 32, (int)((modes[mode] >> 22) & 3), 1, modes[mode]);
                expected_cpu.v[0].s[lane] = (u32)r.value;
                expected_cpu.fpsr |= r.flags;
            }
        int wrong = s.steps[0](&actual) != 0x1004 || memcmp(&actual, &expected_cpu, sizeof actual);
        if (wrong && !failed) fprintf(stderr, "cold chain jit=%d O%c FPCR=%08x FPSR=%08x/%08x V0=%08x,%08x,%08x,%08x / %08x,%08x,%08x,%08x X4=%llx/%llx X5=%llx/%llx\n",
            jit, opt, modes[mode], actual.fpsr, expected_cpu.fpsr,
            actual.v[0].s[0], actual.v[0].s[1], actual.v[0].s[2], actual.v[0].s[3],
            expected_cpu.v[0].s[0], expected_cpu.v[0].s[1], expected_cpu.v[0].s[2], expected_cpu.v[0].s[3],
            (unsigned long long)actual.x[4], (unsigned long long)expected_cpu.x[4],
            (unsigned long long)actual.x[5], (unsigned long long)expected_cpu.x[5]);
        failed |= wrong;
    }
    if (failed) fprintf(stderr, "Shared vector fixups changed CPU state\n");
    llvm_steps_end(&s);
    return failed;
}
int main(int argc, char** argv) {
    if (argc >= 4 && !strcmp(argv[1], "--dump")) {
        char* end;
        unsigned long count = strtoul(argv[2], &end, 10);
        if (*end || !count || count > 1024) return 2;
        return run(1, '1', (unsigned)count, argv[3], argc > 4 && !strcmp(argv[4], "--reference"));
    }
    for (int jit = 0; jit < 2; ++jit)
        for (char opt = '1'; opt <= '2'; ++opt) {
            int result = run(jit, opt, 32, NULL, argc > 1 && !strcmp(argv[1], "--reference"));
            if (result) return result;
        }
    puts("Shared vector fixups: flags, full CPU state, both loaders and O1/O2 pass");
    return 0;
}
