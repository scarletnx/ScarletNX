/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* vk_debug_hide_commands on the 3DS's device (gpu/vk/device.c), as on a Switch game's: the check of each extension's
   commands takes vkCreateShadersEXT as missing, as from a driver that lists VK_EXT_shader_object without it, and the
   device is made again without the extension, which the log says with the device and the command. A device that has
   no shader objects skips (77). The log is what the device's start writes on stderr, read back from a file */
#include "3ds/gpu/vk/vk.h"
#include "nx/runtime/ini.h"
#include "snx_settings.h"
#include "support/3ds_vk_path.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHECK(c)                                                                                                       \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            fprintf(stderr, "3ds_vk_hidden_commands:%d: %s\n", __LINE__, #c);                                          \
            return 1;                                                                                                  \
        }                                                                                                              \
    } while (0)

/* snx_ctr_vk_init, with what it writes on stderr into log (size bytes at most) */
static bool init_said(SnxCtrMemory* m, char* log, size_t size, char* why, size_t why_size) {
    FILE* file = tmpfile();
    if (!file) return false;
    fflush(stderr);
    int saved = dup(2);
    dup2(fileno(file), 2);
    bool made = snx_ctr_vk_init(m, true, why, why_size);
    fflush(stderr);
    dup2(saved, 2);
    close(saved);
    rewind(file);
    size_t n = fread(log, 1, size - 1, file);
    log[n] = 0;
    fclose(file);
    fputs(log, stdout);
    return made;
}

int main(int argc, char** argv) {
    SnxCtrModel model = test_ctr_model(&argc, argv);
    SnxCtrMemory* m = snx_ctr_memory_new(model);
    static char log[65536];
    char why[512];
    CHECK(m);
    if (!snx_ctr_vk_init(m, true, why, sizeof why)) {
        printf("SKIP no Vulkan device (%s)\n", why);
        snx_ctr_memory_free(m);
        return 77;
    }
    bool objects = snx_ctr_vk()->shader_object;
    snx_ctr_vk_free();
    if (!objects) {
        puts("SKIP hidden commands: the device has no shader objects");
        snx_ctr_memory_free(m);
        return 77;
    }
    ini_read(ini_find(g_debug_ini, "vk_debug_hide_commands"), "vkCreateShadersEXT");
    CHECK(init_said(m, log, sizeof log, why, sizeof why));
    const SnxCtrVk* v = snx_ctr_vk();
    CHECK(v && !v->shader_object);
    for (uint32_t i = 0; i < v->extension_count; i++) CHECK(strcmp(v->extensions[i], "VK_EXT_shader_object"));
    CHECK(strstr(log, "vulkan: vk_debug_hide_commands=vkCreateShadersEXT hides 1 command of the extensions"));
    CHECK(strstr(log,
                 "lists VK_EXT_shader_object without vkCreateShadersEXT, which it requires (vk_debug_hide_commands "
                 "hides it from the check): the device is made again without it"));
    const char* off = strstr(log, "vulkan: VK_EXT_shader_object is off on ");
    CHECK(off && strstr(off, "its driver lists it without vkCreateShadersEXT, which it requires"));
    CHECK(strstr(log, "vulkan: vk_debug_hide_commands=vkCreateShadersEXT hides 0 commands of the extensions"));
    snx_ctr_vk_free();
    snx_ctr_memory_free(m);
    puts("PASS 3DS device made again without VK_EXT_shader_object");
    return 0;
}
