/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* An FP helper's reply (cpu.h's SnxFpReply, 16 bytes) comes back as the C compiler returns it: on x86-64 Windows
   through memory, the call passing a local as its hidden first argument (sret), as Clang compiles the helpers there;
   elsewhere as an LLVM structure, in two registers. The code runs where it is built, so the triples here are the
   host's architecture's, Windows or not: FADD S0, S1, S2's slow path calls snx_fp_host_binary on each, and the call
   stays no site of ir_promote_registers either way. */
#include "common/cpu/recompiler/llvm/ir_fpu.h"
#include "common/cpu/aarch64/cpu.h"
#include <llvm-c/Analysis.h>
#include <stdio.h>
#include <stdlib.h>

#define EXIT_SKIPPED 77

/* the reply's type, {i64, i32} */
static int is_reply(SnxIr* ir, LLVMTypeRef type) {
    LLVMTypeRef fields[2];
    if (LLVMGetTypeKind(type) != LLVMStructTypeKind || LLVMCountStructElementTypes(type) != 2) return 0;
    LLVMGetStructElementTypes(type, fields);
    return fields[0] == ir->t_i64 && fields[1] == ir->t_i32;
}

/* 0 when the triple's declaration and call are right, 1 when not, EXIT_SKIPPED without its target */
static int check(const char* triple, int in_memory) {
    char error[512], *message = NULL;
    SnxIr* ir = snx_ir_new_target("reply", '2', triple, error, sizeof error);
    if (!ir) {
        fprintf(stderr, "%s skipped: %s\n", triple, error);
        return EXIT_SKIPPED;
    }
    ir->stats.enabled = 1;
    LLVMTypeRef params[] = {ir->t_ptr, ir->t_i64};
    LLVMValueRef fn = LLVMAddFunction(ir->mod, "fadd", LLVMFunctionType(ir->t_void, params, 2, 0));
    snx_ir_begin(ir, fn);
    SnxA64Insn insn;
    u32 word = 0x1E222820u; /* FADD S0, S1, S2 */
    snx_a64_decode_word(word, 0, &insn);
    snx_ir_fpu(ir, &insn, word, 0x1000);
    LLVMBuildRetVoid(ir->b);
    ir_promote_registers(ir, fn);
    ir_stats_function(ir, fn);

    int failed = 0;
    LLVMValueRef helper = LLVMGetNamedFunction(ir->mod, "snx_fp_host_binary");
    LLVMTypeRef type = helper ? LLVMGlobalGetValueType(helper) : NULL;
    unsigned count = type ? LLVMCountParamTypes(type) : 0;
    LLVMAttributeRef sret =
        helper ? LLVMGetEnumAttributeAtIndex(helper, 1, LLVMGetEnumAttributeKindForName("sret", 4)) : NULL;
    if (!helper) {
        fprintf(stderr, "FAIL: %s: FADD calls no snx_fp_host_binary\n", triple);
        failed = 1;
    } else if (in_memory) {
        LLVMTypeRef first[8];
        if (count) LLVMGetParamTypes(type, first);
        if (LLVMGetReturnType(type) != ir->t_void || count != 7 || first[0] != ir->t_ptr || !sret ||
            !is_reply(ir, LLVMGetTypeAttributeValue(sret))) {
            fprintf(stderr, "FAIL: %s: snx_fp_host_binary is not void (ptr sret({i64, i32}), 6 operands)\n", triple);
            failed = 1;
        }
    } else if (!is_reply(ir, LLVMGetReturnType(type)) || count != 6 || sret) {
        fprintf(stderr, "FAIL: %s: snx_fp_host_binary is not {i64, i32} (6 operands)\n", triple);
        failed = 1;
    }
    if (ir->stats.call_stores || ir->stats.sync_reloads) {
        fprintf(stderr, "FAIL: %s: %llu stores and %llu reloads around the helper call\n", triple,
                (unsigned long long)ir->stats.call_stores, (unsigned long long)ir->stats.sync_reloads);
        failed = 1;
    }
    if (LLVMVerifyModule(ir->mod, LLVMReturnStatusAction, &message)) {
        fprintf(stderr, "FAIL: %s: %s\n", triple, message ? message : "the module does not verify");
        failed = 1;
    }
    if (message) LLVMDisposeMessage(message);
    unsigned char* data = NULL;
    size_t size = 0;
    if (!failed && snx_ir_object(ir, '2', &data, &size, error, sizeof error)) {
        fprintf(stderr, "FAIL: %s: %s\n", triple, error);
        failed = 1;
    }
    free(data);
    snx_ir_free(ir);
    return failed;
}

int main(void) {
    static const struct {
        const char* triple;
        int in_memory;
    } targets[] = {
#ifdef __x86_64__
        {"x86_64-w64-windows-gnu", 1},
        {"x86_64-pc-linux-gnu", 0},
#else
        {"aarch64-w64-windows-gnu", 0},
        {"aarch64-linux-android", 0},
#endif
    };
    char error[512];
    if (snx_ir_init(error, sizeof error)) {
        fprintf(stderr, "FAIL: %s\n", error);
        return 1;
    }
    unsigned checked = 0, failures = 0;
    for (size_t i = 0; i < sizeof targets / sizeof targets[0]; ++i) {
        int rc = check(targets[i].triple, targets[i].in_memory);
        if (rc == EXIT_SKIPPED) continue;
        ++checked;
        failures += rc != 0;
    }
    if (!checked) return EXIT_SKIPPED;
    printf("FP helper replies: %u targets checked, %u failed\n", checked, failures);
    return failures != 0;
}
