/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* How big one device memory allocation may be, and images bigger than that; the budget the caches keep within, and
   the clock their ages are measured with.

   Some drivers fail allocations past 256 MiB even where maxMemoryAllocationSize is larger (NVIDIA's on FreeBSD), and
   a game's textures can be bigger: a 2D ASTC texture of 11584x11584 decodes to 512 MiB of RGBA8, a 3D one of 512^3
   too. No allocation is made bigger than g_vk.alloc_cap: the staging and readback rings stop growing there (cmd.c),
   and an image whose memory is bigger is created with sparse binding and bound to several allocations, each at most
   the cap. Shaders and views see one ordinary image: only its memory is in pieces.

   The pieces are dedicated allocations, so the cap is also what the driver is asked for.

   An image may also have memory only where it is bound (sparse residency): a texture in a virtual pool the guest
   maps a 64 KiB tile at a time, whose holes read as zeros on the console as they do here (residencyNonResidentStrict).
   Its blocks of the device's granularity, in every level before its mip tail and every layer, are bound and released
   one by one, each to an allocation of its own, and each mip tail as a whole; the memory it takes is that of its
   bound blocks and tails alone: a 1664x448x1344 BC1 volume is 477 MB whole and 384 KiB with the 6 tiles a game maps
   of it.

   Both are bound on the graphics queue, in batches: the binds made since the last submission go in one
   vkQueueBindSparse just before the next (vk_sparse_flush), and nothing waits for them on the host. A sparse bind is
   not ordered with command buffers, even on its own queue (the Vulkan spec, "Binding Resource Memory"): a batch
   signals a timeline semaphore of its own, and the submission after it waits for that value, which holds back every
   later command of the queue too (vkQueueSubmit2's waits take the commands later in submission order). A batch that
   changes blocks of an image in use waits for the last graphics submission, so no command reads a block while its
   binding changes; the commands recorded before it are submitted first. The compute queue's batches read no image (a
   dispatch with textures or images records into the frame's commands), so they neither wait for a batch nor a batch
   for them. The memory a block lets go is freed with the frame of the submission after the batch (vk_defer_call),
   once the GPU is past both. The bind is called on the recording thread, in order after the submission it waits
   for: queued ahead of it on the same queue, its wait could hold that submission back where a driver takes a queue's
   batches in order. */
#include <stdlib.h>
#include <string.h>
#if defined(__ANDROID__)
#include <unistd.h>
#include "common/utilities/common.h"
#include "nx/gpu/vk/uma_budget.h"
#endif
#include "nx/runtime/options.h"
#include "nx/runtime/platform/log.h"
#include "nx/runtime/platform/plat.h"
#include "nx/gpu/vk/vk.h"

#define ALLOC_CAP_DEFAULT (256ull << 20)

/* the images in pieces, each with its allocations. One with memory only where it is bound has a page for each of its
   blocks of block texels, layer by layer, in a layer level by level up to its mip tail, in a level x first, then y,
   then z; then one for each of its mip tails (VK_NULL_HANDLE where none is bound) */
typedef struct {
    VkImage img;
    u32 n;
    u32 per_layer;          /* the blocks of a layer; 0: an image in pieces */
    u32 layers, tail_first; /* tail_first: its levels in blocks, those before the mip tail */
    u32 tails;              /* the last of its n pages: 0, 1 for all the layers, or one a layer */
    VkExtent3D block, extent;
    VkMemoryRequirements req;
    u64 tail_offset, tail_size, tail_stride;
    u64 bound; /* the bytes of its bound blocks and tails */
    u64 batch; /* the bind batch its last binds are in, 0 none */
    VmaAllocation pages[];
} Pieces;
static Pieces** g_pieces;
static u32 g_npieces, g_cappieces;

/* The batch as the recording thread takes it: its head, then each image's BindOne and binds, VkSparseMemoryBind for
   an image in pieces, VkSparseImageMemoryBind for blocks */
typedef struct {
    u64 gfx;   /* the graphics timeline value it waits for, 0 none */
    u64 after; /* the batch before it, 0 none */
    u64 value; /* its own on the bind timeline */
    u32 nimg, nopaque;
} BindHead;
typedef struct {
    VkImage img;
    u32 n, opaque;
} BindOne;
/* a batch's bytes at most, a quarter of the recording thread's ring: the 7644 blocks of a volume in one batch */
#define BIND_CAP (1u << 20)
#define GONE_CAP (BIND_CAP / sizeof(VkSparseImageMemoryBind)) /* at most one released block for each bind */
static struct {
    VkSemaphore sem; /* the bind timeline: batch n signals n */
    u64 issued;      /* the last batch handed to the queue */
    u64 waited;      /* the last batch a submission waits for */
    u8* buf;         /* BIND_CAP bytes of the batch, then GONE_CAP allocations its blocks let go */
    u64 used, last;  /* the batch's bytes, 0 none; where its last BindOne is */
    u32 ngone;
    u64 gone_bytes;
    int readers; /* it changes blocks of an image in use: after the work submitted before it */
} g_bind;

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
    if (g_vk.sparse) {
        VkSemaphoreTypeCreateInfo tci = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
                                         .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE};
        VkSemaphoreCreateInfo sci = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &tci};
        g_vk.sparse = VK_OK(vkCreateSemaphore(g_vk.dev, &sci, NULL, &g_bind.sem));
    }
    const int r2 = g_vk.feat.sparseResidencyImage2D, r3 = g_vk.feat.sparseResidencyImage3D;
    g_vk.resident = g_vk.sparse && (r2 || r3) && g_vk.props.sparseProperties.residencyNonResidentStrict;
    LOG("vulkan: allocations of at most %llu MiB, bigger images %s, a texture with holes in its mapping %s\n",
        (unsigned long long)(cap >> 20),
        g_vk.sparse ? "in pieces (sparse binding)" : "in one allocation (no sparse binding)",
        !g_vk.sparse     ? "whole (no sparse binding)"
        : !(r2 || r3)    ? "whole (no sparse residency of 2D or 3D images)"
        : !g_vk.resident ? "whole (sparse residency, but holes don't read as zeros)"
        : r2 && r3       ? "with memory where it is mapped (sparse residency of 2D and 3D images)"
        : r2             ? "with memory where it is mapped if 2D, whole if 3D (sparse residency of 2D images only)"
                         : "with memory where it is mapped if 3D, whole if 2D (sparse residency of 3D images only)");
}

