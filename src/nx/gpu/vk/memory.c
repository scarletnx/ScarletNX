/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* How big one device memory allocation may be, and images bigger than that; the budget the caches keep within, and
   the clock their ages are measured with.

   Some drivers fail allocations past 256 MiB even where maxMemoryAllocationSize is larger (NVIDIA's on FreeBSD), and
   a game's textures can be bigger: a 2D ASTC texture of 11584x11584 decodes to 512 MiB of RGBA8, a 3D one of 512^3
   too. No allocation is made bigger than g_vk.alloc_cap: the staging and readback rings stop growing there (cmd.c),
   and an image whose memory is bigger is created with sparse binding and bound to several allocations, each at most
   the cap. Shaders and views see one ordinary image: only its memory is in pieces.

   The pieces are dedicated allocations, so the cap is also what the driver is asked for. They are bound on the
   graphics queue, and the bind is waited for on the host before the image is used: a bind is not ordered with
   command buffers submitted after it, on that queue or on the compute queue, without a semaphore each of them would
   have to wait on, and an image this big is made once.

   A 3D image may also have memory only where it is bound (sparse residency): a texture in a virtual pool the guest
   maps a 64 KiB tile at a time, whose holes read as zeros on the console as they do here (residencyNonResidentStrict).
   Its blocks of the device's granularity are bound and released one by one, each to an allocation of its own, and
   the memory it takes is that of its bound blocks alone: a 1664x448x1344 BC1 volume is 477 MB whole and 384 KiB with
   the 6 tiles a game maps of it. */
#include <stdlib.h>
#include "nx/runtime/options.h"
#include "nx/runtime/platform/log.h"
#include "nx/runtime/platform/plat.h"
#include "nx/gpu/vk/vk.h"

#define ALLOC_CAP_DEFAULT (256ull << 20)

/* the images in pieces, each with its allocations. One with memory only where it is bound has a page for each of its
   blocks, bx across and by down, of block texels (VK_NULL_HANDLE where none is bound) */
typedef struct {
    VkImage img;
    u32 n;
    u32 bx, by; /* 0: an image in pieces */
    VkExtent3D block, extent;
    VkMemoryRequirements req;
    u64 bound; /* the bytes of its bound blocks */
    VmaAllocation pages[];
} Pieces;
static Pieces** g_pieces;
static u32 g_npieces, g_cappieces;

/* VMA allocations as Tracy memory events. */
#if defined(SNX_TRACY)
void vk_mem_track(VmaAllocation a) {
    VmaAllocationInfo info;
    if (!a) return;
    vmaGetAllocationInfo(g_vk.vma, a, &info);
    SNX_MEM_ALLOC((const void*)a, info.size, "gpu");
}
void vk_mem_untrack(VmaAllocation a) {
    if (a) SNX_MEM_FREE((const void*)a, "gpu");
}
#endif

void vk_mem_init(int sparse_queue) {
    VkPhysicalDeviceMaintenance3Properties m3 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_3_PROPERTIES};
    VkPhysicalDeviceProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &m3};
    vkGetPhysicalDeviceProperties2(g_vk.phys, &p2);
    u64 cap = ALLOC_CAP_DEFAULT;
    /* a smaller cap for testing the pieces on a driver that takes anything (lavapipe) */
    if (g_settings.vulkan_allocation_cap) cap = (u64)g_settings.vulkan_allocation_cap << 20;
    if (m3.maxMemoryAllocationSize && cap > m3.maxMemoryAllocationSize) cap = m3.maxMemoryAllocationSize;
    g_vk.alloc_cap = cap;
    g_vk.sparse = sparse_queue && g_vk.feat.sparseBinding;
    g_vk.resident =
        g_vk.sparse && g_vk.feat.sparseResidencyImage3D && g_vk.props.sparseProperties.residencyNonResidentStrict;
    LOG("vulkan: allocations of at most %llu MiB, bigger images %s, a 3D texture with holes in its mapping %s\n",
        (unsigned long long)(cap >> 20),
        g_vk.sparse ? "in pieces (sparse binding)" : "in one allocation (no sparse binding)",
        g_vk.resident                       ? "with memory where it is mapped (sparse residency)"
        : !g_vk.feat.sparseResidencyImage3D ? "whole (no sparse residency of 3D images)"
                                            : "whole (sparse residency, but holes don't read as zeros)");
}

