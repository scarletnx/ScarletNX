/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The game module through libgccjit (--backend=gcc), for DEC Alpha EV67 hosts, where LLVM has no backend; elsewhere
   --backend=llvm is the one to use. module_llvm.c's structure: chunks of whole functions, each a context compiled
   to an object file (--jobs at a time, jit_units.c), then the module descriptor in a last context that GCC's driver
   links into the shared library with the chunks and the runtime library (rtlib.c). */
#include "nx/loader/module_llvm.h"

#ifndef SNX_WITH_GCCJIT
int snx_build_module_gccjit(SnxProgram* loaded, const SnxCodegenOptions* options, const SnxLlvmBuild* build) {
    (void)loaded;
    (void)options;
    (void)build;
    return snx_error("this recompiler was built without libgccjit (SNX_GCCJIT); use --backend=llvm");
}
#else
#include "common/cpu/recompiler/codegen.h"
#include "nx/loader/codegen_view.h"
#include "common/cpu/recompiler/gccjit/jit.h"
#include "nx/loader/module_abi.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* the runtime library, compiled when the recompiler is built (rtlib.c, snx-rtlib-gen embed) */
extern const unsigned char snx_rtlib_object[];
extern const unsigned long snx_rtlib_object_size;

/* module.c's emit_module through libgccjit: the same tables and the same snx_module */
static int emit_module(SnxJit* jit, const SnxCodegen* codegen, const SnxCodegenOptions* options,
                       const SnxProgram* loaded, size_t* entry_count, char* error, size_t error_size) {
    gcc_jit_context* x = jit->ctx;
    SnxModuleEntry* entries;
    size_t count, arraysize, i, j, nfns = codegen->function_count ? codegen->function_count : 1;
    gcc_jit_rvalue **values, *interp_address, *index, *e;
    gcc_jit_type *t_fns, *t_u32s, *t_addrs, *t_ptrs, *t_info, *t_mods, *t_id, *t_mod, *t_cstr;
    gcc_jit_field* fields[12];
    gcc_jit_lvalue *fns, *ent_off, *ent_fn, *ent_addr, *ent_ptr, *mods, *mod, *interp_entry, *loop_i;
    gcc_jit_function *interp, *fn;
    gcc_jit_block *start, *head, *body, *done;
    gcc_jit_param* interp_params[2];
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
    values = (gcc_jit_rvalue**)snx_alloc((nfns > arraysize ? nfns : arraysize) * sizeof(*values));
    /* static const GuestFn FNS[] */
    for (i = 0; i < codegen->function_count; ++i)
        values[i] = gcc_jit_context_new_cast(
            x, NULL, gcc_jit_function_get_address(snx_jit_guest_function(jit, codegen->functions[i].start, 0), NULL),
            jit->t_guest_ptr);
    if (!codegen->function_count) values[0] = gcc_jit_context_null(x, jit->t_guest_ptr);
    t_fns = gcc_jit_context_new_array_type(x, NULL, jit->t_guest_ptr, (int)nfns);
    fns = gcc_jit_context_new_global(x, NULL, GCC_JIT_GLOBAL_INTERNAL, t_fns, "FNS");
    gcc_jit_global_set_initializer_rvalue(fns, gcc_jit_context_new_array_constructor(x, NULL, t_fns, nfns, values));
    /* static const u32 ENT_OFF[], ENT_FN[] */
    t_u32s = gcc_jit_context_new_array_type(x, NULL, jit->t_u32, (int)arraysize);
    for (i = 0; i < arraysize; ++i)
        values[i] = jit_u32(jit, i < count ? (SnxU32)(entries[i].address - SNX_CODE_BASE) : 0);
    ent_off = gcc_jit_context_new_global(x, NULL, GCC_JIT_GLOBAL_INTERNAL, t_u32s, "ENT_OFF");
    gcc_jit_global_set_initializer_rvalue(ent_off,
                                          gcc_jit_context_new_array_constructor(x, NULL, t_u32s, arraysize, values));
    for (i = 0; i < arraysize; ++i) values[i] = jit_u32(jit, i < count ? (SnxU32)entries[i].function : 0);
    ent_fn = gcc_jit_context_new_global(x, NULL, GCC_JIT_GLOBAL_INTERNAL, t_u32s, "ENT_FN");
    gcc_jit_global_set_initializer_rvalue(ent_fn,
                                          gcc_jit_context_new_array_constructor(x, NULL, t_u32s, arraysize, values));
    /* static u64 ENT_ADDR[]; static GuestFn ENT_PTR[]; */
    t_addrs = gcc_jit_context_new_array_type(x, NULL, jit->t_u64, (int)arraysize);
    t_ptrs = gcc_jit_context_new_array_type(x, NULL, jit->t_guest_ptr, (int)arraysize);
    ent_addr = gcc_jit_context_new_global(x, NULL, GCC_JIT_GLOBAL_INTERNAL, t_addrs, "ENT_ADDR");
    ent_ptr = gcc_jit_context_new_global(x, NULL, GCC_JIT_GLOBAL_INTERNAL, t_ptrs, "ENT_PTR");
    free(values);
    /* static const SnxModInfo MODS[]: the struct as module_abi.h declares it, laid out by GCC for this host */
    t_cstr = gcc_jit_type_get_pointer(gcc_jit_type_get_const(gcc_jit_context_get_type(x, GCC_JIT_TYPE_CHAR)));
    t_id = gcc_jit_context_new_array_type(x, NULL, jit->t_u8, 32);
    fields[0] = gcc_jit_context_new_field(x, NULL, t_cstr, "name");
    fields[1] = gcc_jit_context_new_field(x, NULL, jit->t_u64, "base");
    fields[2] = gcc_jit_context_new_field(x, NULL, jit->t_u64, "size");
    fields[3] = gcc_jit_context_new_field(x, NULL, t_id, "build_id");
    {
        static const char* const names[8] = {"text_off", "text_size", "ro_off",   "ro_size",
                                             "data_off", "data_size", "bss_size", "is_nro"};
        for (i = 0; i < 8; ++i) fields[4 + i] = gcc_jit_context_new_field(x, NULL, jit->t_u32, names[i]);
    }
    t_info = gcc_jit_struct_as_type(gcc_jit_context_new_struct_type(x, NULL, "SnxModInfo", 12, fields));
    values = (gcc_jit_rvalue**)snx_alloc(loaded->module_count * sizeof(*values));
    for (i = 0; i < loaded->module_count; ++i) {
        const SnxModuleImage* m = &loaded->modules[i];
        gcc_jit_rvalue *f[12], *id[32];
        f[0] = gcc_jit_context_new_cast(x, NULL, gcc_jit_context_new_string_literal(x, m->name), t_cstr);
        f[1] = jit_u64(jit, m->base);
        f[2] = jit_u64(jit, m->size);
        for (j = 0; j < 32; ++j) id[j] = jit_k(jit, jit->t_u8, m->build_id[j]);
        f[3] = gcc_jit_context_new_array_constructor(x, NULL, t_id, 32, id);
        f[4] = jit_u32(jit, m->text_off);
        f[5] = jit_u32(jit, m->text_size);
        f[6] = jit_u32(jit, m->ro_off);
        f[7] = jit_u32(jit, m->ro_size);
        f[8] = jit_u32(jit, m->data_off);
        f[9] = jit_u32(jit, m->data_size);
        f[10] = jit_u32(jit, m->bss);
        f[11] = jit_u32(jit, !!m->is_nro);
        values[i] = gcc_jit_context_new_struct_constructor(x, NULL, t_info, 12, NULL, f);
        if (m->base + m->text_off < lo) lo = m->base + m->text_off;
        if (m->base + m->text_off + m->text_size > hi) hi = m->base + m->text_off + m->text_size;
    }
    t_mods = gcc_jit_context_new_array_type(x, NULL, t_info, (int)loaded->module_count);
    mods = gcc_jit_context_new_global(x, NULL, GCC_JIT_GLOBAL_INTERNAL, t_mods, "MODS");
    gcc_jit_global_set_initializer_rvalue(
        mods, gcc_jit_context_new_array_constructor(x, NULL, t_mods, loaded->module_count, values));
    free(values);
    /* static SnxModule MOD; as 8-byte words, written field by field at module_abi.h's offsets */
    t_mod = gcc_jit_context_new_array_type(x, NULL, jit->t_u64, (int)((sizeof(SnxModule) + 7) / 8));
    mod = gcc_jit_context_new_global(x, NULL, GCC_JIT_GLOBAL_INTERNAL, t_mod, "MOD");
    interp_params[0] = gcc_jit_context_new_param(x, NULL, jit->t_ptr, "c");
    interp_params[1] = gcc_jit_context_new_param(x, NULL, jit->t_u64, "e");
    interp = gcc_jit_context_new_function(x, NULL, GCC_JIT_FUNCTION_IMPORTED, jit->t_void, "snx_interp", 2,
                                          interp_params, 0);
    interp_entry = gcc_jit_context_new_global(x, NULL, GCC_JIT_GLOBAL_IMPORTED, jit->t_guest_ptr, "snx_interp_entry");
    interp_address = gcc_jit_context_new_cast(x, NULL, gcc_jit_function_get_address(interp, NULL), jit->t_guest_ptr);
    /* SNX_EXPORT const SnxModule* snx_module(void) */
    fn = gcc_jit_context_new_function(x, NULL, GCC_JIT_FUNCTION_EXPORTED, jit->t_ptr, "snx_module", 0, NULL, 0);
    jit->fn = fn;
    start = gcc_jit_function_new_block(fn, "entry");
    head = gcc_jit_function_new_block(fn, "head");
    body = gcc_jit_function_new_block(fn, "body");
    done = gcc_jit_function_new_block(fn, "done");
    loop_i = gcc_jit_function_new_local(fn, NULL, jit->t_u32, "i");
    gcc_jit_block_add_assignment(start, NULL, loop_i, jit_u32(jit, 0));
    gcc_jit_block_end_with_jump(start, NULL, head);
    /* for(i=0;i<count;i++){ ENT_ADDR[i]=BASE+ENT_OFF[i]; ENT_PTR[i]=FNS[ENT_FN[i]]; } */
    gcc_jit_block_end_with_conditional(head, NULL,
                                       gcc_jit_context_new_comparison(x, NULL, GCC_JIT_COMPARISON_LT,
                                                                      gcc_jit_lvalue_as_rvalue(loop_i),
                                                                      jit_u32(jit, (SnxU32)count)),
                                       body, done);
    index = gcc_jit_lvalue_as_rvalue(loop_i);
    e = gcc_jit_context_new_binary_op(
        x, NULL, GCC_JIT_BINARY_OP_PLUS, jit->t_u64, jit_u64(jit, SNX_CODE_BASE),
        gcc_jit_context_new_cast(x, NULL,
                                 gcc_jit_lvalue_as_rvalue(gcc_jit_context_new_array_access(
                                     x, NULL, gcc_jit_lvalue_as_rvalue(ent_off), index)),
                                 jit->t_u64));
    gcc_jit_block_add_assignment(
        body, NULL, gcc_jit_context_new_array_access(x, NULL, gcc_jit_lvalue_as_rvalue(ent_addr), index), e);
    gcc_jit_block_add_assignment(body, NULL,
                                 gcc_jit_context_new_array_access(x, NULL, gcc_jit_lvalue_as_rvalue(ent_ptr), index),
                                 gcc_jit_lvalue_as_rvalue(gcc_jit_context_new_array_access(
                                     x, NULL, gcc_jit_lvalue_as_rvalue(fns),
                                     gcc_jit_lvalue_as_rvalue(gcc_jit_context_new_array_access(
                                         x, NULL, gcc_jit_lvalue_as_rvalue(ent_fn), index)))));
    gcc_jit_block_add_assignment_op(body, NULL, loop_i, GCC_JIT_BINARY_OP_PLUS, jit_u32(jit, 1));
    gcc_jit_block_end_with_jump(body, NULL, head);
    /* snx_interp_entry=snx_interp; MOD.abi=...; ... return &MOD; */
    {
        gcc_jit_rvalue* base = gcc_jit_lvalue_get_address(mod, NULL);
        jit->block = done;
        gcc_jit_block_add_assignment(done, NULL, interp_entry, interp_address);
#define FIELD(member, type, value)                                                                                     \
    gcc_jit_block_add_assignment(done, NULL, jit_at(jit, base, offsetof(SnxModule, member), type), value)
        FIELD(abi, jit->t_u32, jit_u32(jit, options->abi));
        FIELD(nmods, jit->t_u32, jit_u32(jit, (SnxU32)loaded->module_count));
        FIELD(mods, jit->t_ptr, gcc_jit_context_new_cast(x, NULL, gcc_jit_lvalue_get_address(mods, NULL), jit->t_ptr));
        FIELD(nentries, jit->t_u32, jit_u32(jit, (SnxU32)count));
        FIELD(entry_addr, jit->t_ptr,
              gcc_jit_context_new_cast(x, NULL, gcc_jit_lvalue_get_address(ent_addr, NULL), jit->t_ptr));
        FIELD(entry_fn, jit->t_ptr,
              gcc_jit_context_new_cast(x, NULL, gcc_jit_lvalue_get_address(ent_ptr, NULL), jit->t_ptr));
        FIELD(interp, jit->t_guest_ptr, interp_address);
        FIELD(code_lo, jit->t_u64, jit_u64(jit, lo));
        FIELD(code_hi, jit->t_u64, jit_u64(jit, hi));
        FIELD(title, jit->t_ptr,
              gcc_jit_context_new_cast(x, NULL,
                                       gcc_jit_context_new_string_literal(x, options->title ? options->title : "Game"),
                                       jit->t_ptr));
        FIELD(title_id, jit->t_u64, jit_u64(jit, npdm->title_id));
        FIELD(main_stack, jit->t_u32, jit_u32(jit, (SnxU32)npdm->stack));
        FIELD(main_prio, jit->t_u32, jit_u32(jit, npdm->prio));
        FIELD(main_core, jit->t_u32, jit_u32(jit, npdm->core));
#undef FIELD
        gcc_jit_block_end_with_return(done, NULL, gcc_jit_context_new_cast(x, NULL, base, jit->t_ptr));
    }
    free(entries);
    *entry_count = count;
    return 0;
}

