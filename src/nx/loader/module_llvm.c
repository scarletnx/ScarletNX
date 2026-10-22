/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "nx/loader/module_ir.h"
#include "nx/loader/module_variants.h"
#include "common/cpu/recompiler/codegen.h"
#include "nx/loader/codegen_view.h"
#include "common/cpu/recompiler/llvm/ir.h"
#include "common/cpu/recompiler/llvm/link.h"
#include "nx/loader/module_abi.h"
#include "common/cpu/recompiler/llvm/rtlib_object.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <pthread.h>
#endif

typedef struct {
    size_t first, last; /* functions */
    unsigned char* object;
    size_t size;
} Unit;

typedef struct {
    SnxCodegen* codegen;
    const SnxLlvmBuild* build;
    Unit* units;
    size_t count, next, done;
    int failed;
    char error[1024];
#ifdef _WIN32
    CRITICAL_SECTION lock;
#else
    pthread_mutex_t lock;
#endif
} Queue;

static void lock(Queue* q) {
#ifdef _WIN32
    EnterCriticalSection(&q->lock);
#else
    pthread_mutex_lock(&q->lock);
#endif
}
static void unlock(Queue* q) {
#ifdef _WIN32
    LeaveCriticalSection(&q->lock);
#else
    pthread_mutex_unlock(&q->lock);
#endif
}

static int save_ir(SnxIr* ir, const char* dir, const char* name) {
    char *text = snx_ir_text(ir), *file = snx_format("%s.ll", name), *path = snx_path_join(dir, file);
    int rc = snx_write_file(path, text, strlen(text));
    LLVMDisposeMessage(text);
    free(file);
    free(path);
    return rc;
}

/* one chunk of functions, as module.c's code_NNN.c */
static int compile_unit(Queue* q, size_t index, char* error, size_t error_size) {
    Unit* u = &q->units[index];
    char name[32];
    size_t i;
    SnxIr* ir;
    int rc;
    sprintf(name, "code_%03lu", (unsigned long)index);
    ir = snx_ir_new(name, q->build->opt, error, error_size);
    if (!ir) return -1;
    snx_ir_tune(ir, q->build->tuning);
    for (i = u->first; i < u->last; ++i) snx_ir_emit(ir, q->codegen, &q->codegen->functions[i]);
    rc = q->build->keep_ir && save_ir(ir, q->build->keep_ir, name);
    if (rc) snprintf(error, error_size, "cannot save the IR of %s", name);
    if (!rc) rc = snx_ir_object(ir, q->build->opt, &u->object, &u->size, error, error_size);
    snx_ir_free(ir);
    return rc;
}

static void worker(Queue* q) {
    char error[1024], name[32], *label;
    double start, seconds;
    for (;;) {
        size_t i;
        int rc;
        lock(q);
        if (q->failed || q->next == q->count) {
            unlock(q);
            return;
        }
        i = q->next++;
        unlock(q);
        error[0] = 0;
        start = snx_seconds();
        rc = compile_unit(q, i, error, sizeof(error));
        seconds = snx_seconds() - start;
        label = rc || q->build->quiet ? NULL : snx_chunk_label(q->codegen, q->units[i].first, q->units[i].last);
        lock(q);
        ++q->done;
        if (rc && !q->failed) {
            q->failed = 1;
            snprintf(q->error, sizeof(q->error), "%s", error);
        }
        if (label) {
            /* every unit: a line every few seconds or minutes, whichever the machine manages */
            sprintf(name, "code_%03lu", (unsigned long)i);
            snx_report_unit(q->done, q->count + 1, name, label, 0, seconds); /* + the descriptor */
        }
        unlock(q);
        free(label);
    }
}
#ifdef _WIN32
static DWORD WINAPI thread_main(LPVOID p) {
    worker((Queue*)p);
    return 0;
}
#else
static void* thread_main(void* p) {
    worker((Queue*)p);
    return NULL;
}
#endif

