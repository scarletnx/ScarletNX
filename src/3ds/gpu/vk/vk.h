/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_CTR_VK_H
#define SNX_CTR_VK_H
#include "3ds/memory/ctr_memory.h"
#include "common/gpu/vk/ring.h"
#include "common/gpu/vk/vk_common.h"
#include "common/gpu/vk/window.h"
#include <stdbool.h>
#include <stdint.h>
/* The 3DS's one Vulkan device, which the PICA renderer, the transfer engine and the presenter share (device.c).
   The board's RAM regions (FCRAM, VRAM, DSP RAM, the New 3DS's extra RAM) are the host memory the guest runs in, a
   buffer of the device's each: the host's memory imported (VK_EXT_external_memory_host), or, where the driver will
   not import it (RADV takes no shared memory), the device's own memory, host-visible and cached, whose dma-buf the RAM
   moves into (VK_EXT_external_memory_dma_buf, snx_ctr_memory_adopt), every guest view of a page a map of it. On a GPU
   that shares the system's memory, vertex and index data, textures, render targets' load and store, transfers and the
   shown framebuffers are read and written there in place, and nothing of them is copied by the CPU. A discrete GPU
   would read and write each of those bytes across the bus, every time: it works on a copy of the RAM in its own
   memory instead, which the GPU keeps in step with the RAM by page (render.c): what the CPU wrote is copied in before
   the GPU reads it, and what the GPU wrote back when the CPU touches it. Work is recorded into a command buffer
   until it is submitted, a few submissions in flight at once, each with a serial: nothing waits for the GPU but what
   needs its results. The guest's CPU finds them through write tracking (memory/ctr_memory.h): the pages the GPU writes,
   or keeps newer in its render targets or its copy, trap until they are in memory, and the GSP's interrupts come when
   the submission is done. */
#define SNX_CTR_VK_SLOTS 4
typedef struct {
    VkBuffer buffer;       /* the RAM itself */
    VkDeviceMemory memory; /* its import; VK_NULL_HANDLE when the RAM is the device's memory (SnxCtrVk's ram_memory) */
    /* the GPU's copy of it in the device's own memory, which its work reads and writes (snx_ctr_vk_work);
       VK_NULL_HANDLE when the work reads and writes the RAM in place */
    VkBuffer copy;
    VkDeviceMemory copy_memory;
    uint32_t base, size; /* the physical range it holds */
} SnxCtrVkRam;
typedef struct {
    SnxVkWindow host;   /* the window (none headless), the instance and the surface */
    SDL_Window* second; /* the bottom screen's window when the screens are apart; NULL otherwise */
    VkSurfaceKHR second_surface;
    VkPhysicalDevice phys;
    VkDevice device;
    VkQueue queue;
    uint32_t family;
    VkPhysicalDeviceProperties properties;
    VkPhysicalDeviceMemoryProperties memory_properties;
    bool index8, logic_op; /* VK_EXT_index_type_uint8; the logicOp feature */
    bool clip_distance;    /* shaderClipDistance */
    bool preserve_infnan;  /* shaderSignedZeroInfNanPreserveFloat32: the generated shaders keep infinities and NaNs */
    bool import_host;      /* VK_EXT_external_memory_host */
    bool export_dmabuf;    /* VK_KHR_external_memory_fd and VK_EXT_external_memory_dma_buf */
    SnxCtrVkRam ram[8];
    unsigned ram_count;
    /* the board's RAM as the device's memory, each region's buffer bound at the region's offset in the RAM;
       VK_NULL_HANDLE when the regions are imported */
    VkDeviceMemory ram_memory;
    uint32_t ram_type;    /* its memory type */
    SnxCtrMemory* memory; /* the board's, whose RAM the renderer tracks */
    /* the submissions in flight, SNX_CTR_VK_SLOTS of them: each recording's command buffer, fence and descriptor
       sets, free again once its fence passed and its serial is completed */
    SnxVkRing ring;
} SnxCtrVk;
/* the device, once snx_ctr_vk_init made it; NULL before */
SnxCtrVk* snx_ctr_vk(void);
/* the device's work is on a copy of the RAM (the RAM's buffers have their copy). snx_ctr_vk_init chooses by the
   Switch's setting vk_memory (g_settings.guest_memory), with its meaning: 0 in place on a GPU that shares the system's
   memory (vk_common.h's SNX_VK_UMA) and on the copy on any other, 1 the copy, 2 in place */
bool snx_ctr_vk_copies(void);
/* the 3D render scale of a screen of these framebuffer sizes: the CTR top or bottom resolution setting, as far as
   the device's largest image takes (1 before a device, or with none) */
uint8_t snx_ctr_vk_target_scale(unsigned width, unsigned height);
/* The device and the window (unless headless or none can be made), with memory's RAM regions its buffers: imported,
   or, when the driver refuses that, the RAM moved into the device's memory (which says so, and why, on the standard
   error). Before the guest runs, as the move copies the RAM. False, with the reason in error, when the host has no
   Vulkan device that can: Vulkan 1.3, 8-bit storage buffers, dynamic rendering, and VK_EXT_external_memory_host or
   VK_EXT_external_memory_dma_buf, one of which takes the RAM */
bool snx_ctr_vk_init(SnxCtrMemory* memory, char* error, size_t error_size);
void snx_ctr_vk_free(void);
/* why the last snx_ctr_vk_init failed, without the device's name, which its error adds ("; device <name>, driver
   <name> <info>") once one was chosen; "" when it made the device. A run that ends for want of the device ends with
   it, and a report's signature takes that line: one cause on any GPU is one signature */