static int failed(SnxJit* jit, const char* what) {
    const char* message = snx_jit_error(jit);
    if (!message) return 0;
    snx_error("libgccjit (%s): %s", what, message);
    return 1;
}

static void unit_done(const SnxJitUnits* units, size_t unit, size_t finished, double seconds) {
    char name[32], *label = snx_chunk_label(units->codegen, units->bounds[unit], units->bounds[unit + 1]);
    sprintf(name, "code_%03lu", (unsigned long)unit);
    snx_report_unit(finished, units->count + 1, name, label, 0, seconds); /* + the descriptor */
    free(label);
}

int snx_build_module_gccjit(SnxProgram* loaded, const SnxCodegenOptions* options, const SnxLlvmBuild* build) {
    SnxCodegen codegen;
    SnxJitUnits units;
    SnxJit* jit = NULL;
    size_t first, last, count, limit, i, n = 0, entry_count = 0, *bounds;
    char *dir = NULL, *rtlib = NULL, *tmp = NULL, **objects = NULL, error[1024];
    int rc = -1;
    memset(&codegen, 0, sizeof(codegen));
    memset(&units, 0, sizeof(units));
    if (!loaded->module_count) return snx_error("No modules to generate");
    if (!options->interp_only &&
        snx_nx_codegen_analyze(&codegen, loaded, options->hints, options->hint_count, options->quiet))
        return -1;
    codegen.hook_direct_calls = options->hook_direct_calls;
    dir = snx_temp_dir(NULL);
    if (!dir) {
        snx_codegen_free(&codegen);
        return snx_error("cannot create a temporary directory");
    }
    rtlib = snx_path_join(dir, "rtlib.o");
    if (snx_write_file(rtlib, snx_rtlib_object, snx_rtlib_object_size)) {
        snx_error("cannot write %s", rtlib);
        goto done;
    }
    limit = options->chunk_instrs ? options->chunk_instrs : SNX_CHUNK_INSTRUCTIONS;
    /* the object files' paths, then the units' bounds: one block, which snx_free_strings frees */
    objects = (char**)snx_alloc((codegen.function_count + 1) * (sizeof(*objects) + sizeof(*bounds)));
    bounds = (size_t*)(objects + codegen.function_count + 1);
    for (first = 0; first < codegen.function_count; first = last) {
        char name[32];
        last = first;
        count = 0;
        do { count += codegen.functions[last++].count; } while (last < codegen.function_count && count < limit);
        sprintf(name, "code_%03lu.o", (unsigned long)n);
        objects[n] = snx_path_join(dir, name);
        bounds[n++] = first;
    }
    bounds[n] = codegen.function_count;
    if (!options->quiet)
        printf("libgccjit: %lu code unit%s and the module descriptor\n", (unsigned long)n, n == 1 ? "" : "s");
    units.codegen = &codegen;
    units.bounds = bounds;
    units.objects = objects;
    units.count = n;
    units.opt = build->opt;
    units.jobs = build->jobs;
    units.keep_ir = build->keep_ir;
    if (!options->quiet) units.done = unit_done;
    if (snx_jit_compile_units(&units)) {
        snx_error("%s", units.error);
        goto done;
    }
    /* the descriptor, linked by GCC's driver with the chunks and the runtime library */
    jit = snx_jit_new(build->opt);
    if (emit_module(jit, &codegen, options, loaded, &entry_count, error, sizeof(error))) {
        snx_error("libgccjit: %s", error);
        goto done;
    }
    if (build->keep_ir) {
        char* path = snx_path_join(build->keep_ir, "module.c");
        gcc_jit_context_dump_to_file(jit->ctx, path, 0);
        free(path);
    }
    gcc_jit_context_add_driver_option(jit->ctx, rtlib);
    for (i = 0; i < n; ++i) gcc_jit_context_add_driver_option(jit->ctx, objects[i]);
    tmp = snx_format("%s.tmp", build->output);
    snx_jit_compile_to_file(jit, GCC_JIT_OUTPUT_KIND_DYNAMIC_LIBRARY, tmp);
    if (failed(jit, "module")) goto done;
    if (snx_replace(tmp, build->output)) {
        snx_error("Linking failed: cannot write %s", build->output);
        goto done;
    }
    if (!options->quiet) {
        printf("libgccjit module: %.1f MB, %lu entries\n", (double)snx_file_size(build->output) / 1000000.0,
               (unsigned long)entry_count);
        if (units.fallbacks) printf("libgccjit: %lu instructions run through the interpreter step\n", units.fallbacks);
        puts("note: libgccjit is the backend for DEC Alpha EV67 hosts; elsewhere --backend=llvm (the default) is "
             "the recommended one");
    }
    rc = 0;
done:
    snx_jit_free(jit);
    if (tmp && snx_is_file(tmp)) snx_remove_tree(tmp);
    if (objects) snx_free_strings(objects, n);
    if (dir) snx_remove_tree(dir);
    free(dir);
    free(rtlib);
    free(tmp);
    snx_codegen_free(&codegen);
    return rc;
}
#endif
