/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A game module's fallback made by LLVM (module_fallback.h), in builds with the backend: the descriptor of a 64-bit
   module in LLVM IR where LLVM made some of its code, and a 32-bit program's blocks the AArch32 block compiler
   (cpu/arm/llvm.h) makes, chained. GNU C23, for the LLVM-C headers */
#include "nx/loader/module_fallback.h"
#include "nx/loader/module_abi.h"
#include "nx/loader/module_variants.h"
#include "nx/loader/hook_routes.h"
#include "common/cpu/arm/llvm.h"
#include "common/cpu/recompiler/llvm/ir.h"
#include "common/utilities/bits.h"
#include "common/utilities/common.h"
#include "common/utilities/endian.h"
#include "common/platform/file.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* the module's i64 the runtime writes (SnxArmModule.poll_offset), which chained blocks read */
#define POLL_OFFSET "snx_arm_poll_offset"

char* snx_module_fallback_cache_key(const char* path, char opt, int hook, SnxU64 features, char* error,
                                    size_t error_size) {
    char* key = snx_jit_cache_key(opt, hook, 1, features);
    (void)path;
    if (!key) snprintf(error, error_size, "cannot make the JIT cache key");
    return key;
}

/* name's IR kept in dir (--keep-ir): nonzero, with error saying which and where, when it could not be written */
static int save_ir(SnxIr* ir, const char* dir, const char* name, char* error, size_t error_size) {
    char *text = snx_ir_text(ir), *file = snx_format("%s.ll", name), *path = snx_path_join(dir, file);
    int rc = snx_write_file(path, text, strlen(text));
    if (rc) snprintf(error, error_size, "cannot save the IR of %s in %s", name, dir);
    LLVMDisposeMessage(text);
    free(file);
    free(path);
    return rc;
}

/* the module's IR, made once LLVM is: another machine's for a triple */
static SnxIr* new_ir(const SnxLlvmBuild* build, const char* name, char* error, size_t error_size) {
    if (snx_ir_init(error, error_size)) return NULL;
    return build->triple ? snx_ir_new_target(name, build->opt, build->triple, error, error_size)
                         : snx_ir_new(name, build->opt, error, error_size);
}

/* ---- the module descriptor: module.c's emit_module, as IR ---- */

static LLVMValueRef global(SnxIr* ir, LLVMTypeRef type, const char* name, LLVMValueRef value, int constant) {
    LLVMValueRef g = LLVMAddGlobal(ir->mod, type, name);
    LLVMSetLinkage(g, LLVMInternalLinkage);
    LLVMSetInitializer(g, value ? value : LLVMConstNull(type));
    LLVMSetGlobalConstant(g, constant);
    return g;
}

static LLVMValueRef snx_module_string(SnxIr* ir, const char* text) {
    LLVMValueRef value = LLVMConstStringInContext(ir->ctx, text, (unsigned)strlen(text), 0);
    LLVMValueRef g = global(ir, LLVMTypeOf(value), "", value, 1);
    LLVMSetUnnamedAddress(g, LLVMGlobalUnnamedAddr);
    return g;
}

static LLVMValueRef element(SnxIr* ir, LLVMTypeRef type, LLVMValueRef array, LLVMValueRef index) {
    LLVMValueRef indices[2];
    indices[0] = ir_i64(ir, 0);
    indices[1] = index;
    return LLVMBuildInBoundsGEP2(ir->b, type, array, indices, 2, "");
}

static void field(SnxIr* ir, LLVMValueRef base, size_t offset, LLVMValueRef value) {
    LLVMValueRef index = ir_i64(ir, offset);
    LLVMBuildStore(ir->b, value, LLVMBuildInBoundsGEP2(ir->b, ir->t_i8, base, &index, 1, ""));
}

/* SnxModInfo as an LLVM struct (NULL with the reason when its layout differs from C's) */
static LLVMTypeRef snx_module_info_type(SnxIr* ir, char* error, size_t error_size) {
    LLVMTypeRef fields[11], type;
    LLVMTargetDataRef layout;
    size_t i;
    static const size_t offsets[11] = {
        offsetof(SnxModInfo, name),      offsetof(SnxModInfo, base),     offsetof(SnxModInfo, size),
        offsetof(SnxModInfo, build_id),  offsetof(SnxModInfo, text_off), offsetof(SnxModInfo, text_size),
        offsetof(SnxModInfo, ro_off),    offsetof(SnxModInfo, ro_size),  offsetof(SnxModInfo, data_off),
        offsetof(SnxModInfo, data_size), offsetof(SnxModInfo, bss_size)};
    int ok;
    fields[0] = ir->t_ptr;
    fields[1] = fields[2] = ir->t_i64;
    fields[3] = LLVMArrayType(ir->t_i8, 32);
    for (i = 4; i < 11; ++i) fields[i] = ir->t_i32;
    type = LLVMStructTypeInContext(ir->ctx, fields, 11, 0);
    {
        LLVMTypeRef all[12];
        memcpy(all, fields, sizeof(fields));
        all[11] = ir->t_i32; /* is_nro */
        type = LLVMStructTypeInContext(ir->ctx, all, 12, 0);
    }
    layout = LLVMGetModuleDataLayout(ir->mod);
    ok = LLVMABISizeOfType(layout, type) == sizeof(SnxModInfo) &&
         LLVMOffsetOfElement(layout, type, 11) == offsetof(SnxModInfo, is_nro);
    for (i = 0; i < 11; ++i) ok = ok && LLVMOffsetOfElement(layout, type, (unsigned)i) == offsets[i];
    if (!ok) {
        snprintf(error, error_size, "SnxModInfo layout differs between the recompiler and the LLVM target");
        return NULL;
    }
    return type;
}