/* ------------------------------------------------------------ binds in batches */
/* The images with binds in a batch that hasn't gone to the queue yet. The validation layers check a sparse image's
   memory as each command is recorded, and find these unbound in the commands recorded before their batch goes, just
   ahead of the submission that runs those commands (vk_sparse_unissued, device.c's debug_cb). Images past the list's
   room are left out: the layers' messages about them then count as any */
#define UNISSUED_MAX 256
static PMutex g_unissued_lk = PMUTEX_INIT;
static VkImage g_unissued[UNISSUED_MAX];
static u32 g_nunissued;
static void unissued_add(VkImage img) {
    pm_lock(&g_unissued_lk);
    u32 i = 0;
    while (i < g_nunissued && g_unissued[i] != img) i++;
    if (i == g_nunissued && g_nunissued < UNISSUED_MAX) g_unissued[g_nunissued++] = img;
    pm_unlock(&g_unissued_lk);
}
static void unissued_remove(VkImage img) {
    pm_lock(&g_unissued_lk);
    for (u32 i = 0; i < g_nunissued; i++)
        if (g_unissued[i] == img) {
            g_unissued[i] = g_unissued[--g_nunissued];
            break;
        }
    pm_unlock(&g_unissued_lk);
}
int vk_sparse_unissued(VkImage img) {
    int found = 0;
    pm_lock(&g_unissued_lk);
    for (u32 i = 0; i < g_nunissued && !found; i++) found = g_unissued[i] == img;
    pm_unlock(&g_unissued_lk);
    return found;
}
/* the batch, on the recording thread: one vkQueueBindSparse. A batch the driver refuses ends the run: the memory its
   blocks let go is freed once the submission after it is done, and would still be the image's */
