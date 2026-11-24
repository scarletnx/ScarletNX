/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A function whose code is past the buffer its unit started with (mach/mach_codegen.inc's mach_object grows it and makes
   the function again): made on each of the nine machines, its symbol once in the object, and no place relocated twice */
#include "common/cpu/aarch64/a64_decoder.h"
#include "common/cpu/recompiler/arm64/arm64_codegen.h"
#include "common/cpu/recompiler/la64/la64_codegen.h"
#include "common/cpu/recompiler/mips64/mips64_codegen.h"
#include "common/cpu/recompiler/mips64/mips64be_codegen.h"
#include "common/cpu/recompiler/ppc64/ppc64_codegen.h"
#include "common/cpu/recompiler/ppc64/ppc64be_codegen.h"
#include "common/cpu/recompiler/rv64/rv64_codegen.h"
#include "common/cpu/recompiler/s390x/s390x_codegen.h"
#include "common/cpu/recompiler/x64/x64_codegen.h"
#include "common/utilities/endian.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CODE  0x10000u
#define WORDS 24u
#define TBX   0x4E057020u /* TBX V0.16B, {V1.16B-V4.16B}, V5.16B, which reads V0: none of them is dead */
#define FSQRT 0x1E61C0C6u /* FSQRT D6, D6: a helper's call, and a relocation, where the host's FP is not used */
#define RET   0xD65F03C0u

typedef int (*MakeObject)(SnxCodegen* cg, const SnxNativeOptions* options, unsigned char* made, unsigned char** object,
                          size_t* size, char* error, size_t error_size);
static int make_arm64(SnxCodegen* cg, const SnxNativeOptions* o, unsigned char* m, unsigned char** p, size_t* n,
                      char* e, size_t s) {
    return snx_arm64_object(cg, 0, cg->function_count, o, 0, m, p, n, e, s);
}
#define MAKE(name)                                                                                                     \
    static int make_##name(SnxCodegen* cg, const SnxNativeOptions* o, unsigned char* m, unsigned char** p, size_t* n,  \
                           char* e, size_t s) {                                                                        \
        return snx_##name##_object(cg, 0, cg->function_count, o, m, p, n, e, s);                                       \
    }
MAKE(x64)
MAKE(rv64)
MAKE(ppc64)
MAKE(ppc64be)
MAKE(mips64)
MAKE(mips64be)
MAKE(la64)
MAKE(s390x)

static const struct {
    const char* name;
    MakeObject make;
} k_machines[] = {{"x86-64", make_x64},      {"arm64", make_arm64},      {"riscv64", make_rv64},
                  {"ppc64le", make_ppc64},   {"ppc64", make_ppc64be},    {"mips64el", make_mips64},
                  {"mips64", make_mips64be}, {"loongarch64", make_la64}, {"s390x", make_s390x}};

/* the places a little-endian ELF64 object's relocations name twice */
static unsigned twice(const unsigned char* o, size_t size) {
    unsigned n = 0, s, count;
    if (size < 64 || o[0] != 0x7F || o[5] != 1) return 0;
    count = snx_ld_le16(o + 0x3C);
    for (s = 0; s < count; ++s) {
        const unsigned char *h = o + snx_ld_le64(o + 0x28) + (size_t)s * snx_ld_le16(o + 0x3A), *r, *end, *q;
        if (snx_ld_le32(h + 4) != 4) continue; /* SHT_RELA */
        r = o + snx_ld_le64(h + 0x18);
        end = r + snx_ld_le64(h + 0x20);
        for (; r < end; r += 24)
            for (q = r + 24; q < end; q += 24) n += snx_ld_le64(q) == snx_ld_le64(r);
    }
    return n;
}

int main(void) {
    static const char name[] = "F_10000"; /* with its end: the one name of the string table */
    static uint8_t code[(WORDS + 1) * 4];
    uint64_t hints[1] = {CODE};
    SnxCodegen cg;
    SnxCodeImage view = {0};
    unsigned failures = 0, grown = 0, k, b, m;
    for (k = 0; k <= WORDS; ++k)
        for (b = 0; b < 4; ++b) code[4 * k + b] = (uint8_t)((k == WORDS ? RET : k % 4 ? TBX : FSQRT) >> (8 * b));
    view.image = code;
    view.image_size = view.text_size = sizeof code;
    view.base = CODE;
    if (snx_codegen_analyze(&cg, &view, 1, hints, 1, 1, SNX_A64_TARGET_SWITCH) || cg.function_count != 1) {
        fprintf(stderr, "FAIL: the program's function\n");
        return 1;
    }
    for (m = 0; m < sizeof k_machines / sizeof *k_machines; ++m) {
        SnxNativeOptions options = {0};
        unsigned char made = 0, *object = NULL;
        size_t size = 0, at, names = 0;
        char error[256] = "";
        if (!m && !snx_x64_available()) continue;
        if (k_machines[m].make(&cg, &options, &made, &object, &size, error, sizeof error) < 0 || !made) {
            fprintf(stderr, "FAIL: %s: the function is not made: %s\n", k_machines[m].name, error);
            ++failures;
            continue;
        }
        for (at = 0; at + sizeof name <= size; ++at) names += !memcmp(object + at, name, sizeof name);
        if (names != 1) {
            fprintf(stderr, "FAIL: %s: the object has the function's symbol %zu times\n", k_machines[m].name, names);
            ++failures;
        }
        if (twice(object, size)) {
            fprintf(stderr, "FAIL: %s: %u relocations of a place that has one\n", k_machines[m].name, twice(object, size));
            ++failures;
        }
        /* what mach_object gives the first function: 4096 bytes, and 560 a word */
        grown += size > 2 * (4096 + (WORDS + 1) * 160 + 512) + (WORDS + 1) * 400 + 4096;
        free(object);
    }
    if (grown < 7) {
        fprintf(stderr, "FAIL: %u objects past the buffer, not the 7 of the machines without vectors\n", grown);
        ++failures;
    }
    snx_codegen_free(&cg);
    printf("native_grow: %u failures\n", failures);
    return failures != 0;
}
