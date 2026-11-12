/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Whether a device takes the texture arrays (gpu/vk/texture_arrays.h) and the lines that say so: whole where the
   limits bind them (RADV), off where they do not (PanVK on Bifrost binds 128 samplers a stage) or the device can't
   index them per invocation, each line with the device's values and what the arrays need. The descriptor pools' line
   says the arrays they hold, or that the device takes none */
#include "nx/gpu/vk/texture_arrays.h"
#include <stdio.h>
#include <string.h>

static int g_failed;
#define CHECK(c, what)                                                                                                 \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            printf("FAIL line %d: %s\n", __LINE__, what);                                                              \
            g_failed++;                                                                                                \
        }                                                                                                              \
    } while (0)

int main(void) {
    char line[640];
    /* RADV on an RX 6700 XT: every limit far past what arrays of 256 ask, so they are whole and nothing is said */
    const SnxVkTextureLimits radv = {1048576u, 1048576u, 1048576u, 1048576u};
    CHECK(snx_vk_texture_array_fit(&radv, 64, 256) == 256, "RADV's limits take arrays of 256");
    CHECK(snx_vk_texture_array_say(line, sizeof line, 1, &radv, 64, 256) == 256 && !line[0],
          "and nothing is said of them");
    /* PanVK on Bifrost (panvk_shader.h: 128 samplers and 256 sampled images a stage, 65535 of each a set): arrays of
       256 beside 64 textures need 576, so they are off, and the line says the four values and the 576 */
    const SnxVkTextureLimits panvk = {128u, 256u, 65535u, 65535u};
    CHECK(snx_vk_texture_array_say(line, sizeof line, 1, &panvk, 64, 256) == 0, "PanVK on Bifrost takes no arrays");
    CHECK(strstr(line, "vulkan: texture arrays off: the GPU binds 128 samplers and 256 sampled images a stage, and "
                       "65535 and 65535 in the five stage sets, where the two arrays of 256 beside the 64 textures of "
                       "a stage need 576: a texture a program picks from a table of handles at run time can't be "
                       "bound") == line,
          "the line names the device's four values and the 576 the arrays need");
    if (!strstr(line, "need 576")) printf("said: %s\n", line);
    /* no per-invocation indexing: off, whatever the limits */
    CHECK(snx_vk_texture_array_say(line, sizeof line, 0, &radv, 64, 256) == 0 &&
              strstr(line, "vulkan: texture arrays off: the GPU has no shaderSampledImageArrayNonUniformIndexing") ==
                  line,
          "without non-uniform indexing the arrays are off, and the line names the feature");
    /* the pools' line */
    snx_vk_pool_arrays_text(line, sizeof line, 64, 256);
    CHECK(!strcmp(line, "64 of them with texture arrays of 256 elements"), "a frame's pools say their array sets");
    snx_vk_pool_arrays_text(line, sizeof line, 64, 0);
    CHECK(!strcmp(line, "none with texture arrays, which this GPU takes none of"),
          "a device without arrays: none, not the 64 sets a frame's pools keep for them");
    snx_vk_pool_arrays_text(line, sizeof line, 0, 256);
    CHECK(!strcmp(line, "none with texture arrays"), "the compute pools hold no array sets");
    if (g_failed) {
        printf("FAIL: %d checks failed\n", g_failed);
        return 1;
    }
    puts("PASS texture arrays: whole on RADV, off on PanVK's Bifrost and where none is indexed, each said");
    return 0;
}
