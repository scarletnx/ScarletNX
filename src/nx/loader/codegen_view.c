/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "nx/loader/codegen_view.h"
#include <stdlib.h>
int snx_nx_codegen_analyze(SnxCodegen* codegen, const SnxProgram* program, const SnxU64* hints, size_t hint_count,
                           int quiet) {
    SnxCodeImage* views;
    size_t i;
    int result;
    views = (SnxCodeImage*)snx_alloc(program->module_count * sizeof(*views));
    for (i = 0; i < program->module_count; ++i) {
        const SnxModuleImage* m = &program->modules[i];
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
    result = snx_codegen_analyze(codegen, views, program->module_count, hints, hint_count, quiet, 0); /* the Switch */
    free(views);
    return result;
}
