/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A console's samplers' anisotropy for the Switch's anisotropy setting (common/gpu/vk/vk_common.c's
   snx_vk_anisotropy, the 3DS's and the Vita's): none
   below 2 or without the device's feature, else the value asked held to the device's most, and the line the run says
   of it, with the value asked and the one in effect */
#include "common/gpu/vk/vk_common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int checks;
static void expect(int asked, bool feature, float most, float want, const char* line) {
    char got[256];
    float value = snx_vk_anisotropy(asked, feature, most, got, sizeof got);
    ++checks;
    if (value != want || strcmp(got, line)) {
        fprintf(stderr, "anisotropy=%d, feature %d, most %g: %g \"%s\", expected %g \"%s\"\n", asked, feature,
                (double)most, (double)value, got, (double)want, line);
        exit(1);
    }
}
int main(void) {
    expect(0, true, 16, 0, "");
    expect(1, true, 16, 0, "");
    expect(8, true, 16, 8,
           "anisotropy=8: a mipmapped texture the game filters linearly is filtered anisotropically, at most 8x");
    expect(16, true, 4, 4,
           "anisotropy=16: a mipmapped texture the game filters linearly is filtered anisotropically, at most 4x, the "
           "device's most");
    expect(16, false, 16, 0,
           "anisotropy=16 asks for anisotropic filtering, which the device has none of: textures are filtered as the "
           "game asks");
    /* the resolution setting as a renderer of whole scales takes it, what is not as asked said with both */
    {
        static const struct {
            float asked;
            int scale;
            const char* said;
        } scales[] = {{1.0f, 1, ""},
                      {2.0f, 2, ""},
                      {2.5f, 2, "resolution=2.5: drawn at 2x, the whole scale from 1 to 8 at or under it"},
                      {0.5f, 1, "resolution=0.5: drawn at 1x, the whole scale from 1 to 8 at or under it"},
                      {16.0f, 8, "resolution=16: drawn at 8x, the whole scale from 1 to 8 at or under it"}};
        for (size_t i = 0; i < sizeof scales / sizeof *scales; ++i) {
            char line[160];
            int scale = snx_vk_whole_scale(scales[i].asked, 8, line, sizeof line);
            ++checks;
            if (scale != scales[i].scale || strcmp(line, scales[i].said)) {
                fprintf(stderr, "vk anisotropy: resolution=%g gave %d, \"%s\"\n", (double)scales[i].asked, scale, line);
                return 1;
            }
        }
    }
    printf("vk anisotropy: %d checks\n", checks);
    return 0;
}
