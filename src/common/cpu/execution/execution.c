/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "common/cpu/execution/execution.h"
#include "common/cpu/recompiler/codegen.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#endif

typedef struct {
    u64 address;
    GuestFn function;
} ExecutionEntry;
typedef const ExecutionEntry* (*EntriesFn)(size_t* count);

static void function_name(FILE* out, u64 address) {
    fputs("F_", out);
    snx_a64_hex(out, address);
}

int snx_execution_compile(SnxExecution* e, const SnxExecRange* ranges, size_t count, u64 entry, const char* cc,
                          char* error, size_t error_size) {
    SnxCodeImage* images;
    SnxCodegen codegen;
    char *source = NULL, *library = NULL, *output = NULL;
    const char* args[24];
    FILE* file = NULL;
    EntriesFn get_entries;
    const ExecutionEntry* entries;
    size_t i, j, nentries, n = 0;
    int result = -1;
    memset(e, 0, sizeof(*e));
    memset(&codegen, 0, sizeof(codegen));
    if (error_size) error[0] = 0;
    if (!count) {
        snprintf(error, error_size, "ELF has no executable ranges");
        return -1;
    }
    images = calloc(count, sizeof(*images));
    if (!images) {
        snprintf(error, error_size, "out of memory");
        return -1;
    }
    e->code_lo = UINT64_MAX;
    for (i = 0; i < count; ++i) {
        images[i].image = ranges[i].data;
        images[i].image_size = ranges[i].size;
        images[i].base = ranges[i].address;
        images[i].text_size = ranges[i].size;
        if (ranges[i].address < e->code_lo) e->code_lo = ranges[i].address;
        if (ranges[i].address + ranges[i].size > e->code_hi) e->code_hi = ranges[i].address + ranges[i].size;
    }
    if (e->code_hi - e->code_lo > 256 * 1024 * 1024 || snx_codegen_analyze(&codegen, images, count, &entry, 1, 1)) {
        snprintf(error, error_size, "executable range exceeds dispatch limit");
        goto done;
    }
    e->code_lo &= ~UINT64_C(3);
    codegen.check_fetch = 1;
    e->temporary = snx_temp_dir(NULL);
    if (!e->temporary) {
        snprintf(error, error_size, "cannot create compiler directory");
        goto done;
    }
    source = snx_path_join(e->temporary, "code.c");
#ifdef _WIN32
    library = snx_path_join(e->temporary, "code.dll");
#else
    library = snx_path_join(e->temporary, "code.so");
#endif
    file = snx_fopen(source, "wb");
    if (!file) {
        snprintf(error, error_size, "cannot write generated code");
        goto done;
    }
    fputs("#include \"cpu.h\"\n#include \"cpu_helpers.c\"\n", file);
    fputs("typedef void(*FetchFn)(CPU*,u64,u32*);static FetchFn fetch_fn;\n"
          "SNX_EXPORT void snx_set_fetch(FetchFn f){fetch_fn=f;}\n"
          "static void snx_fetch(CPU*c,u64 pc,u32*w){if(fetch_fn)fetch_fn(c,pc,w);"
          "else memcpy(w,c->mem+pc,4);}\n",
          file);
    for (i = 0; i < codegen.function_count; ++i) {
        fputs("void ", file);
        function_name(file, codegen.functions[i].start);
        fputs("(CPU* RESTRICT c,u64 e);\n", file);
    }
    snx_a64_interp_checked(file);
    for (i = 0; i < codegen.function_count; ++i) snx_codegen_emit(file, &codegen, &codegen.functions[i]);
    fputs("typedef struct{u64 address;GuestFn function;} ExecutionEntry;\nstatic const ExecutionEntry entries[]={",
          file);
    for (i = 0; i < codegen.function_count; ++i)
        for (j = 0; j < codegen.functions[i].count; ++j)
            if (codegen.functions[i].leaders[j]) {
                fprintf(file, "%s{", n++ ? "," : "");
                snx_a64_literal(file, snx_function_address(&codegen.functions[i], j));
                fputc(',', file);
                function_name(file, codegen.functions[i].start);
                fputc('}', file);
            }
    if (!n) fputs("{0,0}", file);
    fprintf(file, "};\nSNX_EXPORT const ExecutionEntry* snx_entries(size_t*n){*n=%zu;return entries;}\n", n);
    fputs("SNX_EXPORT void snx_execute(CPU*c,u64 pc){snx_lookup(c,pc)(c,pc);}\n", file);
    if (ferror(file)) {
        fclose(file);
        file = NULL;
        goto done;
    }
    if (fclose(file)) {
        file = NULL;
        goto done;
    }
    file = NULL;
    n = 0;
    args[n++] = cc ? cc : SNX_HOST_COMPILER;
    args[n++] = "-shared";
#ifndef _WIN32
    args[n++] = "-fPIC";
    args[n++] = "-fvisibility=hidden";
#endif
    args[n++] = "-O2";
    args[n++] = "-std=gnu2x";
    args[n++] = "-fno-strict-aliasing";
    args[n++] = "-fwrapv";
    args[n++] = "-foptimize-sibling-calls";
#if defined(__x86_64__) || defined(_M_X64)
    args[n++] = "-mcx16";
#endif
    args[n++] = "-w";
    args[n++] = "-I";
    args[n++] = SNX_CPU_INCLUDE;
    args[n++] = "-I";
    args[n++] = SNX_SOURCE_INCLUDE;
    args[n++] = source;
    args[n++] = "-o";
    args[n++] = library;
#ifndef _WIN32
    args[n++] = "-lm";
#endif
    args[n] = NULL;
    if (snx_run(args, NULL, NULL, &output)) {
        snprintf(error, error_size, "host compilation failed: %.300s", output ? output : "");
        goto done;
    }
#ifdef _WIN32
    int wide_count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, library, -1, NULL, 0);
    wchar_t* wide_library = wide_count > 0 ? malloc((size_t)wide_count * sizeof(*wide_library)) : NULL;
    if (wide_library && MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, library, -1, wide_library, wide_count))
        e->module = (void*)LoadLibraryW(wide_library);
    free(wide_library);
    if (e->module) {
        get_entries = (EntriesFn)(void*)GetProcAddress((HMODULE)e->module, "snx_entries");
        e->entry = (GuestFn)(void*)GetProcAddress((HMODULE)e->module, "snx_execute");
        e->set_fetch = (void (*)(SnxFetchFn))(void*)GetProcAddress((HMODULE)e->module, "snx_set_fetch");
    } else
        get_entries = NULL;
