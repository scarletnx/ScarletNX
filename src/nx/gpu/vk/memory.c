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

   A texture that guest memory maps only in part is an image with sparse residency instead, with memory only for the
   levels that are mapped (vk_image_create_sparse, below). */
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
    LOG("vulkan: allocations of at most %llu MiB, bigger images %s\n", (unsigned long long)(cap >> 20),
        g_vk.sparse ? "in pieces (sparse binding)" : "in one allocation (no sparse binding)");
    LOG("vulkan: textures mapped in part %s\n", g_vk.sparse && g_vk.feat.sparseResidencyImage2D
                                                    ? "take memory only where guest memory maps them (sparse residency)"
                                                    : "are made whole (no sparse residency)");
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
    if (g_npieces == g_cappieces && p) {
        u32 cap = g_cappieces ? g_cappieces * 2 : 8;
        Pieces** grown = realloc(g_pieces, cap * sizeof *grown);
        if (grown) {
            g_pieces = grown;
            g_cappieces = cap;
        }
    }
    if (!p || g_npieces == g_cappieces || !pieces_bind(*img, &req, p, res)) {
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

/* ------------------------------------------------------------ sparse residency */
/* A texture that guest memory maps only in part (a game's sparse texture: its pool is virtual, and the game maps the
   64 KiB pages of the regions it streams in) takes memory only where guest memory has pages. Its image has sparse
   residency: the mip tail is bound always, and each sparse block of the other levels (64 KiB of the host's texels) is
   bound when guest memory maps a page of its texels, and released when it maps none. A block without memory reads as
   zero (residencyNonResidentStrict) or undefined, as the console's unmapped sparse pages read zero, and writes to it
   are discarded.

   Each block is its own VMA allocation, which VMA takes from its shared blocks. A bind is waited for on the host, as
   pieces_bind does: the commands recorded after it are submitted after it is done. A release unbinds at once, and its
   memory goes when the frame being recorded is done: the frames in flight can still read through the old binding */
#define SPARSE_MIN (4ull << 20) /* a smaller image is made whole: the memory it can save is small */
typedef struct {
    VkImage img;
    VkSparseShape sh;
    VkExtent3D extent; /* level 0's */
    u32 tails;         /* mip tails: 0, 1 (one for all the layers) or one for each layer */
    u64 block;         /* the bytes of one sparse block */
    u64 tail_offset, tail_size, tail_stride;
    u32 memtypes;
    u64 bytes;            /* the memory bound now */
    VmaAllocation slot[]; /* the blocks (VkSparseShape's order), then the tails */
} Sparse;
static Sparse** g_sparse;
static u32 g_nsparse, g_capsparse;
static u64 g_sparse_binds, g_sparse_bind_ns; /* statistics: the binds and the host's wait for them */

static Sparse* sparse_find(VkImage img) {
    for (u32 i = 0; i < g_nsparse; i++)
        if (g_sparse[i]->img == img) return g_sparse[i];
    return NULL;
}
static u32 sparse_dim(u32 v, u32 level) { return v >> level ? v >> level : 1; }
static u64 sparse_nblocks(const Sparse* s) { return (u64)s->sh.layers * s->sh.per_layer; }
/* block b's subresource and region: whole sparse blocks, or to the level's edge, as a bind's extent must be */
static void sparse_region(const Sparse* s, u64 b, VkSparseImageMemoryBind* out) {
    const VkSparseShape* sh = &s->sh;
    u32 layer = (u32)(b / sh->per_layer), in = (u32)(b % sh->per_layer), l = 0;
    while (in >= sh->first[l + 1]) l++;
    in -= sh->first[l];
    u32 x = in % sh->bx[l], y = in / sh->bx[l] % sh->by[l], z = in / sh->bx[l] / sh->by[l];
    u32 w = sparse_dim(s->extent.width, l), h = sparse_dim(s->extent.height, l), d = sparse_dim(s->extent.depth, l);
    u32 x0 = x * sh->gran.width, y0 = y * sh->gran.height, z0 = z * sh->gran.depth;
    *out = (VkSparseImageMemoryBind){.subresource = {VK_IMAGE_ASPECT_COLOR_BIT, l, layer},
                                     .offset = {(i32)x0, (i32)y0, (i32)z0},
                                     .extent = {w - x0 < sh->gran.width ? w - x0 : sh->gran.width,
                                                h - y0 < sh->gran.height ? h - y0 : sh->gran.height,
                                                d - z0 < sh->gran.depth ? d - z0 : sh->gran.depth}};
}

/* the answers vk_sparse_supported gave, by what it was asked */
typedef struct {
    VkFormat f;
    VkImageType type;
    VkImageUsageFlags usage;
    VkImageCreateFlags flags;
    int ok;
} SparseAnswer;
int vk_sparse_supported(const VkImageCreateInfo* ii) {
    static SparseAnswer memo[32];
    static u32 nmemo;
    int need = ii->imageType == VK_IMAGE_TYPE_3D   ? g_vk.feat.sparseResidencyImage3D
               : ii->imageType == VK_IMAGE_TYPE_2D ? g_vk.feat.sparseResidencyImage2D
                                                   : 0;
    if (!g_vk.sparse || !need || ii->samples != VK_SAMPLE_COUNT_1_BIT || ii->tiling != VK_IMAGE_TILING_OPTIMAL ||
        ii->mipLevels > VK_SPARSE_LEVELS)
        return 0;
    for (u32 i = 0; i < nmemo; i++)
        if (memo[i].f == ii->format && memo[i].type == ii->imageType && memo[i].usage == ii->usage &&
            memo[i].flags == ii->flags)
            return memo[i].ok;
    VkImageCreateFlags flags = ii->flags | VK_IMAGE_CREATE_SPARSE_BINDING_BIT | VK_IMAGE_CREATE_SPARSE_RESIDENCY_BIT;
    VkImageFormatProperties ip;
    VkSparseImageFormatProperties sp[2];
    u32 n = 2;
    vkGetPhysicalDeviceSparseImageFormatProperties(g_vk.phys, ii->format, ii->imageType, ii->samples, ii->usage,
                                                   ii->tiling, &n, sp);
    /* one color aspect, and no metadata to bind */
    int ok = n == 1 && sp[0].aspectMask == VK_IMAGE_ASPECT_COLOR_BIT &&
             vkGetPhysicalDeviceImageFormatProperties(g_vk.phys, ii->format, ii->imageType, ii->tiling, ii->usage,
                                                      flags, &ip) == VK_SUCCESS;
    if (nmemo < sizeof memo / sizeof memo[0])
        memo[nmemo++] = (SparseAnswer){ii->format, ii->imageType, ii->usage, ii->flags, ok};
    return ok;
}

int vk_image_create_sparse(const VkImageCreateInfo* ii, VkImage* img) {
    VkImageCreateInfo si = *ii;
    si.flags |= VK_IMAGE_CREATE_SPARSE_BINDING_BIT | VK_IMAGE_CREATE_SPARSE_RESIDENCY_BIT;
    *img = VK_NULL_HANDLE;
    if (!vk_sparse_supported(ii)) return 0;
    VkDeviceImageMemoryRequirements q = {.sType = VK_STRUCTURE_TYPE_DEVICE_IMAGE_MEMORY_REQUIREMENTS,
                                         .pCreateInfo = &si};
    VkMemoryRequirements2 whole = {.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2};
    vkGetDeviceImageMemoryRequirements(g_vk.dev, &q, &whole);
    if (whole.memoryRequirements.size < SPARSE_MIN) return 0;
    if (g_nsparse == g_capsparse) {
        u32 cap = g_capsparse ? g_capsparse * 2 : 16;
        Sparse** grown = realloc(g_sparse, cap * sizeof *grown);
        if (!grown) return 0;
        g_sparse = grown;
        g_capsparse = cap;
    }
    if (!VK_OK(vkCreateImage(g_vk.dev, &si, NULL, img))) {
        *img = VK_NULL_HANDLE;
        return 0;
    }
    VkMemoryRequirements req;
    VkSparseImageMemoryRequirements sr[2];
    u32 nsr = 2;
    vkGetImageMemoryRequirements(g_vk.dev, *img, &req);
    vkGetImageSparseMemoryRequirements(g_vk.dev, *img, &nsr, sr);
    const VkSparseImageFormatProperties* fp = &sr[0].formatProperties;
    Sparse h = {.img = *img,
                .extent = si.extent,
                .block = req.alignment,
                .memtypes = req.memoryTypeBits,
                .tail_offset = sr[0].imageMipTailOffset,
                .tail_size = sr[0].imageMipTailSize,
                .tail_stride = sr[0].imageMipTailStride};
    h.sh.gran = fp->imageGranularity;
    h.sh.layers = si.arrayLayers;
    h.sh.levels = si.mipLevels;
    h.sh.tail_first = sr[0].imageMipTailFirstLod < si.mipLevels ? sr[0].imageMipTailFirstLod : si.mipLevels;
    h.tails = h.sh.tail_first == si.mipLevels || !h.tail_size                ? 0
              : (fp->flags & VK_SPARSE_IMAGE_FORMAT_SINGLE_MIPTAIL_BIT) != 0 ? 1
                                                                             : si.arrayLayers;
    Sparse* s = NULL;
    if (nsr == 1 && fp->aspectMask == VK_IMAGE_ASPECT_COLOR_BIT && req.alignment && h.sh.gran.width &&
        h.sh.gran.height && h.sh.gran.depth) {
        for (u32 l = 0; l < h.sh.tail_first; l++) {
            h.sh.bx[l] = (sparse_dim(h.extent.width, l) + h.sh.gran.width - 1) / h.sh.gran.width;
            h.sh.by[l] = (sparse_dim(h.extent.height, l) + h.sh.gran.height - 1) / h.sh.gran.height;
            h.sh.bz[l] = (sparse_dim(h.extent.depth, l) + h.sh.gran.depth - 1) / h.sh.gran.depth;
            h.sh.first[l + 1] = h.sh.first[l] + h.sh.bx[l] * h.sh.by[l] * h.sh.bz[l];
        }
        h.sh.per_layer = h.sh.first[h.sh.tail_first];
        s = calloc(1, sizeof *s + (sparse_nblocks(&h) + h.tails) * sizeof s->slot[0]);
    }
    if (!s) { /* a shape that isn't one color aspect, or no memory for its slots */
        vkDestroyImage(g_vk.dev, *img, NULL);
        *img = VK_NULL_HANDLE;
        return 0;
    }
    *s = h;
    g_sparse[g_nsparse++] = s;
    return 1;
}

const VkSparseShape* vk_image_sparse_shape(VkImage img) {
    const Sparse* s = sparse_find(img);
    return s ? &s->sh : NULL;
}

/* the blocks and tails a bind of bits adds (add) and releases (drop) */
static void sparse_diff(const Sparse* s, const u8* bits, u64* add, u64* drop) {
    *add = *drop = 0;
    for (u64 b = 0; b < sparse_nblocks(s); b++) {
        int want = (bits[b >> 3] >> (b & 7)) & 1, have = s->slot[b] != VK_NULL_HANDLE;
        *add += want && !have;
        *drop += have && !want;
    }
    for (u32 t = 0; t < s->tails; t++) *add += !s->slot[sparse_nblocks(s) + t];
}

u64 vk_image_sparse_need(VkImage img, const u8* bits) {
    const Sparse* s = sparse_find(img);
    u64 add, drop, tails = 0;
    if (!s) return 0;
    sparse_diff(s, bits, &add, &drop);
    for (u32 t = 0; t < s->tails; t++) tails += !s->slot[sparse_nblocks(s) + t];
    return (add - tails) * s->block + tails * s->tail_size;
}

/* the memory a release unbound, freed once the frames that could read it are done */
typedef struct {
    u32 n;
    VmaAllocation a[];
} SparseDead;
static void sparse_dead_free(void* arg) {
    SparseDead* d = arg;
    for (u32 i = 0; i < d->n; i++) {
        vk_mem_untrack(d->a[i]);
        vmaFreeMemory(g_vk.vma, d->a[i]);
    }
    free(d);
}

/* the binds of info on the graphics queue, waited for */
static int sparse_submit(const VkBindSparseInfo* bi) {
    VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence;
    u64 t0 = plat_ns();
    int ok = VK_OK(vkCreateFence(g_vk.dev, &fi, NULL, &fence));
    if (ok) {
        vk_queue_lock(g_vk.gfx);
        ok = VK_OK(vkQueueBindSparse(g_vk.gfx, 1, bi, fence));
        vk_queue_unlock(g_vk.gfx);
        ok = ok && VK_OK(vkWaitForFences(g_vk.dev, 1, &fence, VK_TRUE, UINT64_MAX));
        vkDestroyFence(g_vk.dev, fence, NULL);
    }
    g_sparse_binds++;
    g_sparse_bind_ns += plat_ns() - t0;
    return ok;
}

int vk_image_sparse_bind(VkImage img, const u8* bits, u64* bytes) {
    Sparse* s = sparse_find(img);
    if (!s) return -1;
    *bytes = s->bytes;
    u64 add, drop, nb = sparse_nblocks(s);
    sparse_diff(s, bits, &add, &drop);
    if (!add && !drop) return 0;
    u32 ntail = 0;
    for (u32 t = 0; t < s->tails; t++) ntail += !s->slot[nb + t];
    u64 nimg = add + drop - ntail, nnew = add - ntail;
    VkSparseImageMemoryBind* ib = calloc(nimg ? nimg : 1, sizeof *ib);
    VkSparseMemoryBind* ob = calloc(ntail ? ntail : 1, sizeof *ob);
    VmaAllocation* fresh = calloc(add ? add : 1, sizeof *fresh);
    VmaAllocationInfo* info = calloc(add ? add : 1, sizeof *info);
    SparseDead* dead = calloc(1, sizeof *dead + (drop ? drop : 1) * sizeof dead->a[0]);
    int ok = ib && ob && fresh && info && dead;
    /* the new blocks, all at once, then the tails */
    VkMemoryRequirements req = {.size = s->block, .alignment = s->block, .memoryTypeBits = s->memtypes};
    VmaAllocationCreateInfo aci = {.preferredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT};
    u64 made = 0;
    if (ok && nnew) {
        ok = VK_OK(vmaAllocateMemoryPages(g_vk.vma, &req, &aci, nnew, fresh, info));
        if (ok) made = nnew;
    }
    for (u32 t = 0; ok && t < ntail; t++) {
        VkMemoryRequirements tr = {.size = s->tail_size, .alignment = s->block, .memoryTypeBits = s->memtypes};
        ok = VK_OK(vmaAllocateMemory(g_vk.vma, &tr, &aci, &fresh[nnew + t], &info[nnew + t]));
        if (ok) made++;
    }
    if (ok) {
        u64 ni = 0, k = 0;
        u32 no = 0;
        for (u64 b = 0; b < nb; b++) {
            int want = (bits[b >> 3] >> (b & 7)) & 1, have = s->slot[b] != VK_NULL_HANDLE;
            if (want == have) continue;
            sparse_region(s, b, &ib[ni]);
            if (want) {
                ib[ni].memory = info[k].deviceMemory;
                ib[ni].memoryOffset = info[k].offset;
                k++;
            } /* else memory VK_NULL_HANDLE: unbound */
            ni++;
        }
        for (u32 t = 0; t < s->tails; t++)
            if (!s->slot[nb + t]) {
                ob[no] = (VkSparseMemoryBind){.resourceOffset = s->tail_offset + t * s->tail_stride,
                                              .size = s->tail_size,
                                              .memory = info[nnew + no].deviceMemory,
                                              .memoryOffset = info[nnew + no].offset};
                no++;
            }
        VkSparseImageMemoryBindInfo ibi = {.image = img, .bindCount = (u32)ni, .pBinds = ib};
        VkSparseImageOpaqueMemoryBindInfo obi = {.image = img, .bindCount = no, .pBinds = ob};
        VkBindSparseInfo bi = {.sType = VK_STRUCTURE_TYPE_BIND_SPARSE_INFO,
                               .imageOpaqueBindCount = no ? 1u : 0u,
                               .pImageOpaqueBinds = &obi,
                               .imageBindCount = ni ? 1u : 0u,
                               .pImageBinds = &ibi};
        ok = sparse_submit(&bi);
    }
    if (!ok) { /* nothing changes: the new memory goes, and what was bound stays */
        for (u64 i = 0; i < made; i++) vmaFreeMemory(g_vk.vma, fresh[i]);
        free(dead);
    } else {
        u64 k = 0;
        for (u64 b = 0; b < nb; b++) {
            int want = (bits[b >> 3] >> (b & 7)) & 1;
            if (want && !s->slot[b]) {
                s->slot[b] = fresh[k++];
                vk_mem_track(s->slot[b]);
            } else if (!want && s->slot[b]) {
                dead->a[dead->n++] = s->slot[b];
                s->slot[b] = VK_NULL_HANDLE;
            }
        }
        for (u32 t = 0; t < s->tails; t++)
            if (!s->slot[nb + t]) {
                s->slot[nb + t] = fresh[k++];
                vk_mem_track(s->slot[nb + t]);
            }
        s->bytes = s->bytes + (add - ntail) * s->block + (u64)ntail * s->tail_size - drop * s->block;
        *bytes = s->bytes;
        if (dead->n) {
            vk_defer_bytes(drop * s->block);
            vk_defer_call(sparse_dead_free, dead);
        } else
            free(dead);
    }
    free(ib);
    free(ob);
    free(fresh);
    free(info);
    return ok ? 1 : -1;
}

int vk_image_sparse_bound(VkImage img, u32 layer, u32 level, u32 y0, u32 y1, u32 z0, u32 z1, int all) {
    const Sparse* s = sparse_find(img);
    if (!s || layer >= s->sh.layers || level >= s->sh.levels || y0 >= y1 || z0 >= z1) return 0;
    const VkSparseShape* sh = &s->sh;
    if (level >= sh->tail_first) return !s->tails || s->slot[sparse_nblocks(s) + (s->tails == 1 ? 0 : layer)];
    u32 by0 = y0 / sh->gran.height, by1 = (y1 - 1) / sh->gran.height, bz0 = z0 / sh->gran.depth,
        bz1 = (z1 - 1) / sh->gran.depth;
    if (by1 >= sh->by[level]) by1 = sh->by[level] - 1;
    if (bz1 >= sh->bz[level]) bz1 = sh->bz[level] - 1;
    u64 base = (u64)layer * sh->per_layer + sh->first[level];
    for (u32 z = bz0; z <= bz1; z++)
        for (u32 y = by0; y <= by1; y++)
            for (u32 x = 0; x < sh->bx[level]; x++) {
                int have = s->slot[base + ((u64)z * sh->by[level] + y) * sh->bx[level] + x] != VK_NULL_HANDLE;
                if (have != all) return have;
            }
    return all;
}

void vk_sparse_stats(u64* binds, u64* bind_ns) {
    *binds = g_sparse_binds;
    *bind_ns = g_sparse_bind_ns;
}

static int sparse_destroy(VkImage img) {
    for (u32 i = 0; i < g_nsparse; i++) {
        Sparse* s = g_sparse[i];
        if (s->img != img) continue;
        vkDestroyImage(g_vk.dev, img, NULL);
        for (u64 k = 0; k < sparse_nblocks(s) + s->tails; k++)
            if (s->slot[k]) {
                vk_mem_untrack(s->slot[k]);
                vmaFreeMemory(g_vk.vma, s->slot[k]);
            }
        free(s);
        g_sparse[i] = g_sparse[--g_nsparse];
        return 1;
    }
    return 0;
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
    if (sparse_destroy(img)) return;
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
