/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The fallback made by LLVM (fallback.h), in builds with the backend. LLVM starts at the first thing asked of it, for
   the process. C in the target's GNU C23, for the LLVM-C headers */
#include "common/cpu/recompiler/fallback.h"
#include "common/cpu/arm/llvm.h"
#include "common/cpu/recompiler/llvm/ir.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int snx_fallback_available(void) { return 1; }
const char* snx_fallback_name(void) { return "LLVM"; }

/* ---- A64 functions ---- */

/* the unit's one global, an array of its functions' addresses in codegen's order: they are internal to it, so that two
   units' F_<address> do not clash in the loader's table, where every global stays. 0, or -1 out of memory */
static int unit_table(SnxIr* ir, SnxCodegen* codegen, const char* name) {
    LLVMValueRef* functions = (LLVMValueRef*)malloc(codegen->function_count * sizeof *functions);
    if (!functions) return -1;
    for (size_t i = 0; i < codegen->function_count; ++i) {
        functions[i] = snx_ir_guest_function(ir, codegen->functions[i].start);
        LLVMSetLinkage(functions[i], LLVMInternalLinkage);
        LLVMSetVisibility(functions[i], LLVMDefaultVisibility); /* a local symbol has no visibility */
    }
    LLVMTypeRef type = LLVMArrayType(ir->t_ptr, (unsigned)codegen->function_count);
    LLVMValueRef table = LLVMAddGlobal(ir->mod, type, name);
    LLVMSetInitializer(table, LLVMConstArray(ir->t_ptr, functions, (unsigned)codegen->function_count));
    LLVMSetGlobalConstant(table, 1);
    LLVMSetVisibility(table, LLVMHiddenVisibility);
    free(functions);
    return 0;
}

int snx_fallback_object(SnxCodegen* codegen, size_t first, size_t last, const SnxNativeOptions* options,
                        const unsigned char* made, const char* name, char opt, unsigned char** object, size_t* size,
                        char* error, size_t error_size) {
    size_t left = 0;
    *object = NULL;
    *size = 0;
    for (size_t i = first; i < last; ++i) left += !made || !made[i - first];
    if (!left) return 0;
    if (options->jit && (first || last != codegen->function_count || left != last - first)) {
        snprintf(error, error_size,
                 "%s: a JIT unit's functions are one object's, and %zu of codegen's %zu are asked for", name, left,
                 codegen->function_count);
        return -1;
    }
    SnxIr* ir = snx_ir_init(error, error_size) ? NULL
                : options->jit                 ? snx_ir_new_jit(name, opt, error, error_size)
                                               : snx_ir_new(name, opt, error, error_size);
    if (!ir) return -1;
    ir->direct = options->direct;
    ir->direct_user = options->direct_user;
    ir->bound = options->bound;
    ir->bound_user = options->bound_user;
    if (options->stale) snx_ir_stale_checks(ir);
    for (size_t i = first; i < last; ++i)
        if (!made || !made[i - first]) snx_ir_emit(ir, codegen, &codegen->functions[i]);
    int rc = 0;
    if (options->jit && unit_table(ir, codegen, options->table_name)) {
        snprintf(error, error_size, "out of memory");
        rc = -1;
    }
    if (!rc) rc = snx_ir_object(ir, opt, object, size, error, error_size);
    snx_ir_free(ir);
    return rc;
}

/* ---- AArch32 blocks ---- */

struct SnxFallbackArm {
    SnxIr* ir;            /* the context and target machine every block's module is made with, made for the first */
    LLVMModuleRef module; /* the blocks since the last object, NULL for none */
    char name[32];
    char opt;
    int jit;
};

SnxFallbackArm* snx_fallback_arm_new(const char* name, char opt, int jit) {
    SnxFallbackArm* f = (SnxFallbackArm*)calloc(1, sizeof *f);
    if (!f) return NULL;
    snprintf(f->name, sizeof f->name, "%s", name);
    f->opt = opt;
    f->jit = jit;
    return f;
}
void snx_fallback_arm_free(SnxFallbackArm* f) {
    if (!f) return;
    if (f->module) LLVMDisposeModule(f->module);
    if (f->ir) snx_ir_free(f->ir);
    free(f);
}
int snx_fallback_arm_start(SnxFallbackArm* f, char* error, size_t error_size) {
    if (!f->ir && !snx_ir_init(error, error_size))
        f->ir = f->jit ? snx_ir_new_jit(f->name, f->opt, error, error_size)
                       : snx_ir_new(f->name, f->opt, error, error_size);
    return f->ir ? 0 : -1;
}
int snx_fallback_arm_block(SnxFallbackArm* f, const char* name, const void* code, size_t count, uint32_t pc, int thumb,
                           const SnxArmCodegenOptions* options, const char* why, size_t* consumed, char* error,
                           size_t error_size) {
    (void)why;
    *consumed = 0;
    if (snx_fallback_arm_start(f, error, error_size)) return -1;
    const int first = !f->module;
    if (first) {
        f->module = LLVMModuleCreateWithNameInContext(f->jit ? name : f->name, f->ir->ctx);
        LLVMSetTarget(f->module, LLVMGetTarget(f->ir->mod));
        LLVMSetDataLayout(f->module, LLVMGetDataLayoutStr(f->ir->mod));
    }
    LLVMValueRef fn =
        thumb ? snx_thumb_llvm_block_ex(f->module, name, (const uint16_t*)code, count, pc, consumed, options)
              : snx_arm_llvm_block_ex(f->module, name, (const uint32_t*)code, count, pc, consumed, options);
    if (fn) return 0;
    if (first) {
        LLVMDisposeModule(f->module);
        f->module = NULL;
    }
    snprintf(error, error_size, "LLVM takes no such block");
    return -1;
}
int snx_fallback_arm_object(SnxFallbackArm* f, unsigned char** object, size_t* size, char* error, size_t error_size) {
    *object = NULL;
    *size = 0;
    if (!f->module) {
        snprintf(error, error_size, "%s: no block was made", f->name);
        return -1;
    }
    int rc = snx_ir_module_object(f->ir, f->module, f->opt, object, size, error, error_size);
    LLVMDisposeModule(f->module);
    f->module = NULL;
    return rc;
}
