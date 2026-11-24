/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* What an x86-64 unit's object is made from (x64_codegen.h's snx_x64_unit_facts), which the unit cache keys the unit
   by: each answer the back end's code depends on changes it, and code the unit never asks about does not. f0 calls
   f1, a PLT stub (ADRP x16, LDR x17, ADD x16, BR x17); below them, NOP; RET is one function and BL +8; RET two (a
   RET only that BL reaches comes next). f0's
   unit's facts are the same twice, and change with f0's index (two functions below it), the guest's features, the
   call's target routed (a hook names it), imported (SnxX64Options.direct), the stub bound (bound), the stale checks
   and the function ir_dump_pc names; MOV x0, #1; RET below them, one function as NOP; RET is but other code, which f0 never reaches, leaves them
   as they were. The back end's own code, the rest of the key (snx_x64_code), is the segment that holds it. On a host
   without the back end it passes, saying so */
#include "common/cpu/recompiler/codegen.h"
#include "common/cpu/recompiler/x64/x64_codegen.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BASE  0x10000u
#define G     0x10000u /* the code below f0 */
#define F0    0x10010u /* BL F1; RET */
#define F1    0x10020u /* the stub */
#define WORDS 16u

static int failures;
static void check(int ok, const char* what) {
    if (!ok) {
        fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

typedef struct {
    unsigned char* bytes;
    size_t size, cap;
} Facts;
static void put(void* user, const void* data, size_t size) {
    Facts* f = (Facts*)user;
    if (f->size + size > f->cap) {
        f->cap = (f->size + size) * 2;
        f->bytes = (unsigned char*)realloc(f->bytes, f->cap);
    }
    memcpy(f->bytes + f->size, data, size);
    f->size += size;
}

static unsigned char g_image[WORDS * 4];
enum { NOP_RET, BL_RET, MOV_RET };
static void image(int below) {
    const unsigned g_code[3] = {below == NOP_RET  ? 0xD503201Fu
                                : below == BL_RET ? 0x94000002u
                                                  : 0xD2800020u,
                                0xD65F03C0u, 0xD65F03C0u};
    const unsigned f0_code[2] = {0x94000004u /* BL F1 */, 0xD65F03C0u};
    /* ADRP x16, page of F1; LDR x17, [x16, #0x100]; ADD x16, x16, #0x100; BR x17 */
    const unsigned f1_code[4] = {0x90000010u, 0xF9408211u, 0x91040210u, 0xD61F0220u};
    unsigned words[WORDS] = {0};
    words[0] = g_code[0], words[1] = g_code[1], words[2] = g_code[2];
    words[4] = f0_code[0], words[5] = f0_code[1];
    for (unsigned i = 0; i < 4; ++i) words[8 + i] = f1_code[i];
    for (unsigned i = 0; i < WORDS; ++i)
        for (unsigned b = 0; b < 4; ++b) g_image[4 * i + b] = (unsigned char)(words[i] >> (8 * b));
}

/* the analysis of the image as image() made it, from F0; the functions it found said where it did not find f0 and
   f1, or not count below them */
static int analyze(SnxCodegen* codegen, size_t below) {
    static SnxCodeImage module;
    const SnxU64 hint = F0;
    const SnxFunction* f0;
    int rc;
    module = (SnxCodeImage){.image = g_image, .image_size = sizeof g_image, .base = BASE, .text_size = sizeof g_image};
    rc = snx_codegen_analyze(codegen, &module, 1, &hint, 1, 1, 0);
    f0 = rc ? NULL : snx_codegen_function(codegen, F0);
    if (f0 && snx_codegen_function(codegen, F1) && (size_t)(f0 - codegen->functions) == below) return 0;
    fprintf(stderr, "the analysis found %zu functions, where f0, f1 and %zu below them were wanted:",
            rc ? 0 : codegen->function_count, below);
    for (size_t i = 0; !rc && i < codegen->function_count; ++i)
        fprintf(stderr, " %llx (%zu words)", (unsigned long long)codegen->functions[i].start,
                codegen->functions[i].count);
    fputc('\n', stderr);
    return -1;
}

static SnxU64 imported(void* user, SnxU64 target) {
    (void)user;
    return target == F1;
}
static SnxU64 bound(void* user, SnxU64 address, int* guard) {
    (void)user;
    *guard = 0;
    return address == F1 + 12 ? F0 : 0;
}

/* f0's unit's facts under options */
static Facts facts(const SnxCodegen* codegen, const SnxX64Options* options) {
    Facts f = {0};
    const SnxFunction* fn = snx_codegen_function(codegen, F0);
    if (fn) {
        size_t i = (size_t)(fn - codegen->functions);
        snx_x64_unit_facts(codegen, i, i + 1, options, put, &f);
    }
    return f;
}
/* whether one of the segments snx_x64_code puts holds the back end's own code */
static int held;
static void segment(void* user, const void* data, size_t size) {
    const unsigned char* at = (const unsigned char*)(const void*)snx_x64_object;
    (void)user;
    if (at >= (const unsigned char*)data && at < (const unsigned char*)data + size) held = 1;
}

static int same(const Facts* a, const Facts* b) {
    return a->size == b->size && a->size && !memcmp(a->bytes, b->bytes, a->size);
}

int main(void) {
    SnxCodegen one, two, other;
    SnxX64Options plain = {0}, options;
    Facts base, again, f;
    if (!snx_x64_available()) {
        printf("x64 unit facts: no x86-64 back end on this host: passed\n");
        return 0;
    }
    image(NOP_RET);
    check(!analyze(&one, 1), "NOP; RET is one function below f0");
    image(BL_RET);
    check(!analyze(&two, 2), "BL +8; RET is two");
    image(NOP_RET);
    base = facts(&one, &plain);
    again = facts(&one, &plain);
    check(base.size > 0 && same(&base, &again), "the same facts twice");
    free(again.bytes);

    f = facts(&two, &plain);
    check(!same(&base, &f), "f0's index (two functions below it) changes them");
    free(f.bytes);
    one.features ^= 1;
    f = facts(&one, &plain);
    check(!same(&base, &f), "the guest's features change them");
    free(f.bytes);
    one.features ^= 1;
    {
        static const SnxU64 routed[1] = {F1 - BASE};
        one.modules[0].routed = routed;
        one.modules[0].routed_count = 1;
        check(snx_codegen_routed(&one, F1), "f1 routed");
        f = facts(&one, &plain);
        check(!same(&base, &f), "the call's target routed changes them");
        free(f.bytes);
        one.modules[0].routed = NULL;
        one.modules[0].routed_count = 0;
    }
    options = plain;
    options.direct = imported;
    f = facts(&one, &options);
    check(!same(&base, &f), "the call's target imported changes them");
    free(f.bytes);
    options = plain;
    options.bound = bound;
    f = facts(&one, &options);
    check(!same(&base, &f), "the stub bound changes them");
    free(f.bytes);
    options = plain;
    options.stale = 1;
    f = facts(&one, &options);
    check(!same(&base, &f), "the stale checks change them");
    free(f.bytes);
    options = plain;
    options.llvm_at = F0;
    f = facts(&one, &options);
    check(!same(&base, &f), "the function ir_dump_pc names changes them");
    free(f.bytes);

    /* other code below f0, one function as before: f0 never reaches it */
    image(MOV_RET);
    check(!analyze(&other, 1), "MOV x0, #1; RET is one function below f0");
    f = facts(&other, &plain);
    check(same(&base, &f), "code f0 never reaches leaves them as they were");
    free(f.bytes);
    free(base.bytes);
    snx_codegen_free(&one);
    snx_codegen_free(&two);
    snx_codegen_free(&other);
    check(!snx_x64_code(segment, NULL) && held, "the back end's own code is one of the segments put");
    printf("x64 unit facts: %s\n", failures ? "FAILED" : "passed");
    return failures != 0;
}