static void bind_run(const void* arg) {
    const BindHead* h = arg;
    const u8* at = (const u8*)(h + 1);
    VkSparseImageMemoryBindInfo* img = calloc(h->nimg + 1, sizeof *img);
    VkSparseImageOpaqueMemoryBindInfo* opaque = calloc(h->nopaque + 1, sizeof *opaque);
    u32 ni = 0, no = 0, binds = 0;
    if (!img || !opaque) fatal("vulkan: out of memory for a batch of %u sparse binds", h->nimg + h->nopaque);
    for (u32 k = 0; k < h->nimg + h->nopaque; k++) {
        BindOne one;
        memcpy(&one, at, sizeof one);
        at += sizeof one;
        if (one.opaque)
            opaque[no++] = (VkSparseImageOpaqueMemoryBindInfo){one.img, one.n, (const VkSparseMemoryBind*)at};
        else
            img[ni++] = (VkSparseImageMemoryBindInfo){one.img, one.n, (const VkSparseImageMemoryBind*)at};
        at += (u64)one.n * (one.opaque ? sizeof(VkSparseMemoryBind) : sizeof(VkSparseImageMemoryBind));
        binds += one.n;
    }
    VkSemaphore wait[2];
    u64 values[2];
    u32 nw = 0;
    if (h->gfx) wait[nw] = vk_gfx_timeline(), values[nw++] = h->gfx;
    if (h->after) wait[nw] = g_bind.sem, values[nw++] = h->after;
    VkTimelineSemaphoreSubmitInfo ts = {.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
                                        .waitSemaphoreValueCount = nw,
                                        .pWaitSemaphoreValues = values,
                                        .signalSemaphoreValueCount = 1,
                                        .pSignalSemaphoreValues = &h->value};
    VkBindSparseInfo bi = {.sType = VK_STRUCTURE_TYPE_BIND_SPARSE_INFO,
                           .pNext = &ts,
                           .waitSemaphoreCount = nw,
                           .pWaitSemaphores = wait,
                           .imageOpaqueBindCount = no,
                           .pImageOpaqueBinds = opaque,
                           .imageBindCount = ni,
                           .pImageBinds = img,
                           .signalSemaphoreCount = 1,
                           .pSignalSemaphores = &g_bind.sem};
    vk_queue_lock(g_vk.gfx);
    VkResult r = vkQueueBindSparse(g_vk.gfx, 1, &bi, VK_NULL_HANDLE);
    vk_queue_unlock(g_vk.gfx);
    for (u32 k = 0; k < ni; k++) unissued_remove(img[k].image);
    for (u32 k = 0; k < no; k++) unissued_remove(opaque[k].image);
    free(img);
    free(opaque);
    if (!VK_OK(r))
        fatal("vulkan: sparse bind batch %llu of %u binds failed (%d)", (unsigned long long)h->value, binds, (int)r);
}

static VmaAllocation* bind_gone(void) { return (VmaAllocation*)(void*)(g_bind.buf + BIND_CAP); }
static void gone_free(void* a) {
    vk_mem_untrack(a);
    vmaFreeMemory(g_vk.vma, a);
}

/* the batch to the graphics queue, after the work submitted before it when it changes blocks of an image in use. The
   memory its blocks let go waits in the frame being recorded, whose next submission waits for the batch */
static void bind_flush(void) {
    if (!g_bind.used && !g_bind.ngone) return; /* at every submission */
    vk_cmd(); /* that frame begun first: beginning it may destroy images (vk_image_destroy), and their binds leave */
    if (g_bind.used) {
        BindHead* h = (BindHead*)(void*)g_bind.buf;
        h->gfx = g_bind.readers ? vk_last_submit() : 0;
        h->after = g_bind.issued;
        h->value = ++g_bind.issued;
        vk_rec_call(bind_run, g_bind.buf, g_bind.used);
    }
    for (u32 i = 0; i < g_bind.ngone; i++) vk_defer_call(gone_free, bind_gone()[i]);
    vk_defer_bytes(g_bind.gone_bytes);
    g_bind.used = g_bind.last = 0;
    g_bind.ngone = 0;
    g_bind.gone_bytes = 0;
    g_bind.readers = 0;
}