static LLVMValueRef snx_module_mods(SnxIr* ir, LLVMTypeRef info, const SnxProgram* loaded) {
    LLVMValueRef *values, mods;
    size_t i, j;
    values = (LLVMValueRef*)snx_alloc(loaded->module_count * sizeof(*values));
    for (i = 0; i < loaded->module_count; ++i) {
        const SnxModuleImage* m = &loaded->modules[i];
        LLVMValueRef f[12], id[32];
        f[0] = snx_module_string(ir, m->name);
        f[1] = ir_i64(ir, m->base);
        f[2] = ir_i64(ir, m->size);
        for (j = 0; j < 32; ++j) id[j] = ir_i8(ir, m->build_id[j]);
        f[3] = LLVMConstArray(ir->t_i8, id, 32);
        f[4] = ir_i32(ir, m->text_off);
        f[5] = ir_i32(ir, m->text_size);
        f[6] = ir_i32(ir, m->ro_off);
        f[7] = ir_i32(ir, m->ro_size);
        f[8] = ir_i32(ir, m->data_off);
        f[9] = ir_i32(ir, m->data_size);
        f[10] = ir_i32(ir, m->bss);
        f[11] = ir_i32(ir, !!m->is_nro);
        values[i] = LLVMConstNamedStruct(info, f, 12);
    }
    mods = global(ir, LLVMArrayType(info, (unsigned)loaded->module_count), "MODS",
                  LLVMConstArray(info, values, (unsigned)loaded->module_count), 1);
    free(values);
    return mods;
}

