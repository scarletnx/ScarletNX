/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The byte ledger: what each part of the renderer holds. An owner has three counts of bytes: the device's memory
   (devmem.c's allocations in the heap the caches budget, vk_mem_heap), the host memory it gave the owner (allocations
   in the other heaps) and the process's heap (malloc, which the owner reports itself). vk_mem_track enters an
   allocation under its owner and vk_mem_untrack takes it out again, so the sum over the owners is what devmem.c has
   handed out; what differs is an allocation that some site did not enter, which the report names as untagged.

   The ledger also times the driver's own allocations (vkAllocateMemory, vkFreeMemory), which devmem.c calls when it
   needs a new block or lets one go: a block costs milliseconds on some drivers, on whichever thread asked. */
#ifndef SNX_VK_LEDGER_H
#define SNX_VK_LEDGER_H
#include "nx/gpu/vk/vk.h"

enum {
    VK_OWN_TEXTURE,     /* the texture cache's images, with their sparse blocks, BC7 forms and kept ASTC blocks */
    VK_OWN_TARGET,      /* the render targets */
    VK_OWN_IMAGE_OTHER, /* images of no cache: unscaled copies, resolves, captures */
    VK_OWN_BUFFER,      /* the buffer cache's own buffers (buffer.c) */
    VK_OWN_MIRROR,      /* the mirror's granules, masks and shadows (mirror.c) */
    VK_OWN_OWNED,       /* results the GPU keeps (owned.c) */
    VK_OWN_ARENA,       /* the compute arenas (compute.c) */
    VK_OWN_WRITELOG,    /* the write logs (writelog.c) */
    VK_OWN_RESULT,      /* the results of draws that write guest memory: their blocks in the heap (buffer.c) */
    VK_OWN_STAGING,     /* the staging and output rings' chunks (cmd.c) */
    VK_OWN_READBACK,    /* the readback buffer (cmd.c) */
    VK_OWN_ENCODER,     /* the ASTC and BC7 encoders' scratch and snapshots (astc.c) */
    VK_OWN_CONVERT,     /* the swizzle and conversion scratch (blocklinear.c, texconv.c) */
    VK_OWN_DESCRIPTORS, /* descriptor buffers and heaps */
    VK_OWN_QUERY,       /* query results and predicates (query.c) */
    VK_OWN_XFB,         /* transform feedback and the index buffers draws build (render.c) */
    VK_OWN_DMA,         /* the DMA engine's buffers (dma.c) */
    VK_OWN_UI,          /* the overlay's atlas, glyphs and frames (ui.c), the presented CPU image */
    VK_OWN_MISC,        /* small fixed buffers: the depth shadow, capture tiles */
    VK_OWN_N
};

/* an allocation of devmem.c entered under owner, and taken out again before it is freed (an allocation never entered
   is left alone). They are also the Tracy memory events of the allocation (SNX_TRACY builds), in a pool named for the owner */
void vk_mem_track(DevMem a, u32 owner);
void vk_mem_untrack(DevMem a);
/* an owner's bytes in the process's heap grew (or, negative, shrank) by delta */
void vk_ledger_heap(u32 owner, i64 delta);
/* the driver's allocate and free, timed: devmem.c calls them in place of the driver's */
VkResult VKAPI_CALL vk_ledger_allocate(VkDevice dev, const VkMemoryAllocateInfo* ai, const VkAllocationCallbacks* cb,
                                       VkDeviceMemory* mem);
void VKAPI_CALL vk_ledger_free(VkDevice dev, VkDeviceMemory mem, const VkAllocationCallbacks* cb);
/* one owner's bytes now, for a caller that budgets it: device, host, heap */
void vk_ledger_owner(u32 owner, u64* device, u64* host, u64* heap);
/* each owner that holds something, a few a line of the log (any thread): the bytes of devmem.c's under an owner. The
   out-of-memory report says them too, as a run may end before the first report */
u64 vk_ledger_owners(void);
/* the report (renderer.c, every 600 presents): each owner that holds something, what devmem.c holds beside them, and
   the driver's allocations since the last report */
void vk_ledger_log(void);
#endif