int vk_sparse_flush(VkSemaphoreSubmitInfo* wait) {
    bind_flush();
    if (g_bind.waited == g_bind.issued) return 0;
    g_bind.waited = g_bind.issued;
    *wait = (VkSemaphoreSubmitInfo){.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
                                    .semaphore = g_bind.sem,
                                    .value = g_bind.issued,
                                    .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT};
    return 1;
}

/* 0 when there is no memory for a batch */
static int bind_ready(void) {
    if (!g_bind.buf) g_bind.buf = malloc(BIND_CAP + GONE_CAP * sizeof(VmaAllocation));
    return g_bind.buf != NULL;
}
/* one bind of img into the batch, after the others of img: a VkSparseMemoryBind when opaque, else a
   VkSparseImageMemoryBind, each bytes. gone: the memory it lets go (VK_NULL_HANDLE none), size bytes, freed after
   the batch. readers: the batch waits for the work submitted before it. The batch it went into */
static u64 bind_add(VkImage img, int opaque, const void* bind, u64 each, VmaAllocation gone, u64 size, int readers) {
    if (g_bind.used + sizeof(BindOne) + each > BIND_CAP || g_bind.ngone == GONE_CAP) bind_flush();
    unissued_add(img);
    if (!g_bind.used) {
        memset(g_bind.buf, 0, sizeof(BindHead));
        g_bind.used = sizeof(BindHead);
    }
    BindHead* h = (BindHead*)(void*)g_bind.buf;
    BindOne one;
    if (g_bind.last) memcpy(&one, g_bind.buf + g_bind.last, sizeof one);
    if (!g_bind.last || one.img != img || one.opaque != (u32)opaque) {
        one = (BindOne){img, 0, (u32)opaque};
        g_bind.last = g_bind.used;
        g_bind.used += sizeof one;
        if (opaque)
            h->nopaque++;
        else
            h->nimg++;
    }
    one.n++;
    memcpy(g_bind.buf + g_bind.last, &one, sizeof one);
    memcpy(g_bind.buf + g_bind.used, bind, each);
    g_bind.used += each;
    if (gone) {
        bind_gone()[g_bind.ngone++] = gone;
        g_bind.gone_bytes += size;
    }
    g_bind.readers |= readers;
    return g_bind.issued + 1;
}

/* an image going: its binds still in the batch leave it (the memory they let go is still freed after the batch). One
   whose batch went to the queue is destroyed once the frames' work is done (vk_defer_image), the batch with it */
static void bind_drop(const Pieces* p) {
    if (p->batch <= g_bind.issued || !g_bind.used) return;
    BindHead* h = (BindHead*)(void*)g_bind.buf;
    u64 at = sizeof *h, to = at;
    while (at < g_bind.used) {
        BindOne one;
        memcpy(&one, g_bind.buf + at, sizeof one);
        u64 len = sizeof one + (u64)one.n * (one.opaque ? sizeof(VkSparseMemoryBind) : sizeof(VkSparseImageMemoryBind));
        if (one.img != p->img) {
            memmove(g_bind.buf + to, g_bind.buf + at, len);
            to += len;
        } else if (one.opaque)
            h->nopaque--;
        else
            h->nimg--;
        at += len;
    }
    g_bind.used = h->nimg + h->nopaque ? to : 0;
    g_bind.last = 0; /* the next bind starts an entry of its own */
}