const char* snx_ctr_vk_error(void);
/* A RAM region's import that failed (snx_ctr_vk_init): the step it stopped at, the driver's answer and what the
   driver was asked */
typedef enum {
    SNX_CTR_VK_IMPORT_ALIGNMENT, /* the host address or the size is not a multiple of the alignment */
    SNX_CTR_VK_IMPORT_POINTER,   /* vkGetMemoryHostPointerPropertiesEXT */
    SNX_CTR_VK_IMPORT_BUFFER,    /* vkCreateBuffer */
    SNX_CTR_VK_IMPORT_TYPE,      /* no host-coherent memory type is both the pointer's and the buffer's */
    SNX_CTR_VK_IMPORT_ALLOCATE,  /* vkAllocateMemory with the host pointer */
    SNX_CTR_VK_IMPORT_BIND       /* vkBindBufferMemory */
} SnxCtrVkImportStep;
typedef struct {
    SnxCtrVkImportStep step;
    VkResult result;                      /* the call's (ALIGNMENT and TYPE make none) */
    uint32_t base, size;                  /* the region's physical range */
    uint64_t host;                        /* where its bytes are in the host's memory */
    uint64_t alignment;                   /* the driver's minImportedHostPointerAlignment, 4096 when it gives none */
    uint32_t pointer_types, buffer_types; /* the memory types the host pointer and the buffer can be in */
    uint32_t type;                        /* the one allocated (ALLOCATE and BIND) */
} SnxCtrVkImport;
/* the error a failed import gives, with the region's name */
void snx_ctr_vk_import_error(const SnxCtrVkImport* import, const char* region, char* error, size_t error_size);
/* The board's RAM as the device's memory, after the import was refused (snx_ctr_vk_init): the step that stopped it,
   the driver's answer and what was asked */
typedef enum {
    SNX_CTR_VK_DEVICE_RAM_EXTENSIONS, /* no VK_KHR_external_memory_fd and VK_EXT_external_memory_dma_buf */
    SNX_CTR_VK_DEVICE_RAM_EXPORT,     /* the RAM's buffers export no dma-buf, or one only in a dedicated allocation */
    SNX_CTR_VK_DEVICE_RAM_BUFFER,     /* vkCreateBuffer */
    SNX_CTR_VK_DEVICE_RAM_ALIGNMENT,  /* a region's offset in the RAM is not a multiple of its buffer's alignment */
    SNX_CTR_VK_DEVICE_RAM_TYPE,       /* no host-visible, coherent, cached memory type every buffer takes */
    SNX_CTR_VK_DEVICE_RAM_ALLOCATE,   /* vkAllocateMemory, exportable as a dma-buf */
    SNX_CTR_VK_DEVICE_RAM_BIND,       /* vkBindBufferMemory */
    SNX_CTR_VK_DEVICE_RAM_EXPORT_FD,  /* vkGetMemoryFdKHR */
    SNX_CTR_VK_DEVICE_RAM_VIEWS       /* snx_ctr_memory_adopt: the guest's views do not map the dma-buf */
} SnxCtrVkDeviceRamStep;
typedef struct {
    SnxCtrVkDeviceRamStep step;
    VkResult result;            /* the call's (BUFFER, ALLOCATE, BIND, EXPORT_FD) */
    uint64_t size;              /* the board's RAM, its regions one after another */
    uint32_t features;          /* the dma-buf's VkExternalMemoryFeatureFlags for the buffers (EXPORT) */
    uint32_t region;            /* the region's index in the board's profile (BUFFER, ALIGNMENT, BIND) */
    uint64_t offset, alignment; /* its offset in the RAM, and its buffer's alignment (ALIGNMENT, BIND) */
    uint32_t buffer_types;      /* the memory types every buffer takes (TYPE) */
    uint32_t visible_types;     /* of those, the host-visible and coherent ones, none cached (TYPE) */
    uint64_t allocation;        /* the bytes allocated: the RAM and its buffers' last padding (ALLOCATE on) */
    uint32_t type;              /* the memory type allocated (ALLOCATE on) */
    int error;                  /* the system's reason the views did not map, an errno (VIEWS) */
} SnxCtrVkDeviceRam;
/* The line the device says of where the GPU's work is (snx_ctr_vk_init), without its "3ds gpu: " and its newline:
   vk_memory's value asked, which the device's memory model (vk_common.h) took. With refused not NULL, the copy asked
   for and refused for that reason, the run taking vk_memory=2; else on the copy (copied, bytes of memory type type),
   or the RAM in place */
void snx_ctr_vk_path_line(int asked, SnxVkMemoryModel model, bool copied, const char* refused, uint64_t bytes,
                          uint32_t type, char* line, size_t line_size);
/* the error the device's memory gives when it cannot hold the RAM, with the region's name (BUFFER, ALIGNMENT, BIND) */
void snx_ctr_vk_device_ram_error(const SnxCtrVkDeviceRam* ram, const char* region, char* error, size_t error_size);
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
/* the buffer the GPU's work reads and writes of a RAM region: its copy, or the RAM in place */
static inline VkBuffer snx_ctr_vk_work(const SnxCtrVkRam* ram) { return ram->copy ? ram->copy : ram->buffer; }
/* A storage-buffer window of the RAM (its copy, where the work is on one) holding [pa, pa + size): the descriptor's
   offset at the alignment the device wants, and *inside where pa falls in it. False when no RAM region holds the span
   whole, or the window that holds it is past the device's largest storage range */
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
/* the barrier before a compute dispatch: the rendering pass a draw may have left open ends first */
void snx_ctr_vk_compute_barrier(VkCommandBuffer cb);
#endif
