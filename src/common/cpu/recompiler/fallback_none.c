/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* No fallback (fallback.h): a build without LLVM has the native back end's code alone, and what that leaves is refused
   here by name. An instruction the native back end has no code for is not asked for: with SnxNativeOptions.alone it is
   undefined where it runs */
#include "common/cpu/recompiler/fallback.h"
#include "common/utilities/common.h"
#include <stdio.h>
#include <stdlib.h>

#define NO_LLVM "this build has no LLVM to make it (SNX_WITH_LLVM=OFF)"

struct SnxFallbackArm {
    char name[32]; /* the unit's, which the errors name */
};

int snx_fallback_available(void) { return 0; }
const char* snx_fallback_name(void) { return "none"; }

int snx_fallback_object(SnxCodegen* codegen, size_t first, size_t last, const SnxNativeOptions* options,
                        const unsigned char* made, const char* name, char opt, unsigned char** object, size_t* size,
                        char* error, size_t error_size) {
    const SnxFunction* f = NULL;
    size_t left = 0;
    (void)options;
    (void)opt;
    *object = NULL;
    *size = 0;
    for (size_t i = first; i < last; ++i)
        if ((!made || !made[i - first]) && !left++) f = &codegen->functions[i];
    if (!f) return 0;
    char text[64] = "no instruction", more[48] = "";
    SnxU32 word = 0;
    if (snx_codegen_word(codegen, f->start, &word)) snx_a64_word_text(word, text, sizeof text);
    if (left > 1) snprintf(more, sizeof more, ", and %zu more of the unit's functions", left - 1);
    snprintf(error, error_size,
             "%s: the function at 0x%llx, which starts with %s (%08x)%s: the native back end (%s) left it and " NO_LLVM,
             name, (unsigned long long)f->start, text, (unsigned)word, more, snx_native_name());
    return -1;
}

SnxFallbackArm* snx_fallback_arm_new(const char* name, char opt, int jit) {
    SnxFallbackArm* f = (SnxFallbackArm*)calloc(1, sizeof *f);
    (void)opt;
    (void)jit;
    if (f) snprintf(f->name, sizeof f->name, "%s", name);
    return f;
}
void snx_fallback_arm_free(SnxFallbackArm* f) { free(f); }
int snx_fallback_arm_start(SnxFallbackArm* f, char* error, size_t error_size) {
    snprintf(error, error_size, "%s: the native back end is %s, and this build has no LLVM (SNX_WITH_LLVM=OFF)",
             f->name, snx_native_name());
    return -1;
}
int snx_fallback_arm_block(SnxFallbackArm* f, const char* name, const void* code, size_t count, uint32_t pc, int thumb,
                           const SnxArmCodegenOptions* options, const char* why, size_t* consumed, char* error,
                           size_t error_size) {
    (void)name;
    (void)options;
    *consumed = 0;
    if (!count)
        snprintf(error, error_size, "%s: the %s block at 0x%08x, of no instructions: %s, and " NO_LLVM, f->name,
                 thumb ? "Thumb" : "ARM", (unsigned)pc, why);
    else if (thumb)
        snprintf(error, error_size, "%s: the Thumb block at 0x%08x, which starts with %04x: %s, and " NO_LLVM, f->name,
                 (unsigned)pc, (unsigned)((const uint16_t*)code)[0], why);
    else
        snprintf(error, error_size, "%s: the ARM block at 0x%08x, which starts with %08x: %s, and " NO_LLVM, f->name,
                 (unsigned)pc, (unsigned)((const uint32_t*)code)[0], why);
    return -1;
}
int snx_fallback_arm_object(SnxFallbackArm* f, unsigned char** object, size_t* size, char* error, size_t error_size) {
    *object = NULL;
    *size = 0;
    snprintf(error, error_size, "%s: no block was made, and " NO_LLVM, f->name);
    return -1;
}