static int emit_module(SnxIr* ir, const SnxCodegen* codegen, const SnxCodegenOptions* options, const SnxProgram* loaded,
                       int wrappers, size_t* wrapped, size_t* entry_count, char* error, size_t error_size) {
    SnxModuleEntry* entries;
    size_t count, arraysize, i, nfns = codegen->function_count ? codegen->function_count : 1;
    LLVMValueRef *values, fns, fn_lo, fn_len, stale, ent_off, ent_fn, ent_addr, ent_ptr, mods, mod, fn, loop_i,
        phi_values[2];
    LLVMTypeRef info, t_fns, t_u32s, t_addrs, t_ptrs;
    LLVMBasicBlockRef start, head, body, done, phi_blocks[2];
    SnxU64 lo = ~(SnxU64)0, hi = 0;
    const SnxNpdm* npdm = &loaded->npdm;
    entries = snx_module_entries(codegen, &count);
    arraysize = count ? count : 1;
    for (i = 0; i < count; ++i)
        if (entries[i].address < SNX_CODE_BASE || entries[i].address - SNX_CODE_BASE > 0xFFFFFFFFUL) {
            free(entries);
            snprintf(error, error_size, "AOT entry exceeds the module ABI's 32-bit offset range");
            return -1;
        }
    info = snx_module_info_type(ir, error, error_size);
    if (!info) {
        free(entries);
        return -1;
    }
    values = (LLVMValueRef*)snx_alloc((nfns > arraysize ? nfns : arraysize) * sizeof(*values));
    /* static const GuestFn FNS[]: every function */
    for (i = 0; i < codegen->function_count; ++i) values[i] = snx_ir_guest_function(ir, codegen->functions[i].start);
    if (!codegen->function_count) values[0] = LLVMConstNull(ir->t_ptr);
    t_fns = LLVMArrayType(ir->t_ptr, (unsigned)nfns);
    fns = global(ir, t_fns, "FNS", LLVMConstArray(ir->t_ptr, values, (unsigned)nfns), 1);
    /* static const u64 FN_LO[]; static const u32 FN_LEN[]: the guest code each function was compiled from, from its
       lowest instruction to the end of its highest (SNX_ABI 14: what a line the guest changes makes stale) */
    for (i = 0; i < nfns; ++i) {
        const SnxFunction* f = i < codegen->function_count ? &codegen->functions[i] : NULL;
        values[i] = ir_i64(ir, f && f->count ? snx_function_address(f, 0) : 0);
    }
    fn_lo = global(ir, LLVMArrayType(ir->t_i64, (unsigned)nfns), "FN_LO",
                   LLVMConstArray(ir->t_i64, values, (unsigned)nfns), 1);
    for (i = 0; i < nfns; ++i) {
        const SnxFunction* f = i < codegen->function_count ? &codegen->functions[i] : NULL;
        SnxU64 first = f && f->count ? snx_function_address(f, 0) : 0;
        SnxU64 end = f && f->count ? snx_function_address(f, f->count - 1) + 4 : 0;
        if (end - first > 0xFFFFFFFFu) {
            free(values);
            free(entries);
            snprintf(error, error_size, "a function's code spans more than the module ABI's 32-bit length");
            return -1;
        }
        values[i] = ir_i32(ir, (SnxU32)(end - first));
    }
    fn_len = global(ir, LLVMArrayType(ir->t_i32, (unsigned)nfns), "FN_LEN",
                    LLVMConstArray(ir->t_i32, values, (unsigned)nfns), 1);
    /* u8 snx_stale[]: the byte each function reads first (ir_codegen.c's stale_check), the runtime's to set */
    stale = LLVMAddGlobal(ir->mod, LLVMArrayType(ir->t_i8, (unsigned)nfns), SNX_IR_STALE);
    LLVMSetInitializer(stale, LLVMConstNull(LLVMArrayType(ir->t_i8, (unsigned)nfns)));
    LLVMSetVisibility(stale, LLVMHiddenVisibility);
    /* static const u32 ENT_OFF[], ENT_FN[] */
    t_u32s = LLVMArrayType(ir->t_i32, (unsigned)arraysize);
    for (i = 0; i < arraysize; ++i)
        values[i] = ir_i32(ir, i < count ? (SnxU32)(entries[i].address - SNX_CODE_BASE) : 0);
    ent_off = global(ir, t_u32s, "ENT_OFF", LLVMConstArray(ir->t_i32, values, (unsigned)arraysize), 1);
    for (i = 0; i < arraysize; ++i) values[i] = ir_i32(ir, i < count ? (SnxU32)entries[i].function : 0);
    ent_fn = global(ir, t_u32s, "ENT_FN", LLVMConstArray(ir->t_i32, values, (unsigned)arraysize), 1);
    /* static u64 ENT_ADDR[]; static GuestFn ENT_PTR[]; */
    t_addrs = LLVMArrayType(ir->t_i64, (unsigned)arraysize);
    t_ptrs = LLVMArrayType(ir->t_ptr, (unsigned)arraysize);
    ent_addr = global(ir, t_addrs, "ENT_ADDR", NULL, 0);
    ent_ptr = global(ir, t_ptrs, "ENT_PTR", NULL, 0);
    free(values);
    /* snx_aot_<addr>: what the cache objects' direct calls name (ir_codegen.c). The
       entry's function is in a code chunk, so this is a tail-calling wrapper, not an
       alias: an alias may only name a definition in its own module */
    for (i = 0; wrappers && i < count; ++i) {
        char name[40];
        LLVMValueRef target = snx_ir_guest_function(ir, codegen->functions[entries[i].function].start);
        LLVMValueRef wrapper, call, args[2];
        sprintf(name, "snx_aot_%llx", (unsigned long long)entries[i].address);
        wrapper = LLVMAddFunction(ir->mod, name, ir->t_guest);
        LLVMPositionBuilderAtEnd(ir->b, LLVMAppendBasicBlockInContext(ir->ctx, wrapper, "entry"));
        args[0] = LLVMGetParam(wrapper, 0);
        args[1] = LLVMGetParam(wrapper, 1);
        call = LLVMBuildCall2(ir->b, ir->t_guest, target, args, 2, "");
        LLVMSetTailCallKind(call, LLVMTailCallKindTail);
        LLVMBuildRetVoid(ir->b);
        ++*wrapped;
    }
    /* static const SnxModInfo MODS[] */
    mods = snx_module_mods(ir, info, loaded);
    for (i = 0; i < loaded->module_count; ++i) {
        const SnxModuleImage* m = &loaded->modules[i];
        if (m->base + m->text_off < lo) lo = m->base + m->text_off;
        if (m->base + m->text_off + m->text_size > hi) hi = m->base + m->text_off + m->text_size;
    }
    /* static SnxModule MOD; */
    mod = global(ir, LLVMArrayType(ir->t_i8, sizeof(SnxModule)), "MOD", NULL, 0);
    LLVMSetAlignment(mod, 8);
    /* SNX_EXPORT const SnxModule* snx_module(void) */
    fn = LLVMAddFunction(ir->mod, "snx_module", LLVMFunctionType(ir->t_ptr, NULL, 0, 0));
    if (ir->windows) LLVMSetDLLStorageClass(fn, LLVMDLLExportStorageClass);
    ir->fn = fn;
    start = LLVMAppendBasicBlockInContext(ir->ctx, fn, "entry");
    head = LLVMAppendBasicBlockInContext(ir->ctx, fn, "head");
    body = LLVMAppendBasicBlockInContext(ir->ctx, fn, "body");
    done = LLVMAppendBasicBlockInContext(ir->ctx, fn, "done");
    LLVMPositionBuilderAtEnd(ir->b, start);
    LLVMBuildBr(ir->b, head);
    /* for(i=0;i<count;i++){ ENT_ADDR[i]=BASE+ENT_OFF[i]; ENT_PTR[i]=FNS[ENT_FN[i]]; } */
    LLVMPositionBuilderAtEnd(ir->b, head);
    loop_i = LLVMBuildPhi(ir->b, ir->t_i32, "i");
    LLVMBuildCondBr(ir->b, LLVMBuildICmp(ir->b, LLVMIntULT, loop_i, ir_i32(ir, (SnxU32)count), ""), body, done);
    LLVMPositionBuilderAtEnd(ir->b, body);
    {
        LLVMValueRef index = LLVMBuildZExt(ir->b, loop_i, ir->t_i64, ""), offset, function;
        offset = ir_load(ir, ir->t_i32, element(ir, t_u32s, ent_off, index));
        ir_store(ir, LLVMBuildAdd(ir->b, ir_i64(ir, SNX_CODE_BASE), LLVMBuildZExt(ir->b, offset, ir->t_i64, ""), ""),
                 element(ir, t_addrs, ent_addr, index));
        function = ir_load(ir, ir->t_i32, element(ir, t_u32s, ent_fn, index));
        function = ir_load(ir, ir->t_ptr, element(ir, t_fns, fns, LLVMBuildZExt(ir->b, function, ir->t_i64, "")));
        ir_store(ir, function, element(ir, t_ptrs, ent_ptr, index));
        phi_values[1] = LLVMBuildAdd(ir->b, loop_i, ir_i32(ir, 1), "");
    }
    LLVMBuildBr(ir->b, head);
    phi_values[0] = ir_i32(ir, 0);
    phi_blocks[0] = start;
    phi_blocks[1] = body;
    LLVMAddIncoming(loop_i, phi_values, phi_blocks, 2);
    LLVMPositionBuilderAtEnd(ir->b, done);
    field(ir, mod, offsetof(SnxModule, abi), ir_i32(ir, options->abi));
    field(ir, mod, offsetof(SnxModule, nmods), ir_i32(ir, (SnxU32)loaded->module_count));
    field(ir, mod, offsetof(SnxModule, mods), mods);
    field(ir, mod, offsetof(SnxModule, nentries), ir_i32(ir, (SnxU32)count));
    field(ir, mod, offsetof(SnxModule, entry_addr), ent_addr);
    field(ir, mod, offsetof(SnxModule, entry_fn), ent_ptr);
    /* no interpreter any more: the runtime's JIT compiles what the module lacks. The field stays (the layout since
       SNX_ABI 8) */
    field(ir, mod, offsetof(SnxModule, interp), LLVMConstNull(ir->t_ptr));
    field(ir, mod, offsetof(SnxModule, code_lo), ir_i64(ir, lo));
    field(ir, mod, offsetof(SnxModule, code_hi), ir_i64(ir, hi));
    field(ir, mod, offsetof(SnxModule, title), snx_module_string(ir, options->title ? options->title : "Game"));
    field(ir, mod, offsetof(SnxModule, title_id), ir_i64(ir, npdm->title_id));
    field(ir, mod, offsetof(SnxModule, main_stack), ir_i32(ir, (SnxU32)npdm->stack));
    field(ir, mod, offsetof(SnxModule, main_prio), ir_i32(ir, npdm->prio));
    field(ir, mod, offsetof(SnxModule, main_core), ir_i32(ir, npdm->core));
    field(ir, mod, offsetof(SnxModule, nfns), ir_i32(ir, (SnxU32)codegen->function_count));
    field(ir, mod, offsetof(SnxModule, fn), fns);
    field(ir, mod, offsetof(SnxModule, fn_lo), fn_lo);
    field(ir, mod, offsetof(SnxModule, fn_len), fn_len);
    field(ir, mod, offsetof(SnxModule, stale), stale);
    LLVMBuildRet(ir->b, mod);
    free(entries);
    *entry_count = count;
    return 0;
}

