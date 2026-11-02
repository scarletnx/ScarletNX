/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_CTR_VK_H
#define SNX_CTR_VK_H
#include "3ds/memory/ctr_memory.h"
#include "common/gpu/vk/ring.h"
#include "common/gpu/vk/window.h"
#include <stdbool.h>
#include <stdint.h>
/* The 3DS's one Vulkan device, which the PICA renderer, the transfer engine and the presenter share (device.c).
   The board's RAM regions (FCRAM, VRAM, DSP RAM, the New 3DS's extra RAM) are the host memory the guest runs in,
   imported into Vulkan as buffers (VK_EXT_external_memory_host): vertex and index data, textures, render targets'
   load and store, transfers and the shown framebuffers are read and written in place on the GPU, and nothing of them
   is copied by the CPU. Work is recorded into a command buffer until it is submitted, a few submissions in flight
   at once, each with a serial: nothing waits for the GPU but what needs its results. The guest's CPU finds them
   through write tracking (memory/ctr_memory.h): the pages the GPU writes, or keeps newer in its render targets, trap
   until they are in memory, and the GSP's interrupts come when the submission is done. */
#define SNX_CTR_VK_SLOTS 4
typedef struct {
    VkBuffer buffer;
    VkDeviceMemory memory;
    uint32_t base, size; /* the physical range it holds */
} SnxCtrVkRam;
typedef struct {
    SnxVkWindow host; /* the window (none headless), the instance and the surface */
    VkPhysicalDevice phys;
    VkDevice device;
    VkQueue queue;
    uint32_t family;
    VkPhysicalDeviceProperties properties;
    VkPhysicalDeviceMemoryProperties memory_properties;
    bool index8, logic_op; /* VK_EXT_index_type_uint8; the logicOp feature */
    bool clip_distance;    /* shaderClipDistance */
    float max_anisotropy;  /* the sampler anisotropy the device allows, 0 when the feature is off */
    VkPipelineCache pipeline_cache; /* the pipelines' cache, kept in the file snx_ctr_vk_cache_file names */
    bool shader_object; /* VK_EXT_shader_object with the dynamic state it draws with: no pipelines */
    bool preserve_infnan;  /* shaderSignedZeroInfNanPreserveFloat32: the generated shaders keep infinities and NaNs */
    SnxCtrVkRam ram[8];
    unsigned ram_count;
    SnxCtrMemory* memory; /* the board's, whose RAM the renderer tracks */
    /* the submissions in flight, SNX_CTR_VK_SLOTS of them: each recording's command buffer, fence and descriptor
       sets, free again once its fence passed and its serial is completed */
    SnxVkRing ring;
} SnxCtrVk;
/* the device, once snx_ctr_vk_init made it; NULL before */
SnxCtrVk* snx_ctr_vk(void);
/* The device and the window (unless headless or none can be made), with memory's RAM regions imported. False, with
   the reason in error, when the host has no Vulkan device that can: Vulkan 1.2, 8-bit storage buffers and
   VK_EXT_external_memory_host */
bool snx_ctr_vk_init(SnxCtrMemory* memory, char* error, size_t error_size);
/* The file the pipeline cache is read from when the device is made, and written to when it is freed (copied; call
   before snx_ctr_vk_init). Without one the driver's own cache is all there is. */
void snx_ctr_vk_cache_file(const char* path);
/* The file the renderer's compiled shader objects are read from when the renderer starts, and written to when it
   stops (copied; call before snx_ctr_vk_init). With it, a shader seen in an earlier run is created without
   compiling. */
void snx_ctr_vk_shader_cache_file(const char* path);
void snx_ctr_vk_free(void);
/* the open command buffer, begun if none was (in a slot free again, which may wait for its submission) */
VkCommandBuffer snx_ctr_vk_commands(void);
/* The recording submitted, without waiting: its serial (the last one's when nothing was recorded and there is no
   semaphore). wait and signal, when not null, are a swapchain's acquire and present semaphores */
uint64_t snx_ctr_vk_submit(void);
uint64_t snx_ctr_vk_submit_with(VkSemaphore wait, VkSemaphore signal);
/* the serial the open recording (or the next) will have */
uint64_t snx_ctr_vk_serial(void);
/* the submissions done so far, without waiting: their slots free again. The last serial done */
uint64_t snx_ctr_vk_poll(void);
/* until serial is done (submitting the recording first when serial is its), or timeout_ns passed (UINT64_MAX: no
   bound). True when it is done */
bool snx_ctr_vk_wait(uint64_t serial, uint64_t timeout_ns);
/* the recording submitted and waited for: everything the GPU wrote is in guest memory but what the render targets
   keep newer (write tracking lands it when touched) */
bool snx_ctr_vk_finish(void);
/* the slot of the open recording (or the next): per-submission resources are kept by it */
unsigned snx_ctr_vk_slot(void);
/* a descriptor set of the layout for the open recording, free again when its submission is done */
VkDescriptorSet snx_ctr_vk_set(VkDescriptorSetLayout layout);
/* the imported region that holds [pa, pa + size), and pa's offset into it; NULL when no RAM region holds it whole */
const SnxCtrVkRam* snx_ctr_vk_ram(uint32_t pa, uint32_t size, VkDeviceSize* offset);
/* A storage-buffer window of the RAM holding [pa, pa + size): the descriptor's offset at the alignment the device
   wants, and *inside where pa falls in it. False when no RAM region holds the span whole, or the window that holds it
   is past the device's largest storage range */
typedef struct {
    VkBuffer buffer;
    VkDeviceSize offset, range;
} SnxCtrVkWindow;
bool snx_ctr_vk_window(uint32_t pa, uint64_t size, SnxCtrVkWindow* window, uint32_t* inside);
/* a memory type of the bits with the properties, UINT32_MAX for none */
uint32_t snx_ctr_vk_memory_type(uint32_t bits, VkMemoryPropertyFlags properties);
/* a compute pipeline from SPIR-V words: bindings storage buffers (then images storage images) at set 0, push
   constants of push bytes. The layouts come back with it */
typedef struct {
    VkPipeline pipeline;
    VkPipelineLayout layout;
    VkDescriptorSetLayout set;
} SnxCtrVkCompute;
bool snx_ctr_vk_compute(const uint32_t* words, size_t count, unsigned buffers, unsigned images, uint32_t push,
                        SnxCtrVkCompute* out);
void snx_ctr_vk_compute_free(SnxCtrVkCompute* compute);
/* a buffer write the next commands may read or write, or one the host reads after snx_ctr_vk_finish */
void snx_ctr_vk_barrier(VkCommandBuffer cb);
#endif