static int pieces_bind(VkImage img, const VkMemoryRequirements* req, Pieces* p, VkResult* res) {
    u64 chunk = g_vk.alloc_cap / req->alignment * req->alignment;
    if (!chunk) return 0;
    if (!bind_ready()) {
        *res = VK_ERROR_OUT_OF_HOST_MEMORY;
        return 0;
    }
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
    /* a new image: nothing reads it yet, and its commands are recorded after this, so the batch waits for no work */
    for (u32 i = 0; i < n && ok; i++) p->batch = bind_add(img, 1, &binds[i], sizeof *binds, VK_NULL_HANDLE, 0, 0);
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
/* a level's texels along one dimension of v at level 0 */
static u32 level_dim(u32 v, u32 level) { return v >> level ? v >> level : 1; }

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
        ok = ok && vk_wait_fence(fence);
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
    unissued_remove(img); /* its binds leave the batch (bind_drop), or went with it already */
    for (u32 i = 0; i < g_npieces; i++)
        if (g_pieces[i]->img == img) {
            bind_drop(g_pieces[i]);
            vkDestroyImage(g_vk.dev, img, NULL);
            pieces_free(g_pieces[i]);
            g_pieces[i] = g_pieces[--g_npieces];
            return;
        }
    if (sparse_destroy(img)) return;
    vkDestroyImage(g_vk.dev, img, NULL); /* made without memory */
}

/* ------------------------------------------------------------ memory only where it is bound */
int vk_resident_shape(const VkImageCreateInfo* ii, VkResidentShape* s) {
    VkImageCreateInfo si = *ii;
    VkImageFormatProperties ip;
    VkSparseImageFormatProperties fp[2];
    VkSparseImageMemoryRequirements2 sr[2] = {{.sType = VK_STRUCTURE_TYPE_SPARSE_IMAGE_MEMORY_REQUIREMENTS_2},
                                              {.sType = VK_STRUCTURE_TYPE_SPARSE_IMAGE_MEMORY_REQUIREMENTS_2}};
    u32 n = 2, nsr = 2;
    const int can = ii->imageType == VK_IMAGE_TYPE_3D   ? g_vk.feat.sparseResidencyImage3D
                    : ii->imageType == VK_IMAGE_TYPE_2D ? g_vk.feat.sparseResidencyImage2D
                                                        : 0;
    if (!g_vk.resident || !can || ii->samples != VK_SAMPLE_COUNT_1_BIT || ii->tiling != VK_IMAGE_TILING_OPTIMAL)
        return 0;
    si.flags |= VK_IMAGE_CREATE_SPARSE_BINDING_BIT | VK_IMAGE_CREATE_SPARSE_RESIDENCY_BIT;
    vkGetPhysicalDeviceSparseImageFormatProperties(g_vk.phys, ii->format, ii->imageType, ii->samples, ii->usage,
                                                   ii->tiling, &n, fp);
    /* one aspect, color: a metadata aspect would be bound apart */
    if (n != 1 || fp[0].aspectMask != VK_IMAGE_ASPECT_COLOR_BIT ||
        vkGetPhysicalDeviceImageFormatProperties(g_vk.phys, ii->format, ii->imageType, ii->tiling, ii->usage, si.flags,
                                                 &ip) != VK_SUCCESS ||
        ip.maxMipLevels < ii->mipLevels || ip.maxArrayLayers < ii->arrayLayers ||
        ip.maxExtent.width < ii->extent.width || ip.maxExtent.height < ii->extent.height ||
        ip.maxExtent.depth < ii->extent.depth)
        return 0;
    VkDeviceImageMemoryRequirements q = {.sType = VK_STRUCTURE_TYPE_DEVICE_IMAGE_MEMORY_REQUIREMENTS,
                                         .pCreateInfo = &si};
    vkGetDeviceImageSparseMemoryRequirements(g_vk.dev, &q, &nsr, sr);
    const VkSparseImageMemoryRequirements* r = &sr[0].memoryRequirements;
    if (nsr != 1 || r->formatProperties.aspectMask != VK_IMAGE_ASPECT_COLOR_BIT) return 0;
    s->block = r->formatProperties.imageGranularity;
    s->tail_first = r->imageMipTailFirstLod < ii->mipLevels ? r->imageMipTailFirstLod : ii->mipLevels;
    s->tails = s->tail_first == ii->mipLevels                                          ? 0
               : r->formatProperties.flags & VK_SPARSE_IMAGE_FORMAT_SINGLE_MIPTAIL_BIT ? 1
                                                                                       : ii->arrayLayers;
    return s->block.width && s->block.height && s->block.depth;
}

