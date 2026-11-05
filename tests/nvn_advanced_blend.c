/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Compare the ordinary-blend reduction with independent coverage-weight equations. Test unsupported parameter
   combinations separately: they must return unsupported without changing even one byte of the caller's state. */
#include "nx/gpu/nvn/nvn_blend.h"
#include "support/nvn_advanced_blend_ref.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static int checks, failures;
static void check(int yes, const char* name) {
    checks++;
    if (!yes && failures++ < 12) fprintf(stderr, "FAIL: %s\n", name);
}
static float factor(u8 value, const float source[4], const float destination[4], u32 component) {
    switch (value) {
        case DS_FACTOR_ZERO: return 0;
        case DS_FACTOR_ONE: return 1;
        case DS_FACTOR_SRC_ALPHA: return source[3];
        case DS_FACTOR_ONE_MINUS_SRC_ALPHA: return 1 - source[3];
        case DS_FACTOR_DST_ALPHA: return destination[3];
        case DS_FACTOR_ONE_MINUS_DST_ALPHA: return 1 - destination[3];
        case DS_FACTOR_ONE_MINUS_SRC_COLOR: return 1 - source[component];
        default: check(0, "unexpected factor"); return 0;
    }
}
int main(void) {
    static const u32 modes[] = {
        NVN_BLEND_ADVANCED_MODE_ZERO,     NVN_BLEND_ADVANCED_MODE_SRC,      NVN_BLEND_ADVANCED_MODE_DST,
        NVN_BLEND_ADVANCED_MODE_SRC_OVER, NVN_BLEND_ADVANCED_MODE_DST_OVER, NVN_BLEND_ADVANCED_MODE_SRC_IN,
        NVN_BLEND_ADVANCED_MODE_DST_IN,   NVN_BLEND_ADVANCED_MODE_SRC_OUT,  NVN_BLEND_ADVANCED_MODE_DST_OUT,
        NVN_BLEND_ADVANCED_MODE_SRC_ATOP, NVN_BLEND_ADVANCED_MODE_DST_ATOP, NVN_BLEND_ADVANCED_MODE_XOR,
        NVN_BLEND_ADVANCED_MODE_SCREEN};
    for (u32 mode = 0; mode < sizeof modes / sizeof *modes; mode++)
        for (u32 premultiplied = 0; premultiplied < 2; premultiplied++)
            for (u32 overlap = 0; overlap < 3; overlap++) {
                u64 state = nvn_set_field(0, NVN_BLEND_ADV, modes[mode]);
                state = nvn_set_field(state, NVN_BLEND_ADV_PMS, premultiplied);
                state = nvn_set_field(state, NVN_BLEND_ADV_OVL, overlap);
                state = nvn_set_field(state, NVN_BLEND_ADV_NPM, 1);
                DsBlend reduced, original;
                memset(&reduced, 0xa5, sizeof reduced);
                original = reduced;
                int applied = nvn_blend_advanced(&reduced, 0, state);
                int expected = mode < 3 || (overlap == 0 && (premultiplied || mode == 3));
                check(applied == expected, "supported mode and coverage/premultiplication gate");
                if (!applied) {
                    check(!memcmp(&reduced, &original, sizeof reduced), "unsupported mode preserves the full state");
                    continue;
                }
                check(reduced.en[0] == 1 && reduced.cop[0] == DS_BLEND_ADD && reduced.aop[0] == DS_BLEND_ADD,
                      "reduction enables addition");
                for (u32 sample = 0; sample < 289; sample++) {
                    float a = (float)(sample / 17) / 16, b = (float)(sample % 17) / 16;
                    float source[4] = {0.25f, 0.75f, 0.5f, a}, destination[4] = {b * 0.75f, b * 0.25f, b * 0.5f, b};
                    if (premultiplied)
                        for (u32 c = 0; c < 3; c++) source[c] *= a;
                    float answer[4];
                    nvn_advanced_blend_reference(modes[mode], (int)premultiplied, overlap, source, destination, answer);
                    for (u32 c = 0; c < 4; c++) {
                        u8 sf = c == 3 ? reduced.sa[0] : reduced.sc[0], df = c == 3 ? reduced.da[0] : reduced.dc[0];
                        float result = source[c] * factor(sf, source, destination, c) +
                                       destination[c] * factor(df, source, destination, c);
                        check(fabsf(result - answer[c]) < 0.000001f,
                              "reduction agrees with independent coverage equation");
                    }
                }
                check(!nvn_blend_advanced(&reduced, 1, state), "advanced mode is supported only on target zero");
                state = nvn_set_field(state, NVN_BLEND_ADV_NPM, 0);
                check(!nvn_blend_advanced(&reduced, 0, state), "non-normalized destination is explicitly unsupported");
            }
    DsBlend sentinel;
    memset(&sentinel, 0xa5, sizeof sentinel);
    DsBlend original = sentinel;
    u64 unsupported =
        nvn_set_field(nvn_set_field(0, NVN_BLEND_ADV, NVN_BLEND_ADVANCED_MODE_OVERLAY), NVN_BLEND_ADV_NPM, 1);
    check(!nvn_blend_advanced(&sentinel, 0, unsupported) && !memcmp(&sentinel, &original, sizeof original),
          "artistic modes remain unsupported and preserve state");
    printf("nvn advanced blend: %d checks, %d failures\n", checks, failures);
    return failures != 0;
}