static int pieces_bind(VkImage img, const VkMemoryRequirements* req, Pieces* p, VkResult* res) {
    u64 chunk = g_vk.alloc_cap / req->alignment * req->alignment;
    if (!chunk) return 0;
    u32 n = (u32)((req->size + chunk - 1) / chunk);
    VkSparseMemoryBind* binds = calloc(n, sizeof *binds);
    if (!binds) return 0;
    int ok = 1;
    for (u32 i = 0; i < n && ok; i++) {
        u64 at = (u64)i * chunk, size = req->size - at < chunk ? req->size - at : chunk;
        VkMemoryRequirements piece = {.size = size, .alignment = req->alignment, .memoryTypeBits = req->memoryTypeBits};
        VmaAllocationCreateInfo aci = {.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT,
                                       .preferredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT};
        VmaAllocationInfo info;
        *res = vmaAllocateMemory(g_vk.vma, &piece, &aci, &p->pages[i], &info);
        if (!VK_OK(*res)) {
            ok = 0;
            break;
        }
        p->n++;
        vk_mem_track(p->pages[i]);
        binds[i] = (VkSparseMemoryBind){
            .resourceOffset = at, .size = size, .memory = info.deviceMemory, .memoryOffset = info.offset};
    }
    if (ok) {
        VkSparseImageOpaqueMemoryBindInfo opaque = {.image = img, .bindCount = n, .pBinds = binds};
        VkBindSparseInfo bi = {.sType = VK_STRUCTURE_TYPE_BIND_SPARSE_INFO, .imageOpaqueBindCount = 1,
                               .pImageOpaqueBinds = &opaque};
        VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        VkFence fence;
        ok = VK_OK(vkCreateFence(g_vk.dev, &fi, NULL, &fence));
        if (ok) {
            /* on the graphics queue, which the recording thread submits to meanwhile */
            vk_queue_lock(g_vk.gfx);
            ok = VK_OK(vkQueueBindSparse(g_vk.gfx, 1, &bi, fence));
            vk_queue_unlock(g_vk.gfx);
            ok = ok && VK_OK(vkWaitForFences(g_vk.dev, 1, &fence, VK_TRUE, UINT64_MAX));
            vkDestroyFence(g_vk.dev, fence, NULL);
        }
    }
    free(binds);
    return ok;
}

static void pieces_free(Pieces* p) {
    for (u32 i = 0; i < p->n; i++) {
        vk_mem_untrack(p->pages[i]);
        vmaFreeMemory(g_vk.vma, p->pages[i]);
    }
    free(p);
}

/* room in the list for one more image */
static int pieces_room(void) {
    if (g_npieces < g_cappieces) return 1;
    u32 cap = g_cappieces ? g_cappieces * 2 : 8;
    Pieces** grown = realloc(g_pieces, cap * sizeof *grown);
    if (!grown) return 0;
    g_pieces = grown;
    g_cappieces = cap;
    return 1;
}
static Pieces* pieces_of(VkImage img) {
    for (u32 i = 0; i < g_npieces; i++)
        if (g_pieces[i]->img == img) return g_pieces[i];
    return NULL;
}