#else
    e->module = dlopen(library, RTLD_NOW | RTLD_LOCAL);
    if (e->module) {
        get_entries = (EntriesFn)dlsym(e->module, "snx_entries");
        e->entry = (GuestFn)dlsym(e->module, "snx_execute");
        e->set_fetch = (void (*)(SnxFetchFn))dlsym(e->module, "snx_set_fetch");
    } else
        get_entries = NULL;
#endif
    if (!get_entries || !e->entry) {
        snprintf(error, error_size, "cannot load generated module");
        goto done;
    }
    e->dispatch = calloc((size_t)(e->code_hi - e->code_lo + 3) / 4, sizeof(*e->dispatch));
    if (!e->dispatch) {
        snprintf(error, error_size, "out of memory for dispatch table");
        goto done;
    }
    entries = get_entries(&nentries);
    for (i = 0; i < nentries; ++i) e->dispatch[(entries[i].address - e->code_lo) / 4] = entries[i].function;
    result = 0;
done:
    if (file) fclose(file);
    free(images);
    free(source);
    free(library);
    free(output);
    snx_codegen_free(&codegen);
    if (result) {
        if (error_size && !error[0]) snprintf(error, error_size, "cannot generate host code");
        snx_execution_free(e);
    }
    return result;
}

void snx_execution_run(SnxExecution* e, CPU* cpu, u64 entry) {
    cpu->disp = e->dispatch;
    cpu->code_lo = e->code_lo;
    cpu->code_hi = e->code_hi;
    if (e->set_fetch) e->set_fetch(e->fetch);
    e->entry(cpu, entry);
}

void snx_execution_free(SnxExecution* e) {
    if (e->module) {
#ifdef _WIN32
        FreeLibrary((HMODULE)e->module);
#else
        dlclose(e->module);
#endif
    }
    free(e->dispatch);
    if (e->temporary) {
        snx_remove_tree(e->temporary);
        free(e->temporary);
    }
    memset(e, 0, sizeof(*e));
}
