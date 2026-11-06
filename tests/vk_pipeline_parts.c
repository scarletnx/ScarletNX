/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Graphics pipeline libraries (gpu/vk/pipeline_parts.c), with no GPU: the choice of the library path from the setting
   and what a stand-in device has, each condition that turns it off named in the log line, and the dynamic
   pre-rasterization state taken only where the device has its member and the renderer uses its feature */
#include "nx/gpu/vk/pipeline_parts.h"
#include <stdio.h>
#include <string.h>

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

/* the RTX 2080 of this machine: everything */
static VkGplDevice full(void) {
    VkGplDevice d = {.setting = 1, .has_ext = 1, .library = 1, .fast_link = 1, .independent_interpolation = 1,
                     .polygon = 1, .depth_clamp = 1, .provoking = 1, .conservative_mode = 1, .conservative_size = 1,
                     .patch = 1, .has_provoking_last = 1, .has_conservative = 1};
    return d;
}

static void check_off(void) {
    static const struct {
        size_t offset;
        const char* says;
    } cases[] = {
        {offsetof(VkGplDevice, setting), "pipeline_libraries=0"},
        {offsetof(VkGplDevice, has_ext), "no VK_EXT_graphics_pipeline_library"},
        {offsetof(VkGplDevice, library), "without graphicsPipelineLibrary"},
        {offsetof(VkGplDevice, fast_link), "without fast linking"},
        {offsetof(VkGplDevice, independent_interpolation), "no independent interpolation"},
    };
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        VkGplDevice d = full();
        int* field = (int*)((char*)&d + cases[i].offset);
        *field = 0;
        char line[512];
        u32 dyn = ~0u;
        int on = vk_gpl_choose(&d, &dyn, line, sizeof line);
        CHECK(!on, cases[i].says);
        CHECK(dyn == 0, "no dynamic state with the libraries off");
        CHECK(strstr(line, "libraries off") && strstr(line, cases[i].says), line);
    }
}

static void check_dynamic(void) {
    char line[512];
    u32 dyn = 0;
    VkGplDevice d = full();
    CHECK(vk_gpl_choose(&d, &dyn, line, sizeof line), "on with everything");
    CHECK(dyn == (VK_DYN_PR_POLYGON | VK_DYN_PR_DEPTH_CLAMP | VK_DYN_PR_PROVOKING | VK_DYN_PR_CONSERVATIVE |
                  VK_DYN_PR_PATCH),
          "every pre-rasterization state dynamic");
    CHECK(strstr(line, "libraries on") && strstr(line, "polygon mode, depth clamp, provoking vertex, conservative "
                                                       "rasterization, patch control points"),
          line);
    /* each member alone off leaves its state in the part's key, and the rest dynamic */
    static const struct {
        size_t offset;
        u32 bit;
    } members[] = {
        {offsetof(VkGplDevice, polygon), VK_DYN_PR_POLYGON},
        {offsetof(VkGplDevice, depth_clamp), VK_DYN_PR_DEPTH_CLAMP},
        {offsetof(VkGplDevice, provoking), VK_DYN_PR_PROVOKING},
        {offsetof(VkGplDevice, has_provoking_last), VK_DYN_PR_PROVOKING},
        {offsetof(VkGplDevice, conservative_mode), VK_DYN_PR_CONSERVATIVE},
        {offsetof(VkGplDevice, conservative_size), VK_DYN_PR_CONSERVATIVE},
        {offsetof(VkGplDevice, has_conservative), VK_DYN_PR_CONSERVATIVE},
        {offsetof(VkGplDevice, patch), VK_DYN_PR_PATCH},
    };
    for (size_t i = 0; i < sizeof members / sizeof *members; i++) {
        d = full();
        *(int*)((char*)&d + members[i].offset) = 0;
        CHECK(vk_gpl_choose(&d, &dyn, line, sizeof line), "on without one member");
        CHECK(!(dyn & members[i].bit), "the member's state static");
        CHECK((dyn | members[i].bit) == 31u, "the other states dynamic");
    }
    /* a device without EDS2 and EDS3: libraries with every pre-rasterization state static */
    d = full();
    d.polygon = d.depth_clamp = d.provoking = d.conservative_mode = d.conservative_size = d.patch = 0;
    CHECK(vk_gpl_choose(&d, &dyn, line, sizeof line) && dyn == 0, "on, nothing dynamic");
    CHECK(strstr(line, "dynamic pre-rasterization state: none") != NULL, line);
}

/* the states a whole pipeline listed before pipeline libraries, in their order (pipe_create): a change of the order is a
   new pipeline cache entry for every pipeline */
static const VkDynamicState g_before[24] = {
    VK_DYNAMIC_STATE_VIEWPORT_WITH_COUNT, VK_DYNAMIC_STATE_SCISSOR_WITH_COUNT, VK_DYNAMIC_STATE_CULL_MODE,
    VK_DYNAMIC_STATE_FRONT_FACE, VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY, VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE,
    VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE, VK_DYNAMIC_STATE_DEPTH_COMPARE_OP, VK_DYNAMIC_STATE_DEPTH_BOUNDS_TEST_ENABLE,
    VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE, VK_DYNAMIC_STATE_STENCIL_OP, VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE,
    VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE, VK_DYNAMIC_STATE_RASTERIZER_DISCARD_ENABLE,
    VK_DYNAMIC_STATE_LINE_WIDTH, VK_DYNAMIC_STATE_DEPTH_BIAS, VK_DYNAMIC_STATE_BLEND_CONSTANTS,
    VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_STENCIL_REFERENCE,
    VK_DYNAMIC_STATE_DEPTH_BOUNDS, VK_DYNAMIC_STATE_ATTACHMENT_FEEDBACK_LOOP_ENABLE_EXT,
    VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE, VK_DYNAMIC_STATE_VERTEX_INPUT_EXT};

