/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Where the constant buffers go through windows of the staging ring (gpu/vk/cb_windows.h), and the line that says
   why not: on with descriptor sets or buffers alike, off where a line of the extension bans holds (lavapipe's), with
   heaps, and where a limit falls short, each line with the device's value and what the windows need; vk_cb_windows's
   Off and On, over a ban too */
#include "nx/gpu/vk/cb_windows.h"
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

/* the push bytes the windows' offsets end at (shader.h's SH_PUSH_BYTES_WINDOWS) */
#define WINDOW_PUSH 224u
#define AUTO        SNX_VK_CB_WINDOWS_AUTO

static int said(const char* line, const char* want) {
    if (!strcmp(line, want)) return 1;
    printf("said: %s\nwant: %s\n", line, want);
    return 0;
}

int main(void) {
    char line[1024];
    /* the ban as vk_ban_why gives it to pipeline.c */
    static const char ban[] =
        "lavapipe hands its linear rasterizer the push constants of every stage a draw binds as one buffer (#2461; extension_bans.txt:9)";
    /* RADV on an RX 6700 XT (256 push bytes, 4 GiB uniform ranges): on, with descriptor buffers too (the device
       carries no flag of them: they never turned the windows off) */
    const SnxVkCbWindowDevice radv = {256u, 4294967295u, 18u, 0, NULL};
    CHECK(snx_vk_cb_windows_say(line, sizeof line, &radv, WINDOW_PUSH, AUTO) == 1, "RADV binds windows");
    CHECK(said(line, "vulkan: constant buffers bound as windows of the staging ring, their offsets pushed"),
          "and says so");
    /* Turnip on an Adreno 610 (256 push bytes, 64 KiB ranges, a window's whole): on */
    const SnxVkCbWindowDevice turnip = {256u, 65536u, 18u, 0, NULL};
    CHECK(snx_vk_cb_windows_say(line, sizeof line, &turnip, WINDOW_PUSH, AUTO) == 1, "Turnip binds windows");
    /* lavapipe, whose limits take them and whose ban holds: off, the line with the ban as the table says it */
    const SnxVkCbWindowDevice lavapipe = {256u, 65536u, 18u, 0, ban};
    CHECK(snx_vk_cb_windows_say(line, sizeof line, &lavapipe, WINDOW_PUSH, AUTO) == 0,
          "lavapipe binds them one by one");
    CHECK(
        said(
            line,
            "vulkan: constant buffers bound one by one: lavapipe hands its linear rasterizer the push constants of every stage a draw binds as one buffer (#2461; extension_bans.txt:9)"),
        "and says the ban, its source and its line");
    /* heaps: off, as their set addresses take the bytes from 128 */
    const SnxVkCbWindowDevice heaps = {256u, 65536u, 18u, 1, NULL};
    CHECK(snx_vk_cb_windows_say(line, sizeof line, &heaps, WINDOW_PUSH, AUTO) == 0, "heaps bind them one by one");
    CHECK(said(line, "vulkan: constant buffers bound one by one: descriptor heaps push their sets' addresses from byte "
                     "128 (VK_DESCRIPTOR_HEAP_PUSH_OFFSET), where the windows' offsets go"),
          "and says why");
    /* 128 push bytes, Vulkan's least: off, the line with the 128 and the 224 */
    const SnxVkCbWindowDevice small = {128u, 65536u, 18u, 0, NULL};
    CHECK(snx_vk_cb_windows_say(line, sizeof line, &small, WINDOW_PUSH, AUTO) == 0, "128 push bytes take no windows");
    CHECK(said(line, "vulkan: constant buffers bound one by one: the GPU pushes 128 bytes of constants, and the "
                     "windows' offsets end at byte 224"),
          "and says the 128 and the 224");
    /* 16 KiB uniform ranges, Vulkan's least: off, the line with the 16384 and the 65536 */
    const SnxVkCbWindowDevice ranges = {256u, 16384u, 18u, 0, NULL};
    CHECK(snx_vk_cb_windows_say(line, sizeof line, &ranges, WINDOW_PUSH, AUTO) == 0, "16 KiB ranges take no windows");
    CHECK(said(line, "vulkan: constant buffers bound one by one: the GPU binds uniform ranges of 16384 bytes, and a "
                     "window takes 65536"),
          "and says the 16384 and the 65536");
    /* no constant buffer bound as a uniform buffer: nothing to window */
    const SnxVkCbWindowDevice storage = {256u, 65536u, 0u, 0, NULL};
    CHECK(snx_vk_cb_windows_say(line, sizeof line, &storage, WINDOW_PUSH, AUTO) == 0, "storage banks take no windows");
    CHECK(said(line, "vulkan: constant buffers bound one by one: none of the 18 is bound as a uniform buffer"),
          "and says none is");
    /* the limits come first: lavapipe with 128 push bytes says the bytes */
    const SnxVkCbWindowDevice lavapipe_small = {128u, 65536u, 18u, 0, ban};
    CHECK(snx_vk_cb_windows_say(line, sizeof line, &lavapipe_small, WINDOW_PUSH, AUTO) == 0 &&
              strstr(line, "pushes 128 bytes"),
          "a limit that falls short is said before the driver");
    /* vk_cb_windows=0: off where they would be on, the line with the setting's value */
    CHECK(snx_vk_cb_windows_say(line, sizeof line, &radv, WINDOW_PUSH, SNX_VK_CB_WINDOWS_OFF) == 0,
          "Off binds them one by one");
    CHECK(said(line, "vulkan: constant buffers bound one by one: vk_cb_windows=0 (Off)"), "and says vk_cb_windows=0");
    CHECK(snx_vk_cb_windows_say(line, sizeof line, &lavapipe, WINDOW_PUSH, SNX_VK_CB_WINDOWS_OFF) == 0 &&
              said(line, "vulkan: constant buffers bound one by one: vk_cb_windows=0 (Off)"),
          "Off on lavapipe says the setting, not the driver");
    /* vk_cb_windows=1: on lavapipe too, the line with the setting's value and the ban it goes over */
    CHECK(snx_vk_cb_windows_say(line, sizeof line, &lavapipe, WINDOW_PUSH, SNX_VK_CB_WINDOWS_ON) == 1,
          "On binds windows on lavapipe");
    CHECK(
        said(
            line,
            "vulkan: constant buffers bound as windows of the staging ring, their offsets pushed, as "
            "vk_cb_windows=1 (On) asks over the ban: lavapipe hands its linear rasterizer the push constants of every stage a draw binds as one buffer (#2461; extension_bans.txt:9)"),
        "and says the 1 asked and the ban");
    CHECK(snx_vk_cb_windows_say(line, sizeof line, &radv, WINDOW_PUSH, SNX_VK_CB_WINDOWS_ON) == 1 &&
              said(line, "vulkan: constant buffers bound as windows of the staging ring, their offsets pushed"),
          "On elsewhere is Automatic's line");
    /* vk_cb_windows=1 where the device can't take them: one by one, the line with the 1 asked and why not */
    CHECK(snx_vk_cb_windows_say(line, sizeof line, &heaps, WINDOW_PUSH, SNX_VK_CB_WINDOWS_ON) == 0,
          "On with heaps binds them one by one");
    CHECK(said(line, "vulkan: constant buffers bound one by one, where vk_cb_windows=1 (On) asks for windows: "
                     "descriptor heaps push their sets' addresses from byte 128 (VK_DESCRIPTOR_HEAP_PUSH_OFFSET), "
                     "where the windows' offsets go"),
          "and says the 1 asked and why not");
    CHECK(snx_vk_cb_windows_say(line, sizeof line, &small, WINDOW_PUSH, SNX_VK_CB_WINDOWS_ON) == 0 &&
              strstr(line, "vk_cb_windows=1 (On) asks for windows: the GPU pushes 128 bytes"),
          "On with 128 push bytes says the 1 and the 128");
    CHECK(snx_vk_cb_windows_say(line, 0, &lavapipe, WINDOW_PUSH, SNX_VK_CB_WINDOWS_ON) == 1,
          "On on lavapipe with no line");
    /* no room for the line: the answer alone */
    CHECK(snx_vk_cb_windows_say(line, 0, &radv, WINDOW_PUSH, AUTO) == 1, "the answer with no line");
    if (g_failed) {
        printf("FAIL vk_cb_windows: %d checks\n", g_failed);
        return 1;
    }
    puts("PASS vk_cb_windows: on with sets or buffers, off where a ban holds, with heaps and short limits, "
         "vk_cb_windows Off and On (over a ban too), each line with its values");
    return 0;
}