/* an image whose memory is more than one allocation may be: bound to pieces of at most g_vk.alloc_cap */
static int image_in_pieces(const VkImageCreateInfo* ii, VkImage* img, VkResult* res) {
    VkImageCreateInfo si = *ii;
    si.flags |= VK_IMAGE_CREATE_SPARSE_BINDING_BIT;
    *res = vkCreateImage(g_vk.dev, &si, NULL, img);
    if (!VK_OK(*res)) return 0;
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(g_vk.dev, *img, &req);
    u64 chunk = req.alignment ? g_vk.alloc_cap / req.alignment * req.alignment : 0;
    u32 n = chunk ? (u32)((req.size + chunk - 1) / chunk) : 0;
    Pieces* p = n ? calloc(1, sizeof *p + n * sizeof p->pages[0]) : NULL;
    if (!p || !pieces_room() || !pieces_bind(*img, &req, p, res)) {
        WARN("vulkan: cannot bind a %llu MiB image in %u pieces of %llu MiB\n", (unsigned long long)(req.size >> 20), n,
             (unsigned long long)(chunk >> 20));
        vkDestroyImage(g_vk.dev, *img, NULL);
        *img = VK_NULL_HANDLE;
        if (p) pieces_free(p);
        return 0;
    }
    p->img = *img;
    g_pieces[g_npieces++] = p;
    LOG_LIMIT(8, "vulkan: a %ux%ux%u image of %llu MiB, bound in %u pieces\n", ii->extent.width, ii->extent.height,
              ii->extent.depth * ii->arrayLayers, (unsigned long long)(req.size >> 20), n);
    return 1;
}

int vk_image_create(const VkImageCreateInfo* ii, VkImage* img, VmaAllocation* mem) {
    VkResult res;
    return vk_image_create_r(ii, img, mem, &res);
}
int vk_image_create_r(const VkImageCreateInfo* ii, VkImage* img, VmaAllocation* mem, VkResult* res) {
    *res = VK_ERROR_UNKNOWN;
    *img = VK_NULL_HANDLE;
    *mem = VK_NULL_HANDLE;
    VkDeviceImageMemoryRequirements q = {.sType = VK_STRUCTURE_TYPE_DEVICE_IMAGE_MEMORY_REQUIREMENTS,
                                         .pCreateInfo = ii};
    VkMemoryRequirements2 req = {.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2};
    vkGetDeviceImageMemoryRequirements(g_vk.dev, &q, &req);
    if (req.memoryRequirements.size > g_vk.alloc_cap) {
        if (g_vk.sparse) return image_in_pieces(ii, img, res);
        /* without sparse binding the one allocation is the only way: the driver may still take it */
        LOG_LIMIT(4, "vulkan: a %llu MiB image in one allocation, past the %llu MiB cap\n",
                  (unsigned long long)(req.memoryRequirements.size >> 20), (unsigned long long)(g_vk.alloc_cap >> 20));
    }
    VmaAllocationCreateInfo aci = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    *res = vmaCreateImage(g_vk.vma, ii, &aci, img, mem, NULL);
    if (VK_OK(*res)) vk_mem_track(*mem);
    return VK_OK(*res);
}

void vk_image_destroy(VkImage img, VmaAllocation mem) {
    if (mem || !img) {
        vk_mem_untrack(mem);
        vmaDestroyImage(g_vk.vma, img, mem);
        return;
    }
    for (u32 i = 0; i < g_npieces; i++)
        if (g_pieces[i]->img == img) {
            vkDestroyImage(g_vk.dev, img, NULL);
            pieces_free(g_pieces[i]);
            g_pieces[i] = g_pieces[--g_npieces];
            return;
        }
    vkDestroyImage(g_vk.dev, img, NULL); /* made without memory */
}

/* ------------------------------------------------------------ memory only where it is bound */
int vk_resident_block(VkFormat f, VkImageUsageFlags usage, VkExtent3D* block) {
    VkSparseImageFormatProperties p[2];
    u32 n = 2;
    if (!g_vk.resident) return 0;
    vkGetPhysicalDeviceSparseImageFormatProperties(g_vk.phys, f, VK_IMAGE_TYPE_3D, VK_SAMPLE_COUNT_1_BIT, usage,
                                                   VK_IMAGE_TILING_OPTIMAL, &n, p);
    /* one aspect, color: a metadata aspect would be bound apart */
    if (n != 1 || p[0].aspectMask != VK_IMAGE_ASPECT_COLOR_BIT) return 0;
    *block = p[0].imageGranularity;
    return 1;
}

