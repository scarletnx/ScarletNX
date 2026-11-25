/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* LLVM's AArch32 blocks (common/cpu/arm/llvm.h) behind the functions support/arm_back.h names, for the tests that hold
   them to Unicorn beside the native back ends' while LLVM still makes the blocks a native back end leaves
   (common/cpu/arm/exec.c). The module is verified before LLVM's passes and after them, at -O0 or -O2 (arm_llvm_opt),
   and its object is the JIT's, which the code loader reads on every host. arm_llvm_break spoils the module before or
   after the passes, so that a test of the checks (arm_llvm_checks.py) sees each verification refuse it */
#ifndef SNX_TEST_ARM_LLVM_BACK_H
#define SNX_TEST_ARM_LLVM_BACK_H
#include "common/cpu/arm/llvm.h"
#include "common/cpu/recompiler/llvm/ir.h"
#include <llvm-c/Analysis.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { ARM_LLVM_INTACT, ARM_LLVM_BREAK_BEFORE, ARM_LLVM_BREAK_AFTER };

typedef struct {
    SnxIr* ir;            /* the context and the JIT's target machine, made at the first block */
    LLVMModuleRef module; /* every block's, one object as the native back ends make */
    SnxArmCodegenOptions options;
    char opt;  /* the passes' -O level, '0' or '2' */
    int spoil; /* ARM_LLVM_BREAK_*: where the module is made invalid */
    size_t made;
    char why[1024];
} ArmLlvm;

/* a declaration of internal linkage, which only the verifier refuses */
static inline void arm_llvm_spoil(LLVMModuleRef module) {
    LLVMTypeRef type = LLVMFunctionType(LLVMVoidTypeInContext(LLVMGetModuleContext(module)), NULL, 0, 0);
    LLVMSetLinkage(LLVMAddFunction(module, "snx_spoiled", type), LLVMInternalLinkage);
}
/* LLVM's verifier on the module, where says when: 0, -1 with what it says in why */
static inline int arm_llvm_verify(LLVMModuleRef module, const char* where, char* why, size_t why_size) {
    char* message = NULL;
    int rc = LLVMVerifyModule(module, LLVMReturnStatusAction, &message);
    if (rc) snprintf(why, why_size, "LLVM's verifier, %s: %.900s", where, message ? message : "");
    LLVMDisposeMessage(message);
    return rc ? -1 : 0;
}

static inline int arm_llvm_available(void) { return 1; }
static inline ArmLlvm* arm_llvm_new(void) {
    ArmLlvm* b = (ArmLlvm*)calloc(1, sizeof *b);
    if (b) b->opt = '2';
    return b;
}
static inline void arm_llvm_free(ArmLlvm* b) {
    if (!b) return;
    if (b->module) LLVMDisposeModule(b->module);
    snx_ir_free(b->ir);
    free(b);
}
/* the -O level, before the first block: 0, or -1 for one the tests do not hold to Unicorn */
static inline int arm_llvm_opt(ArmLlvm* b, const char* given) {
    if (b->module || (strcmp(given, "0") && strcmp(given, "2"))) return -1;
    b->opt = given[0];
    return 0;
}
static inline void arm_llvm_break(ArmLlvm* b, int where) { b->spoil = where; }
/* LLVM takes or refuses them at each block */
static inline int arm_llvm_options(ArmLlvm* b, const SnxArmCodegenOptions* options) {
    b->options = *options;
    return 0;
}
static inline const char* arm_llvm_why(const ArmLlvm* b) { return b->why; }
static inline int arm_llvm_module(ArmLlvm* b) {
    if (b->module) return 0;
    if (snx_ir_init(b->why, sizeof b->why) || !(b->ir = snx_ir_new_jit("arm-blocks", b->opt, b->why, sizeof b->why)))
        return -1;
    b->module = LLVMModuleCreateWithNameInContext("arm-blocks", b->ir->ctx);
    LLVMSetTarget(b->module, LLVMGetTarget(b->ir->mod));
    LLVMSetDataLayout(b->module, LLVMGetDataLayoutStr(b->ir->mod));
    return 0;
}
/* the block of count units at pc, its code little-endian as guest memory holds it, as the native back ends' block
   functions: 1 with *consumed the units it covers, 0 where LLVM leaves it (why says) */
static inline int arm_llvm_any_block(ArmLlvm* b, int thumb, const char* name, const unsigned char* code, size_t count,
                                     uint32_t pc, size_t* consumed) {
    uint32_t words[64];
    uint16_t halfwords[64];
    LLVMValueRef fn;
    *consumed = 0;
    if (count > 64) {
        snprintf(b->why, sizeof b->why, "%zu units, of 64 at most", count);
        return 0;
    }
    if (arm_llvm_module(b)) return 0;
    for (size_t k = 0; k < count; ++k)
        if (thumb)
            halfwords[k] = (uint16_t)(code[2 * k] | code[2 * k + 1] << 8);
        else
            words[k] = (uint32_t)code[4 * k] | (uint32_t)code[4 * k + 1] << 8 | (uint32_t)code[4 * k + 2] << 16 |
                       (uint32_t)code[4 * k + 3] << 24;
    fn = thumb ? snx_thumb_llvm_block_ex(b->module, name, halfwords, count, pc, consumed, &b->options)
               : snx_arm_llvm_block_ex(b->module, name, words, count, pc, consumed, &b->options);
    if (fn && *consumed) {
        ++b->made;
        return 1;
    }
    if (fn) LLVMDeleteFunction(fn);
    *consumed = 0;
    snprintf(b->why, sizeof b->why, "LLVM takes no such block");
    return 0;
}
static inline int arm_llvm_block(ArmLlvm* b, const char* name, const unsigned char* code, size_t count, uint32_t pc,
                                 size_t* consumed, const void* next, void* user) {
    (void)next, (void)user;
    return arm_llvm_any_block(b, 0, name, code, count, pc, consumed);
}
static inline int arm_llvm_thumb_block(ArmLlvm* b, const char* name, const unsigned char* code, size_t count,
                                       uint32_t pc, size_t* consumed, const void* next, void* user) {
    (void)next, (void)user;
    return arm_llvm_any_block(b, 1, name, code, count, pc, consumed);
}
/* the blocks' object (malloc'd), made as exec.c makes one with LLVM, the module verified before the passes and after
   them: 0, -1 with why */
static inline int arm_llvm_object(ArmLlvm* b, unsigned char** object, size_t* size, size_t* blocks) {
    *object = NULL;
    *size = 0;
    if (blocks) *blocks = b->made;
    if (arm_llvm_module(b)) return -1;
    if (b->spoil == ARM_LLVM_BREAK_BEFORE) arm_llvm_spoil(b->module);
    if (arm_llvm_verify(b->module, "before the passes", b->why, sizeof b->why) ||
        snx_ir_module_optimize(b->ir, b->module, b->opt, b->why, sizeof b->why))
        return -1;
    if (b->spoil == ARM_LLVM_BREAK_AFTER) arm_llvm_spoil(b->module);
    if (arm_llvm_verify(b->module, b->opt == '0' ? "after the -O0 passes" : "after the -O2 passes", b->why,
                        sizeof b->why))
        return -1;
    /* the passes ran: -O0's, which this runs again, leave the code as it is (the always-inliner has nothing left), and
       the target machine was made for the level (snx_ir_new_jit) */
    return snx_ir_module_object(b->ir, b->module, '0', object, size, b->why, sizeof b->why);
}
#endif
