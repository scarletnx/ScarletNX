/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* An FP helper's reply (cpu.h's SnxFpReply, 16 bytes) comes back as the C compiler returns it: on x86-64 Windows,
   s390x and big-endian PowerPC64's ELFv1 through memory, the call passing a local as its hidden first argument
   (sret), as Clang compiles the helpers there; elsewhere as an LLVM structure, in two registers, {i64, i32}, or
   {i64, i64} on a big-endian host, whose ABI loads the structure's second doubleword whole (the flags in its high
   half). The code runs where it is built, so the triples here are the host's architecture's, Windows or not, and the
   PowerPC64 and MIPS64 forms, which follow the target's byte order and ABI: FADD S0, S1, S2's slow path calls
   snx_fp_host_binary on each, and the call stays no site of ir_promote_registers either way. */
#include "common/cpu/recompiler/llvm/ir_fpu.h"
#include "common/cpu/aarch64/cpu.h"
#include <llvm-c/Analysis.h>
#include <llvm-c/Target.h>
#include <stdio.h>
#include <stdlib.h>

#define EXIT_SKIPPED 77

enum { IN_REGISTERS, IN_MEMORY, IN_REGISTERS_HIGH };

/* the reply's type, {i64, i32}, or {i64, i64} with high */
static int is_reply(SnxIr* ir, LLVMTypeRef type, int high) {
    LLVMTypeRef fields[2];
    if (LLVMGetTypeKind(type) != LLVMStructTypeKind || LLVMCountStructElementTypes(type) != 2) return 0;
    LLVMGetStructElementTypes(type, fields);
    return fields[0] == ir->t_i64 && fields[1] == (high ? ir->t_i64 : ir->t_i32);
}

/* 0 when the triple's declaration and call are right, 1 when not, EXIT_SKIPPED without its target */
static int check(const char* triple, int form) {
    const int in_memory = form == IN_MEMORY, high = form == IN_REGISTERS_HIGH;
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
            !is_reply(ir, LLVMGetTypeAttributeValue(sret), 0)) {
            fprintf(stderr, "FAIL: %s: snx_fp_host_binary is not void (ptr sret({i64, i32}), 6 operands)\n", triple);
            failed = 1;
        }
    } else if (!is_reply(ir, LLVMGetReturnType(type), high) || count != 6 || sret) {
        fprintf(stderr, "FAIL: %s: snx_fp_host_binary is not {i64, %s} (6 operands)\n", triple, high ? "i64" : "i32");
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
    /* snx_ir_object refuses the other byte order's object, which it reads back */
    if (!failed && ir->big_endian == (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__) &&
        snx_ir_object(ir, '2', &data, &size, error, sizeof error)) {
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
        int form;
    } targets[] = {
#ifdef __x86_64__
        {"x86_64-w64-windows-gnu", IN_MEMORY},
        {"x86_64-pc-linux-gnu", IN_REGISTERS},
#elif !defined(__s390x__)
        {"aarch64-w64-windows-gnu", IN_REGISTERS},
        {"aarch64-linux-android", IN_REGISTERS},
#endif
#if !defined(__s390x__)
        {"powerpc64le-unknown-linux-gnu", IN_REGISTERS},
        {"powerpc64-unknown-linux-musl", IN_REGISTERS_HIGH}, /* ELFv2 */
        {"powerpc64-unknown-linux-gnu", IN_MEMORY},          /* ELFv1 */
        {"mips64-unknown-linux-gnuabi64", IN_REGISTERS_HIGH},
        {"mips64el-unknown-linux-gnuabi64", IN_REGISTERS},
#else
        {"s390x-ibm-linux-gnu", IN_MEMORY},
#endif
    };
    char error[512];
    if (snx_ir_init(error, sizeof error)) {
        fprintf(stderr, "FAIL: %s\n", error);
        return 1;
    }
    /* the other architectures' targets too, which snx_ir_init leaves to the host's */
    LLVMInitializeAllTargetInfos();
    LLVMInitializeAllTargets();
    LLVMInitializeAllTargetMCs();
    LLVMInitializeAllAsmPrinters();
    unsigned checked = 0, failures = 0;
    for (size_t i = 0; i < sizeof targets / sizeof targets[0]; ++i) {
        int rc = check(targets[i].triple, targets[i].form);
        if (rc == EXIT_SKIPPED) continue;
        ++checked;
        failures += rc != 0;
    }
    if (!checked) return EXIT_SKIPPED;
    printf("FP helper replies: %u targets checked, %u failed\n", checked, failures);
    return failures != 0;
}