int vk_image_create_resident(const VkImageCreateInfo* ii, const VkExtent3D* block, VkImage* img) {
    VkImageCreateInfo si = *ii;
    VkMemoryRequirements req;
    VkSparseImageMemoryRequirements sr[2] = {0};
    u32 nsr = 2;
    *img = VK_NULL_HANDLE;
    if (!g_vk.resident || ii->imageType != VK_IMAGE_TYPE_3D || ii->mipLevels != 1 || ii->arrayLayers != 1) return 0;
    si.flags |= VK_IMAGE_CREATE_SPARSE_BINDING_BIT | VK_IMAGE_CREATE_SPARSE_RESIDENCY_BIT;
    if (!VK_OK(vkCreateImage(g_vk.dev, &si, NULL, img))) return 0;
    vkGetImageMemoryRequirements(g_vk.dev, *img, &req);
    vkGetImageSparseMemoryRequirements(g_vk.dev, *img, &nsr, sr);
    /* its one level bound in blocks, none of it in a mip tail (which takes an opaque bind of its own) */
    const VkExtent3D g = sr[0].formatProperties.imageGranularity;
    u64 bx = g.width ? (ii->extent.width + g.width - 1) / g.width : 0,
        by = g.height ? (ii->extent.height + g.height - 1) / g.height : 0,
        bz = g.depth ? (ii->extent.depth + g.depth - 1) / g.depth : 0;
    Pieces* p = NULL;
    if (nsr == 1 && sr[0].formatProperties.aspectMask == VK_IMAGE_ASPECT_COLOR_BIT && sr[0].imageMipTailFirstLod >= 1 &&
        g.width == block->width && g.height == block->height && g.depth == block->depth && req.alignment &&
        bx * by * bz <= UINT32_MAX)
        p = calloc(1, sizeof *p + bx * by * bz * sizeof p->pages[0]);
    if (!p || !pieces_room()) {
        WARN(
            "vulkan: a %ux%ux%u image with memory where it is bound has %u aspects, blocks of %ux%ux%u texels (%ux%ux%u "
            "asked for), its level in a mip tail from level %u: it is made whole\n",
            ii->extent.width, ii->extent.height, ii->extent.depth, nsr, g.width, g.height, g.depth, block->width,
            block->height, block->depth, nsr ? sr[0].imageMipTailFirstLod : 0);
        vkDestroyImage(g_vk.dev, *img, NULL);
        *img = VK_NULL_HANDLE;
        free(p);
        return 0;
    }
    p->img = *img;
    p->n = (u32)(bx * by * bz);
    p->bx = (u32)bx;
    p->by = (u32)by;
    p->block = g;
    p->extent = ii->extent;
    p->req = req;
    g_pieces[g_npieces++] = p;
    LOG_LIMIT(8, "vulkan: a %ux%ux%u image of %llu KiB with memory where it is bound, %u blocks of %ux%ux%u texels\n",
              ii->extent.width, ii->extent.height, ii->extent.depth, (unsigned long long)(req.size >> 10), p->n,
              g.width, g.height, g.depth);
    return 1;
}

/* the binds on the graphics queue, after everything recorded before them and the compute queue's batches submitted:
   no command may read a block while its binding changes. Waited for on the host, so the commands recorded after it
   see the new bindings, and released memory is no command's any more */
