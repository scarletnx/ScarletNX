/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The object of arm_checked.c's blocks made by LLVM at build time, beside arm_checked_emit.c's native one, while LLVM
   still makes the blocks a native back end leaves: no execution engine.
     snx-test-arm-checked-llvm OBJECT COUNTS OPT [--break before|after|noalias]
   COUNTS gets the words or halfwords each covers, an initializer; OPT is the passes' -O level, 0 or 2. --break spoils
   the module before or after the passes, or takes noalias off the hosted blocks' cpu, so that a test of the checks
   (arm_llvm_checks.py) sees each refuse it */
#include "common/cpu/arm/llvm.h"
#include "arm_checked_cases.h"
#include <llvm-c/Analysis.h>
#include <llvm-c/Error.h>
#include <llvm-c/Target.h>
#include <llvm-c/TargetMachine.h>
#include <llvm-c/Transforms/PassBuilder.h>
#include <stdio.h>
#include <string.h>

/* a declaration of internal linkage, which only the verifier refuses */
static void spoil(LLVMModuleRef mod) {
    LLVMTypeRef type = LLVMFunctionType(LLVMVoidTypeInContext(LLVMGetModuleContext(mod)), NULL, 0, 0);
    LLVMSetLinkage(LLVMAddFunction(mod, "snx_spoiled", type), LLVMInternalLinkage);
}

