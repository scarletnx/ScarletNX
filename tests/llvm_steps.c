/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The LLVM backend's step functions for the instruction tests (llvm_steps.h) */
#include "llvm_steps.h"
#include "common/cpu/recompiler/llvm/link.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#endif

extern const unsigned char snx_rtlib_object[];
extern const unsigned long snx_rtlib_object_size;

typedef LlvmStep* (*TableFn)(void);

static int fail(char* error, size_t size, const char* what, const char* why) {
    snprintf(error, size, "%s%s%s", what, why ? ": " : "", why ? why : "");
    return -1;
}

int llvm_steps_begin(LlvmSteps* s, int jit, int portable, char* error, size_t error_size) {
    return llvm_steps_begin_opt(s, jit, portable, '2', error, error_size);
}

int llvm_steps_begin_opt(LlvmSteps* s, int jit, int portable, char opt, char* error, size_t error_size) {
    static SnxIrFeatures plain; /* the architecture alone */
    memset(s, 0, sizeof(*s));
    s->jit = jit;
    s->opt = opt;
    s->portable = portable;
    if (snx_ir_init(error, error_size)) return -1;
    s->ir = jit ? snx_ir_new_jit("steps", opt, error, error_size) : snx_ir_new("steps", opt, error, error_size);
    if (!s->ir) return -1;
    if (portable) {
        memset(&plain, 0, sizeof(plain));
        plain.x86 = s->ir->features->x86;
        plain.aarch64 = s->ir->features->aarch64;
        s->ir->features = &plain;
    }
    return 0;
}

size_t llvm_steps_add(LlvmSteps* s, u32 word, u64 pc, u64 features) {
    char name[32];
    if (s->count == s->cap) {
        s->cap = s->cap ? s->cap * 2 : 256;
        s->fns = (LLVMValueRef*)snx_realloc(s->fns, s->cap * sizeof(*s->fns));
    }
    sprintf(name, "S_%lu", (unsigned long)s->count);
    s->fns[s->count] = snx_ir_static_step(s->ir, word, pc, features, name);
    LLVMSetVisibility(s->fns[s->count], LLVMHiddenVisibility);
    return s->count++;
}

size_t llvm_steps_add_function(LlvmSteps* s, SnxCodegen* codegen, const SnxFunction* function) {
    if (s->count == s->cap) {
        s->cap = s->cap ? s->cap * 2 : 256;
        s->fns = (LLVMValueRef*)snx_realloc(s->fns, s->cap * sizeof(*s->fns));
    }
    snx_ir_emit(s->ir, codegen, function);
    s->fns[s->count] = snx_ir_guest_function(s->ir, snx_function_address(function, 0));
    return s->count++;
}

/* a unit that reaches into this executable, as JIT code reaches into the runtime: a hidden function, called
   (R_X86_64_PLT32, through a stub when it is out of reach), and a default-visibility variable (through the GOT) */
static u64 probe_value = 0x1234;
static u64 probe_add(u64 a) { return a + 7; }

static int load_probe(SnxCodeLoader* loader, char* error, size_t error_size) {
    const char* names[1] = {"snx_test_probe"};
    SnxU64 address;
    SnxIr* ir = snx_ir_new_jit("probe", '2', error, error_size);
    LLVMValueRef value, add, probe, args[1];
    LLVMTypeRef params[1];
    unsigned char* object;
    size_t object_size, used;
    SnxLinkInput input;
    u64 (*call)(u64);
    if (!ir) return -1;
    params[0] = ir->t_i64;
    value = LLVMAddGlobal(ir->mod, ir->t_i64, "snx_test_probe_value");
    add = ir_extern(ir, "snx_test_probe_add", ir->t_i64, params, 1);
    probe = LLVMAddFunction(ir->mod, "snx_test_probe", LLVMFunctionType(ir->t_i64, params, 1, 0));
    LLVMPositionBuilderAtEnd(ir->b, LLVMAppendBasicBlockInContext(ir->ctx, probe, "entry"));
    args[0] = LLVMGetParam(probe, 0);
    LLVMBuildRet(ir->b, LLVMBuildAdd(ir->b, LLVMBuildLoad2(ir->b, ir->t_i64, value, "v"),
                                     LLVMBuildCall2(ir->b, LLVMGlobalGetValueType(add), add, args, 1, "c"), "r"));
    if (snx_ir_object(ir, '2', &object, &object_size, error, error_size)) return -1;
    snx_ir_free(ir);
    input.data = object;
    input.size = object_size;
    input.name = "probe";
    if (snx_code_define(loader, "snx_test_probe_value", (SnxU64)(size_t)&probe_value) ||
        snx_code_define(loader, "snx_test_probe_add", (SnxU64)(size_t)&probe_add))
        return fail(error, error_size, "jit: the probe's symbols are defined already", NULL);
    if (snx_code_load(loader, &input, 1, names, 1, &address, error, error_size)) return -1;
    call = (u64 (*)(u64))(size_t)address;
    if (call(100) != 100 + 7 + 0x1234) return fail(error, error_size, "jit: the probe returned a wrong sum", NULL);
    /* the same unit again defines snx_test_probe twice: refused, with the arena as it was */
    used = snx_code_used(loader);
    if (!snx_code_load(loader, &input, 1, NULL, 0, NULL, error, error_size) || snx_code_used(loader) != used)
        return fail(error, error_size, "jit: a second snx_test_probe was loaded", NULL);
    free(object);
    /* an empty unit names what earlier ones defined */
    if (snx_code_load(loader, NULL, 0, names, 1, &address, error, error_size) || address != (SnxU64)(size_t)call)
        return fail(error, error_size, "jit: snx_test_probe is not where it was loaded", NULL);
    return 0;
}

