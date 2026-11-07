/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "vita/gpu/gxp.h"
#include "vita/gpu/usse.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void check_instruction(void* context, const char* name, bool known) {
    unsigned* count = context;
    assert(name && !strcmp(name, "VDUAL"));
    assert(known);
    ++*count;
}

int main(void) {
    static const uint64_t instructions[] = {
        0x28d4313ccfb60380ull, 0x28d4313ccfb60381ull, 0x2024600080090082ull, 0x2024600280c90082ull};
    unsigned char gxp[160 + sizeof instructions] = {0};
    snx_st_le32(gxp, GXP_MAGIC);
    snx_st_le32(gxp + GXP_FLAGS, GXP_FLAG_FRAGMENT);
    snx_st_le32(gxp + GXP_PRIMARY_COUNT, (uint32_t)(sizeof instructions / sizeof instructions[0]));
    snx_st_le32(gxp + GXP_PRIMARY, sizeof gxp - GXP_PRIMARY - sizeof instructions);
    for (unsigned i = 0; i < sizeof instructions / sizeof instructions[0]; ++i)
        snx_st_le64(gxp + 160 + i * 8, instructions[i]);

    unsigned surveyed = 0;
    usse_survey(gxp, sizeof gxp, check_instruction, &surveyed);
    assert(surveyed == sizeof instructions / sizeof instructions[0]);

    UsseShader shader;
    char error[256] = {0};
    if (!usse_translate(gxp, sizeof gxp, NULL, false, &shader, error, sizeof error)) {
        fprintf(stderr, "logged VDUAL translation failed: %s\n", error);
        return 1;
    }
    assert(shader.fragment && shader.words && shader.count);
    usse_shader_free(&shader);
    return 0;
}
