/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* What the console layers' renderers share of VK_EXT_shader_object (issue #1454): a draw binds compiled shaders and
   sets the rest as dynamic state, so no pipeline is made for each combination of shaders and state.

   The driver's binaries of the shaders are kept between runs, in a file of their own for each driver
   (snx_vk_driver_path), as the pipeline cache is: a shader whose SPIR-V the file holds a binary of is made from the
   binary, which costs no compile. The file starts with the device's shaderBinaryUUID and shaderBinaryVersion, and a
   file of another device or driver version is not read. A binary the driver still refuses is made again from its
   SPIR-V. */
#ifndef SNX_VK_SHADER_OBJECT_H
#define SNX_VK_SHADER_OBJECT_H
#include "common/gpu/vk/vk_common.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* the binaries of one device's shaders, by the key of their create info (snx_vk_shader_key). Not locked: one
   thread uses it at a time */
typedef struct {
    uint8_t uuid[VK_UUID_SIZE];
    uint32_t version;
    struct SnxVkShaderBinary* entries;
    size_t count, capacity;
    uint32_t* slots; /* an open-addressed table of entry indices plus 1, 0 for an empty slot */
    size_t slot_count;
    unsigned char* blob; /* every binary, one after the other */
    size_t blob_size, blob_capacity;
    bool dirty; /* a binary added since the last load or save */
} SnxVkShaderBinaries;

/* the key a shader's binary is kept by: what of its create info a binary depends on and a later run gives the same,
   its stage and next stage, its code, its count of set layouts and its push constant ranges (the set layouts are
   handles, which a run makes anew: a renderer keeps them the same between runs) */
uint64_t snx_vk_shader_key(const VkShaderCreateInfoEXT* info);
/* b, empty, for the device of uuid and version (VkPhysicalDeviceShaderObjectPropertiesEXT) */
void snx_vk_shader_binaries_init(SnxVkShaderBinaries* b, const uint8_t uuid[VK_UUID_SIZE], uint32_t version);
/* the binaries the file at path holds for b's device added to b: their bytes in *loaded, 0 for a file that is not
   there, is cut, or is another device's or driver version's */
void snx_vk_shader_binaries_load(SnxVkShaderBinaries* b, const char* path, size_t* loaded);
/* the binary of key, NULL for none */
const void* snx_vk_shader_binary(const SnxVkShaderBinaries* b, uint64_t key, size_t* size);
/* the driver's binary of shader kept by key, unless b has one already. False when the driver gives none or the host is
   out of memory, which only costs a compile in the next run */
bool snx_vk_shader_binaries_keep(SnxVkShaderBinaries* b, VkDevice dev, uint64_t key, VkShaderEXT shader);
/* b written to path as snx_vk_pipeline_cache_save writes, through a temporary file. 0 when written, its bytes in *saved;
   -1 with the reason in why */
int snx_vk_shader_binaries_save(SnxVkShaderBinaries* b, const char* path, size_t* saved, char* why, size_t n);
void snx_vk_shader_binaries_free(SnxVkShaderBinaries* b);

/* A shader object of info, whose code is SPIR-V: from b's binary of key where b has one the driver takes (*compiled
   false), else from the SPIR-V (*compiled true), and its binary then kept in b. b may be NULL (a worker thread's
   call), and the caller keeps the binary later (snx_vk_shader_binaries_keep) */
VkResult snx_vk_shader_object(VkDevice dev, SnxVkShaderBinaries* b, uint64_t key, const VkShaderCreateInfoEXT* info,
                              VkShaderEXT* out, bool* compiled);

/* words, a SPIR-V module of one entry point, with SignedZeroInfNanPreserve for 32-bit floats declared
   (SPV_KHR_float_controls), as a generated shader declares it where the device keeps infinities and NaNs: glslang
   declares it for no GLSL. A copy the caller frees, *count its words; NULL when the host is out of memory or the module
   has no entry point */
uint32_t* snx_vk_spirv_preserve_infnan(const uint32_t* words, size_t count, size_t* out_count);

/* The dynamic state a draw with shader objects must set that the console layers draw only one way: filled polygons
   of one sample, all of its samples written, no alpha to coverage, depth clamp, depth bias or depth bounds, no
   primitive restart, and the rasterizer on. Set once in a command buffer that binds no graphics pipeline, as nothing
   else there changes it */
void snx_vk_shader_object_defaults(VkCommandBuffer cb);
#endif