int llvm_steps_dump(LlvmSteps* s, const char* prefix, char* error, size_t error_size) {
    char *raw = snx_format("%s-raw.ll", prefix), *after = snx_format("%s-after.ll", prefix);
    char* path = snx_format("%s.o", prefix);
    char* message = NULL;
    unsigned char* object = NULL;
    size_t size = 0;
    int failed = LLVMPrintModuleToFile(s->ir->mod, raw, &message);
    if (!failed) failed = snx_ir_object(s->ir, s->opt, &object, &size, error, error_size);
    if (!failed) failed = LLVMPrintModuleToFile(s->ir->mod, after, &message);
    if (!failed && snx_write_file(path, object, size)) {
        snprintf(error, error_size, "cannot write %s", path);
        failed = 1;
    }
    if (message) { snprintf(error, error_size, "%s", message); LLVMDisposeMessage(message); }
    free(raw); free(after); free(path); free(object);
    return failed ? -1 : 0;
}

int llvm_steps_load(LlvmSteps* s, char* error, size_t error_size) {
    const char* exports[1] = {"snx_test_steps"};
    LLVMValueRef table, getter;
    unsigned char* object;
    size_t object_size;
    SnxLinkInput inputs[2];
    TableFn get = NULL;
    SnxIr* ir = s->ir;
    if (!s->count) llvm_steps_add(s, 0xD503201Fu, 0, 0); /* an empty table is no array */
    table = LLVMAddGlobal(ir->mod, LLVMArrayType(ir->t_ptr, (unsigned)s->count), "TABLE");
    LLVMSetLinkage(table, LLVMInternalLinkage);
    LLVMSetInitializer(table, LLVMConstArray(ir->t_ptr, s->fns, (unsigned)s->count));
    getter = LLVMAddFunction(ir->mod, "snx_test_steps", LLVMFunctionType(ir->t_ptr, NULL, 0, 0));
    LLVMPositionBuilderAtEnd(ir->b, LLVMAppendBasicBlockInContext(ir->ctx, getter, "entry"));
    LLVMBuildRet(ir->b, table);
    if (ir->fallbacks) return fail(error, error_size, "a word has no emitter", NULL);
    if (snx_ir_object(ir, s->opt, &object, &object_size, error, error_size)) return -1;
    snx_ir_free(ir);
    s->ir = NULL;
    inputs[0].data = snx_rtlib_object;
    inputs[0].size = snx_rtlib_object_size;
    inputs[0].name = "rtlib";
    inputs[1].data = object;
    inputs[1].size = object_size;
    inputs[1].name = "steps";
    if (s->jit) {
        /* the runtime library first, as the JIT service loads it, then the steps calling into it */
        SnxU64 address;
        s->loader = snx_code_loader_new(0, error, error_size);
        if (!s->loader || snx_code_load(s->loader, &inputs[0], 1, NULL, 0, NULL, error, error_size) ||
            snx_code_load(s->loader, &inputs[1], 1, exports, 1, &address, error, error_size) ||
            load_probe(s->loader, error, error_size)) {
            free(object);
            return -1;
        }
        get = (TableFn)(size_t)address;
    } else {
        SnxLinkOutput linked;
        char* path;
        s->dir = snx_temp_dir(NULL);
        if (!s->dir) {
            free(object);
            return fail(error, error_size, "cannot make a temporary directory", NULL);
        }
#ifdef _WIN32
        path = snx_path_join(s->dir, "steps.dll");
#else
        path = snx_path_join(s->dir, "steps.so");
#endif
        /* The embedded object belongs to the JIT loader: on Apple it is ELF with Darwin's calling convention.
           A native module instead needs the helper bitcode compiled to its own object format, as AOT does. */
        unsigned char* rtlib;
        size_t rtlib_size;
        if (snx_ir_rtlib_object(s->opt, &rtlib, &rtlib_size, error, error_size)) {
            free(object);
            free(path);
            return -1;
        }
        inputs[0].data = rtlib;
        inputs[0].size = rtlib_size;
        int rc = snx_link(inputs, 2, exports, 1, "steps", &linked, error, error_size);
        free(rtlib);
        if (rc) {
            free(object);
            free(path);
            return -1;
        }
        if (snx_write_file(path, linked.data, linked.size)) {
            free(linked.data);
            free(object);
            free(path);
            return fail(error, error_size, "cannot write the module", NULL);
        }
        free(linked.data);
#ifdef _WIN32
        s->library = LoadLibraryA(path);
        get = s->library ? (TableFn)(void*)GetProcAddress((HMODULE)s->library, "snx_test_steps") : NULL;
        if (!get) fail(error, error_size, "cannot load the module", NULL);
#else
        s->library = dlopen(path, RTLD_NOW | RTLD_LOCAL);
        get = s->library ? (TableFn)dlsym(s->library, "snx_test_steps") : NULL;
        if (!get) fail(error, error_size, "cannot load the module", dlerror());
#endif
        free(path);
    }
    free(object);
    if (!get) return -1;
    s->steps = get();
    return 0;
}

void llvm_steps_end(LlvmSteps* s) {
    if (s->ir) snx_ir_free(s->ir);
    if (s->loader) snx_code_loader_free(s->loader);
#ifdef _WIN32
    if (s->library) FreeLibrary((HMODULE)s->library);
#else
    if (s->library) dlclose(s->library);
#endif
    if (s->dir) {
        snx_remove_tree(s->dir);
        free(s->dir);
    }
    free(s->fns);
    memset(s, 0, sizeof(*s));
}