/* the blocks of one layer of an image of extent, in blocks of g texels, in its levels before tail_first */
static u64 layer_blocks(VkExtent3D extent, VkExtent3D g, u32 tail_first) {
    u64 n = 0;
    for (u32 l = 0; l < tail_first; l++)
        n += (u64)((level_dim(extent.width, l) + g.width - 1) / g.width) *
             ((level_dim(extent.height, l) + g.height - 1) / g.height) *
             ((level_dim(extent.depth, l) + g.depth - 1) / g.depth);
    return n;
}

int vk_image_create_resident(const VkImageCreateInfo* ii, const VkResidentShape* s, VkImage* img) {
    VkImageCreateInfo si = *ii;
    VkMemoryRequirements req;
    VkSparseImageMemoryRequirements sr[2] = {0};
    u32 nsr = 2;
    *img = VK_NULL_HANDLE;
    if (!g_vk.resident) return 0;
    si.flags |= VK_IMAGE_CREATE_SPARSE_BINDING_BIT | VK_IMAGE_CREATE_SPARSE_RESIDENCY_BIT;
    if (!VK_OK(vkCreateImage(g_vk.dev, &si, NULL, img))) return 0;
    vkGetImageMemoryRequirements(g_vk.dev, *img, &req);
    vkGetImageSparseMemoryRequirements(g_vk.dev, *img, &nsr, sr);
    /* the shape it was asked for: its blocks, its levels in them, its mip tails */
    const VkExtent3D g = sr[0].formatProperties.imageGranularity;
    const u32 tail_first = sr[0].imageMipTailFirstLod < ii->mipLevels ? sr[0].imageMipTailFirstLod : ii->mipLevels;
    const u32 tails = tail_first == ii->mipLevels                                                ? 0
                      : sr[0].formatProperties.flags & VK_SPARSE_IMAGE_FORMAT_SINGLE_MIPTAIL_BIT ? 1
                                                                                                 : ii->arrayLayers;
    const u64 per_layer = g.width && g.height && g.depth ? layer_blocks(ii->extent, g, tail_first) : 0;
    const u64 n = per_layer * ii->arrayLayers + tails;
    Pieces* p = NULL;
    if (nsr == 1 && sr[0].formatProperties.aspectMask == VK_IMAGE_ASPECT_COLOR_BIT && g.width == s->block.width &&
        g.height == s->block.height && g.depth == s->block.depth && tail_first == s->tail_first && tails == s->tails &&
        per_layer && req.alignment && (!tails || sr[0].imageMipTailSize) && n <= UINT32_MAX)
        p = calloc(1, sizeof *p + n * sizeof p->pages[0]);
    if (!p || !pieces_room()) {
        WARN("vulkan: a %ux%ux%u image of %u levels and %u layers with memory where it is bound has %u aspects, blocks "
             "of %ux%ux%u texels (%ux%ux%u asked for), its mip tail from level %u (%u asked for) and %u of them (%u "
             "asked for): it is made whole\n",
             ii->extent.width, ii->extent.height, ii->extent.depth, ii->mipLevels, ii->arrayLayers, nsr, g.width,
             g.height, g.depth, s->block.width, s->block.height, s->block.depth, tail_first, s->tail_first, tails,
             s->tails);
        vkDestroyImage(g_vk.dev, *img, NULL);
        *img = VK_NULL_HANDLE;
        free(p);
        return 0;
    }
    p->img = *img;
    p->n = (u32)n;
    p->per_layer = (u32)per_layer;
    p->layers = ii->arrayLayers;
    p->tail_first = tail_first;
    p->tails = tails;
    p->block = g;
    p->extent = ii->extent;
    p->req = req;
    p->tail_offset = sr[0].imageMipTailOffset;
    p->tail_size = sr[0].imageMipTailSize;
    p->tail_stride = sr[0].imageMipTailStride;
    g_pieces[g_npieces++] = p;
    LOG_LIMIT(8,
              "vulkan: a %ux%ux%u image of %u levels and %u layers, %llu KiB, with memory where it is bound: %u blocks "
              "of %ux%ux%u texels a layer, %u mip tails of %llu KiB from level %u\n",
              ii->extent.width, ii->extent.height, ii->extent.depth, ii->mipLevels, ii->arrayLayers,
              (unsigned long long)(req.size >> 10), p->per_layer, g.width, g.height, g.depth, tails,
              (unsigned long long)(p->tail_size >> 10), tail_first);
    return 1;
}