/* SNX_EXPORT const SnxRoutes* snx_routes(void): the route list the module's calls were compiled with (module_abi.h),
   for hook_install to say which hook of the runtime's the module calls directly */
static int emit_routes(SnxIr* ir, int all, char* error, size_t error_size) {
    size_t count = snx_hook_route_count();
    SnxU64* hashes = (SnxU64*)snx_alloc((count ? count : 1) * sizeof(*hashes));
    LLVMValueRef* values = (LLVMValueRef*)snx_alloc((count ? count : 1) * sizeof(*values));
    for (size_t i = 0; i < count; ++i) hashes[i] = snx_hook_route_hash(snx_hook_route_name(i));
    qsort(hashes, count, sizeof(*hashes), snx_compare_u64);
    for (size_t i = 0; i < count; ++i) values[i] = ir_i64(ir, hashes[i]);
    LLVMValueRef names = global(ir, LLVMArrayType(ir->t_i64, (unsigned)count), "ROUTE_NAMES",
                                LLVMConstArray(ir->t_i64, values, (unsigned)count), 1);
    free(values);
    free(hashes);
    LLVMTypeRef fields[6] = {ir->t_i32, ir->t_i32, ir->t_i64, ir->t_ptr, ir->t_i32, ir->t_i32};
    LLVMTypeRef type = LLVMStructTypeInContext(ir->ctx, fields, 6, 0);
    LLVMTargetDataRef layout = LLVMGetModuleDataLayout(ir->mod);
    if (LLVMABISizeOfType(layout, type) != sizeof(SnxRoutes) ||
        LLVMOffsetOfElement(layout, type, 2) != offsetof(SnxRoutes, list) ||
        LLVMOffsetOfElement(layout, type, 3) != offsetof(SnxRoutes, names) ||
        LLVMOffsetOfElement(layout, type, 4) != offsetof(SnxRoutes, all)) {
        snprintf(error, error_size, "SnxRoutes layout differs between the recompiler and the LLVM target");
        return -1;
    }
    LLVMValueRef f[6] = {ir_i32(ir, SNX_ROUTES_VERSION),
                         ir_i32(ir, (SnxU32)count),
                         ir_i64(ir, snx_hook_route_list_hash()),
                         names,
                         ir_i32(ir, all != 0),
                         ir_i32(ir, 0)};
    LLVMValueRef routes = global(ir, type, "ROUTES", LLVMConstStructInContext(ir->ctx, f, 6, 0), 1);
    LLVMValueRef fn = LLVMAddFunction(ir->mod, "snx_routes", LLVMFunctionType(ir->t_ptr, NULL, 0, 0));
    if (ir->windows) LLVMSetDLLStorageClass(fn, LLVMDLLExportStorageClass);
    ir->fn = fn;
    LLVMPositionBuilderAtEnd(ir->b, LLVMAppendBasicBlockInContext(ir->ctx, fn, "entry"));
    LLVMBuildRet(ir->b, routes);
    return 0;
}

/* SNX_EXPORT const SnxNros* snx_nros(void): where the NROs the program maps while it runs go (module_abi.h), for
   ldr:ro to map each at the address its own module was made for */
static int emit_nros(SnxIr* ir, const SnxCodegenOptions* options, const SnxProgram* loaded, char* error,
                     size_t error_size) {
    LLVMTypeRef slot_fields[3] = {LLVMArrayType(ir->t_i8, 32), ir->t_i64, ir->t_i64};
    LLVMTypeRef slot_type = LLVMStructTypeInContext(ir->ctx, slot_fields, 3, 0);
    LLVMTypeRef fields[5] = {ir->t_i32, ir->t_i32, ir->t_i64, ir->t_i64, ir->t_ptr};
    LLVMTypeRef type = LLVMStructTypeInContext(ir->ctx, fields, 5, 0);
    LLVMTargetDataRef layout = LLVMGetModuleDataLayout(ir->mod);
    SnxU64 lo = 0;
    if (LLVMABISizeOfType(layout, slot_type) != sizeof(SnxNroSlot) ||
        LLVMOffsetOfElement(layout, slot_type, 1) != offsetof(SnxNroSlot, base) ||
        LLVMOffsetOfElement(layout, slot_type, 2) != offsetof(SnxNroSlot, size) ||
        LLVMABISizeOfType(layout, type) != sizeof(SnxNros) ||
        LLVMOffsetOfElement(layout, type, 2) != offsetof(SnxNros, lo) ||
        LLVMOffsetOfElement(layout, type, 3) != offsetof(SnxNros, hi) ||
        LLVMOffsetOfElement(layout, type, 4) != offsetof(SnxNros, slots)) {
        snprintf(error, error_size, "SnxNros layout differs between the recompiler and the LLVM target");
        return -1;
    }
    for (size_t i = 0; i < loaded->module_count; ++i)
        if (loaded->modules[i].base + loaded->modules[i].size > lo)
            lo = loaded->modules[i].base + loaded->modules[i].size;
    LLVMValueRef* values = (LLVMValueRef*)snx_alloc(options->nro_count * sizeof(*values));
    for (size_t i = 0; i < options->nro_count; ++i) {
        LLVMValueRef f[3], hash[32];
        for (size_t j = 0; j < 32; ++j) hash[j] = ir_i8(ir, options->nros[i].hash[j]);
        f[0] = LLVMConstArray(ir->t_i8, hash, 32);
        f[1] = ir_i64(ir, options->nros[i].base);
        f[2] = ir_i64(ir, options->nros[i].size);
        values[i] = LLVMConstStructInContext(ir->ctx, f, 3, 0);
    }
    LLVMValueRef slots = global(ir, LLVMArrayType(slot_type, (unsigned)options->nro_count), "NRO_SLOTS",
                                LLVMConstArray(slot_type, values, (unsigned)options->nro_count), 1);
    free(values);
    LLVMValueRef f[5] = {ir_i32(ir, SNX_NROS_VERSION), ir_i32(ir, (SnxU32)options->nro_count), ir_i64(ir, lo),
                         ir_i64(ir, options->nro_end), slots};
    LLVMValueRef nros = global(ir, type, "NROS", LLVMConstStructInContext(ir->ctx, f, 5, 0), 1);
    LLVMValueRef fn = LLVMAddFunction(ir->mod, "snx_nros", LLVMFunctionType(ir->t_ptr, NULL, 0, 0));
    if (ir->windows) LLVMSetDLLStorageClass(fn, LLVMDLLExportStorageClass);
    ir->fn = fn;
    LLVMPositionBuilderAtEnd(ir->b, LLVMAppendBasicBlockInContext(ir->ctx, fn, "entry"));
    LLVMBuildRet(ir->b, nros);
    return 0;
}

