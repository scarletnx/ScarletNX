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
typedef struct {
    SnxU64 hash;
    size_t count, index;
    int placed; /* its code depends on where it sits */
} Words;
static int compare_words(const void* a, const void* b) {
    const Words *x = (const Words*)a, *y = (const Words*)b;
    if (x->count != y->count) return x->count < y->count ? -1 : 1;
    if (x->hash != y->hash) return x->hash < y->hash ? -1 : 1;
    return x->index < y->index ? -1 : x->index > y->index;
}
static SnxU32 word_at(const SnxCodegen* codegen, const SnxFunction* f, size_t k) {
    SnxU32 word = 0;
    (void)snx_codegen_word((SnxCodegen*)codegen, snx_function_address(f, k), &word);
    return word;
}
void snx_nx_repeated_functions(const SnxCodegen* codegen, SnxU64 same[2], SnxU64 alike[2]) {
    Words* all = (Words*)snx_alloc((codegen->function_count ? codegen->function_count : 1) * sizeof(*all));
    same[0] = same[1] = alike[0] = alike[1] = 0;
    for (size_t i = 0; i < codegen->function_count; ++i) {
        const SnxFunction* f = &codegen->functions[i];
        SnxU64 hash = 1469598103934665603ull;
        int placed = 0;
        for (size_t k = 0; k < f->count; ++k) {
            const SnxU32 w = word_at(codegen, f, k);
            const SnxU64 at = snx_function_address(f, k);
            SnxU64 far = 0; /* a branch's offset, its sign carried up by hand */
            int branch = 1;
            hash = (hash ^ w) * 1099511628211ull;
            if ((w & 0x7C000000u) == 0x14000000u) /* B, BL: imm26 */
                far = ((SnxU64)(w & 0x3FFFFFFu) << 2) | (w & 0x2000000u ? ~(SnxU64)0 << 28 : 0);
            else if ((w & 0xFF000010u) == 0x54000000u ||
                     (w & 0x7E000000u) == 0x34000000u) /* B.cond, CBZ, CBNZ: imm19 */
                far = ((SnxU64)((w >> 5) & 0x7FFFFu) << 2) | (w & 0x800000u ? ~(SnxU64)0 << 21 : 0);
            else if ((w & 0x7E000000u) == 0x36000000u) /* TBZ, TBNZ: imm14 */
                far = ((SnxU64)((w >> 5) & 0x3FFFu) << 2) | (w & 0x40000u ? ~(SnxU64)0 << 16 : 0);
            else
                branch = 0;
            if ((w & 0x1F000000u) == 0x10000000u || (w & 0x3B000000u) == 0x18000000u)
                placed = 1; /* ADR, ADRP, a literal */
            if (branch && !snx_function_contains(f, at + far)) placed = 1;
        }
        all[i] = (Words){hash, f->count, i, placed};
    }
    if (codegen->function_count) qsort(all, codegen->function_count, sizeof(*all), compare_words);
    for (size_t i = 1, first = 0; i < codegen->function_count; ++i) {
        const SnxFunction *a = &codegen->functions[all[first].index], *b = &codegen->functions[all[i].index];
        size_t k = 0;
        if (all[i].count == all[first].count && all[i].hash == all[first].hash)
            while (k < b->count && word_at(codegen, a, k) == word_at(codegen, b, k)) ++k;
        if (all[i].count != all[first].count || all[i].hash != all[first].hash || k < b->count) {
            first = i;
            continue;
        }
        ++same[0];
        same[1] += b->count;
        if (all[i].placed) continue;
        ++alike[0];
        alike[1] += b->count;
    }
    free(all);
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
    if (!result && !quiet && codegen->function_count > 1) {
        SnxU64 same[2], alike[2];
        snx_nx_repeated_functions(codegen, same, alike);
        printf("  functions with another's words: %llu (%llu instructions), %llu of them the same code wherever they "
               "sit (%llu instructions)\n",
               (unsigned long long)same[0], (unsigned long long)same[1], (unsigned long long)alike[0],
               (unsigned long long)alike[1]);
    }
    return result;
}
