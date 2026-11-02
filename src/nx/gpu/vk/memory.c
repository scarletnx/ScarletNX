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
   have to wait on, and an image this big is made once. */
#include <stdlib.h>
#include "nx/runtime/options.h"
#include "nx/runtime/platform/log.h"
#include "nx/runtime/platform/plat.h"
#include "nx/gpu/vk/vk.h"

#define ALLOC_CAP_DEFAULT (256ull << 20)

/* the images in pieces, each with its allocations */
typedef struct {
    VkImage img;
    u32 n;
    VmaAllocation pages[];
} Pieces;
static Pieces** g_pieces;
static u32 g_npieces, g_cappieces;

static int pieces_add(Pieces* p) {
    if (g_npieces == g_cappieces) {
        u32 cap = g_cappieces ? g_cappieces * 2 : 8;
        Pieces** grown = realloc(g_pieces, cap * sizeof *grown);
        if (!grown) return 0;
        g_pieces = grown;
        g_cappieces = cap;
    }
    g_pieces[g_npieces++] = p;
    return 1;
}

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
    VkSemaphoreTypeCreateInfo ti = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
                                    .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE};
    VkSemaphoreCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &ti};
    VK_OK(vkCreateSemaphore(g_vk.dev, &si, NULL, &g_vk.sparse_sem));
    LOG("vulkan: allocations of at most %llu MiB, bigger images %s\n", (unsigned long long)(cap >> 20),
        g_vk.sparse ? "in pieces (sparse binding)" : "in one allocation (no sparse binding)");
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
    if (!p || !pieces_bind(*img, &req, p, res) || !pieces_add(p)) {
        WARN("vulkan: cannot bind a %llu MiB image in %u pieces of %llu MiB\n", (unsigned long long)(req.size >> 20), n,
             (unsigned long long)(chunk >> 20));
        vkDestroyImage(g_vk.dev, *img, NULL);
        *img = VK_NULL_HANDLE;
        if (p) pieces_free(p);
        return 0;
    }
    p->img = *img;
    LOG_LIMIT(8, "vulkan: a %ux%ux%u image of %llu MiB, bound in %u pieces\n", ii->extent.width, ii->extent.height,
              ii->extent.depth * ii->arrayLayers, (unsigned long long)(req.size >> 20), n);
    return 1;
}

/* A streamed array is a new image on each upload. Earlier draws keep their old image until their frame is done,
   so binding new levels neither changes an earlier draw nor needs a GPU wait during draw setup. */
typedef struct {
    VkMemoryRequirements req;
    VkSparseImageMemoryRequirements sparse;
    VkExtent3D extent;
    Pieces* pieces;
    VkSparseImageMemoryBind* images;
    VkSparseMemoryBind* opaque;
    u32 ni, no;
    u64 bytes;
} ResidentImage;

static int resident_alloc(ResidentImage* r, u64 size, VmaAllocationInfo* info) {
    VkMemoryRequirements req = {.size = size, .alignment = r->req.alignment, .memoryTypeBits = r->req.memoryTypeBits};
    VmaAllocationCreateInfo aci = {.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT,
                                   .preferredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT};
    VmaAllocation* a = &r->pieces->pages[r->pieces->n];
    if (!VK_OK(vmaAllocateMemory(g_vk.vma, &req, &aci, a, info))) return 0;
    r->pieces->n++;
    r->bytes += info->size;
    vk_mem_track(*a);
    return 1;
}

