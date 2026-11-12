/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The renderer's device memory. It asks the driver for memory in blocks, and hands out ranges of them to buffers,
   images and sparse bindings. A request names where its memory must be (DM_*), in the renderer's own terms: the
   memory type follows from that and from the device, by one rule (type_pick in devmem.c), not from a library's
   reading of buffer usage flags.

   A block is a power of two of bytes, 64 MiB at most, and its free space is kept in 1 KiB units as runs that join
   when they touch (space.h): a request costs its own size in units, and takes the shortest run that holds it.
   Buffers and images share the blocks where the device's granularity between the two is a unit or less. A request
   of more than a quarter of a block, and one whose resource the driver wants alone, gets device memory of its own,
   which goes back to the driver when the request does. A block with nothing in it goes back too, but for one of its
   kind, which the next request finds without a call to the driver.

   A heap that the driver has no more of takes no more blocks: the request then goes to the next memory type that
   suits it, as a driver's own overflow does, and not to a small allocation of its own in the full heap.

   One lock guards it all: any thread may ask. The driver's allocate and free go through the ledger, which times
   them (ledger.h) */
#ifndef SNX_VK_DEVMEM_H
#define SNX_VK_DEVMEM_H
#include "common/cpu/aarch64/cpu.h"
#include "volk.h"

typedef u32 DevMem; /* an allocation, 0 for none */

enum {
    DM_DEVICE, /* the GPU's own memory: the CPU does not touch it */
    DM_HOST,   /* system memory that the CPU fills in order and does not read: not cached where the device has such
                  a type, which a driver can map write-combined */
    DM_WINDOW, /* as DM_HOST, but in the GPU's memory where the CPU can map that (the BAR window of a discrete GPU) */
    DM_CACHED  /* system memory that the CPU reads, or writes here and there: cached where the device has such a type */
};
typedef struct {
    u32 at;                     /* DM_* */
    VkMemoryPropertyFlags need; /* properties the memory type must have besides */
    u8 mapped;                  /* DevMemInfo.ptr is wanted (not for DM_DEVICE) */
    u8 own;                     /* device memory of its own, whatever its size */
    u8 image;                   /* vk_devmem_alloc: the memory is for an image */
} DevMemWant;
typedef struct {
    VkDeviceMemory memory;
    u64 offset, size; /* where it is in memory, and the bytes asked for */
    void* ptr;        /* the CPU's address of it, NULL when not mapped */
    u32 type;         /* its memory type */
    u32 tag;          /* vk_devmem_tag's, 0 at first */
} DevMemInfo;
typedef struct {
    u64 usage, budget;                /* the heap's use by every process and this one's budget, as the driver says
                                         (VK_EXT_memory_budget), else this process's blocks and 4/5 of the heap */
    u64 block_bytes, allocated_bytes; /* the device memory held here, and what of it is handed out */
    u32 blocks, allocations;
} DevMemBudget;

/* after the device is made. bda: buffers may take device addresses */
int vk_devmem_init(int bda);
const VkPhysicalDeviceMemoryProperties* vk_devmem_props(void);

/* memory for req. *info, when not NULL, says where it is */
VkResult vk_devmem_alloc(const VkMemoryRequirements* req, const DevMemWant* want, DevMem* mem, DevMemInfo* info);
void vk_devmem_free(DevMem mem); /* 0 is ignored */
/* a buffer or an image with its memory bound, and both gone again (either may be none) */
VkResult vk_devmem_buffer(const VkBufferCreateInfo* bci, const DevMemWant* want, VkBuffer* buf, DevMem* mem,
                          DevMemInfo* info);
void vk_devmem_buffer_destroy(VkBuffer buf, DevMem mem);
VkResult vk_devmem_image(const VkImageCreateInfo* ici, const DevMemWant* want, VkImage* img, DevMem* mem);
void vk_devmem_image_destroy(VkImage img, DevMem mem);

void vk_devmem_info(DevMem mem, DevMemInfo* info);
void vk_devmem_tag(DevMem mem, u32 tag); /* a number of the caller's, kept with the allocation (the ledger's owner) */
/* the CPU wrote [off, off + size) of a mapped allocation, or reads it next (size VK_WHOLE_SIZE: to its end): what
   memory that is not coherent needs, and nothing for memory that is */
VkResult vk_devmem_flush(DevMem mem, u64 off, u64 size);
VkResult vk_devmem_invalidate(DevMem mem, u64 off, u64 size);

/* each heap's state, VK_MAX_MEMORY_HEAPS of them. The driver's numbers are asked again at vk_devmem_frame (each
   present) and after every 30 requests, and follow this process's blocks in between */
void vk_devmem_budgets(DevMemBudget* b);
void vk_devmem_frame(void);
/* a line of the log for each memory type and kind with memory in it: its blocks, what of them is handed out, the
   largest piece free in one, and the allocations with memory of their own (the ledger's report) */
void vk_devmem_log(void);

#endif