/* SNX_EXPORT const SnxNroImports* snx_imports(void), and snx_aot_<address> for each import: what the code units call
   for a function of the game's modules (ir_codegen.c's call), a jump through the import's pointer. Each pointer starts
   as snx_import_dyn, CALLDYN(address), and the runtime puts the game module's function there (nro_slots.h) */
static int emit_imports(SnxIr* ir, const SnxU64* imports, size_t count, char* error, size_t error_size) {
    LLVMTypeRef fields[5] = {ir->t_i32, ir->t_i32, ir->t_ptr, ir->t_ptr, ir->t_ptr};
    LLVMTypeRef type = LLVMStructTypeInContext(ir->ctx, fields, 5, 0);
    LLVMTypeRef t_fns = LLVMArrayType(ir->t_ptr, (unsigned)count), t_addrs = LLVMArrayType(ir->t_i64, (unsigned)count);
    LLVMTargetDataRef layout = LLVMGetModuleDataLayout(ir->mod);
    LLVMValueRef args[2], site;
    if (LLVMABISizeOfType(layout, type) != sizeof(SnxNroImports) ||
        LLVMOffsetOfElement(layout, type, 2) != offsetof(SnxNroImports, address) ||
        LLVMOffsetOfElement(layout, type, 3) != offsetof(SnxNroImports, function) ||
        LLVMOffsetOfElement(layout, type, 4) != offsetof(SnxNroImports, dynamic)) {
        snprintf(error, error_size, "SnxNroImports layout differs between the recompiler and the LLVM target");
        return -1;
    }
    LLVMValueRef dynamic = LLVMAddFunction(ir->mod, "snx_import_dyn", ir->t_guest);
    LLVMSetLinkage(dynamic, LLVMInternalLinkage);
    snx_ir_begin(ir, dynamic);
    args[0] = LLVMGetParam(dynamic, 0);
    args[1] = LLVMGetParam(dynamic, 1);
    site = LLVMBuildCall2(ir->b, ir->t_guest, ir_call_helper(ir, SNX_IR_LOOKUP, args, 2), args, 2, "");
    LLVMSetTailCallKind(site, LLVMTailCallKindMustTail);
    LLVMBuildRetVoid(ir->b);
    LLVMValueRef* values = (LLVMValueRef*)snx_alloc(count * sizeof(*values));
    for (size_t i = 0; i < count; ++i) values[i] = dynamic;
    LLVMValueRef fns = global(ir, t_fns, "IMPORT_FN", LLVMConstArray(ir->t_ptr, values, (unsigned)count), 0);
    for (size_t i = 0; i < count; ++i) values[i] = ir_i64(ir, imports[i]);
    LLVMValueRef addrs = global(ir, t_addrs, "IMPORT_ADDR", LLVMConstArray(ir->t_i64, values, (unsigned)count), 1);
    free(values);
    for (size_t i = 0; i < count; ++i) {
        char name[40];
        sprintf(name, "snx_aot_%llx", (unsigned long long)imports[i]);
        LLVMValueRef thunk = LLVMAddFunction(ir->mod, name, ir->t_guest);
        LLVMSetVisibility(thunk, LLVMHiddenVisibility);
        LLVMPositionBuilderAtEnd(ir->b, LLVMAppendBasicBlockInContext(ir->ctx, thunk, "entry"));
        args[0] = LLVMGetParam(thunk, 0);
        args[1] = LLVMGetParam(thunk, 1);
        /* relaxed: the runtime changes it while guest threads run (a slot taken later, nro_slots.h) */
        LLVMValueRef to = ir_load(ir, ir->t_ptr, element(ir, t_fns, fns, ir_i64(ir, i)));
        LLVMSetOrdering(to, LLVMAtomicOrderingMonotonic);
        site = LLVMBuildCall2(ir->b, ir->t_guest, to, args, 2, "");
        LLVMSetTailCallKind(site, LLVMTailCallKindMustTail);
        LLVMBuildRetVoid(ir->b);
    }
    LLVMValueRef f[5] = {ir_i32(ir, SNX_NRO_IMPORTS_VERSION), ir_i32(ir, (SnxU32)count), addrs, fns, dynamic};
    LLVMValueRef table = global(ir, type, "IMPORTS", LLVMConstStructInContext(ir->ctx, f, 5, 0), 1);
    LLVMValueRef fn = LLVMAddFunction(ir->mod, "snx_imports", LLVMFunctionType(ir->t_ptr, NULL, 0, 0));
    if (ir->windows) LLVMSetDLLStorageClass(fn, LLVMDLLExportStorageClass);
    ir->fn = fn;
    LLVMPositionBuilderAtEnd(ir->b, LLVMAppendBasicBlockInContext(ir->ctx, fn, "entry"));
    LLVMBuildRet(ir->b, table);
    return 0;
}

