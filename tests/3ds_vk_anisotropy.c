/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The 3DS samplers' anisotropy for the Switch's anisotropy setting (gpu/vk/device.c's snx_ctr_vk_anisotropy): none
   below 2 or without the device's feature, else the value asked held to the device's most, and the line the run says
   of it, with the value asked and the one in effect */
#include "3ds/gpu/vk/vk.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int checks;
static void expect(int asked, bool feature, float most, float want, const char* line) {
    char got[256];
    float value = snx_ctr_vk_anisotropy(asked, feature, most, got, sizeof got);
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
    printf("3ds anisotropy: %d checks\n", checks);
    return 0;
}