static int resident_bind(VkImage img, const VkSparseImageMemoryBind* binds, u32 n) {
    u64 cs = 0, cs_done = 0;
    VkFence fence;
    VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    vk_submit(VK_NULL_HANDLE, 0, VK_NULL_HANDLE);
    vk_rec_queue(g_vk.gfx);
    if (g_vk_csem) vk_compute_progress(&cs, &cs_done);
    VkSemaphore sems[2] = {vk_gfx_timeline(), g_vk_csem};
    u64 values[2] = {vk_last_submit(), cs};
    u32 nsem = cs > cs_done ? 2 : 1;
    VkTimelineSemaphoreSubmitInfo ts = {.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
                                        .waitSemaphoreValueCount = nsem,
                                        .pWaitSemaphoreValues = values};
    VkSparseImageMemoryBindInfo ib = {.image = img, .bindCount = n, .pBinds = binds};
    VkBindSparseInfo bi = {.sType = VK_STRUCTURE_TYPE_BIND_SPARSE_INFO,
                           .pNext = &ts,
                           .waitSemaphoreCount = nsem,
                           .pWaitSemaphores = sems,
                           .imageBindCount = 1,
                           .pImageBinds = &ib};
    if (!VK_OK(vkCreateFence(g_vk.dev, &fi, NULL, &fence))) return 0;
    vk_queue_lock(g_vk.gfx);
    int ok = VK_OK(vkQueueBindSparse(g_vk.gfx, 1, &bi, fence));
    vk_queue_unlock(g_vk.gfx);
    ok = ok && VK_OK(vkWaitForFences(g_vk.dev, 1, &fence, VK_TRUE, UINT64_MAX));
    vkDestroyFence(g_vk.dev, fence, NULL);
    return ok;
}

int vk_image_resident(VkImage img, const u8* want, u32 n, u64* bytes) {
    Pieces* p = pieces_of(img);
    *bytes = p ? p->bound : 0;
    if (!p || !p->bx || n != p->n) return 0;
    u32 changes = 0;
    for (u32 i = 0; i < n; i++) changes += !want[i] != !p->pages[i];
    if (!changes) return 1;
    VkSparseImageMemoryBind* binds = calloc(changes, sizeof *binds);
    u32* which = calloc(changes, sizeof *which);
    VmaAllocation* had = calloc(changes, sizeof *had);
    if (!binds || !which || !had) {
        free(binds);
        free(which);
        free(had);
        return 0;
    }
    const VkExtent3D g = p->block;
    const VkMemoryRequirements one = {
        .size = p->req.alignment, .alignment = p->req.alignment, .memoryTypeBits = p->req.memoryTypeBits};
    const VmaAllocationCreateInfo aci = {.preferredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT};
    VkResult res = VK_SUCCESS;
    u32 nb = 0;
    for (u32 i = 0; i < n && VK_OK(res); i++) {
        if (!want[i] == !p->pages[i]) continue;
        u32 x = i % p->bx * g.width, y = i / p->bx % p->by * g.height, z = i / (p->bx * p->by) * g.depth;
        VkSparseImageMemoryBind b = {.subresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0},
                                     .offset = {(i32)x, (i32)y, (i32)z},
                                     .extent = {p->extent.width - x < g.width ? p->extent.width - x : g.width,
                                                p->extent.height - y < g.height ? p->extent.height - y : g.height,
                                                p->extent.depth - z < g.depth ? p->extent.depth - z : g.depth}};
        had[nb] = p->pages[i];
        if (want[i]) {
            VmaAllocationInfo info;
            res = vmaAllocateMemory(g_vk.vma, &one, &aci, &p->pages[i], &info);
            if (!VK_OK(res)) break;
            vk_mem_track(p->pages[i]);
            b.memory = info.deviceMemory;
            b.memoryOffset = info.offset;
        } else
            p->pages[i] = VK_NULL_HANDLE;
        which[nb] = i;
        binds[nb++] = b;
    }
    int ok = nb && resident_bind(img, binds, nb);
    /* bound: the released blocks' memory goes. Not bound: the blocks keep what they had */
    for (u32 k = 0; k < nb; k++) {
        VmaAllocation gone = ok ? had[k] : p->pages[which[k]];
        if (!ok) p->pages[which[k]] = had[k];
        if (gone) {
            vk_mem_untrack(gone);
            vmaFreeMemory(g_vk.vma, gone);
        }
        if (ok && had[k])
            p->bound -= one.size;
        else if (ok)
            p->bound += one.size;
    }
    if (!ok || !VK_OK(res))
        WARN("vulkan: %u of %u blocks of a %ux%ux%u image with memory where it is bound changed (%s): %llu KiB bound\n",
             ok ? nb : 0, changes, p->extent.width, p->extent.height, p->extent.depth,
             !VK_OK(res) ? "out of memory" : "the bind failed", (unsigned long long)(p->bound >> 10));
    *bytes = p->bound;
    free(binds);
    free(which);
    free(had);
    return ok && VK_OK(res);
}