int snx_module_fallback_descriptor(const SnxCodegen* codegen, const SnxCodegenOptions* options,
                                   const SnxProgram* loaded, const SnxLlvmBuild* build, const SnxU64* imports,
                                   size_t import_count, const SnxJitCache* cache, unsigned char** object, size_t* size,
                                   size_t* entry_count, size_t* wrapped, size_t* variant_count, char* error,
                                   size_t error_size) {
    const size_t cache_count = cache ? snx_jit_cache_count(cache) : 0;
    *object = NULL;
    *size = *entry_count = *wrapped = *variant_count = 0;
    SnxIr* ir = new_ir(build, "module", error, error_size);
    if (!ir) return -1;
    /* only the profile cache's units name the wrappers: a module without any needs none */
    int rc = emit_module(ir, codegen, options, loaded, cache_count != 0, wrapped, entry_count, error, error_size) ||
                     emit_routes(ir, options->hook_direct_calls, error, error_size) ||
                     (import_count && emit_imports(ir, imports, import_count, error, error_size)) ||
                     (options->nro_count && emit_nros(ir, options, loaded, error, error_size)) ||
                     (cache_count && snx_module_variants_emit(ir, cache, variant_count, error, error_size)) ||
                     (build->keep_ir && save_ir(ir, build->keep_ir, "module", error, error_size)) ||
                     snx_ir_object(ir, build->opt, object, size, error, error_size)
                 ? -1
                 : 0;
    snx_ir_free(ir);
    return rc;
}

/* ---- a 32-bit program's blocks ---- */

int snx_module_fallback_arm_start(char* error, size_t error_size) { return snx_ir_init(error, error_size); }

static void block_name(char* out, SnxU32 key) {
    sprintf(out, "%c_%08lx", key & 1 ? 'T' : 'A', (unsigned long)(key & ~1u));
}

/* the halfwords or words a block compiler reads: the guest's little-endian code as host integers */
static void* units_of(const unsigned char* code, size_t count, int thumb) {
    size_t i;
    if (thumb) {
        uint16_t* h = (uint16_t*)snx_alloc(count * sizeof(*h));
        for (i = 0; i < count; ++i) h[i] = (uint16_t)(code[i * 2] | code[i * 2 + 1] << 8);
        return h;
    } else {
        uint32_t* w = (uint32_t*)snx_alloc(count * sizeof(*w));
        for (i = 0; i < count; ++i) w[i] = snx_ld_le32(code + i * 4);
        return w;
    }
}

/* compiles the block into module as name, and *units how much code it covers. NULL when the block compiler refuses
   the input (it doesn't for code it can read: it compiles what it can't run to an exit) */
static LLVMValueRef compile_block(LLVMModuleRef module, const SnxModuleArmCode* b, const char* name, size_t* units) {
    size_t consumed = 0;
    int thumb = (int)(b->key & 1);
    SnxU32 address = b->key & ~1u;
    void* input = units_of(b->code, b->count, thumb);
    LLVMValueRef fn = thumb ? snx_thumb_llvm_block(module, name, (const uint16_t*)input, b->count, address, &consumed)
                            : snx_arm_llvm_block(module, name, (const uint32_t*)input, b->count, address, &consumed);
    free(input);
    *units = consumed;
    return fn;
}

/* a thread's module for discovery's blocks, each compiled once there to learn where it ends and thrown away: the
   units compile them again, in parallel */
struct SnxModuleArmScratch {
    SnxIr* ir;
};
int snx_module_fallback_arm_measure(SnxModuleArmScratch** scratch, const SnxModuleArmCode* block, const char* why,
                                    size_t* units, char* error, size_t error_size) {
    (void)why;
    *units = 0;
    if (!*scratch) {
        SnxIr* ir = snx_ir_init(error, error_size) ? NULL : snx_ir_new("discovery", '0', error, error_size);
        if (!ir) return -1;
        if (!(*scratch = (SnxModuleArmScratch*)calloc(1, sizeof **scratch))) {
            snx_ir_free(ir);
            snprintf(error, error_size, "out of memory");
            return -1;
        }
        (*scratch)->ir = ir;
    }
    LLVMValueRef fn = compile_block((*scratch)->ir->mod, block, "block", units);
    if (!fn || !*units) {
        char name[16];
        block_name(name, block->key);
        snprintf(error, error_size, "the AArch32 block compiler refused the block %s", name);
        return -1;
    }
    LLVMDeleteFunction(fn);
    return 0;
}
void snx_module_fallback_arm_scratch_free(SnxModuleArmScratch* scratch) {
    if (!scratch) return;
    snx_ir_free(scratch->ir);
    free(scratch);
}

/* Block chaining: a block that ends where a block of the module starts goes on to it by a tail call, instead of
   returning to the runtime's loop (arm_thread_run), which looks the next block up after every one. Each of the
   block's returns becomes: return when it set an exit; else, for each successor discovery found, when
   r15 and CPSR.T are its address and state, a musttail call of it; else return. A successor at or before the
   block's start (every loop has one) first reads the thread's poll, as the loop does after each block: when a thread
   of higher priority waits for the core, the block returns and the loop yields it */
