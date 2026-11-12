/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Build-time static object generation only. No execution engine or MCJIT. */
#include "common/cpu/arm/llvm.h"
#include "arm_checked_cases.h"
#include <llvm-c/Analysis.h>
#include <llvm-c/Error.h>
#include <llvm-c/Target.h>
#include <llvm-c/TargetMachine.h>
#include <llvm-c/Transforms/PassBuilder.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char** argv) {
    char *triple, *error = NULL;
    LLVMTargetRef target;
    if (argc != 3 || LLVMInitializeNativeTarget() || LLVMInitializeNativeAsmPrinter()) return 2;
    LLVMContextRef ctx = LLVMContextCreate();
    LLVMModuleRef mod = LLVMModuleCreateWithNameInContext("arm-checked-aot", ctx);
    triple = LLVMGetDefaultTargetTriple();
    if (LLVMGetTargetFromTriple(triple, &target, &error)) goto fail;
    LLVMTargetMachineRef tm = LLVMCreateTargetMachine(target, triple, "generic", "", LLVMCodeGenLevelDefault,
                                                      LLVMRelocPIC, LLVMCodeModelDefault);
    LLVMTargetDataRef layout = LLVMCreateTargetDataLayout(tm);
    LLVMSetTarget(mod, triple);
    LLVMSetModuleDataLayout(mod, layout);
    LLVMValueRef functions[F_count], counts[F_count];
    for (unsigned i = 0; i < F_count; ++i) {
        const ArmFixture* f = &arm_fixtures[i];
        SnxArmCodegenOptions o = {SNX_ARM_CODEGEN_OPTIONS_ABI, (SnxArmProfile)f->profile,
                                  f->checked ? SNX_ARM_MEMORY_CHECKED : SNX_ARM_MEMORY_FLAT, 0, 0};
        char name[80];
        snprintf(name, sizeof name, "snx_fixture_%s", f->name);
        size_t consumed = 0;
        if (f->thumb) {
            uint16_t code[8];
            for (size_t n = 0; n < f->count; ++n) code[n] = (uint16_t)f->code[n];
            functions[i] = snx_thumb_llvm_block_ex(mod, name, code, f->count, f->pc, &consumed, &o);
        } else if (!f->checked && !f->profile) {
            functions[i] = snx_arm_llvm_block(mod, name, f->code, f->count, f->pc, &consumed);
        } else
            functions[i] = snx_arm_llvm_block_ex(mod, name, f->code, f->count, f->pc, &consumed, &o);
        if (!functions[i] || !consumed) {
            fprintf(stderr, "emit failed: %s\n", f->name);
            return 1;
        }
        counts[i] = LLVMConstInt(LLVMInt32TypeInContext(ctx), consumed, 0);
    }
    LLVMTypeRef pointers = LLVMArrayType(LLVMPointerTypeInContext(ctx, 0), F_count);
    LLVMValueRef table = LLVMAddGlobal(mod, pointers, "snx_test_blocks");
    LLVMSetInitializer(table, LLVMConstArray(LLVMPointerTypeInContext(ctx, 0), functions, F_count));
    LLVMSetGlobalConstant(table, 1);
    table = LLVMAddGlobal(mod, LLVMArrayType(LLVMInt32TypeInContext(ctx), F_count), "snx_test_consumed");
    LLVMSetInitializer(table, LLVMConstArray(LLVMInt32TypeInContext(ctx), counts, F_count));
    LLVMSetGlobalConstant(table, 1);
    /* Rejected options must not leave a partially emitted function behind: a bad ABI, profile or memory mode, and bus
       cycles counted by a profile other than the ARM7TDMI, into one place only, or into the table itself */
    SnxArmCodegenOptions bad[] = {{99, 0, 0, 0, 0},
                                  {1, 99, 0, 0, 0},
                                  {1, 0, 99, 0, 0},
                                  {1, SNX_ARM_PROFILE_A57, SNX_ARM_MEMORY_FLAT, 1024, 1032},
                                  {1, SNX_ARM_PROFILE_ARM7TDMI, SNX_ARM_MEMORY_FLAT, 1024, 0},
                                  {1, SNX_ARM_PROFILE_ARM7TDMI, SNX_ARM_MEMORY_FLAT, 1024, 1024}};
    for (size_t i = 0; i < sizeof bad / sizeof *bad; ++i) {
        size_t n = 123;
        uint32_t arm = 0xe1a00000;
        uint16_t thumb = 0x46c0;
        if (snx_arm_llvm_block_ex(mod, "bad_arm", &arm, 1, 0, &n, &bad[i]) || n || LLVMGetNamedFunction(mod, "bad_arm"))
            return 1;
        n = 123;
        if (snx_thumb_llvm_block_ex(mod, "bad_thumb", &thumb, 1, 0, &n, &bad[i]) || n ||
            LLVMGetNamedFunction(mod, "bad_thumb"))
            return 1;
    }
    /* A hosted block has a noalias cpu and no entry checks, and SETEND BE stops it. The AGB's ARM7TDMI is not hosted. */
    {
        SnxArmCodegenOptions hosted = {SNX_ARM_CODEGEN_OPTIONS_ABI, SNX_ARM_PROFILE_A9, SNX_ARM_MEMORY_HOSTED, 0, 0};
        SnxArmCodegenOptions agb = {SNX_ARM_CODEGEN_OPTIONS_ABI, SNX_ARM_PROFILE_ARM7TDMI, SNX_ARM_MEMORY_HOSTED, 0, 0};
        uint32_t arm[2] = {0xe1a00000, 0xf1010200};
        uint16_t thumb[2] = {0x46c0, 0x46c0};
        size_t n = 0;
        LLVMValueRef a = snx_arm_llvm_block_ex(mod, "hosted_arm", arm, 2, 0x1000, &n, &hosted);
        LLVMValueRef t = snx_thumb_llvm_block_ex(mod, "hosted_thumb", thumb, 2, 0x2000, &n, &hosted);
        unsigned noalias = LLVMGetEnumAttributeKindForName("noalias", 7);
        if (!a || !t || !LLVMGetEnumAttributeAtIndex(a, 1 /* first parameter */, noalias) ||
            !LLVMGetEnumAttributeAtIndex(t, 1 /* first parameter */, noalias))
            return 1;
        n = 123;
        if (snx_arm_llvm_block_ex(mod, "hosted_agb", arm, 1, 0, &n, &agb) || n || LLVMGetNamedFunction(mod, "hosted_agb"))
            return 1;
    }
    if (LLVMVerifyModule(mod, LLVMReturnStatusAction, &error)) goto fail;
    LLVMDisposeMessage(error);
    error = NULL;
    LLVMPassBuilderOptionsRef options = LLVMCreatePassBuilderOptions();
    LLVMErrorRef err = LLVMRunPasses(mod, !strcmp(argv[2], "0") ? "default<O0>" : "default<O2>", tm, options);
    LLVMDisposePassBuilderOptions(options);
    if (err) {
        char* message = LLVMGetErrorMessage(err);
        fprintf(stderr, "%s\n", message);
        LLVMDisposeErrorMessage(message);
        return 1;
    }
    if (LLVMVerifyModule(mod, LLVMReturnStatusAction, &error)) goto fail;
    LLVMDisposeMessage(error);
    error = NULL;
    if (LLVMTargetMachineEmitToFile(tm, mod, argv[1], LLVMObjectFile, &error)) goto fail;
    LLVMDisposeTargetData(layout);
    LLVMDisposeTargetMachine(tm);
    LLVMDisposeMessage(triple);
    LLVMDisposeModule(mod);
    LLVMContextDispose(ctx);
    return 0;
fail:
    fprintf(stderr, "%s\n", error ? error : "LLVM error");
    LLVMDisposeMessage(error);
    return 1;
}