static int resident_level(ResidentImage* r, u32 layer, u32 level) {
    VkExtent3D gran = r->sparse.formatProperties.imageGranularity;
    u32 w = r->extent.width >> level ? r->extent.width >> level : 1;
    u32 h = r->extent.height >> level ? r->extent.height >> level : 1;
    u32 nx = (w + gran.width - 1) / gran.width, ny = (h + gran.height - 1) / gran.height;
    u64 tiles = (u64)nx * ny, per = g_vk.alloc_cap / r->req.alignment;
    for (u64 at = 0; at < tiles; at += per) {
        u64 n = tiles - at < per ? tiles - at : per;
        VmaAllocationInfo info;
        if (!resident_alloc(r, n * r->req.alignment, &info)) return 0;
        /* A piece is at most a partial first row, complete rows, then a partial last row. */
        for (u64 tile = at, end = at + n; tile < end;) {
            u32 x = (u32)(tile % nx), y = (u32)(tile / nx);
            u32 cols = end - tile < nx - x ? (u32)(end - tile) : nx - x;
            u32 rows = !x && cols == nx ? (u32)((end - tile) / nx) : 1;
            u32 ew = cols * gran.width, eh = rows * gran.height;
            if (ew > w - x * gran.width) ew = w - x * gran.width;
            if (eh > h - y * gran.height) eh = h - y * gran.height;
            r->images[r->ni++] = (VkSparseImageMemoryBind){
                .subresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, layer},
                .offset = {(i32)(x * gran.width), (i32)(y * gran.height), 0},
                .extent = {ew, eh, 1}, .memory = info.deviceMemory,
                .memoryOffset = info.offset + (tile - at) * r->req.alignment};
            tile += (u64)cols * rows;
        }
    }
    return 1;
}

static int resident_tail(ResidentImage* r, u32 layer) {
    u64 chunk = g_vk.alloc_cap / r->req.alignment * r->req.alignment;
    u64 base = r->sparse.imageMipTailOffset + layer * r->sparse.imageMipTailStride;
    for (u64 at = 0; at < r->sparse.imageMipTailSize; at += chunk) {
        u64 size = r->sparse.imageMipTailSize - at < chunk ? r->sparse.imageMipTailSize - at : chunk;
        VmaAllocationInfo info;
        if (!resident_alloc(r, size, &info)) return 0;
        r->opaque[r->no++] = (VkSparseMemoryBind){
            .resourceOffset = base + at, .size = size, .memory = info.deviceMemory, .memoryOffset = info.offset};
    }
    return 1;
}