/* page i of p bound to memory of mem's size where want[i] is set (then 2: the memory is new to it, its texels
   undefined), released where it isn't; the memory it binds in *memory at *offset, the memory it lets go in *had */
static VkResult page_change(Pieces* p, u32 i, u8* want, const VkMemoryRequirements* mem, VkDeviceMemory* memory,
                            VkDeviceSize* offset, VmaAllocation* had) {
    const VmaAllocationCreateInfo aci = {.preferredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT};
    VmaAllocationInfo info;
    *had = p->pages[i];
    if (!want[i]) {
        p->pages[i] = VK_NULL_HANDLE;
        p->bound -= mem->size;
        return VK_SUCCESS;
    }
    *had = VK_NULL_HANDLE;
    VkResult res = vmaAllocateMemory(g_vk.vma, mem, &aci, &p->pages[i], &info);
    if (!VK_OK(res)) return res;
    vk_mem_track(p->pages[i]);
    *memory = info.deviceMemory;
    *offset = info.offset;
    p->bound += mem->size;
    want[i] = 2;
    return VK_SUCCESS;
}

int vk_image_resident(VkImage img, u8* want, u32 n, u64* bytes) {
    Pieces* p = pieces_of(img);
    *bytes = p ? p->bound : 0;
    if (!p || !p->per_layer || n != p->n) return 0;
    u32 changes = 0;
    for (u32 i = 0; i < n; i++) changes += !want[i] != !p->pages[i];
    if (!changes) return 1;
    if (!bind_ready()) return 0;
    /* the commands recorded so far read the blocks as they are: submitted, so the batch can wait for them. One of
       this image's binds still in the batch goes before, as a batch may bind a block only once */
    vk_submit_recorded();
    if (p->batch > g_bind.issued) bind_flush();
    const VkExtent3D g = p->block;
    const VkMemoryRequirements one = {p->req.alignment, p->req.alignment, p->req.memoryTypeBits},
                               tail = {p->tail_size, p->req.alignment, p->req.memoryTypeBits};
    VkResult res = VK_SUCCESS;
    VmaAllocation had;
    u32 nb = 0, i = 0;
    for (u32 layer = 0; layer < p->layers && VK_OK(res); layer++)
        for (u32 l = 0; l < p->tail_first && VK_OK(res); l++) {
            const u32 w = level_dim(p->extent.width, l), h = level_dim(p->extent.height, l),
                      d = level_dim(p->extent.depth, l);
            for (u32 z = 0; z < d && VK_OK(res); z += g.depth)
                for (u32 y = 0; y < h && VK_OK(res); y += g.height)
                    for (u32 x = 0; x < w && VK_OK(res); x += g.width, i++) {
                        if (!want[i] == !p->pages[i]) continue;
                        VkSparseImageMemoryBind b = {.subresource = {VK_IMAGE_ASPECT_COLOR_BIT, l, layer},
                                                     .offset = {(i32)x, (i32)y, (i32)z},
                                                     .extent = {w - x < g.width ? w - x : g.width,
                                                                h - y < g.height ? h - y : g.height,
                                                                d - z < g.depth ? d - z : g.depth}};
                        res = page_change(p, i, want, &one, &b.memory, &b.memoryOffset, &had);
                        if (!VK_OK(res)) break;
                        p->batch = bind_add(img, 0, &b, sizeof b, had, one.size, 1);
                        nb++;
                    }
        }
    /* the mip tails, each bound whole: one range of the image's memory a tail */
    for (u32 t = 0; t < p->tails && VK_OK(res); t++, i++) {
        if (!want[i] == !p->pages[i]) continue;
        VkSparseMemoryBind b = {.resourceOffset = p->tail_offset + t * p->tail_stride, .size = p->tail_size};
        res = page_change(p, i, want, &tail, &b.memory, &b.memoryOffset, &had);
        if (!VK_OK(res)) break;
        p->batch = bind_add(img, 1, &b, sizeof b, had, tail.size, 1);
        nb++;
    }
    if (res != VK_SUCCESS)
        WARN("vulkan: %u of %u blocks and mip tails of a %ux%ux%u image of %u layers with memory where it is bound "
             "changed (out of memory): %llu KiB bound\n",
             nb, changes, p->extent.width, p->extent.height, p->extent.depth, p->layers,
             (unsigned long long)(p->bound >> 10));
    *bytes = p->bound;
    return res == VK_SUCCESS;
}

