/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* What an x86-64 unit's object is made from (x64_codegen.h's snx_x64_unit_facts), which the unit cache keys the unit
   by: each answer the back end's code depends on changes it, and code the unit never asks about does not. f0 calls
   f1, a PLT stub (ADRP x16, LDR x17, ADD x16, BR x17). Below them NOP; RET is one function; BL +8; RET two, the second
   NOP; NOP falling through into f0; BL h; RET two, h between f0 and f1. f0's unit's facts are the same twice, and
   change with f0's index and f1's, f1's alone, the guest's features, the call's target routed (a hook names it),
   imported (SnxX64Options.direct), the stub bound (bound), the stale checks and the function ir_dump_pc names; f1's
   unit's with its own index alone and its BR bound; the fall-through's with what its next function answers (routed).
   MOV x0, #1; RET below them, one function as NOP; RET is but other code, which f0 never reaches, leaves f0's as they
   were; a JIT unit's table name changes them. The host's features, the one fact no test on one host changes, are left
   to the reading. On a host without the back end it passes, saying so */
#include "common/cpu/recompiler/codegen.h"
#include "common/cpu/recompiler/x64/x64_cache.h"
#include "common/utilities/common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BASE  0x10000u
#define G     0x10000u /* the code below f0 */
#define X     0x10008u /* NOP; NOP, into f0, where BL +8 reaches it */
#define F0    0x10010u /* BL F1; RET */
#define H     0x10018u /* RET, where BL h reaches it */
#define F1    0x10020u /* the stub */
#define WORDS 16u

/* the checks that failed, and the program's code */
static struct {
    int failures;
    unsigned char image[WORDS * 4];
} g_x64_unit_facts;
static void check(int ok, const char* what) {
    if (!ok) {
        fprintf(stderr, "FAIL: %s\n", what);
        ++g_x64_unit_facts.failures;
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
        f->bytes = (unsigned char*)snx_realloc(f->bytes, f->cap);
    }
    memcpy(f->bytes + f->size, data, size);
    f->size += size;
}

enum { NOP_RET, BL_RET, MOV_RET, BL_H };
static void image(int below) {
    static const unsigned first[] = {0xD503201Fu /* NOP */, 0x94000002u /* BL X */, 0xD2800020u /* MOV x0, #1 */,
                                     0x94000006u /* BL H */};
    const unsigned g_code[4] = {first[below], 0xD65F03C0u, 0xD503201Fu, 0xD503201Fu};
    const unsigned f0_code[2] = {0x94000004u /* BL F1 */, 0xD65F03C0u};
    /* ADRP x16, page of F1; LDR x17, [x16, #0x100]; ADD x16, x16, #0x100; BR x17 */
    const unsigned f1_code[4] = {0x90000010u, 0xF9408211u, 0x91040210u, 0xD61F0220u};
    unsigned words[WORDS] = {0};
    for (unsigned i = 0; i < 4; ++i) words[i] = g_code[i];
    words[4] = f0_code[0], words[5] = f0_code[1];
    words[6] = 0xD65F03C0u; /* h */
    for (unsigned i = 0; i < 4; ++i) words[8 + i] = f1_code[i];
    for (unsigned i = 0; i < WORDS; ++i)
        for (unsigned b = 0; b < 4; ++b) g_x64_unit_facts.image[4 * i + b] = (unsigned char)(words[i] >> (8 * b));
}

/* the analysis of the image as image() made it, from F0; the functions it found said where it did not find f0 and
   f1, or not count below f0 and f1 at f1_index */