int main(int argc, char** argv) {
    char *triple, *error = NULL;
    const char* spoiled = argc == 6 && !strcmp(argv[4], "--break") ? argv[5] : "";
    LLVMTargetRef target;
    size_t counts[F_count];
    FILE* f;
    if ((argc != 4 && !*spoiled) || (strcmp(argv[3], "0") && strcmp(argv[3], "2"))) {
        fprintf(stderr, "usage: snx-test-arm-checked-llvm OBJECT COUNTS 0|2 [--break before|after|noalias]\n");
        return 2;
    }
    if (*spoiled && strcmp(spoiled, "before") && strcmp(spoiled, "after") && strcmp(spoiled, "noalias")) {
        fprintf(stderr, "--break %s: before, after or noalias\n", spoiled);
        return 2;
    }
    if (LLVMInitializeNativeTarget() || LLVMInitializeNativeAsmPrinter()) return 2;
    LLVMContextRef ctx = LLVMContextCreate();
    LLVMModuleRef mod = LLVMModuleCreateWithNameInContext("arm-checked-aot", ctx);
    triple = LLVMGetDefaultTargetTriple();
    if (LLVMGetTargetFromTriple(triple, &target, &error)) goto fail;
    LLVMTargetMachineRef tm = LLVMCreateTargetMachine(target, triple, "generic", "", LLVMCodeGenLevelDefault,
                                                      LLVMRelocPIC, LLVMCodeModelDefault);
    LLVMTargetDataRef layout = LLVMCreateTargetDataLayout(tm);
    LLVMSetTarget(mod, triple);
    LLVMSetModuleDataLayout(mod, layout);
    for (unsigned i = 0; i < F_count; ++i) {
        const ArmFixture* fixture = &arm_fixtures[i];
        SnxArmCodegenOptions o = {SNX_ARM_CODEGEN_OPTIONS_ABI, (SnxArmProfile)fixture->profile,
                                  fixture->checked ? SNX_ARM_MEMORY_CHECKED : SNX_ARM_MEMORY_FLAT, 0, 0};
        char name[80];
        LLVMValueRef fn;
        snprintf(name, sizeof name, "snx_fx_%s", fixture->name);
        counts[i] = 0;
        if (fixture->thumb) {
            uint16_t code[8];
            for (size_t n = 0; n < fixture->count; ++n) code[n] = (uint16_t)fixture->code[n];
            fn = snx_thumb_llvm_block_ex(mod, name, code, fixture->count, fixture->pc, &counts[i], &o);
        } else if (!fixture->checked && !fixture->profile) {
            fn = snx_arm_llvm_block(mod, name, fixture->code, fixture->count, fixture->pc, &counts[i]);
        } else
            fn = snx_arm_llvm_block_ex(mod, name, fixture->code, fixture->count, fixture->pc, &counts[i], &o);
        if (!fn || !counts[i]) {
            fprintf(stderr, "emit failed: %s\n", fixture->name);
            return 1;
        }
    }
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
        if (snx_arm_llvm_block_ex(mod, "bad_arm", &arm, 1, 0, &n, &bad[i]) || n ||
            LLVMGetNamedFunction(mod, "bad_arm")) {
            fprintf(stderr, "bad options %zu made an ARM block\n", i);
            return 1;
        }
        n = 123;
        if (snx_thumb_llvm_block_ex(mod, "bad_thumb", &thumb, 1, 0, &n, &bad[i]) || n ||
            LLVMGetNamedFunction(mod, "bad_thumb")) {
            fprintf(stderr, "bad options %zu made a Thumb block\n", i);
            return 1;
        }
    }
    /* A hosted block has a noalias cpu and no entry checks, and SETEND BE stops it (arm_checked.c runs them). The AGB's
       ARM7TDMI is not hosted. */
    {
        SnxArmCodegenOptions hosted = {SNX_ARM_CODEGEN_OPTIONS_ABI, SNX_ARM_PROFILE_A9, SNX_ARM_MEMORY_HOSTED, 0, 0};
        SnxArmCodegenOptions agb = {SNX_ARM_CODEGEN_OPTIONS_ABI, SNX_ARM_PROFILE_ARM7TDMI, SNX_ARM_MEMORY_HOSTED, 0, 0};
        uint32_t arm[2] = {0xe1a00000, 0xf1010200};
        uint16_t thumb[2] = {0x46c0, 0x46c0};
        size_t n = 0;
        LLVMValueRef a = snx_arm_llvm_block_ex(mod, "snx_hosted_arm", arm, 2, 0x1000, &n, &hosted);
        LLVMValueRef t = snx_thumb_llvm_block_ex(mod, "snx_hosted_thumb", thumb, 2, 0x2000, &n, &hosted);
        unsigned noalias = LLVMGetEnumAttributeKindForName("noalias", 7);
        if (!a || !t) {
            fprintf(stderr, "no hosted block\n");
            return 1;
        }
        if (!strcmp(spoiled, "noalias")) {
            LLVMRemoveEnumAttributeAtIndex(a, 1, noalias);
            LLVMRemoveEnumAttributeAtIndex(t, 1, noalias);
        }
        if (!LLVMGetEnumAttributeAtIndex(a, 1 /* first parameter */, noalias) ||
            !LLVMGetEnumAttributeAtIndex(t, 1 /* first parameter */, noalias)) {
            fprintf(stderr, "a hosted block's cpu is not noalias\n");
            return 1;
        }
        n = 123;
        if (snx_arm_llvm_block_ex(mod, "hosted_agb", arm, 1, 0, &n, &agb) || n ||
            LLVMGetNamedFunction(mod, "hosted_agb")) {
            fprintf(stderr, "the AGB's ARM7TDMI made a hosted block\n");
            return 1;
        }
    }
    if (!strcmp(spoiled, "before")) spoil(mod);
    if (LLVMVerifyModule(mod, LLVMReturnStatusAction, &error)) {
        fprintf(stderr, "LLVM's verifier, before the passes: ");
        goto fail;
    }
    LLVMDisposeMessage(error);
    error = NULL;
    LLVMPassBuilderOptionsRef options = LLVMCreatePassBuilderOptions();
    LLVMErrorRef err = LLVMRunPasses(mod, !strcmp(argv[3], "0") ? "default<O0>" : "default<O2>", tm, options);
    LLVMDisposePassBuilderOptions(options);
    if (err) {
        char* message = LLVMGetErrorMessage(err);
        fprintf(stderr, "%s\n", message);
        LLVMDisposeErrorMessage(message);
        return 1;
    }
    if (!strcmp(spoiled, "after")) spoil(mod);
    if (LLVMVerifyModule(mod, LLVMReturnStatusAction, &error)) {
        fprintf(stderr, "LLVM's verifier, after the -O%s passes: ", argv[3]);
        goto fail;
    }
    LLVMDisposeMessage(error);
    error = NULL;
    if (LLVMTargetMachineEmitToFile(tm, mod, argv[1], LLVMObjectFile, &error)) goto fail;
    f = fopen(argv[2], "w");
    if (!f) return 1;
    for (unsigned i = 0; i < F_count; ++i) fprintf(f, "%zu,\n", counts[i]);
    if (fclose(f)) return 1;
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