static void chain_block(SnxIr* ir, LLVMValueRef fn, SnxU32 key, const SnxU32* next, size_t count) {
    LLVMTypeRef block_type = LLVMFunctionType(ir->t_void, &ir->t_ptr, 1, 0);
    LLVMValueRef *rets, cpu = LLVMGetParam(fn, 0), poll_offset = LLVMGetNamedGlobal(ir->mod, POLL_OFFSET);
    size_t nrets = 0, i, k;
    rets = (LLVMValueRef*)snx_alloc(LLVMCountBasicBlocks(fn) * sizeof(*rets));
    for (LLVMBasicBlockRef bb = LLVMGetFirstBasicBlock(fn); bb; bb = LLVMGetNextBasicBlock(bb)) {
        LLVMValueRef t = LLVMGetBasicBlockTerminator(bb);
        if (t && LLVMGetInstructionOpcode(t) == LLVMRet) rets[nrets++] = t;
    }
    if (!poll_offset) {
        poll_offset = LLVMAddGlobal(ir->mod, ir->t_i64, POLL_OFFSET);
        LLVMSetVisibility(poll_offset, LLVMHiddenVisibility); /* the module's own: reached PC-relative */
    }
    /* one tail for all of them: a block's returns are its exits too (a fault, an SVC), which the tail's first check
       takes back out */
    LLVMBasicBlockRef tail = LLVMAppendBasicBlockInContext(ir->ctx, fn, "chain"),
                      go = LLVMAppendBasicBlockInContext(ir->ctx, fn, "next"),
                      out = LLVMAppendBasicBlockInContext(ir->ctx, fn, "out");
    LLVMValueRef c, pc, t;
    for (i = 0; i < nrets; ++i) {
        LLVMPositionBuilderBefore(ir->b, rets[i]);
        LLVMBuildBr(ir->b, tail);
        LLVMInstructionEraseFromParent(rets[i]);
    }
    LLVMPositionBuilderAtEnd(ir->b, out);
    LLVMBuildRetVoid(ir->b);
    LLVMPositionBuilderAtEnd(ir->b, tail);
    c = LLVMBuildGEP2(ir->b, ir->t_i8, cpu, (LLVMValueRef[]){ir_i64(ir, offsetof(SnxArmCPU, exit))}, 1, "");
    c = LLVMBuildICmp(ir->b, LLVMIntEQ, LLVMBuildLoad2(ir->b, ir->t_i32, c, ""), ir_i32(ir, SNX_ARM_OK), "");
    LLVMBuildCondBr(ir->b, c, go, out);
    LLVMPositionBuilderAtEnd(ir->b, go);
    pc = LLVMBuildGEP2(ir->b, ir->t_i8, cpu, (LLVMValueRef[]){ir_i64(ir, offsetof(SnxArmCPU, r) + 15 * 4)}, 1, "");
    pc = LLVMBuildLoad2(ir->b, ir->t_i32, pc, "");
    t = LLVMBuildGEP2(ir->b, ir->t_i8, cpu, (LLVMValueRef[]){ir_i64(ir, offsetof(SnxArmCPU, cpsr))}, 1, "");
    t = LLVMBuildAnd(ir->b, LLVMBuildLoad2(ir->b, ir->t_i32, t, ""), ir_i32(ir, SNX_ARM_T), "");
    for (k = 0; k < count; ++k) {
        LLVMBasicBlockRef call = LLVMAppendBasicBlockInContext(ir->ctx, fn, "call"),
                          miss = LLVMAppendBasicBlockInContext(ir->ctx, fn, "other");
        LLVMValueRef target, site;
        char name[16];
        c = LLVMBuildAnd(ir->b, LLVMBuildICmp(ir->b, LLVMIntEQ, pc, ir_i32(ir, next[k] & ~1u), ""),
                         LLVMBuildICmp(ir->b, LLVMIntEQ, t, ir_i32(ir, next[k] & 1 ? SNX_ARM_T : 0), ""), "");
        LLVMBuildCondBr(ir->b, c, call, miss);
        LLVMPositionBuilderAtEnd(ir->b, call);
        if ((next[k] & ~1u) <= (key & ~1u)) {
            LLVMBasicBlockRef run = LLVMAppendBasicBlockInContext(ir->ctx, fn, "run");
            LLVMValueRef off = LLVMBuildLoad2(ir->b, ir->t_i64, poll_offset, "");
            LLVMValueRef poll = LLVMBuildLoad2(ir->b, ir->t_i32, LLVMBuildGEP2(ir->b, ir->t_i8, cpu, &off, 1, ""), "");
            LLVMSetOrdering(poll, LLVMAtomicOrderingMonotonic); /* the loop's relaxed load */
            LLVMSetAlignment(poll, 4);
            LLVMBuildCondBr(ir->b, LLVMBuildICmp(ir->b, LLVMIntEQ, poll, ir_i32(ir, 0), ""), run, out);
            LLVMPositionBuilderAtEnd(ir->b, run);
        }
        block_name(name, next[k]);
        target = LLVMGetNamedFunction(ir->mod, name);
        if (!target) target = LLVMAddFunction(ir->mod, name, block_type);
        site = LLVMBuildCall2(ir->b, block_type, target, &cpu, 1, "");
        LLVMSetTailCallKind(site, LLVMTailCallKindMustTail);
        LLVMBuildRetVoid(ir->b);
        LLVMPositionBuilderAtEnd(ir->b, miss);
    }
    LLVMBuildBr(ir->b, out);
    free(rets);
}

int snx_module_fallback_arm_unit(const SnxLlvmBuild* build, const char* name, const SnxModuleArmBlock* blocks,
                                 size_t count, unsigned char** object, size_t* size, char* error, size_t error_size) {
    char block[16];
    size_t i, units;
    SnxIr* ir;
    int rc;
    *object = NULL;
    *size = 0;
    ir = snx_ir_init(error, error_size) ? NULL : snx_ir_new(name, build->opt, error, error_size);
    if (!ir) return -1;
    snx_ir_tune(ir, build->tuning);
    /* every block of the unit first: the block compiler refuses a name the module has, and chain_block declares the
       successors it calls */
    LLVMValueRef* fns = (LLVMValueRef*)snx_alloc((count ? count : 1) * sizeof(*fns));
    for (i = 0; i < count; ++i) {
        block_name(block, blocks[i].code.key);
        fns[i] = compile_block(ir->mod, &blocks[i].code, block, &units);
        if (!fns[i] || units != blocks[i].units) {
            snprintf(error, error_size, "the AArch32 block %s compiled differently the second time", block);
            break;
        }
    }
    rc = i < count ? -1 : 0;
    for (i = 0; !rc && i < count; ++i) chain_block(ir, fns[i], blocks[i].code.key, blocks[i].next, blocks[i].nnext);
    free(fns);
    if (!rc && build->keep_ir) rc = save_ir(ir, build->keep_ir, name, error, error_size);
    if (!rc) rc = snx_ir_object(ir, build->opt, object, size, error, error_size);
    snx_ir_free(ir);
    return rc;
}