static void run(Queue* q, unsigned jobs) {
    unsigned i, created = 0;
#ifdef _WIN32
    HANDLE* threads;
    InitializeCriticalSection(&q->lock);
#else
    pthread_t* threads;
    pthread_mutex_init(&q->lock, NULL);
#endif
    if (jobs > q->count) jobs = (unsigned)q->count;
    if (jobs < 1) jobs = 1;
    threads = snx_alloc(jobs * sizeof(*threads));
    for (i = 1; i < jobs; ++i) {
#ifdef _WIN32
        HANDLE h = CreateThread(NULL, 0, thread_main, q, 0, NULL);
        if (!h) break;
        threads[created++] = h;
#else
        if (pthread_create(&threads[created], NULL, thread_main, q)) break;
        ++created;
#endif
    }
    worker(q);
    for (i = 0; i < created; ++i) {
#ifdef _WIN32
        WaitForSingleObject(threads[i], INFINITE);
        CloseHandle(threads[i]);
#else
        pthread_join(threads[i], NULL);
#endif
    }
#ifdef _WIN32
    DeleteCriticalSection(&q->lock);
#else
    pthread_mutex_destroy(&q->lock);
#endif
    free(threads);
}

/* ---- the module descriptor: module.c's emit_module, as IR ---- */

static LLVMValueRef global(SnxIr* ir, LLVMTypeRef type, const char* name, LLVMValueRef value, int constant) {
    LLVMValueRef g = LLVMAddGlobal(ir->mod, type, name);
    LLVMSetLinkage(g, LLVMInternalLinkage);
    LLVMSetInitializer(g, value ? value : LLVMConstNull(type));
    LLVMSetGlobalConstant(g, constant);
    return g;
}