int vk_image_resident_create(const VkImageCreateInfo* ii, u32* masks, VkImage* img, u64* bytes) {
    *img = VK_NULL_HANDLE;
    *bytes = 0;
    if (!g_vk.sparse || !g_vk.feat.sparseResidencyImage2D ||
        !g_vk.props.sparseProperties.residencyNonResidentStrict) {
        LOG_LIMIT(1, "vulkan: streamed textures need sparse 2D residency with zero reads of unbound texels\n");
        return 0;
    }
    VkImageCreateInfo si = *ii;
    si.flags |= VK_IMAGE_CREATE_SPARSE_BINDING_BIT | VK_IMAGE_CREATE_SPARSE_RESIDENCY_BIT;
    if (!VK_OK(vkCreateImage(g_vk.dev, &si, NULL, img))) return 0;
    ResidentImage r = {.extent = ii->extent};
    vkGetImageMemoryRequirements(g_vk.dev, *img, &r.req);
    u32 count = 0;
    vkGetImageSparseMemoryRequirements(g_vk.dev, *img, &count, NULL);
    if (count == 1) vkGetImageSparseMemoryRequirements(g_vk.dev, *img, &count, &r.sparse);
    u64 chunk = r.req.alignment ? g_vk.alloc_cap / r.req.alignment * r.req.alignment : 0;
    if (count != 1 || r.sparse.formatProperties.aspectMask != VK_IMAGE_ASPECT_COLOR_BIT || !chunk) {
        LOG_LIMIT(1, "vulkan: the streamed texture format has no supported sparse color layout\n");
        vkDestroyImage(g_vk.dev, *img, NULL);
        *img = VK_NULL_HANDLE;
        return 0;
    }
    u64 total = 0;
    u32 n = 0, tail_bit = r.sparse.imageMipTailFirstLod < ii->mipLevels
                             ? ((1u << ii->mipLevels) - 1) & (~0u << r.sparse.imageMipTailFirstLod) : 0;
    int single = (r.sparse.formatProperties.flags & VK_SPARSE_IMAGE_FORMAT_SINGLE_MIPTAIL_BIT) != 0;
    u32 tail_layers = 0;
    VkExtent3D gran = r.sparse.formatProperties.imageGranularity;
    for (u32 layer = 0; layer < ii->arrayLayers; layer++) {
        for (u32 l = 0; l < ii->mipLevels && l < r.sparse.imageMipTailFirstLod; l++) {
            if (!(masks[layer] & (1u << l))) continue;
            u32 w = ii->extent.width >> l ? ii->extent.width >> l : 1;
            u32 h = ii->extent.height >> l ? ii->extent.height >> l : 1;
            u64 size = (u64)((w + gran.width - 1) / gran.width) * ((h + gran.height - 1) / gran.height) * r.req.alignment;
            total += size;
            n += (u32)((size + chunk - 1) / chunk);
        }
        if (masks[layer] & tail_bit) tail_layers++;
    }
    if (single && tail_layers) tail_layers = 1;
    total += tail_layers * r.sparse.imageMipTailSize;
    n += tail_layers * (u32)((r.sparse.imageMipTailSize + chunk - 1) / chunk);
    /* Upload every level sharing a bound tail, including zero guest bytes of its unmapped levels. */
    for (u32 layer = 0; layer < ii->arrayLayers; layer++)
        if ((single && tail_layers) || (masks[layer] & tail_bit)) masks[layer] |= tail_bit;
    vk_mem_trim(total, 0);
    r.pieces = calloc(1, sizeof *r.pieces + n * sizeof r.pieces->pages[0]);
    /* One piece needs at most three rectangular image binds, or one opaque tail bind. */
    r.images = calloc(n ? n : 1, 3 * sizeof *r.images + sizeof *r.opaque);
    r.opaque = r.images ? (VkSparseMemoryBind*)(r.images + 3 * n) : NULL;
    int ok = r.pieces && r.images;
    u32 tails = 0;
    for (u32 layer = 0; layer < ii->arrayLayers && ok; layer++) {
        for (u32 l = 0; l < ii->mipLevels && l < r.sparse.imageMipTailFirstLod && ok; l++)
            if (masks[layer] & (1u << l)) ok = resident_level(&r, layer, l);
        if (ok && (masks[layer] & tail_bit) && (!single || !tails++))
            ok = resident_tail(&r, single ? 0 : layer);
    }
    if (ok) {
        r.pieces->img = *img;
        ok = pieces_add(r.pieces);
    }
    if (ok && n) {
        /* The bind signals a timeline value consumed by both queues' normal submissions. It does not submit the
           draw's command buffer, wait on the CPU, or land storage results into guest memory. */
        u64 value = g_vk.sparse_value + 1;
        VkTimelineSemaphoreSubmitInfo ti = {.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
                                            .signalSemaphoreValueCount = 1, .pSignalSemaphoreValues = &value};
        VkSparseImageMemoryBindInfo images = {.image = *img, .bindCount = r.ni, .pBinds = r.images};
        VkSparseImageOpaqueMemoryBindInfo opaque = {.image = *img, .bindCount = r.no, .pBinds = r.opaque};
        VkBindSparseInfo bi = {.sType = VK_STRUCTURE_TYPE_BIND_SPARSE_INFO, .pNext = &ti,
                               .imageBindCount = r.ni ? 1 : 0, .pImageBinds = &images,
                               .imageOpaqueBindCount = r.no ? 1 : 0, .pImageOpaqueBinds = &opaque,
                               .signalSemaphoreCount = 1, .pSignalSemaphores = &g_vk.sparse_sem};
        vk_rec_queue(g_vk.gfx);
        ok = VK_OK(vkQueueBindSparse(g_vk.gfx, 1, &bi, VK_NULL_HANDLE));
        if (ok) g_vk.sparse_value = value;
        else g_npieces--; /* registered immediately above, but no bind was queued */
    }
    free(r.images);
    if (!ok) {
        vkDestroyImage(g_vk.dev, *img, NULL);
        *img = VK_NULL_HANDLE;
        if (r.pieces) pieces_free(r.pieces);
        return 0;
    }
    *bytes = r.bytes;
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
