/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A GXP program whose words name more than it holds: each table the translator walks by a count of the game's
   (parameters, containers, uniform buffers, dependent samplers, literals) ending past the program's bytes, and a
   texture read whose coordinate index is past the eleven a fragment program has, are refused by what they are, the
   translator reading and writing nothing outside the program and its own arrays */
#include "vita/gpu/gxp.h"
#include "vita/gpu/usse.h"
#include <stdio.h>
#include <string.h>

enum { PROGRAM = 216, VARYINGS = PROGRAM, DESCRIPTOR = VARYINGS + 24, SIZE = DESCRIPTOR + 16 };

/* tests/vita_usse_cube.c's fragment program, one cube sampler read through a dependent sampler, with room after it */
static void program(unsigned char* gxp) {
    memset(gxp, 0, SIZE);
    snx_st_le32(gxp, GXP_MAGIC);
    snx_st_le32(gxp + GXP_FLAGS, GXP_FLAG_FRAGMENT);
    snx_st_le32(gxp + GXP_PARAMETER_COUNT, 1);
    snx_st_le32(gxp + GXP_PARAMETERS, 144); /* parameter record at 184 */
    snx_st_le32(gxp + GXP_PRIMARY_COUNT, 1);
    snx_st_le32(gxp + GXP_PRIMARY, 112); /* instruction at 176 */
    snx_st_le32(gxp + GXP_DEPENDENT_SAMPLER_COUNT, 1);
    snx_st_le32(gxp + GXP_DEPENDENT_SAMPLERS, 36); /* dependent sampler at 168 */
    snx_st_le32(gxp + GXP_CONTAINER_COUNT, 1);
    snx_st_le32(gxp + GXP_CONTAINERS, 12); /* container at 160 */
    snx_st_le16(gxp + 160, 19);
    snx_st_le16(gxp + 164, 1);
    snx_st_le16(gxp + 168, 0);
    snx_st_le16(gxp + 170, 1);
    snx_st_le64(gxp + 176, 0xe0000800c0000080ull);
    gxp[184 + 4] = GXP_CATEGORY_SAMPLER;
    gxp[184 + 7] = 0x10;
    snx_st_le32(gxp + 184 + 8, 1);
    snx_st_le32(gxp + 184 + 12, 0);
}
/* whether the program is refused with want in its error */
static int refused(const unsigned char* gxp, const char* want, const char* what) {
    UsseHints hints = {0};
    UsseShader shader;
    char error[256] = {0};
    if (usse_translate(gxp, SIZE, &hints, false, &shader, error, sizeof error)) {
        fprintf(stderr, "%s: translated, where \"%s\" refuses it\n", what, want);
        usse_shader_free(&shader);
        return 0;
    }
    if (!strstr(error, want)) {
        fprintf(stderr, "%s: refused with \"%s\", not \"%s\"\n", what, error, want);
        return 0;
    }
    return 1;
}

int main(void) {
    static const struct {
        unsigned count_field;
        const char* want;
    } tables[] = {{GXP_PARAMETER_COUNT, "parameters"},
                  {GXP_CONTAINER_COUNT, "containers"},
                  {GXP_UNIFORM_BUFFER_COUNT, "uniform buffers"},
                  {GXP_DEPENDENT_SAMPLER_COUNT, "dependent samplers"},
                  {GXP_LITERALS_COUNT, "literals"}};
    unsigned char gxp[SIZE];
    UsseHints hints = {0};
    UsseShader shader;
    char error[256] = {0};
    int ok = 1;
    program(gxp);
    if (!usse_translate(gxp, SIZE, &hints, false, &shader, error, sizeof error)) {
        fprintf(stderr, "the whole program: %s\n", error);
        return 1;
    }
    usse_shader_free(&shader);
    /* a table of 2^28 records, and one a record longer than the bytes left after where it starts */
    for (unsigned i = 0; i < sizeof tables / sizeof *tables; ++i) {
        char want[64];
        program(gxp);
        snx_st_le32(gxp + tables[i].count_field, 1u << 28);
        snprintf(want, sizeof want, "268435456 %s", tables[i].want);
        ok &= refused(gxp, want, tables[i].want);
        program(gxp);
        snx_st_le32(gxp + tables[i].count_field, SIZE / 4);
        snprintf(want, sizeof want, "%u %s", (unsigned)SIZE / 4, tables[i].want);
        ok &= refused(gxp, want, tables[i].want);
    }
    /* a table that starts past the program, one record of it */
    program(gxp);
    snx_st_le32(gxp + GXP_LITERALS, SIZE), snx_st_le32(gxp + GXP_LITERALS_COUNT, 1);
    ok &= refused(gxp, "1 literals, from byte 372", "literals past the end");
    /* a texture read into PA through coordinate 11, and through 0x401: its descriptor after the program */
    for (unsigned i = 0; i < 2; ++i) {
        char want[64];
        const uint32_t index = i ? 0x401 : 11;
        program(gxp);
        snx_st_le32(gxp + GXP_VARYINGS, VARYINGS - GXP_VARYINGS);
        snx_st_le16(gxp + VARYINGS + 12, 1);
        snx_st_le32(gxp + VARYINGS + 16, DESCRIPTOR - (VARYINGS + 16));
        snx_st_le32(gxp + DESCRIPTOR, 0xf000u | index); /* no varying, a texture read */
        snx_st_le32(gxp + DESCRIPTOR + 12, 3u << 4);    /* float components */
        snprintf(want, sizeof want, "texture coordinate %u", (unsigned)index);
        ok &= refused(gxp, want, "coordinate index");
    }
    return ok ? 0 : 1;
}