LLVMValueRef snx_module_string(SnxIr* ir, const char* text) {
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

/* SnxModInfo as an LLVM struct; its layout is checked against the C one */
LLVMTypeRef snx_module_info_type(SnxIr* ir, char* error, size_t error_size) {
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

LLVMValueRef snx_module_mods(SnxIr* ir, LLVMTypeRef info, const SnxProgram* loaded) {
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
                       size_t* entry_count, char* error, size_t error_size) {
    SnxModuleEntry* entries;
    size_t count, arraysize, i, nfns = codegen->function_count ? codegen->function_count : 1;
    LLVMValueRef *values, fns, ent_off, ent_fn, ent_addr, ent_ptr, mods, mod, fn, loop_i, phi_values[2];
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
    for (i = 0; i < count; ++i) {
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
    LLVMBuildRet(ir->b, mod);
    free(entries);
    *entry_count = count;
    return 0;
}

unsigned snx_module_abi(void) { return SNX_ABI; }
unsigned snx_module_abi_oldest(void) { return SNX_ABI_OLDEST; }
static const char* base_name(const char* path) {
    const char *name = path, *p;
    for (p = path; *p; ++p)
        if (*p == '/' || *p == '\\') name = p + 1;
    return name;
}

int snx_build_module_llvm(SnxProgram* loaded, const SnxCodegenOptions* options, const SnxLlvmBuild* build) {
    SnxCodegen codegen;
    Queue q;
    SnxIr* ir = NULL;
    SnxJitCache* cache = NULL;
    SnxLinkInput* inputs = NULL;
    SnxLinkOutput linked;
    unsigned char *module_object = NULL, *rtlib_object = NULL;
    size_t module_size = 0, rtlib_size = 0, first, last, count, limit, i, entry_count = 0, n = 0;
    size_t cache_count = 0, variant_count = 0, export_count = 1;
    const char* exports[2];
    char error[1024], *tmp = NULL;
    double start;
    int rc = -1;
    memset(&codegen, 0, sizeof(codegen));
    memset(&q, 0, sizeof(q));
    memset(&linked, 0, sizeof(linked));
    if (!loaded->module_count) return snx_error("No modules to generate");
    if (snx_ir_init(error, sizeof(error))) return snx_error("%s", error);
    if (options->jit_cache) {
        char* key = snx_jit_cache_key(build->opt, options->hook_direct_calls, 1);
        if (!key) {
            snx_error("LLVM backend: cannot make the JIT cache key");
            goto done;
        }
        cache = snx_jit_cache_open(options->jit_cache, key, error, sizeof(error));
        free(key);
        if (!cache) {
            snx_error("JIT cache: %s", error);
            goto done;
        }
        cache_count = snx_jit_cache_count(cache);
    }
    if (!options->interp_only &&
        snx_nx_codegen_analyze(&codegen, loaded, options->hints, options->hint_count, options->quiet))
        goto done;
    codegen.direct_svc = 1; /* the runtime's RtAPI.svcs (svc.c's g_svc64) */
    codegen.hook_direct_calls = options->hook_direct_calls;
    /* chunks of whole functions, as module.c splits the C */
    limit = options->chunk_instrs ? options->chunk_instrs : SNX_CHUNK_INSTRUCTIONS;
    q.units = (Unit*)snx_alloc((codegen.function_count + 1) * sizeof(*q.units));
    memset(q.units, 0, (codegen.function_count + 1) * sizeof(*q.units));
    for (first = 0; first < codegen.function_count; first = last) {
        last = first;
        count = 0;
        do { count += codegen.functions[last++].count; } while (last < codegen.function_count && count < limit);
        q.units[q.count].first = first;
        q.units[q.count++].last = last;
    }
    q.codegen = &codegen;
    q.build = build;
    if (!options->quiet)
        printf("LLVM: %lu code unit%s and the module descriptor\n", (unsigned long)q.count, q.count == 1 ? "" : "s");
    if (q.count) run(&q, build->jobs);
    if (q.failed) {
        snx_error("LLVM backend: %s", q.error);
        goto done;
    }
    /* the module descriptor */
    start = snx_seconds();
    ir = snx_ir_new("module", build->opt, error, sizeof(error));
    if (!ir) {
        snx_error("LLVM backend: %s", error);
        goto done;
    }
    if (emit_module(ir, &codegen, options, loaded, &entry_count, error, sizeof(error))) {
        snx_error("LLVM backend: %s", error);
        goto done;
    }
    if (cache_count &&
        snx_module_variants_emit(ir, cache, &variant_count, error, sizeof(error))) {
        snx_error("LLVM backend: %s", error);
        goto done;
    }
    if (options->jit_cache && !options->quiet)
        printf("AOT pack: %lu cached function%s%s\n", (unsigned long)variant_count, variant_count == 1 ? "" : "s",
               variant_count ? "" : " (the cache did not match the game)");
    if ((build->keep_ir && save_ir(ir, build->keep_ir, "module")) ||
        snx_ir_object(ir, build->opt, &module_object, &module_size, error, sizeof(error))) {
        snx_error("LLVM backend: %s", error);
        goto done;
    }
    if (!options->quiet) {
        char* label = snx_format("SnxModule descriptor, %lu entries", (unsigned long)entry_count);
        snx_report_unit(q.count + 1, q.count + 1, "module", label, 0, snx_seconds() - start);
        free(label);
    }
    if (snx_ir_rtlib_object(build->opt, &rtlib_object, &rtlib_size, error, sizeof(error))) {
        snx_error("LLVM backend: %s", error);
        goto done;
    }
    /* link: runtime library, descriptor, chunks, the profile cache's units */
    inputs = (SnxLinkInput*)snx_alloc((q.count + 2 + cache_count) * sizeof(*inputs));
    inputs[n].data = rtlib_object;
    inputs[n].size = rtlib_size;
    inputs[n++].name = "rtlib";
    inputs[n].data = module_object;
    inputs[n].size = module_size;
    inputs[n++].name = "module";
    for (i = 0; i < q.count; ++i) {
        inputs[n].data = q.units[i].object;
        inputs[n].size = q.units[i].size;
        inputs[n++].name = "code chunk";
    }
    for (i = 0; i < cache_count; ++i) {
        const SnxJitCacheRecord* record = snx_jit_cache_record(cache, i);
        inputs[n].data = record->object;
        inputs[n].size = record->object_size;
        inputs[n++].name = record->name;
    }
    exports[0] = "snx_module";
    if (variant_count) exports[export_count++] = "snx_variants";
    if (snx_link(inputs, n, exports, export_count, base_name(build->output), &linked, error, sizeof(error))) {
        snx_error("Linking failed: %s", error);
        goto done;
    }
    tmp = snx_format("%s.tmp", build->output);
    if (snx_write_file(tmp, linked.data, linked.size) || snx_replace(tmp, build->output)) {
        snx_error("Linking failed: cannot write %s", build->output);
        if (snx_is_file(tmp)) snx_remove_tree(tmp);
        goto done;
    }
    if (!options->quiet) {
        printf("LLVM module: %.1f MB, %lu entries\n", (double)linked.size / 1000000.0, (unsigned long)entry_count);
    }
    rc = 0;
done:
    for (i = 0; i < q.count; ++i) free(q.units[i].object);
    free(q.units);
    free(inputs);
    free(module_object);
    free(rtlib_object);
    free(linked.data);
    free(tmp);
    snx_ir_free(ir);
    snx_jit_cache_close(cache);
    snx_codegen_free(&codegen);
    return rc;
}
