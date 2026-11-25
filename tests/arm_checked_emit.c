/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The object of arm_checked.c's blocks, made at build time by the host's native back end for its linker: no execution
   engine.
     snx-test-arm-checked-emit OBJECT COUNTS POOL
   COUNTS gets the words or halfwords each covers, an initializer. POOL is the host registers their values are in (0:
   all of them), so that they spill */
#include "common/cpu/arm/codegen.h"
#include "arm_checked_cases.h"
#include "support/arm_back.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* the object's block of code, guest memory's bytes: its units, 0 where it was not made (said) */
static size_t emit(Back* back, const char* name, int thumb, const uint32_t* units, size_t n, uint32_t pc,
                   const SnxArmCodegenOptions* options) {
    unsigned char code[4 * 8];
    unsigned width = thumb ? 2 : 4;
    size_t consumed = 0;
    for (size_t k = 0; k < n; ++k)
        for (unsigned byte = 0; byte < width; ++byte) code[k * width + byte] = (unsigned char)(units[k] >> 8 * byte);
    if (back_(options)(back, options) ||
        !(thumb ? back_(thumb_block)(back, name, code, n, pc, &consumed, NULL, NULL)
                : back_(block)(back, name, code, n, pc, &consumed, NULL, NULL)) ||
        !consumed) {
        fprintf(stderr, "emit failed: %s: %s\n", name, back_(why)(back));
        return 0;
    }
    return consumed;
}

int main(int argc, char** argv) {
    size_t counts[F_count], size = 0, blocks = 0;
    unsigned char* object = NULL;
    FILE* f;
    if (argc != 4) return 2;
    back_begin();
    Back* back = back_(new)();
    if (!back) {
        fprintf(stderr, "no " BACK_NAME " back end\n");
        return 1;
    }
    back_(limit)(back, (int)strtol(argv[3], NULL, 10), ~0u);
    for (unsigned i = 0; i < F_count; ++i) {
        const ArmFixture* fixture = &arm_fixtures[i];
        SnxArmCodegenOptions o = {SNX_ARM_CODEGEN_OPTIONS_ABI, (SnxArmProfile)fixture->profile,
                                  fixture->checked ? SNX_ARM_MEMORY_CHECKED : SNX_ARM_MEMORY_FLAT, 0, 0};
        char name[80];
        snprintf(name, sizeof name, "snx_fx_%s", fixture->name);
        counts[i] = emit(back, name, fixture->thumb, fixture->code, fixture->count, fixture->pc,
                         fixture->checked || fixture->profile ? &o : NULL);
        if (!counts[i]) return 1;
    }
    /* Rejected options: a bad ABI, profile or memory mode, and bus cycles counted by a profile other than the ARM7TDMI,
       into one place only, or into the table itself; and the AGB's ARM7TDMI hosted. None of them leaves a block, and
       the compiler's own options are as they were */
    SnxArmCodegenOptions bad[] = {{99, 0, 0, 0, 0},
                                  {1, 99, 0, 0, 0},
                                  {1, 0, 99, 0, 0},
                                  {1, SNX_ARM_PROFILE_A57, SNX_ARM_MEMORY_FLAT, 1024, 1032},
                                  {1, SNX_ARM_PROFILE_ARM7TDMI, SNX_ARM_MEMORY_FLAT, 1024, 0},
                                  {1, SNX_ARM_PROFILE_ARM7TDMI, SNX_ARM_MEMORY_FLAT, 1024, 1024},
                                  {1, SNX_ARM_PROFILE_ARM7TDMI, SNX_ARM_MEMORY_HOSTED, 0, 0}};
    for (size_t i = 0; i < sizeof bad / sizeof *bad; ++i)
        if (!back_(options)(back, &bad[i])) {
            fprintf(stderr, "options %zu of the bad ones taken\n", i);
            return 1;
        }
    /* a hosted block has no entry checks, and SETEND BE stops it (arm_checked.c runs them) */
    SnxArmCodegenOptions hosted = {SNX_ARM_CODEGEN_OPTIONS_ABI, SNX_ARM_PROFILE_A9, SNX_ARM_MEMORY_HOSTED, 0, 0};
    uint32_t arm[2] = {0xe1a00000, 0xf1010200}, thumb[2] = {0x46c0, 0x46c0};
    if (!emit(back, "snx_hosted_arm", 0, arm, 2, 0x1000, &hosted) ||
        !emit(back, "snx_hosted_thumb", 1, thumb, 2, 0x2000, &hosted))
        return 1;
    if (back_(object)(back, &object, &size, &blocks) || blocks != F_count + 2) {
        fprintf(stderr, "the object has %zu blocks, not the %u made\n", blocks, F_count + 2);
        return 1;
    }
    f = fopen(argv[1], "wb");
    if (!f || fwrite(object, 1, size, f) != size || fclose(f)) return 1;
    f = fopen(argv[2], "w");
    if (!f) return 1;
    for (unsigned i = 0; i < F_count; ++i) fprintf(f, "%zu,\n", counts[i]);
    if (fclose(f)) return 1;
    free(object);
    back_(free)(back);
    return 0;
}
