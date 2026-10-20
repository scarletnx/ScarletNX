/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* thumb_emit HALFWORDS OUTPUT.o [OPT [a57|arm7tdmi]]: each input line is PC followed by up to
   64 hexadecimal halfwords. Exports block_0, block_1, ... (SnxArmBlock ABI). */
#include "common/cpu/arm/llvm.h"
#include <llvm-c/Analysis.h>
#include <llvm-c/Target.h>
#include <llvm-c/TargetMachine.h>
#include <llvm-c/Transforms/PassBuilder.h>
#include <llvm-c/Error.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char** argv) {
    LLVMContextRef ctx;
    LLVMModuleRef mod;
    LLVMTargetRef target;
    LLVMTargetMachineRef tm;
    LLVMTargetDataRef layout;
    LLVMPassBuilderOptionsRef options;
    LLVMErrorRef err;
    char *triple, *error = NULL, line[2048];
    unsigned index = 0;
    FILE* input;
    SnxArmCodegenOptions codegen = {SNX_ARM_CODEGEN_OPTIONS_ABI, SNX_ARM_PROFILE_A57, SNX_ARM_MEMORY_FLAT};
    if (argc < 3 || argc > 5) return 2;
    if (argc == 5) {
        if (!strcmp(argv[4], "arm7tdmi"))
            codegen.profile = SNX_ARM_PROFILE_ARM7TDMI;
        else if (strcmp(argv[4], "a57"))
            return 2;
    }
    input = fopen(argv[1], "r");
    if (!input) return 2;
    if (LLVMInitializeNativeTarget() || LLVMInitializeNativeAsmPrinter()) return 2;
    ctx = LLVMContextCreate();
    mod = LLVMModuleCreateWithNameInContext("thumb-test", ctx);
    triple = LLVMGetDefaultTargetTriple();
    if (LLVMGetTargetFromTriple(triple, &target, &error)) goto fail;
    tm = LLVMCreateTargetMachine(target, triple, "generic", "", LLVMCodeGenLevelDefault, LLVMRelocPIC,
                                 LLVMCodeModelDefault);
    LLVMSetTarget(mod, triple);
    layout = LLVMCreateTargetDataLayout(tm);
    LLVMSetModuleDataLayout(mod, layout);
    while (fgets(line, sizeof(line), input)) {
        uint16_t halfwords[64];
        uint32_t pc;
        size_t n = 0, consumed;
        char name[32], *token = strtok(line, " \t\r\n");
        if (!token) continue;
        pc = (uint32_t)strtoul(token, NULL, 16);
        while ((token = strtok(NULL, " \t\r\n")) != NULL) {
            if (n == 64) return 2;
            halfwords[n++] = (uint16_t)strtoul(token, NULL, 16);
        }
        snprintf(name, sizeof(name), "block_%u", index++);
        if (!(argc == 5 ? snx_thumb_llvm_block_ex(mod, name, halfwords, n, pc, &consumed, &codegen)
                        : snx_thumb_llvm_block(mod, name, halfwords, n, pc, &consumed)) ||
            !consumed)
            return 2;
    }
    fclose(input);
    if (LLVMVerifyModule(mod, LLVMReturnStatusAction, &error)) goto fail;
    LLVMDisposeMessage(error);
    error = NULL;
    options = LLVMCreatePassBuilderOptions();
    err = LLVMRunPasses(mod, argc >= 4 && !strcmp(argv[3], "0") ? "default<O0>" : "default<O2>", tm, options);
    if (err) {
        char* message = LLVMGetErrorMessage(err);
        fprintf(stderr, "%s\n", message);
        LLVMDisposeErrorMessage(message);
        return 1;
    }
    LLVMDisposePassBuilderOptions(options);
    if (LLVMVerifyModule(mod, LLVMReturnStatusAction, &error)) goto fail;
    LLVMDisposeMessage(error);
    error = NULL;
    if (LLVMTargetMachineEmitToFile(tm, mod, argv[2], LLVMObjectFile, &error)) goto fail;
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