/* ------------------------------------------------------------ the budget */
/* The heap images go to: the biggest device-local one (the only one on a GPU sharing system memory). The BAR window,
   device local too, holds the staging rings, which keep within its budget themselves (cmd.c ring_misplaced) */
static int g_heap = -1;
static u64 g_budget_cap; /* vk_budget_mb, 0 when unset */
#if defined(__ANDROID__)
/* where the GPU shares the system's memory, the budget vk_uma_budget gives, measured once a second and held between
   measures: an eviction leaves a hole in a VMA block or waits for a frame in flight before the system gets its memory
   back, and a budget measured again at every trim would take that for room still missing and evict again each
   present */
static struct {
    u64 reserve; /* 0: the driver's budget alone */
    u64 ns;      /* when it was measured, 0 never */
    u64 budget;
} g_uma_budget;
#endif

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
#if defined(__ANDROID__)
    if (g_vk.mem_model == VK_MEM_UMA) {
        long pages = sysconf(_SC_PHYS_PAGES), page = sysconf(_SC_PAGESIZE);
        VmaBudget b[VK_MAX_MEMORY_HEAPS];
        vmaGetHeapBudgets(g_vk.vma, b);
        if (pages > 0 && page > 0) {
            u64 system = (u64)pages * (u64)page;
            g_uma_budget.reserve = vk_uma_reserve(system);
            LOG("vulkan: heap %d is the system's memory: the caches leave %llu MB of its %llu to the rest of the "
                "system, within the driver's budget of %llu MB\n",
                g_heap, (unsigned long long)(g_uma_budget.reserve >> 20), (unsigned long long)(system >> 20),
                (unsigned long long)(b[g_heap].budget >> 20));
        } else
            WARN("vulkan: heap %d is the system's memory, of a size sysconf doesn't tell (%ld pages of %ld bytes): the "
                 "caches keep within the driver's %llu MB\n",
                 g_heap, pages, page, (unsigned long long)(b[g_heap].budget >> 20));
    }
#endif
}

int vk_mem_heap(void) {
    if (g_heap < 0) heap_find();
    return g_heap;
}

/* Without the extension VMA's numbers are its own guess, not the driver's: its blocks for the use, 80% of the heap's
   size for the budget. The stats box shows ? for them then, as for any value the device does not tell */
void vk_mem_driver_use(u64* usage, u64* budget) {
    VmaBudget b[VK_MAX_MEMORY_HEAPS];
    int heap = vk_mem_heap();
    *usage = *budget = 0;
    if (!g_vk.has_budget) return;
    vmaGetHeapBudgets(g_vk.vma, b); /* the driver's, asked at frame_recycle's vmaSetCurrentFrameIndex */
    *usage = b[heap].usage;
    *budget = b[heap].budget;
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
#if defined(__ANDROID__)
    if (g_uma_budget.reserve) {
        u64 now = plat_ns();
        if (!g_uma_budget.ns || now - g_uma_budget.ns >= 1000000000ull) {
            SnxU64 available;
            if (!snx_memory_available(&available)) {
                WARN("vulkan: the system no longer tells the memory it can still give: the caches keep within %llu "
                     "MB, the driver's budget or vk_budget_mb\n",
                     (unsigned long long)(*budget >> 20));
                g_uma_budget.reserve = 0;
                return;
            }
            g_uma_budget.ns = now;
            g_uma_budget.budget = vk_uma_budget(b[g_heap].budget, *usage, available, g_uma_budget.reserve);
        }
        if (g_uma_budget.budget < *budget) *budget = g_uma_budget.budget;
    }
#endif
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
