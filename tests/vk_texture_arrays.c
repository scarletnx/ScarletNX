/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The texture arrays' elements a device's limits leave (gpu/vk/texture_arrays.h) and the lines that say them: whole
   where the limits bind them (RADV), 32 where a stage binds 128 samplers (PanVK on Bifrost), none where not one
   element fits or the device can't index them per invocation; each line with the device's values and what was asked.
   The descriptor pools' line says the arrays they hold, or that the device takes none */
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
    /* PanVK on Bifrost (panvk_shader.h: 128 samplers and 256 sampled images a stage, 65535 of each a set): the
       samplers leave 128 - 64 = 64 for the two arrays */
    const SnxVkTextureLimits panvk = {128u, 256u, 65535u, 65535u};
    CHECK(snx_vk_texture_array_fit(&panvk, 64, 256) == 32, "PanVK on Bifrost takes arrays of 32");
    CHECK(snx_vk_texture_array_say(line, sizeof line, 1, &panvk, 64, 256) == 32, "and says 32");
    CHECK(strstr(line, "vulkan: texture arrays of 32 elements, not 256: the GPU binds 128 samplers and 256 sampled "
                       "images a stage, and 65535 and 65535 in the five stage sets, where arrays of 256 need 576 a "
                       "stage (64 textures and two arrays): a draw whose tables of handles name more than 32 textures "
                       "is not drawn") == line,
          "the line names the elements in effect, the 256 asked, the device's four values and the 576 needed");
    if (!strstr(line, "of 32 elements")) printf("said: %s\n", line);
    /* a set limit that binds before the stage's: 5 sets of 64 + 2 x 40 = 144 each */
    const SnxVkTextureLimits sets = {4096u, 4096u, 720u, 4096u};
    CHECK(snx_vk_texture_array_fit(&sets, 64, 256) == 40, "the five stage sets' samplers bind first: arrays of 40");
    /* fewer asked (a test's) where the limits bind more: what was asked, with nothing to say */
    CHECK(snx_vk_texture_array_say(line, sizeof line, 1, &radv, 64, 32) == 32 && !line[0],
          "32 asked of a device that binds 256 is 32, said by the pools' line alone");
    /* not one element: off, the line with the 66 a single element of each array needs */
    const SnxVkTextureLimits tight = {64u, 128u, 1024u, 1024u};
    CHECK(snx_vk_texture_array_say(line, sizeof line, 1, &tight, 64, 256) == 0, "64 samplers a stage take no array");
    CHECK(strstr(line, "vulkan: texture arrays off: the GPU binds 64 samplers and 128 sampled images a stage, and "
                       "1024 and 1024 in the five stage sets, where one element of each of the two arrays beside the "
                       "64 textures of a stage needs 66") == line,
          "the line says off, the device's values and the 66 one element needs");
    CHECK(strstr(line, "a texture a program picks from a table of handles at run time can't be bound") != NULL,
          "and what that costs");
    /* no per-invocation indexing: off, whatever the limits */
    CHECK(snx_vk_texture_array_say(line, sizeof line, 0, &radv, 64, 256) == 0 &&
              strstr(line, "vulkan: texture arrays off: the GPU has no shaderSampledImageArrayNonUniformIndexing") ==
                  line,
          "without non-uniform indexing the arrays are off, and the line names the feature");
    /* the pools' line */
    snx_vk_pool_arrays_text(line, sizeof line, 64, 256);
    CHECK(!strcmp(line, "64 of them with texture arrays of 256 elements"), "a frame's pools say their array sets");
    snx_vk_pool_arrays_text(line, sizeof line, 64, 32);
    CHECK(!strcmp(line, "64 of them with texture arrays of 32 elements"), "and the elements in effect");
    snx_vk_pool_arrays_text(line, sizeof line, 64, 0);
    CHECK(!strcmp(line, "none with texture arrays, which this GPU takes none of"),
          "a device without arrays: none, not the 64 sets a frame's pools keep for them");
    snx_vk_pool_arrays_text(line, sizeof line, 0, 256);
    CHECK(!strcmp(line, "none with texture arrays"), "the compute pools hold no array sets");
    if (g_failed) {
        printf("FAIL: %d checks failed\n", g_failed);
        return 1;
    }
    puts("PASS texture arrays: whole on RADV, 32 on PanVK's Bifrost, off where none fits or none is indexed");
    return 0;
}