/* ---- the descriptor: SnxArmModule, a constant the exported snx_module_arm() returns ---- */

static LLVMValueRef constant(SnxIr* ir, LLVMTypeRef type, const char* name, LLVMValueRef value) {
    LLVMValueRef g = LLVMAddGlobal(ir->mod, type, name);
    LLVMSetLinkage(g, LLVMInternalLinkage);
    LLVMSetInitializer(g, value);
    LLVMSetGlobalConstant(g, 1);
    return g;
}

static int emit_descriptor(SnxIr* ir, const SnxU32* keys, size_t count, const SnxCodegenOptions* options,
                           const SnxProgram* p, char* error, size_t error_size) {
    static const size_t offsets[13] = {
        offsetof(SnxArmModule, abi),        offsetof(SnxArmModule, nmods),      offsetof(SnxArmModule, mods),
        offsetof(SnxArmModule, nentries),   offsetof(SnxArmModule, entry_addr), offsetof(SnxArmModule, entry_fn),
        offsetof(SnxArmModule, title),      offsetof(SnxArmModule, title_id),   offsetof(SnxArmModule, main_stack),
        offsetof(SnxArmModule, main_prio),  offsetof(SnxArmModule, main_core),  offsetof(SnxArmModule, aspace),
        offsetof(SnxArmModule, poll_offset)};
    LLVMTypeRef fields[13], type, info, block_type;
    LLVMValueRef *values, f[13], mod, fn, poll_offset;
    LLVMTargetDataRef layout;
    size_t n = count ? count : 1, i;
    char name[16];
    int ok;
    info = snx_module_info_type(ir, error, error_size);
    if (!info) return -1;
    fields[0] = fields[1] = fields[3] = ir->t_i32;
    fields[2] = fields[4] = fields[5] = fields[6] = ir->t_ptr;
    fields[7] = ir->t_i64;
    for (i = 8; i < 12; ++i) fields[i] = ir->t_i32;
    fields[12] = ir->t_ptr;
    type = LLVMStructTypeInContext(ir->ctx, fields, 13, 0);
    layout = LLVMGetModuleDataLayout(ir->mod);
    ok = LLVMABISizeOfType(layout, type) == sizeof(SnxArmModule);
    for (i = 0; i < 13; ++i) ok = ok && LLVMOffsetOfElement(layout, type, (unsigned)i) == offsets[i];
    if (!ok) {
        snprintf(error, error_size, "SnxArmModule layout differs between the recompiler and the LLVM target");
        return -1;
    }
    block_type = LLVMFunctionType(ir->t_void, &ir->t_ptr, 1, 0);
    values = (LLVMValueRef*)snx_alloc(n * sizeof(*values));
    for (i = 0; i < n; ++i) values[i] = ir_i32(ir, i < count ? keys[i] : 0);
    f[4] =
        constant(ir, LLVMArrayType(ir->t_i32, (unsigned)n), "ENT_ADDR", LLVMConstArray(ir->t_i32, values, (unsigned)n));
    for (i = 0; i < n; ++i) {
        if (i < count) {
            block_name(name, keys[i]);
            values[i] = LLVMAddFunction(ir->mod, name, block_type);
        } else
            values[i] = LLVMConstNull(ir->t_ptr);
    }
    f[5] =
        constant(ir, LLVMArrayType(ir->t_ptr, (unsigned)n), "ENT_FN", LLVMConstArray(ir->t_ptr, values, (unsigned)n));
    free(values);
    f[0] = ir_i32(ir, SNX_ARM_MODULE_ABI);
    f[1] = ir_i32(ir, (SnxU32)p->module_count);
    f[2] = snx_module_mods(ir, info, p);
    f[3] = ir_i32(ir, (SnxU32)count);
    f[6] = snx_module_string(ir, options->title ? options->title : "Game");
    f[7] = ir_i64(ir, p->npdm.title_id);
    f[8] = ir_i32(ir, (SnxU32)p->npdm.stack);
    f[9] = ir_i32(ir, p->npdm.prio);
    f[10] = ir_i32(ir, p->npdm.core);
    f[11] = ir_i32(ir, p->npdm.aspace);
    /* the blocks' poll offset (chain_block), which arm_module writes */
    poll_offset = LLVMAddGlobal(ir->mod, ir->t_i64, POLL_OFFSET);
    LLVMSetVisibility(poll_offset, LLVMHiddenVisibility);
    LLVMSetInitializer(poll_offset, ir_i64(ir, 0));
    f[12] = poll_offset;
    mod = constant(ir, type, "MOD", LLVMConstStructInContext(ir->ctx, f, 13, 0));
    /* SNX_EXPORT const SnxArmModule* snx_module_arm(void) */
    fn = LLVMAddFunction(ir->mod, "snx_module_arm", LLVMFunctionType(ir->t_ptr, NULL, 0, 0));
    if (ir->windows) LLVMSetDLLStorageClass(fn, LLVMDLLExportStorageClass);
    LLVMPositionBuilderAtEnd(ir->b, LLVMAppendBasicBlockInContext(ir->ctx, fn, "entry"));
    LLVMBuildRet(ir->b, mod);
    return 0;
}

int snx_module_fallback_arm_descriptor(const SnxProgram* loaded, const SnxCodegenOptions* options,
                                       const SnxLlvmBuild* build, const SnxU32* keys, size_t count,
                                       unsigned char** object, size_t* size, char* error, size_t error_size) {
    *object = NULL;
    *size = 0;
    SnxIr* ir = snx_ir_init(error, error_size) ? NULL : snx_ir_new("module", build->opt, error, error_size);
    int rc = !ir || emit_descriptor(ir, keys, count, options, loaded, error, error_size) ||
                     (build->keep_ir && save_ir(ir, build->keep_ir, "module", error, error_size)) ||
                     snx_ir_object(ir, build->opt, object, size, error, error_size)
                 ? -1
                 : 0;
    snx_ir_free(ir);
    return rc;
}