/* ------------------------------------------------------------ the budget */
/* The heap images go to: the biggest device-local one (the only one on a GPU sharing system memory). The BAR window,
   device local too, holds the staging rings, which keep within its budget themselves (cmd.c ring_misplaced) */
static int g_heap = -1;
static u64 g_budget_cap; /* vk_budget_mb, 0 when unset */

static void heap_find(void) {
    const VkPhysicalDeviceMemoryProperties* mp;
    vmaGetMemoryProperties(g_vk.vma, &mp);
    g_heap = 0;
    for (u32 h = 0; h < mp->memoryHeapCount; h++)
        if ((mp->memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) &&
            (!(mp->memoryHeaps[g_heap].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) ||
             mp->memoryHeaps[h].size > mp->memoryHeaps[g_heap].size))
            g_heap = (int)h;
    if (g_settings.vulkan_memory_budget) {
        g_budget_cap = (u64)g_settings.vulkan_memory_budget << 20;
        LOG("vulkan: the caches keep within %llu MB of heap %d (vk_budget_mb)\n",
            (unsigned long long)(g_budget_cap >> 20), g_heap);
    }
}

int vk_mem_heap(void) {
    if (g_heap < 0) heap_find();
    return g_heap;
}

/* the heap's use as the caches see it, and its budget. The use leaves out the free space inside VMA's blocks, which
   the next allocations fill before VMA asks the driver for more, and the memory of evicted entries the frames in
   flight still hold (vk_deferred_bytes): it goes once they are done */
void vk_mem_use(u64* usage, u64* budget) {
    VmaBudget b[VK_MAX_MEMORY_HEAPS];
    if (g_heap < 0) heap_find();
    vmaGetHeapBudgets(g_vk.vma, b);
    u64 u = b[g_heap].usage, holes = b[g_heap].statistics.blockBytes - b[g_heap].statistics.allocationBytes,
        gone = vk_deferred_bytes();
    u = u > holes ? u - holes : 0;
    *usage = u > gone ? u - gone : 0;
    *budget = g_budget_cap && g_budget_cap < b[g_heap].budget ? g_budget_cap : b[g_heap].budget;
}

/* the present clock: a present and its time every quarter second at most, seventeen minutes back */
#define CLOCK_N 4096
static struct {
    u32 present;
    u64 ns;
} g_clock[CLOCK_N];
static u32 g_clock_n, g_clock_at; /* samples kept; where the newest is */

void vk_mem_present(void) {
    u64 now = plat_ns();
    if (g_clock_n && now - g_clock[g_clock_at].ns < 250000000ull) return;
    g_clock_at = g_clock_n ? (g_clock_at + 1) % CLOCK_N : 0;
    g_clock[g_clock_at].present = g_vk_present;
    g_clock[g_clock_at].ns = now;
    if (g_clock_n < CLOCK_N) g_clock_n++;
}

u32 vk_present_before(u64 ago_ns, u32 presents) {
    if (g_vk_present <= presents) return 0;
    u32 before = g_vk_present - presents;
    if (!ago_ns) return before;
    /* the newest sample at least ago_ns old: what was last used before its present has been unused since then */
    u64 now = plat_ns();
    for (u32 i = 0; i < g_clock_n; i++) {
        u32 at = (g_clock_at + CLOCK_N - i) % CLOCK_N;
        if (now - g_clock[at].ns < ago_ns) continue;
        return (i32)(g_clock[at].present - before) < 0 ? g_clock[at].present : before;
    }
    return 0; /* the clock doesn't reach back that far: nothing is that old yet */
}
