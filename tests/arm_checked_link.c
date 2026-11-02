/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A checked ARM block links into an AOT module for Windows x86-64, which defines nothing of the compiler runtime: the
   allocas of its fetch must not call the stack probe (___chkstk_ms), whichever host builds the module */
#include "common/cpu/arm/llvm.h"
#include "common/cpu/recompiler/llvm/ir.h"
#include "common/cpu/recompiler/llvm/link.h"
#include <stdio.h>
#include <stdlib.h>

#define EXIT_SKIPPED 77

static int link_block(char opt) {
    static const uint32_t code[] = {0xE1A00000, 0xEAFFFFFE}; /* mov r0, r0; b . */
    static const char* const exports[] = {"block"};
    char error[512];
    SnxIr* ir = snx_ir_new_target("checked", opt, "x86_64-w64-windows-gnu", error, sizeof error);
    if (!ir) {
        fprintf(stderr, "skipped: %s\n", error);
        return EXIT_SKIPPED;
    }
    SnxArmCodegenOptions options = {SNX_ARM_CODEGEN_OPTIONS_ABI, SNX_ARM_PROFILE_ARM11, SNX_ARM_MEMORY_CHECKED};
    size_t consumed = 0;
    if (!snx_arm_llvm_block_ex(ir->mod, "block", code, 2, 0x1000, &consumed, &options)) {
        fprintf(stderr, "FAIL: -O%c: the checked block is not emitted\n", opt);
        return 1;
    }
    unsigned char* data = NULL;
    size_t size = 0;
    if (snx_ir_object(ir, opt, &data, &size, error, sizeof error)) {
        fprintf(stderr, "FAIL: -O%c: %s\n", opt, error);
        return 1;
    }
    SnxLinkInput object = {data, size, "block"};
    SnxLinkOutput module = {0};
    int rc = snx_link(&object, 1, exports, 1, "block.dll", &module, error, sizeof error);
    if (rc) fprintf(stderr, "FAIL: -O%c: the module does not link: %s\n", opt, error);
    free(data);
    free(module.data);
    snx_ir_free(ir);
    return rc != 0;
}

int main(void) {
    char error[512];
    if (snx_ir_init(error, sizeof error)) {
        fprintf(stderr, "FAIL: %s\n", error);
        return 1;
    }
    int rc0 = link_block('0');
    int rc1 = link_block('1');
    if (rc0 == 1 || rc1 == 1) return 1;
    if (rc0 == EXIT_SKIPPED || rc1 == EXIT_SKIPPED) return EXIT_SKIPPED;
    puts("a checked ARM block links without the compiler runtime");
    return 0;
}
