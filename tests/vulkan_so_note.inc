/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A shader object the driver makes (shader_objects.c stage_get) is the crash report's note while the driver compiles
   it, as a pipeline is (pipeline.c pipe_note): a driver that crashed compiling one named only its own offset, and the
   report said the thread noted nothing. vkCreateShadersEXT is wrapped here to read the note during the call; after it
   the note is gone. The object is the empty fragment program's, made from its SPIR-V as a draw's would be, with the
   pipeline workers quiet so that none makes one of its own meanwhile */
#include "nx/gpu/vk/internal_spv.h"
#include "nx/gpu/vk/shader_objects.h"
#include "common/platform/thread.h" /* plat_thread_note_get */
/* the note the wrapped call read, and the driver's call it wraps */
static struct {
    char seen[PLAT_THREAD_NOTE_SIZE];
    PFN_vkCreateShadersEXT create;
} g_so_note;
static VkResult VKAPI_CALL so_note_create(VkDevice dev, uint32_t n, const VkShaderCreateInfoEXT* info,
                                          const VkAllocationCallbacks* alloc, VkShaderEXT* out) {
    snprintf(g_so_note.seen, sizeof g_so_note.seen, "%s", plat_thread_note_get());
    return g_so_note.create(dev, n, info, alloc, out);
}
static int check_so_note(void) {
    if (!g_vk.has_shader_objects) {
        puts("shader object note: the device makes no shader objects, not checked");
        return 0;
    }
    const size_t words = snx_null_fs_spv_size / sizeof *snx_null_fs_spv;
    VkShaderModule fs = vk_module_spv(snx_null_fs_spv, words);
    CHECK(fs);
    vk_so_keep_spirv(fs, snx_null_fs_spv, words);
    PipeKey k = {0};
    VkShaderEXT made[5];
    g_so_note.seen[0] = 0;
    vk_pipeline_quiesce(); /* no worker makes an object of its own meanwhile */
    g_so_note.create = vkCreateShadersEXT;
    vkCreateShadersEXT = so_note_create;
    const int ok = vk_so_make(&k, fs, made);
    vkCreateShadersEXT = g_so_note.create;
    if (strcmp(g_so_note.seen, "making a shader object, fs (a runtime shader)") || plat_thread_note_get()[0]) {
        fprintf(stderr, "shader object note: \"%s\" while the driver made it, \"%s\" after\n", g_so_note.seen,
                plat_thread_note_get());
        return 1;
    }
    CHECK(ok && made[4]); /* kept by the shader objects' table, as a draw's is */
    puts("shader object note: the crash report names the object the driver makes, and the note goes after it");
    return 0;
}
