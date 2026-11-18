/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Where the constant buffers go through windows of the staging ring (gpu/vk/cb_windows.h), and the line that says
   why not: on with descriptor sets or buffers alike, off on lavapipe, with heaps, and where a limit falls short, each
   line with the device's value and what the windows need */
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

static int said(const char* line, const char* want) {
    if (!strcmp(line, want)) return 1;
    printf("said: %s\nwant: %s\n", line, want);
    return 0;
}

int main(void) {
    char line[512];
    /* RADV on an RX 6700 XT (256 push bytes, 4 GiB uniform ranges): on, with descriptor buffers too (the device
       carries no flag of them: they never turned the windows off) */
    const SnxVkCbWindowDevice radv = {256u, 4294967295u, 18u, 0, 0};
    CHECK(snx_vk_cb_windows_say(line, sizeof line, &radv, WINDOW_PUSH) == 1, "RADV binds windows");
    CHECK(said(line, "vulkan: constant buffers bound as windows of the staging ring, their offsets pushed"),
          "and says so");
    /* Turnip on an Adreno 610 (256 push bytes, 64 KiB ranges, a window's whole): on */
    const SnxVkCbWindowDevice turnip = {256u, 65536u, 18u, 0, 0};
    CHECK(snx_vk_cb_windows_say(line, sizeof line, &turnip, WINDOW_PUSH) == 1, "Turnip binds windows");
    /* lavapipe, whose limits take them: off, the line with the 224 bytes a stage, the 256 its rasterizer holds and
       the 448 of two stages */
    const SnxVkCbWindowDevice lavapipe = {256u, 65536u, 18u, 0, 1};
    CHECK(snx_vk_cb_windows_say(line, sizeof line, &lavapipe, WINDOW_PUSH) == 0, "lavapipe binds them one by one");
    CHECK(said(line, "vulkan: constant buffers bound one by one: lavapipe hands its linear rasterizer the push "
                     "constants of every stage a draw binds as one buffer, 224 bytes a stage with windows, and the "
                     "rasterizer copies them into a stack array that holds 256 bytes' worth: a vertex and a fragment "
                     "program's 448 smash its stack (issue #2461)"),
          "and says the 224, 256 and 448");
    /* heaps: off, as their set addresses take the bytes from 128 */
    const SnxVkCbWindowDevice heaps = {256u, 65536u, 18u, 1, 0};
    CHECK(snx_vk_cb_windows_say(line, sizeof line, &heaps, WINDOW_PUSH) == 0, "heaps bind them one by one");
    CHECK(said(line, "vulkan: constant buffers bound one by one: descriptor heaps push their sets' addresses from byte "
                     "128 (VK_DESCRIPTOR_HEAP_PUSH_OFFSET), where the windows' offsets go"),
          "and says why");
    /* 128 push bytes, Vulkan's least: off, the line with the 128 and the 224 */
    const SnxVkCbWindowDevice small = {128u, 65536u, 18u, 0, 0};
    CHECK(snx_vk_cb_windows_say(line, sizeof line, &small, WINDOW_PUSH) == 0, "128 push bytes take no windows");
    CHECK(said(line, "vulkan: constant buffers bound one by one: the GPU pushes 128 bytes of constants, and the "
                     "windows' offsets end at byte 224"),
          "and says the 128 and the 224");
    /* 16 KiB uniform ranges, Vulkan's least: off, the line with the 16384 and the 65536 */
    const SnxVkCbWindowDevice ranges = {256u, 16384u, 18u, 0, 0};
    CHECK(snx_vk_cb_windows_say(line, sizeof line, &ranges, WINDOW_PUSH) == 0, "16 KiB ranges take no windows");
    CHECK(said(line, "vulkan: constant buffers bound one by one: the GPU binds uniform ranges of 16384 bytes, and a "
                     "window takes 65536"),
          "and says the 16384 and the 65536");
    /* no constant buffer bound as a uniform buffer: nothing to window */
    const SnxVkCbWindowDevice storage = {256u, 65536u, 0u, 0, 0};
    CHECK(snx_vk_cb_windows_say(line, sizeof line, &storage, WINDOW_PUSH) == 0, "storage banks take no windows");
    CHECK(said(line, "vulkan: constant buffers bound one by one: none of the 18 is bound as a uniform buffer"),
          "and says none is");
    /* the limits come first: lavapipe with 128 push bytes says the bytes */
    const SnxVkCbWindowDevice lavapipe_small = {128u, 65536u, 18u, 0, 1};
    CHECK(snx_vk_cb_windows_say(line, sizeof line, &lavapipe_small, WINDOW_PUSH) == 0 &&
              strstr(line, "pushes 128 bytes"),
          "a limit that falls short is said before the driver");
    /* no room for the line: the answer alone */
    CHECK(snx_vk_cb_windows_say(line, 0, &radv, WINDOW_PUSH) == 1, "the answer with no line");
    if (g_failed) {
        printf("FAIL vk_cb_windows: %d checks\n", g_failed);
        return 1;
    }
    puts("PASS vk_cb_windows: on with sets or buffers, off on lavapipe, with heaps and short limits, each line with "
         "its values");
    return 0;
}