static int analyze(SnxCodegen* codegen, size_t below, size_t f1_index) {
    const SnxU64 hint = F0;
    const SnxCodeImage module = {.image = g_x64_unit_facts.image,
                                 .image_size = sizeof g_x64_unit_facts.image,
                                 .base = BASE,
                                 .text_size = sizeof g_x64_unit_facts.image};
    int rc = snx_codegen_analyze(codegen, &module, 1, &hint, 1, 1, 0);
    const SnxFunction* f0 = rc ? NULL : snx_codegen_function(codegen, F0);
    const SnxFunction* f1 = rc ? NULL : snx_codegen_function(codegen, F1);
    if (f0 && f1 && (size_t)(f0 - codegen->functions) == below && (size_t)(f1 - codegen->functions) == f1_index)
        return 0;
    fprintf(stderr, "the analysis found %zu functions, where f0 at index %zu and f1 at %zu were wanted:",
            rc ? 0 : codegen->function_count, below, f1_index);
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

/* the facts of the unit of the function at start under options */
static Facts unit_facts(const SnxCodegen* codegen, SnxU64 start, const SnxX64Options* options) {
    Facts f = {0};
    const SnxFunction* fn = snx_codegen_function(codegen, start);
    if (fn) {
        size_t i = (size_t)(fn - codegen->functions);
        snx_x64_unit_facts(codegen, i, i + 1, options, put, &f);
    }
    return f;
}
static Facts facts(const SnxCodegen* codegen, const SnxX64Options* options) { return unit_facts(codegen, F0, options); }
static int same(const Facts* a, const Facts* b) {
    return a->size == b->size && a->size && !memcmp(a->bytes, b->bytes, a->size);
}
/* whether f0's unit's facts under options are base's */
static int kept(const Facts* base, const SnxCodegen* codegen, const SnxX64Options* options) {
    Facts f = facts(codegen, options);
    int is = same(base, &f);
    free(f.bytes);
    return is;
}

/* the keys' SHA-256 (x64_cache.h) on FIPS 180-2's examples: an empty message, "abc", the 448-bit one that ends in a
   second block, and a million "a" */
static void sha256_vectors(void) {
    static const struct {
        const char* text;
        size_t repeat;
        const char* hex;
    } v[] = {{"", 1, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
             {"abc", 1, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"},
             {"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 1,
              "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"},
             {"a", 1000000, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"}};
    for (size_t i = 0; i < sizeof v / sizeof *v; ++i) {
        size_t n = strlen(v[i].text) * v[i].repeat;
        char *data = (char*)snx_alloc(n + 1), hex[65];
        unsigned char out[32];
        for (size_t k = 0; k < v[i].repeat; ++k) memcpy(data + k * strlen(v[i].text), v[i].text, strlen(v[i].text));
        snx_x64_cache_sha256(data, n, out);
        for (size_t k = 0; k < 32; ++k) snprintf(hex + 2 * k, 3, "%02x", out[k]);
        if (strcmp(hex, v[i].hex)) fprintf(stderr, "SHA-256 of example %zu: %s, not %s\n", i, hex, v[i].hex);
        check(!strcmp(hex, v[i].hex), "the keys' SHA-256 gives FIPS 180-2's digests");
        free(data);
    }
}

int main(void) {
    sha256_vectors();
    if (!snx_x64_available()) {
        printf("x64 unit facts: no x86-64 back end on this host: %s\n",
               g_x64_unit_facts.failures ? "FAILED" : "passed");
        return g_x64_unit_facts.failures != 0;
    }
    SnxCodegen one, two, mid, other;
    image(NOP_RET);
    check(!analyze(&one, 1, 2), "NOP; RET is one function below f0");
    image(BL_RET);
    check(!analyze(&two, 2, 3), "BL +8; RET is two");
    check(snx_codegen_function(&two, X) && snx_codegen_function(&two, X)->count == 2,
          "the second, NOP; NOP, ends where f0 starts");
    image(BL_H);
    check(!analyze(&mid, 1, 3) && snx_codegen_function(&mid, H), "BL h; RET puts h between f0 and f1");
    image(NOP_RET);
    const SnxX64Options plain = {0};
    Facts base = facts(&one, &plain);
    check(base.size > 0 && kept(&base, &one, &plain), "the same facts twice");
    check(!kept(&base, &two, &plain), "f0's index and f1's (two functions below both) change them");
    check(!kept(&base, &mid, &plain), "f1's index alone (h between f0 and f1) changes them");
    {
        Facts a = unit_facts(&one, F1, &plain), b = unit_facts(&mid, F1, &plain);
        check(a.size && !same(&a, &b), "f1's unit's own index alone (h below it) changes its facts");
        free(b.bytes);
        SnxX64Options options = {.bound = bound};
        Facts c = unit_facts(&one, F1, &options);
        check(!same(&a, &c), "f1's BR bound changes its unit's facts");
        free(c.bytes);
        free(a.bytes);
    }
    {
        static const SnxU64 routed[1] = {F0 - BASE};
        Facts a = unit_facts(&two, X, &plain);
        two.modules[0].routed = routed;
        two.modules[0].routed_count = 1;
        Facts b = unit_facts(&two, X, &plain);
        check(a.size && !same(&a, &b), "the function it falls through to routed changes the fall-through's facts");
        two.modules[0].routed = NULL;
        two.modules[0].routed_count = 0;
        free(a.bytes);
        free(b.bytes);
    }
    one.features ^= 1;
    check(!kept(&base, &one, &plain), "the guest's features change them");
    one.features ^= 1;
    {
        static const SnxU64 routed[1] = {F1 - BASE};
        one.modules[0].routed = routed;
        one.modules[0].routed_count = 1;
        check(snx_codegen_routed(&one, F1), "f1 routed");
        check(!kept(&base, &one, &plain), "the call's target routed changes them");
        one.modules[0].routed = NULL;
        one.modules[0].routed_count = 0;
    }
    check(!kept(&base, &one, &(SnxX64Options){.direct = imported}), "the call's target imported changes them");
    check(!kept(&base, &one, &(SnxX64Options){.bound = bound}), "the stub bound changes them");
    check(!kept(&base, &one, &(SnxX64Options){.stale = 1}), "the stale checks change them");
    {
        Facts f = facts(&one, &(SnxX64Options){.jit = 1, .table_name = "snx_jit_1"});
        check(!kept(&f, &one, &(SnxX64Options){.jit = 1, .table_name = "snx_jit_2"}),
              "a JIT unit's table name changes them");
        free(f.bytes);
    }
    check(!kept(&base, &one, &(SnxX64Options){.llvm_at = F0}), "the function ir_dump_pc names changes them");

    /* other code below f0, one function as before: f0 never reaches it */
    image(MOV_RET);
    check(!analyze(&other, 1, 2), "MOV x0, #1; RET is one function below f0");
    check(kept(&base, &other, &plain), "code f0 never reaches leaves them as they were");
    free(base.bytes);
    snx_codegen_free(&one);
    snx_codegen_free(&two);
    snx_codegen_free(&mid);
    snx_codegen_free(&other);
    printf("x64 unit facts: %s\n", g_x64_unit_facts.failures ? "FAILED" : "passed");
    return g_x64_unit_facts.failures != 0;
}
