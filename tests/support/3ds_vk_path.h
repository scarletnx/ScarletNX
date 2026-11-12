/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_TEST_3DS_VK_PATH_H
#define SNX_TEST_3DS_VK_PATH_H
/* Where a 3DS GPU test's work reads and writes the RAM, as the driver takes it: the Switch's setting vk_memory
   (gpu/vk/vk.h), given as --vk_memory=N among the test's arguments, so the suite runs each path on any device. None
   is the setting's default, the device's own choice. */
#include "3ds/gpu/vk/vk.h"
#include "nx/runtime/ini.h"
#include "snx_settings.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Each --vk_memory=N of argv read into the setting, through its row as the driver reads it, and taken out of argv: the
   value it holds then, or -1 when none was given */
static inline int test_ctr_vk_memory(int* argc, char** argv) {
    int given = -1, kept = 1;
    for (int i = 1; i < *argc; ++i) {
        if (strncmp(argv[i], "--vk_memory=", 12)) {
            argv[kept++] = argv[i];
            continue;
        }
        ini_read(ini_find(g_debug_ini, "vk_memory"), argv[i] + 12);
        given = g_settings.guest_memory;
    }
    *argc = kept;
    argv[kept] = NULL;
    return given;
}
/* Each --vk_ext_*=V of argv (a Vulkan extension's setting, as the driver takes it: --vk_ext_shader_object=off draws
   through pipelines on a device with shader objects) read into its setting and taken out of argv */
static inline void test_ctr_vk_extensions(int* argc, char** argv) {
    int kept = 1;
    for (int i = 1; i < *argc; ++i) {
        const char* eq = strchr(argv[i], '=');
        char key[96];
        size_t n = eq ? (size_t)(eq - argv[i]) : 0;
        const IniOpt* o = NULL;
        if (n > 2 && n - 2 < sizeof key && !strncmp(argv[i], "--vk_", 5)) {
            memcpy(key, argv[i] + 2, n - 2);
            key[n - 2] = 0;
            o = ini_find(g_vk_extensions_ini, key);
        }
        if (o)
            ini_read(o, eq + 1);
        else
            argv[kept++] = argv[i];
    }
    *argc = kept;
    argv[kept] = NULL;
}
/* once the device started: the path given (1 the copy, 2 in place) is the one it took, else the test ends, status 1;
   a device that can take the RAM neither way works on a copy in its host-visible memory, which answers for 1 and not
   for 2, where the test has nothing to check */
static inline void test_ctr_vk_memory_taken(int given) {
    if (given <= 0) return;
    if (snx_ctr_vk_shadow()) {
        if (given == 1) return;
        fprintf(stderr, "vk_memory=2 asks for the RAM in place, and the device keeps a copy of it in its host-visible "
                        "memory instead: no in-place path here\n");
        snx_ctr_vk_free();
        exit(77);
    }
    if (snx_ctr_vk_copies() == (given == 1)) return;
    fprintf(stderr, "vk_memory=%d, and the device's work is %s\n", given,
            snx_ctr_vk_copies() ? "on a copy of the RAM" : "on the RAM in place");
    exit(1);
}
#endif
