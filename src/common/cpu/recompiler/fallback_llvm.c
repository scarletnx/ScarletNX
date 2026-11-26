/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The fallback made by LLVM (fallback.h), in builds with the backend. LLVM starts at the first thing asked of it, for
   the process. C in the target's GNU C23, for the LLVM-C headers */
#include "common/cpu/recompiler/fallback.h"
#include "common/cpu/arm/llvm.h"
#include "common/cpu/recompiler/llvm/ir.h"
#include "common/utilities/common.h"
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
    if (fn) {
        /* an AOT module exports what its linker is told to, the descriptor or the blocks by name */
        if (!f->jit) LLVMSetVisibility(fn, LLVMHiddenVisibility);
        return 0;
    }
    if (first) {
        LLVMDisposeModule(f->module);
        f->module = NULL;
    }
    snprintf(error, error_size, "LLVM takes no such block");
    return -1;
}
int snx_fallback_arm_ir(SnxFallbackArm* f, char** text, char* error, size_t error_size) {
    *text = NULL;
    if (!f->module) {
        snprintf(error, error_size, "%s: no block was made", f->name);
        return -1;
    }
    char* printed = LLVMPrintModuleToString(f->module);
    *text = snx_strdup(printed);
    LLVMDisposeMessage(printed);
    if (*text) return 0;
    snprintf(error, error_size, "%s: out of memory for the IR of its blocks", f->name);
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

/* ---- data ---- */

struct SnxFallbackData {
    SnxIr* ir; /* made for the first global, with the module */
    LLVMModuleRef module;
    char opt;
    size_t module_name; /* where the module's name starts in names */
    char names[];       /* the unit's, then the module's */
};

SnxFallbackData* snx_fallback_data_new(const char* unit, const char* module, char opt, const char* why) {
    const size_t n = strlen(unit) + 1, m = strlen(module) + 1;
    SnxFallbackData* d = (SnxFallbackData*)calloc(1, sizeof *d + n + m);
    (void)why;
    if (!d) return NULL;
    memcpy(d->names, unit, n);
    memcpy(d->names + n, module, m);
    d->module_name = n;
    d->opt = opt;
    return d;
}
void snx_fallback_data_free(SnxFallbackData* d) {
    if (!d) return;
    if (d->module) LLVMDisposeModule(d->module);
    if (d->ir) snx_ir_free(d->ir);
    free(d);
}
/* the module, made for this machine as the unit's code is (ir.c): the target, CPU and ABI the helpers are compiled for,
   which the object is linked with. 0, or -1 with why not in error */
static int data_start(SnxFallbackData* d, char* error, size_t error_size) {
    if (d->module) return 0;
    if (snx_ir_init(error, error_size) || !(d->ir = snx_ir_new(d->names, d->opt, error, error_size))) return -1;
    d->module = LLVMModuleCreateWithNameInContext(d->names + d->module_name, d->ir->ctx);
    LLVMSetTarget(d->module, LLVMGetTarget(d->ir->mod));
    LLVMSetDataLayout(d->module, LLVMGetDataLayoutStr(d->ir->mod));
    return 0;
}
/* an array of count values of type, constant and internal to the object */
static void data_array(SnxFallbackData* d, const char* name, LLVMTypeRef type, LLVMValueRef* values, unsigned count) {
    LLVMValueRef g = LLVMAddGlobal(d->module, LLVMArrayType(type, count), name);
    LLVMSetInitializer(g, LLVMConstArray(type, values, count));
    LLVMSetGlobalConstant(g, 1);
    LLVMSetLinkage(g, LLVMInternalLinkage);
}
/* the values the arrays hold: count of them, 0 with the room in *values, -1 with the reason in error */
static int data_room(SnxFallbackData* d, const char* name, size_t count, LLVMValueRef** values, char* error,
                     size_t error_size) {
    *values = NULL;
    if (count > UINT32_MAX) {
        snprintf(error, error_size, "%s: %s of %zu values, of %u at most", d->names, name, count, UINT32_MAX);
        return -1;
    }
    if (data_start(d, error, error_size)) return -1;
    if ((*values = (LLVMValueRef*)malloc((count ? count : 1) * sizeof **values))) return 0;
    snprintf(error, error_size, "%s: out of memory for %s, of %zu values", d->names, name, count);
    return -1;
}
int snx_fallback_data_blocks(SnxFallbackData* d, const char* name, size_t count,
                             int (*name_of)(void* user, size_t i, char* out, size_t size), void* user, char* error,
                             size_t error_size) {
    LLVMValueRef* values;
    if (data_room(d, name, count, &values, error, error_size)) return -1;
    LLVMTypeRef ptr = d->ir->t_ptr, block_type = LLVMFunctionType(d->ir->t_void, &ptr, 1, 0);
    LLVMValueRef none = LLVMConstNull(ptr);
    for (size_t i = 0; i < count; ++i) {
        char id[64];
        if (!name_of(user, i, id, sizeof id)) {
            values[i] = none;
            continue;
        }
        if (!(values[i] = LLVMGetNamedFunction(d->module, id))) {
            values[i] = LLVMAddFunction(d->module, id, block_type);
            LLVMSetVisibility(values[i], LLVMHiddenVisibility);
        }
    }
    data_array(d, name, ptr, values, (unsigned)count);
    free(values);
    return 0;
}
/* the type of integers of bits bits, NULL for another width */
static LLVMTypeRef data_integer(SnxIr* ir, unsigned bits) {
    return bits == 8 ? ir->t_i8 : bits == 32 ? ir->t_i32 : bits == 64 ? ir->t_i64 : NULL;
}
int snx_fallback_data_integers(SnxFallbackData* d, const char* name, unsigned bits, size_t count, const void* values,
                               char* error, size_t error_size) {
    LLVMValueRef* items;
    if (data_room(d, name, count, &items, error, error_size)) return -1;
    LLVMTypeRef type = data_integer(d->ir, bits);
    if (!type) {
        snprintf(error, error_size, "%s: %s of integers of %u bits, which are 8, 32 or 64", d->names, name, bits);
        free(items);
        return -1;
    }
    for (size_t i = 0; i < count; ++i)
        items[i] = LLVMConstInt(type,
                                bits == 8    ? ((const uint8_t*)values)[i]
                                : bits == 32 ? ((const uint32_t*)values)[i]
                                             : ((const uint64_t*)values)[i],
                                0);
    data_array(d, name, type, items, (unsigned)count);
    free(items);
    return 0;
}
/* the field's type and, where value is not NULL, its value: 0, or -1 with the reason in error */
static int data_field(SnxFallbackData* d, const char* name, const SnxFallbackField* f, LLVMTypeRef* type,
                      LLVMValueRef* value, char* error, size_t error_size) {
    SnxIr* ir = d->ir;
    if (f->bits) {
        if (!(*type = data_integer(ir, f->bits))) {
            snprintf(error, error_size, "%s: %s has a field of %u bits, which are 8, 32 or 64", d->names, name,
                     f->bits);
            return -1;
        }
        if (value) *value = LLVMConstInt(*type, f->value, 0);
        return 0;
    }
    if (!f->bytes) {
        *type = ir->t_ptr;
        if (!value) return 0;
        *value = f->global ? LLVMGetNamedGlobal(d->module, f->global) : LLVMConstNull(ir->t_ptr);
        if (*value) return 0;
        snprintf(error, error_size, "%s: %s points to %s, which no global before it is", d->names, name, f->global);
        return -1;
    }
    *type = LLVMArrayType(ir->t_i8, (unsigned)f->count);
    if (!value) return 0;
    LLVMValueRef* bytes = (LLVMValueRef*)malloc((f->count ? f->count : 1) * sizeof *bytes);
    if (!bytes) {
        snprintf(error, error_size, "%s: out of memory for %s", d->names, name);
        return -1;
    }
    for (size_t i = 0; i < f->count; ++i) bytes[i] = LLVMConstInt(ir->t_i8, f->bytes[i], 0);
    *value = LLVMConstArray(ir->t_i8, bytes, (unsigned)f->count);
    free(bytes);
    return 0;
}
/* the struct type of the nfields fields, as C lays them out in size bytes: NULL with the reason in error */
static LLVMTypeRef data_type(SnxFallbackData* d, const char* name, const SnxFallbackField* fields, size_t nfields,
                             size_t size, char* error, size_t error_size) {
    LLVMTypeRef* types = (LLVMTypeRef*)malloc((nfields ? nfields : 1) * sizeof *types);
    LLVMTypeRef type = NULL;
    int rc = types ? 0 : -1;
    if (!types) snprintf(error, error_size, "%s: out of memory for %s", d->names, name);
    for (size_t j = 0; !rc && j < nfields; ++j)
        rc = data_field(d, name, &fields[j], &types[j], NULL, error, error_size);
    if (!rc) {
        type = LLVMStructTypeInContext(d->ir->ctx, types, (unsigned)nfields, 0);
        LLVMTargetDataRef layout = LLVMGetModuleDataLayout(d->module);
        size_t j = 0;
        while (j < nfields && LLVMOffsetOfElement(layout, type, (unsigned)j) == fields[j].offset) ++j;
        if (LLVMABISizeOfType(layout, type) != size || j < nfields) {
            snprintf(error, error_size,
                     "%s: %s is laid out otherwise for the target LLVM makes it for (%llu bytes, field %zu at %llu) "
                     "than in C (%zu bytes, field %zu at %zu)",
                     d->names, name, (unsigned long long)LLVMABISizeOfType(layout, type), j,
                     j < nfields ? (unsigned long long)LLVMOffsetOfElement(layout, type, (unsigned)j) : 0ull, size, j,
                     j < nfields ? fields[j].offset : 0);
            type = NULL;
        }
    }
    free(types);
    return type;
}
/* count structs of the type, from fields: their values into out, 0 or -1 with the reason in error */
static int data_values(SnxFallbackData* d, const char* name, LLVMTypeRef type, size_t count,
                       const SnxFallbackField* fields, size_t nfields, LLVMValueRef* out, char* error,
                       size_t error_size) {
    LLVMValueRef* values = (LLVMValueRef*)malloc((nfields ? nfields : 1) * sizeof *values);
    int rc = values ? 0 : -1;
    if (!values) snprintf(error, error_size, "%s: out of memory for %s", d->names, name);
    for (size_t i = 0; !rc && i < count; ++i) {
        LLVMTypeRef field;
        for (size_t j = 0; !rc && j < nfields; ++j)
            rc = data_field(d, name, &fields[i * nfields + j], &field, &values[j], error, error_size);
        if (!rc) out[i] = LLVMConstNamedStruct(type, values, (unsigned)nfields);
    }
    free(values);
    return rc;
}
int snx_fallback_data_structs(SnxFallbackData* d, const char* name, size_t count, const SnxFallbackField* fields,
                              size_t nfields, size_t size, char* error, size_t error_size) {
    LLVMValueRef* items;
    if (data_room(d, name, count, &items, error, error_size)) return -1;
    LLVMTypeRef type = data_type(d, name, fields, nfields, size, error, error_size);
    int rc = type ? data_values(d, name, type, count, fields, nfields, items, error, error_size) : -1;
    if (!rc) data_array(d, name, type, items, (unsigned)count);
    free(items);
    return rc;
}
int snx_fallback_data_descriptor(SnxFallbackData* d, const char* name, const SnxFallbackField* fields, size_t nfields,
                                 size_t size, char* error, size_t error_size) {
    LLVMValueRef value;
    if (data_start(d, error, error_size)) return -1;
    LLVMTypeRef type = data_type(d, name, fields, nfields, size, error, error_size);
    if (!type || data_values(d, name, type, 1, fields, nfields, &value, error, error_size)) return -1;
    LLVMValueRef global = LLVMAddGlobal(d->module, type, name);
    LLVMSetInitializer(global, value);
    LLVMSetGlobalConstant(global, 1);
    return 0;
}
int snx_fallback_data_ir(SnxFallbackData* d, char** text, char* error, size_t error_size) {
    *text = NULL;
    if (data_start(d, error, error_size)) return -1;
    char* printed = LLVMPrintModuleToString(d->module);
    *text = snx_strdup(printed);
    LLVMDisposeMessage(printed);
    if (*text) return 0;
    snprintf(error, error_size, "%s: out of memory for the IR of %s", d->names, d->names + d->module_name);
    return -1;
}
int snx_fallback_data_object(SnxFallbackData* d, unsigned char** object, size_t* size, char* error, size_t error_size) {
    *object = NULL;
    *size = 0;
    /* a module of data alone has no code to optimize */
    return data_start(d, error, error_size)
               ? -1
               : snx_ir_module_object(d->ir, d->module, '0', object, size, error, error_size);
}
