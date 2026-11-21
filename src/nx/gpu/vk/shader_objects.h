/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Draws with shader objects (VK_EXT_shader_object, issue #1454), where the device has them (g_vk.has_shader_objects).
   A draw's key (PipeKey) gives a set of shader objects in place of a pipeline: one object for each stage's program,
   made once and shared by every key that runs the program with the same set layouts and fragment specialization. The
   draw binds the set, and the state that a pipeline held is set as dynamic state from the key at the bind
   (vk_so_bind). render.c sets the rest as it sets it for pipelines, the pre-rasterization state (pr_dynamic) too.

   An object is made from the program's SPIR-V, which the translator's module keeps here (vk_so_keep_spirv): a draw
   whose modules have no SPIR-V kept, such as the runtime's own clears, is made as a pipeline. So is a key that has
   state that is no dynamic state for shader objects, or that this path does not set yet (vk_so_takes). With
   descriptor heaps each object maps its stage's set as a pipeline's stage does, in place of the set layouts.

   The driver's binaries of the objects are kept between runs in a file of their own for each driver, beside the
   pipeline cache: an object whose binary the file has is made from it, with no compile */
#ifndef SNX_VK_SHADER_OBJECTS_H
#define SNX_VK_SHADER_OBJECTS_H
#include "nx/gpu/vk/vk.h"

/* the states that the device's extensions make a draw with shader objects set, recorded when the renderer starts */
void vk_so_init(void);
/* the binaries of earlier runs, read from dir's file of this driver (tag, snx_vk_driver_tag), once the shader file's
   key is settled (vk_pipeline_file_key) */
void vk_so_open(const char* dir, const char* tag);
/* the binaries written to the file when objects were made since the last write (vk_pipeline_save's lock held) */
void vk_so_save(void);
/* a copy of module m's SPIR-V kept, for the shader objects of its program. Any thread */
void vk_so_keep_spirv(VkShaderModule m, const u32* words, size_t count);
/* m's SPIR-V forgotten, before m is destroyed: a module that the driver gives its handle again is another program */
void vk_so_forget_spirv(VkShaderModule m);
/* whether a draw of key k, whose fragment module is fs (k->fs or the empty one), draws with shader objects */
int vk_so_takes(const PipeKey* k, VkShaderModule fs);
/* the objects of k's stages (vertex, tessellation control and evaluation, geometry, fragment: VK_NULL_HANDLE for a
   stage it has not; a mesh draw's mesh and task in the first two), each made or waited for while another thread makes
   it. 0 when the device refuses one: the key is made as a pipeline then. A pipeline worker's call */
int vk_so_make(const PipeKey* k, VkShaderModule fs, VkShaderEXT out[5]);
/* the objects bound to cb's graphics stages, and the state of k that a pipeline held set as dynamic state. The
   recording thread's call, or the GPU thread's without it */
void vk_so_bind(VkCommandBuffer cb, const PipeKey* k, const VkShaderEXT sh[5]);
/* the objects made (*made), those of them from a binary of an earlier run (*from_binary), their ms, and the MB of
   SPIR-V the modules keep for them */
void vk_so_stats(u64* made, u64* from_binary, u64* ms, u64* spirv_mb);

#endif
