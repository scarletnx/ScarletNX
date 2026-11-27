/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "nx/loader/codegen_view.h"
#include <stdio.h>
#include <stdlib.h>
void snx_nx_module_share(const SnxCodegen* codegen, const SnxModuleImage* module, size_t* functions,
                         SnxU64* instructions) {
    *functions = 0;
    *instructions = 0;
    for (size_t i = 0; i < codegen->function_count; ++i) {
        const SnxFunction* f = &codegen->functions[i];
        if (f->start < module->base || f->start - module->base >= module->size) continue;
        ++*functions;
        *instructions += f->count;
    }
}
int snx_nx_codegen_analyze(SnxCodegen* codegen, const SnxProgram* program, const SnxU64* hints, size_t hint_count,
                           int quiet, SnxU64 features) {
    SnxCodeImage* views;
    size_t i;
    int result;
    views = (SnxCodeImage*)snx_alloc(program->module_count * sizeof(*views));
    for (i = 0; i < program->module_count; ++i) {
        const SnxModuleImage* m = &program->modules[i];
        views[i] = (SnxCodeImage){0}; /* a field this loop doesn't name, as the updatable copy's, is none */
        views[i].image = m->image;
        views[i].image_size = m->image_size;
        views[i].base = m->base;
        views[i].text_off = m->text_off;
        views[i].text_size = m->text_size;
        views[i].functions = m->functions;
        views[i].function_count = m->function_count;
        views[i].relptrs = m->relptrs;
        views[i].relptr_count = m->relptr_count;
        views[i].routed = m->routed;
        views[i].routed_count = m->routed_count;
        views[i].fde = m->fde;
        views[i].fde_count = m->fde_count;
    }
    result = snx_codegen_analyze(codegen, views, program->module_count, hints, hint_count, quiet, features);
    free(views);
    /* each module's share of the code, where there are several: what a thread a module would be given */
    for (i = 0; !result && !quiet && program->module_count > 1 && i < program->module_count; ++i) {
        size_t functions;
        SnxU64 instructions;
        snx_nx_module_share(codegen, &program->modules[i], &functions, &instructions);
        printf("  %s: %zu functions, %llu instructions\n", program->modules[i].name ? program->modules[i].name : "?",
               functions, (unsigned long long)instructions);
    }
    return result;
}