static int has(const VkDynamicState* s, u32 n, VkDynamicState want) {
    for (u32 i = 0; i < n; i++)
        if (s[i] == want) return 1;
    return 0;
}

static void check_states(void) {
    VkDynamicState s[VK_PIPE_DYN_MAX];
    /* without libraries: the 21 of every pipeline, then the three the device and the key add, as before */
    for (u32 m = 0; m < 8; m++) {
        PipeDynIn in = {.feedback_dynamic = (int)(m & 1), .stride = (int)(m >> 1 & 1), .vertex_input = (int)(m >> 2)};
        u32 n = vk_pipe_dynamic(VK_PART_ALL, &in, s), at = 0, ok = 1;
        for (u32 i = 0; i < 24; i++) {
            if ((i == 21 && !in.feedback_dynamic) || (i == 22 && !in.stride) || (i == 23 && !in.vertex_input)) continue;
            ok &= at < n && s[at++] == g_before[i];
        }
        CHECK(ok && at == n, "a whole pipeline's states as before");
    }
    /* each pre-rasterization bit adds its states after those; patch control points only with tessellation */
    static const struct {
        u32 bit;
        VkDynamicState state;
    } pr[] = {{VK_DYN_PR_POLYGON, VK_DYNAMIC_STATE_POLYGON_MODE_EXT},
              {VK_DYN_PR_DEPTH_CLAMP, VK_DYNAMIC_STATE_DEPTH_CLAMP_ENABLE_EXT},
              {VK_DYN_PR_PROVOKING, VK_DYNAMIC_STATE_PROVOKING_VERTEX_MODE_EXT},
              {VK_DYN_PR_CONSERVATIVE, VK_DYNAMIC_STATE_CONSERVATIVE_RASTERIZATION_MODE_EXT},
              {VK_DYN_PR_CONSERVATIVE, VK_DYNAMIC_STATE_EXTRA_PRIMITIVE_OVERESTIMATION_SIZE_EXT},
              {VK_DYN_PR_PATCH, VK_DYNAMIC_STATE_PATCH_CONTROL_POINTS_EXT}};
    for (u32 i = 0; i < sizeof pr / sizeof *pr; i++)
        for (int tess = 0; tess < 2; tess++) {
            PipeDynIn in = {.dyn_pr = pr[i].bit, .tess = tess};
            u32 n = vk_pipe_dynamic(VK_PART_ALL, &in, s);
            int want = pr[i].bit != VK_DYN_PR_PATCH || tess;
            CHECK(has(s, n, pr[i].state) == want, "a pre-rasterization state with its bit");
            CHECK(n == 21u + (want ? (pr[i].bit == VK_DYN_PR_CONSERVATIVE ? 2u : 1u) : 0u), "only that state added");
            CHECK(!want || s[n - 1] == pr[i].state || pr[i].bit == VK_DYN_PR_CONSERVATIVE, "after the others");
        }
    /* the four parts together list what the whole pipeline does, each state once, but the feedback loop's, which the
       fragment shader and fragment output parts both hold */
    PipeDynIn all = {.dyn_pr = 31, .feedback_dynamic = 1, .stride = 1, .vertex_input = 1, .tess = 1};
    VkDynamicState whole[VK_PIPE_DYN_MAX], part[4][VK_PIPE_DYN_MAX];
    u32 nwhole = vk_pipe_dynamic(VK_PART_ALL, &all, whole), np[4];
    for (u32 p = 0; p < 4; p++) np[p] = vk_pipe_dynamic(1u << p, &all, part[p]);
    for (u32 i = 0; i < nwhole; i++) {
        u32 in = 0;
        for (u32 p = 0; p < 4; p++) in += (u32)has(part[p], np[p], whole[i]);
        CHECK(in == (whole[i] == VK_DYNAMIC_STATE_ATTACHMENT_FEEDBACK_LOOP_ENABLE_EXT ? 2u : 1u), "in its part");
    }
    CHECK(np[0] + np[1] + np[2] + np[3] == nwhole + 1, "no state in a part that the whole pipeline lacks");
    /* the parts' states, by the specification's subsets */
    CHECK(has(part[0], np[0], VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY) && has(part[0], np[0], VK_DYNAMIC_STATE_VERTEX_INPUT_EXT),
          "the vertex input part holds the topology and the layout");
    CHECK(has(part[1], np[1], VK_DYNAMIC_STATE_VIEWPORT_WITH_COUNT) &&
              has(part[1], np[1], VK_DYNAMIC_STATE_POLYGON_MODE_EXT) &&
              has(part[1], np[1], VK_DYNAMIC_STATE_PATCH_CONTROL_POINTS_EXT),
          "the pre-rasterization part holds the viewports and the rasterization state");
    CHECK(has(part[2], np[2], VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE) && has(part[2], np[2], VK_DYNAMIC_STATE_STENCIL_OP),
          "the fragment shader part holds the depth and stencil tests");
    CHECK(np[3] == 2 && has(part[3], np[3], VK_DYNAMIC_STATE_BLEND_CONSTANTS), "the output part holds the blend constants");
}

int main(void) {
    check_off();
    check_dynamic();
    check_states();
    printf("%s: %u checks, %u failed\n", g_test.failures ? "FAIL" : "PASS", g_test.checks, g_test.failures);
    return g_test.failures != 0;
}
