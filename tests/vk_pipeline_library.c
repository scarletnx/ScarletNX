/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Pipelines linked from parts (gpu/vk/pipeline_library.c), with no GPU: the device's choice against stand-in devices
   (without the extensions, without the feature, without fast linking, without independent interpolation, banned by
   extension_bans.txt, which takes it out of the device's list, and with all of it), each log line naming the device,
   the driver, its version and what decided; and the part each dynamic state belongs to, with every state a pipeline
   lists in one part, so that the four libraries of a pipeline list them all between them */
#include "nx/gpu/vk/pipeline_library.h"
#include <stdio.h>
#include <string.h>
#include "volk.h"

static struct {
    unsigned checks, failures;
} g_test;
#define CHECK(c, what)                                                                                                 \
    do {                                                                                                               \
        g_test.checks++;                                                                                               \
        if (!(c)) {                                                                                                    \
            g_test.failures++;                                                                                         \
            printf("FAIL line %d: %s\n", __LINE__, what);                                                              \
        }                                                                                                              \
    } while (0)

static void check_choices(void) {
    static const struct {
        int has, fast, independent;
        const char* ban;
        int cpu, on;
        const char* why; /* the words that say what decided */
    } cases[] = {
        {0, 1, 1, NULL, 0, 0, "has no VK_EXT_graphics_pipeline_library"},
        {1, 1, 1, NULL, 0, 0, "without its graphicsPipelineLibrary feature"},
        {2, 0, 1, NULL, 0, 0, "no graphicsPipelineLibraryFastLinking"},
        {2, 1, 0, NULL, 0, 0, "no graphicsPipelineLibraryIndependentInterpolationDecoration"},
        {0, 1, 1, "a stand-in driver before 1.2.3 (a stand-in source)", 0, 0,
         "banned on Stand-in GPU (RADV, Mesa 26.2.6, driver version 26.2.6): a stand-in driver before 1.2.3 (a stand-in "
         "source)"},
        {2, 1, 1, NULL, 0, 1, "the pipeline made whole replaces it in the background"},
        /* a CPU device draws with the link as fast: it stays */
        {2, 1, 1, NULL, 1, 1, "the link stays, as fast to draw with on a CPU device"},
    };
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        VkGplDevice d = {
            "Stand-in GPU", "RADV",        "Mesa 26.2.6",        VK_DRIVER_ID_MESA_RADV, VK_MAKE_VERSION(26, 2, 6),
            cases[i].has,   cases[i].fast, cases[i].independent, cases[i].ban,           cases[i].cpu};
        char line[1024], what[1200];
        int on = vk_gpl_choose(&d, line, sizeof line);
        snprintf(what, sizeof what, "case %zu: %s", i, line);
        CHECK(on == cases[i].on, what);
        CHECK(strstr(line, "Stand-in GPU") && strstr(line, "RADV") && strstr(line, "Mesa 26.2.6") &&
                  strstr(line, "driver version 26.2.6"),
              what);
        CHECK(strstr(line, cases[i].why) != NULL, what);
        CHECK((strstr(line, "made whole, a draw's first waiting for all of it") != NULL) == !on, what);
    }
}

static void check_parts(void) {
    static const u32 parts[4] = {VK_GRAPHICS_PIPELINE_LIBRARY_VERTEX_INPUT_INTERFACE_BIT_EXT,
                                 VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT,
                                 VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT,
                                 VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_OUTPUT_INTERFACE_BIT_EXT};
    u32 n, seen = 0;
    const u32* all = vk_pipe_dynamic_states(&n);
    /* every pipeline's states, then the ones pipe_create adds where they apply */
    u32 states[32];
    memcpy(states, all, n * sizeof *all);
    CHECK(n == 21, "the states every pipeline has");
    states[n++] = VK_DYNAMIC_STATE_ATTACHMENT_FEEDBACK_LOOP_ENABLE_EXT;
    states[n++] = VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE;
    states[n++] = VK_DYNAMIC_STATE_VERTEX_INPUT_EXT;
    for (u32 i = 0; i < n; i++) {
        u32 part = vk_gpl_dynamic_part(states[i]);
        char what[64];
        snprintf(what, sizeof what, "dynamic state %u in one part", states[i]);
        CHECK(part && !(part & (part - 1)), what); /* one part, never none or two */
        seen |= part;
    }
    for (int p = 0; p < 4; p++) CHECK(seen & parts[p], "each part has states of the renderer's");
    /* a few by name, as the extension's groups give them */
    CHECK(vk_gpl_dynamic_part(VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY) == parts[0], "the topology: vertex input");
    CHECK(vk_gpl_dynamic_part(VK_DYNAMIC_STATE_CULL_MODE) == parts[1], "the cull mode: before rasterization");
    CHECK(vk_gpl_dynamic_part(VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE) == parts[2], "the depth test: fragment program");
    CHECK(vk_gpl_dynamic_part(VK_DYNAMIC_STATE_BLEND_CONSTANTS) == parts[3], "the blend constants: fragment output");
    CHECK(vk_gpl_dynamic_part(VK_DYNAMIC_STATE_FRAGMENT_SHADING_RATE_KHR) == 0, "a state the table doesn't know");
}

int main(void) {
    check_choices();
    check_parts();
    printf("%s: %u checks, %u failed\n", g_test.failures ? "FAIL" : "PASS", g_test.checks, g_test.failures);
    return g_test.failures != 0;
}
