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

int main(void) {
    check_off();
    check_dynamic();
    printf("%s: %u checks, %u failed\n", g_test.failures ? "FAIL" : "PASS", g_test.checks, g_test.failures);
    return g_test.failures != 0;
}
