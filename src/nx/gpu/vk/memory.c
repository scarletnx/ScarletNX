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
#include "nx/gpu/vk/ledger.h"
#include "nx/gpu/vk/resource_stats.h"

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
    u64 bound; /* the bytes of its bound blocks and tails: each allocation once */
    u64 batch; /* the bind batch its last binds are in, 0 none */
    /* an image with memory where it is bound, made so that its blocks can share memory (the device's
       sparseResidencyAliased): key[i] is what page i shows as its binder was told (vk_image_resident), 0 for a page
       like no other, and the pages with one key have one allocation. NULL: each page its own */
    u64* key;
    DevMem pages[];
} Pieces;
/* a block or a page and the key of what it shows, to find the ones of a key by */
typedef struct {
    u64 key;
    u32 block;
} KeyBlock;
static int key_block_cmp(const void* a, const void* b) {
    const KeyBlock *x = a, *y = b;
    return x->key < y->key ? -1 : x->key > y->key ? 1 : x->block < y->block ? -1 : x->block > y->block;
}
/* the first of sorted v's n entries with key, or n */
static u32 key_find(const KeyBlock* v, u32 n, u64 key) {
    u32 lo = 0, hi = n;
    while (lo < hi) {
        u32 mid = lo + (hi - lo) / 2;
        if (v[mid].key < key)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo < n && v[lo].key == key ? lo : n;
}
static Pieces** g_pieces;
static u32 g_npieces, g_cappieces;

/* The batch as the recording thread takes it: its head, then each image's BindOne and binds, VkSparseMemoryBind for
   an image in pieces, VkSparseImageMemoryBind for blocks */
typedef struct {
    u64 gfx;   /* the graphics timeline value it waits for, 0 none */
    u64 after; /* the batch before it, 0 none */
    u64 value; /* its own on the bind timeline */
    u32 nimg, nopaque, nbuf; /* its entries of each kind */
} BindHead;
/* kind: BIND_BLOCKS an image's blocks (VkSparseImageMemoryBind each), BIND_OPAQUE an image in pieces and BIND_BUFFER
   a buffer's ranges (VkSparseMemoryBind each) */
enum { BIND_BLOCKS, BIND_OPAQUE, BIND_BUFFER };
typedef struct {
    union {
        VkImage img;
        VkBuffer buf;
    } of;
    u32 n, kind;
} BindOne;
static u64 bind_each(u32 kind) {
    return kind == BIND_BLOCKS ? sizeof(VkSparseImageMemoryBind) : sizeof(VkSparseMemoryBind);
}
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

/* An allocation of size bytes of system memory, no bigger than g_vk.alloc_cap, found none while its heap had the
   room: the driver gives less in one allocation than the device's limit says, and from now on none is bigger than a
   sixteenth of the cap under what failed. 1 when the cap came down, and the caller asks again within it.

   NVIDIA's driver for a GT 1030 (2 GB, a window of 246 MiB) states a limit of 4 GiB and gives 240 MiB of system memory
   or of the window at once, not 256, with 18 GB of that heap budgeted (the GPU's own memory gives 1 GiB at once). A
   compute arena that had grown to 256 MiB for one dispatch's buffers found no memory, and the dispatches after it were
   skipped. Asking the driver at the start would find the limit before any failure, but the answer is an allocation of
   that size made and freed: 520 ms there, on every start of every device */
int vk_alloc_cap_lower(u64 size) {
    const VkPhysicalDeviceMemoryProperties* mp;
    DevMemBudget b[VK_MAX_MEMORY_HEAPS];
    const u64 least = 16ull << 20, step = ALLOC_CAP_DEFAULT / 16;
    if (size > g_vk.alloc_cap || size <= least || g_vk.mem_model == VK_MEM_UMA) return 0;
    mp = vk_devmem_props();
    vk_devmem_budgets(b);
    u32 h = 0;
    while (h < mp->memoryHeapCount && (mp->memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)) h++;
    if (h == mp->memoryHeapCount || b[h].usage + size > b[h].budget) return 0; /* no such heap, or it is full */
    u64 cap = size > least + step ? size - step : least;
    LOG("vulkan: the driver had no %llu MiB of system memory in one allocation, with %llu of %llu MiB of the heap in "
        "use: no allocation is bigger than %llu MiB from now on\n",
        (unsigned long long)(size >> 20), (unsigned long long)(b[h].usage >> 20),
        (unsigned long long)(b[h].budget >> 20), (unsigned long long)(cap >> 20));
    g_vk.alloc_cap = cap;
    return 1;
}

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
    VkSparseBufferMemoryBindInfo* buf = calloc(h->nbuf + 1, sizeof *buf);
    u32 ni = 0, no = 0, nb = 0, binds = 0;
    if (!img || !opaque || !buf)
        fatal("vulkan: out of memory for a batch of %u sparse binds", h->nimg + h->nopaque + h->nbuf);
    for (u32 k = 0; k < h->nimg + h->nopaque + h->nbuf; k++) {
        BindOne one;
        memcpy(&one, at, sizeof one);
        at += sizeof one;
        if (one.kind == BIND_BUFFER)
            buf[nb++] = (VkSparseBufferMemoryBindInfo){one.of.buf, one.n, (const VkSparseMemoryBind*)at};
        else if (one.kind == BIND_OPAQUE)
            opaque[no++] = (VkSparseImageOpaqueMemoryBindInfo){one.of.img, one.n, (const VkSparseMemoryBind*)at};
        else
            img[ni++] = (VkSparseImageMemoryBindInfo){one.of.img, one.n, (const VkSparseImageMemoryBind*)at};
        at += (u64)one.n * bind_each(one.kind);
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
                           .bufferBindCount = nb,
                           .pBufferBinds = buf,
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
    free(buf);
    if (!VK_OK(r))
        fatal("vulkan: sparse bind batch %llu of %u binds failed (%d)", (unsigned long long)h->value, binds, (int)r);
}

static DevMem* bind_gone(void) { return (DevMem*)(void*)(g_bind.buf + BIND_CAP); }
static void gone_free(void* a) {
    DevMem m = (DevMem)(uintptr_t)a;
    vk_mem_untrack(m);
    vk_devmem_free(m);
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
    for (u32 i = 0; i < g_bind.ngone; i++) vk_defer_call(gone_free, (void*)(uintptr_t)bind_gone()[i]);
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
    if (!g_bind.buf) g_bind.buf = malloc(BIND_CAP + GONE_CAP * sizeof(DevMem));
    return g_bind.buf != NULL;
}
/* one bind of img into the batch, after the others of img: a VkSparseMemoryBind when opaque, else a
   VkSparseImageMemoryBind, each bytes. gone: the memory it lets go (VK_NULL_HANDLE none), size bytes, freed after
   the batch. readers: the batch waits for the work submitted before it. The batch it went into */
static u64 bind_put(BindOne key, const void* bind, DevMem gone, u64 size, int readers) {
    u64 each = bind_each(key.kind);
    if (g_bind.used + sizeof(BindOne) + each > BIND_CAP || g_bind.ngone == GONE_CAP) bind_flush();
    if (key.kind != BIND_BUFFER) unissued_add(key.of.img);
    if (!g_bind.used) {
        memset(g_bind.buf, 0, sizeof(BindHead));
        g_bind.used = sizeof(BindHead);
    }
    BindHead* h = (BindHead*)(void*)g_bind.buf;
    BindOne one;
    if (g_bind.last) memcpy(&one, g_bind.buf + g_bind.last, sizeof one);
    /* the bytes compared: a key is made zeroed, and a buffer's handle is as wide as an image's */
    if (!g_bind.last || one.kind != key.kind || memcmp(&one.of, &key.of, sizeof one.of)) {
        one = key;
        one.n = 0;
        g_bind.last = g_bind.used;
        g_bind.used += sizeof one;
        if (key.kind == BIND_BUFFER)
            h->nbuf++;
        else if (key.kind == BIND_OPAQUE)
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

static u64 bind_add(VkImage img, int opaque, const void* bind, u64 each, DevMem gone, u64 size, int readers) {
    BindOne key;
    memset(&key, 0, sizeof key); /* compared by its bytes (bind_put) */
    key.of.img = img;
    key.kind = opaque ? BIND_OPAQUE : BIND_BLOCKS;
    (void)each;
    return bind_put(key, bind, gone, size, readers);
}
u64 vk_buffer_sparse_bind(VkBuffer buf, const VkSparseMemoryBind* bind, DevMem gone, u64 size, int readers) {
    if (!bind_ready()) return 0;
    BindOne key;
    memset(&key, 0, sizeof key);
    key.of.buf = buf;
    key.kind = BIND_BUFFER;
    return bind_put(key, bind, gone, size, readers);
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
        u64 len = sizeof one + (u64)one.n * bind_each(one.kind);
        if (one.kind == BIND_BUFFER || one.of.img != p->img) {
            memmove(g_bind.buf + to, g_bind.buf + at, len);
            to += len;
        } else if (one.kind == BIND_OPAQUE)
            h->nopaque--;
        else
            h->nimg--;
        at += len;
    }
    g_bind.used = h->nimg + h->nopaque + h->nbuf ? to : 0;
    g_bind.last = 0; /* the next bind starts an entry of its own */
}

static int pieces_bind(VkImage img, const VkMemoryRequirements* req, Pieces* p, u32 owner, VkResult* res) {
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
        DevMemWant aci = {.at = DM_DEVICE, .own = 1, .image = 1};
        DevMemInfo info;
        *res = vk_devmem_alloc(&piece, &aci, &p->pages[i], &info);
        if (!VK_OK(*res)) {
            ok = 0;
            break;
        }
        p->n++;
        vk_mem_track(p->pages[i], owner);
        binds[i] = (VkSparseMemoryBind){
            .resourceOffset = at, .size = size, .memory = info.memory, .memoryOffset = info.offset};
    }
    /* a new image: nothing reads it yet, and its commands are recorded after this, so the batch waits for no work */
    for (u32 i = 0; i < n && ok; i++) p->batch = bind_add(img, 1, &binds[i], sizeof *binds, 0, 0, 0);
    free(binds);
    return ok;
}

static void pieces_free(Pieces* p) {
    for (u32 i = 0; i < p->n; i++) {
        if (!p->pages[i]) continue;
        /* an allocation its pages share goes once, with the first of them */
        if (p->key && p->key[i]) {
            u32 j = 0;
            while (j < i && !(p->pages[j] && p->key[j] == p->key[i])) j++;
            if (j < i) continue;
        }
        vk_mem_untrack(p->pages[i]);
        vk_devmem_free(p->pages[i]);
    }
    free(p->key);
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
static int image_in_pieces(const VkImageCreateInfo* ii, VkImage* img, u32 owner, VkResult* res) {
    VkImageCreateInfo si = *ii;
    si.flags |= VK_IMAGE_CREATE_SPARSE_BINDING_BIT;
    *res = vkCreateImage(g_vk.dev, &si, NULL, img);
    if (!VK_OK(*res)) return 0;
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(g_vk.dev, *img, &req);
    u64 chunk = req.alignment ? g_vk.alloc_cap / req.alignment * req.alignment : 0;
    u32 n = chunk ? (u32)((req.size + chunk - 1) / chunk) : 0;
    Pieces* p = n ? calloc(1, sizeof *p + n * sizeof p->pages[0]) : NULL;
    if (!p || !pieces_room() || !pieces_bind(*img, &req, p, owner, res)) {
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

int vk_image_create(const VkImageCreateInfo* ii, VkImage* img, DevMem* mem) {
    VkResult res;
    return vk_image_create_r(ii, img, mem, VK_OWN_IMAGE_OTHER, &res);
}
static int image_create_r(const VkImageCreateInfo* ii, VkImage* img, DevMem* mem, u32 owner, VkResult* res) {
    *res = VK_ERROR_UNKNOWN;
    *img = VK_NULL_HANDLE;
    *mem = 0;
    VkDeviceImageMemoryRequirements q = {.sType = VK_STRUCTURE_TYPE_DEVICE_IMAGE_MEMORY_REQUIREMENTS,
                                         .pCreateInfo = ii};
    VkMemoryRequirements2 req = {.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2};
    VkResourceScope query = vk_resource_begin();
    vkGetDeviceImageMemoryRequirements(g_vk.dev, &q, &req);
    vk_resource_end(VK_RESOURCE_IMAGE_QUERY, query);
    if (req.memoryRequirements.size > g_vk.alloc_cap) {
        if (g_vk.sparse) return image_in_pieces(ii, img, owner, res);
        /* without sparse binding the one allocation is the only way: the driver may still take it */
        LOG_LIMIT(4, "vulkan: a %llu MiB image in one allocation, past the %llu MiB cap\n",
                  (unsigned long long)(req.memoryRequirements.size >> 20), (unsigned long long)(g_vk.alloc_cap >> 20));
    }
    DevMemWant aci = {.at = DM_DEVICE};
    *res = vk_devmem_image(ii, &aci, img, mem);
    if (VK_OK(*res)) vk_mem_track(*mem, owner);
    return VK_OK(*res);
}
int vk_image_create_r(const VkImageCreateInfo* ii, VkImage* img, DevMem* mem, u32 owner, VkResult* res) {
    VkResourceScope scope = vk_resource_begin();
    int result = image_create_r(ii, img, mem, owner, res);
    vk_resource_end(VK_RESOURCE_IMAGE_CREATE, scope);
    return result;
}

/* ------------------------------------------------------------ sparse residency */
/* A texture that guest memory maps only in part (a game's sparse texture: its pool is virtual, and the game maps the
   64 KiB pages of the regions it streams in) takes memory only where guest memory has pages. Its image has sparse
   residency: the mip tail is bound always, and each sparse block of the other levels (64 KiB of the host's texels) is
   bound when guest memory maps a page of its texels, and released when it maps none. A block without memory reads as
   zero (residencyNonResidentStrict) or undefined, as the console's unmapped sparse pages read zero, and writes to it
   are discarded.

   Each block's memory is an allocation of its own, which devmem.c takes from its blocks of device memory. A bind is
   waited for on the host, as pieces_bind does: the commands recorded after it are submitted after it is done. A release unbinds at
   once, and its memory goes when the frame being recorded is done: the frames in flight can still read through the old
   binding.

   Blocks that show the same bytes share one allocation, where the device binds memory to more than one place of an
   image (sparseResidencyAliased). The caller gives each block a key for what it shows (texture.c: the guest memory
   under its texels and where in the block each piece lies), and blocks with one key bind the same memory. One game
   maps the tiles of its sparse textures to the same pages of its memory again and again: in a race 476,722 pages of
   14 textures' GPU addresses showed 230,706 pages of memory, and the blocks held 3.5 GB of a 5.9 GB budget, the
   same texels twice or more in half of them, while the cache evicted and made again ten thousand textures for room.
   What goes up to a block goes to every block of its key: the same bytes, as they show the same memory */
#define SPARSE_MIN (4ull << 20) /* a smaller image is made whole: the memory it can save is small */
typedef struct {
    VkImage img;
    VkSparseShape sh;
    VkExtent3D extent; /* level 0's */
    u32 tails;         /* mip tails: 0, 1 (one for all the layers) or one for each layer */
    u64 block;         /* the bytes of one sparse block */
    u64 tail_offset, tail_size, tail_stride;
    u32 memtypes;
    u64 bytes; /* the memory bound now: each allocation once */
    int alias; /* made with VK_IMAGE_CREATE_SPARSE_ALIASED_BIT: blocks of one key share memory */
    /* the blocks' allocations: of[b] is block b's, its index in mem and one more, 0 for none. free_mem: the first
       free record and one more (next goes on), 0 for none */
    struct SparseMem {
        DevMem a;
        u64 key;  /* what its blocks show, 0: one block's own, which no other joins */
        u32 refs; /* the blocks bound to it, 0: free */
        u32 next;
    } * mem;
    u32 nmem, capmem, free_mem;
    u32* of;
    DevMem slot[]; /* the blocks' memory (VkSparseShape's order), then the tails' */
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

/* the device also makes that image with its blocks bound to memory that other blocks of it are bound to */
static int sparse_alias_supported(const VkImageCreateInfo* ii) {
    static SparseAnswer memo[32];
    static u32 nmemo;
    if (!g_vk.feat.sparseResidencyAliased || !g_settings.sparse_texture_sharing) return 0;
    for (u32 i = 0; i < nmemo; i++)
        if (memo[i].f == ii->format && memo[i].type == ii->imageType && memo[i].usage == ii->usage &&
            memo[i].flags == ii->flags)
            return memo[i].ok;
    VkImageFormatProperties ip;
    int ok = vkGetPhysicalDeviceImageFormatProperties(g_vk.phys, ii->format, ii->imageType, ii->tiling, ii->usage,
                                                      ii->flags | VK_IMAGE_CREATE_SPARSE_BINDING_BIT |
                                                          VK_IMAGE_CREATE_SPARSE_RESIDENCY_BIT |
                                                          VK_IMAGE_CREATE_SPARSE_ALIASED_BIT,
                                                      &ip) == VK_SUCCESS;
    if (nmemo < sizeof memo / sizeof memo[0])
        memo[nmemo++] = (SparseAnswer){ii->format, ii->imageType, ii->usage, ii->flags, ok};
    return ok;
}

int vk_image_create_sparse(const VkImageCreateInfo* ii, VkImage* img) {
    VkImageCreateInfo si = *ii;
    *img = VK_NULL_HANDLE;
    if (!vk_sparse_supported(ii)) return 0;
    const int alias = sparse_alias_supported(ii);
    si.flags |= VK_IMAGE_CREATE_SPARSE_BINDING_BIT | VK_IMAGE_CREATE_SPARSE_RESIDENCY_BIT |
                (alias ? VK_IMAGE_CREATE_SPARSE_ALIASED_BIT : 0);
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
                .alias = alias,
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
        if (s && !(h.of = calloc(sparse_nblocks(&h) + 1, sizeof *h.of))) {
            free(s);
            s = NULL;
        }
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

/* the memory a release unbound, freed once the frames that could read it are done */
typedef struct {
    u32 n;
    DevMem a[];
} SparseDead;
static void sparse_dead_free(void* arg) {
    SparseDead* d = arg;
    for (u32 i = 0; i < d->n; i++) {
        vk_mem_untrack(d->a[i]);
        vk_devmem_free(d->a[i]);
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

/* What a bind of bits and keys changes, with nothing changed yet. to[b]: what block b does. It stays as it is, lets
   its memory go, or takes memory: a record of s->mem (its index), or the plan's new record j (TO_NEW + j), which one
   or more blocks of a key take together */
enum { TO_STAY = 0xFFFFFFFFu, TO_DROP = 0xFFFFFFFEu, TO_NEW = 0x80000000u };
typedef struct {
    u32* to;       /* a block each */
    u32* after;    /* each record's blocks once the bind is done */
    u64* new_key;  /* the new records' keys */
    u32 nnew;      /* new records */
    u32 nbind;     /* blocks whose binding changes */
    u32 ntail;     /* tails with no memory yet */
    u32 nrecycled; /* new records that take the allocation of a record all of whose blocks leave it */
    u32 nfresh;    /* and those that take one made now */
} SparsePlan;
static void plan_free(SparsePlan* p) {
    free(p->to);
    free(p->after);
    free(p->new_key);
}
static u64 block_key(const Sparse* s, const u64* keys, u64 b) { return s->alias && keys ? keys[b] : 0; }
static int sparse_plan(const Sparse* s, const u8* bits, const u64* keys, SparsePlan* p) {
    u64 nb = sparse_nblocks(s);
    *p = (SparsePlan){0};
    p->to = malloc((nb + 1) * sizeof *p->to);
    p->after = malloc((s->nmem + 1) * sizeof *p->after);
    KeyBlock* take = malloc((nb + 1) * sizeof *take);  /* the blocks that take memory of a key, by key */
    KeyBlock* have = malloc((s->nmem + 1) * sizeof *have); /* the records of a key, by key */
    p->new_key = malloc((nb + 1) * sizeof *p->new_key);
    if (!p->to || !p->after || !take || !have || !p->new_key) {
        free(take);
        free(have);
        plan_free(p);
        return 0;
    }
    u32 ntake = 0, nhave = 0;
    for (u32 i = 0; i < s->nmem; i++) {
        p->after[i] = s->mem[i].refs;
        if (s->mem[i].refs && s->mem[i].key) have[nhave++] = (KeyBlock){s->mem[i].key, i};
    }
    for (u64 b = 0; b < nb; b++) {
        int want = (bits[b >> 3] >> (b & 7)) & 1;
        u32 at = s->of[b];
        u64 key = block_key(s, keys, b);
        if (want && at && s->mem[at - 1].key == key) { /* what it shows is what it showed */
            p->to[b] = TO_STAY;
            continue;
        }
        if (!want && !at) {
            p->to[b] = TO_STAY;
            continue;
        }
        if (at) p->after[at - 1]--;
        p->nbind++;
        if (!want)
            p->to[b] = TO_DROP;
        else if (!key) {
            p->new_key[p->nnew] = 0;
            p->to[b] = TO_NEW + p->nnew++;
        } else
            take[ntake++] = (KeyBlock){key, (u32)b};
    }
    qsort(take, ntake, sizeof *take, key_block_cmp);
    qsort(have, nhave, sizeof *have, key_block_cmp);
    for (u32 i = 0, h = 0; i < ntake;) { /* a key's blocks: into the record that has it, or a new one for them all */
        u32 e = i, to;
        while (e < ntake && take[e].key == take[i].key) e++;
        while (h < nhave && have[h].key < take[i].key) h++;
        if (h < nhave && have[h].key == take[i].key) {
            to = have[h].block;
            p->after[to] += e - i;
        } else {
            p->new_key[p->nnew] = take[i].key;
            to = TO_NEW + p->nnew++;
        }
        for (; i < e; i++) p->to[take[i].block] = to;
    }
    free(take);
    free(have);
    u32 emptied = 0;
    for (u32 i = 0; i < s->nmem; i++) emptied += s->mem[i].refs && !p->after[i];
    p->nrecycled = emptied < p->nnew ? emptied : p->nnew;
    p->nfresh = p->nnew - p->nrecycled;
    for (u32 t = 0; t < s->tails; t++) p->ntail += !s->slot[nb + t];
    return 1;
}

u64 vk_image_sparse_need(VkImage img, const u8* bits, const u64* keys) {
    const Sparse* s = sparse_find(img);
    SparsePlan p;
    if (!s || !sparse_plan(s, bits, keys, &p)) return 0;
    u64 need = (u64)p.nfresh * s->block + (u64)p.ntail * s->tail_size;
    plan_free(&p);
    return need;
}

/* a record of s->mem for a new allocation: its index, or ~0 with no memory for the table */
static u32 mem_new(Sparse* s) {
    if (s->free_mem) {
        u32 i = s->free_mem - 1;
        s->free_mem = s->mem[i].next;
        return i;
    }
    if (s->nmem == s->capmem) {
        u32 cap = s->capmem ? s->capmem * 2 : 64;
        struct SparseMem* grown = realloc(s->mem, cap * sizeof *grown);
        if (!grown) return ~0u;
        s->mem = grown;
        s->capmem = cap;
    }
    return s->nmem++;
}

int vk_image_sparse_bind(VkImage img, const u8* bits, const u64* keys, u64* bytes) {
    Sparse* s = sparse_find(img);
    if (!s) return -1;
    *bytes = s->bytes;
    u64 nb = sparse_nblocks(s);
    SparsePlan p;
    if (!sparse_plan(s, bits, keys, &p)) return -1;
    if (!p.nbind && !p.ntail) {
        plan_free(&p);
        return 0;
    }
    u32 nalloc = p.nfresh + p.ntail, nmem0 = s->nmem; /* nmem0: the records the plan is of */
    VkSparseImageMemoryBind* ib = calloc(p.nbind ? p.nbind : 1, sizeof *ib);
    VkSparseMemoryBind* ob = calloc(p.ntail ? p.ntail : 1, sizeof *ob);
    DevMem* fresh = calloc(nalloc ? nalloc : 1, sizeof *fresh);
    DevMemInfo* info = calloc(nalloc ? nalloc : 1, sizeof *info);
    /* the new records' places in s->mem and their allocations: those of the records emptied first, then fresh ones */
    u32* rec = calloc(p.nnew ? p.nnew : 1, sizeof *rec);
    DevMem* rec_a = calloc(p.nnew ? p.nnew : 1, sizeof *rec_a);
    SparseDead* dead = calloc(1, sizeof *dead + (s->nmem + 1) * sizeof dead->a[0]);
    int ok = ib && ob && fresh && info && rec && rec_a && dead;
    /* the fresh blocks, all at once, then the tails */
    VkMemoryRequirements req = {.size = s->block, .alignment = s->block, .memoryTypeBits = s->memtypes};
    DevMemWant aci = {.at = DM_DEVICE, .image = 1};
    u32 made = 0;
    for (u32 j = 0; ok && j < p.nfresh; j++) {
        ok = VK_OK(vk_devmem_alloc(&req, &aci, &fresh[j], &info[j]));
        if (ok) made++;
    }
    for (u32 t = 0; ok && t < p.ntail; t++) {
        VkMemoryRequirements tr = {.size = s->tail_size, .alignment = s->block, .memoryTypeBits = s->memtypes};
        ok = VK_OK(vk_devmem_alloc(&tr, &aci, &fresh[p.nfresh + t], &info[p.nfresh + t]));
        if (ok) made++;
    }
    /* room in the table for the new records before anything changes: one emptied gives its place too */
    u32 nrec = 0;
    for (u32 i = 0, e = 0; ok && i < nmem0 && e < p.nrecycled; i++)
        if (s->mem[i].refs && !p.after[i]) {
            rec[nrec] = i;
            rec_a[nrec++] = s->mem[i].a;
            e++;
        }
    for (u32 j = 0; ok && j < p.nfresh; j++) {
        u32 at = mem_new(s);
        if (at == ~0u) {
            ok = 0;
            break;
        }
        s->mem[at] = (struct SparseMem){0}; /* taken from the free list: no block's until the bind is done */
        rec[nrec] = at;
        rec_a[nrec++] = fresh[j];
    }
    if (ok) {
        u32 ni = 0, no = 0;
        for (u64 b = 0; b < nb; b++) {
            u32 to = p.to[b];
            if (to == TO_STAY) continue;
            sparse_region(s, b, &ib[ni]);
            if (to != TO_DROP) { /* else memory VK_NULL_HANDLE: unbound */
                DevMemInfo ai;
                vk_devmem_info(to >= TO_NEW ? rec_a[to - TO_NEW] : s->mem[to].a, &ai);
                ib[ni].memory = ai.memory;
                ib[ni].memoryOffset = ai.offset;
            }
            ni++;
        }
        for (u32 t = 0; t < s->tails; t++)
            if (!s->slot[nb + t]) {
                ob[no] = (VkSparseMemoryBind){.resourceOffset = s->tail_offset + t * s->tail_stride,
                                              .size = s->tail_size,
                                              .memory = info[p.nfresh + no].memory,
                                              .memoryOffset = info[p.nfresh + no].offset};
                no++;
            }
        VkSparseImageMemoryBindInfo ibi = {.image = img, .bindCount = ni, .pBinds = ib};
        VkSparseImageOpaqueMemoryBindInfo obi = {.image = img, .bindCount = no, .pBinds = ob};
        VkBindSparseInfo bi = {.sType = VK_STRUCTURE_TYPE_BIND_SPARSE_INFO,
                               .imageOpaqueBindCount = no ? 1u : 0u,
                               .pImageOpaqueBinds = &obi,
                               .imageBindCount = ni ? 1u : 0u,
                               .pImageBinds = &ibi};
        ok = sparse_submit(&bi);
    }
    if (!ok) { /* nothing changes: the new memory goes, and what was bound stays */
        for (u32 i = 0; i < made; i++) vk_devmem_free(fresh[i]);
        for (u32 j = p.nrecycled; j < nrec; j++) { /* the places taken for fresh records, free again */
            s->mem[rec[j]].refs = 0;
            s->mem[rec[j]].next = s->free_mem;
            s->free_mem = rec[j] + 1;
        }
        free(dead);
    } else {
        u64 gone = 0;
        /* the records emptied: a new record has the allocation of one, and the others' go with the frames */
        for (u32 i = 0, e = 0; i < nmem0; i++)
            if (s->mem[i].refs && !p.after[i]) {
                if (e++ < p.nrecycled) continue; /* rec[] names it: filled below */
                dead->a[dead->n++] = s->mem[i].a;
                s->mem[i] = (struct SparseMem){.next = s->free_mem};
                s->free_mem = i + 1;
                gone++;
            }
        for (u32 j = 0; j < p.nnew; j++) {
            s->mem[rec[j]] = (struct SparseMem){.a = rec_a[j], .key = p.new_key[j]};
            if (j >= p.nrecycled) vk_mem_track(rec_a[j], VK_OWN_TEXTURE);
        }
        for (u32 i = 0; i < s->nmem; i++) s->mem[i].refs = 0; /* each record's blocks, counted again below */
        for (u64 b = 0; b < nb; b++) {
            u32 to = p.to[b];
            if (to == TO_DROP) {
                s->of[b] = 0;
                s->slot[b] = 0;
            } else if (to != TO_STAY) {
                u32 at = to >= TO_NEW ? rec[to - TO_NEW] : to;
                s->of[b] = at + 1;
                s->slot[b] = s->mem[at].a;
            }
            if (s->of[b]) s->mem[s->of[b] - 1].refs++;
        }
        for (u32 t = 0, k = p.nfresh; t < s->tails; t++)
            if (!s->slot[nb + t]) {
                s->slot[nb + t] = fresh[k++];
                vk_mem_track(s->slot[nb + t], VK_OWN_TEXTURE);
            }
        s->bytes = s->bytes + (u64)p.nfresh * s->block + (u64)p.ntail * s->tail_size - gone * s->block;
        *bytes = s->bytes;
        if (dead->n) {
            vk_defer_bytes(gone * s->block);
            vk_defer_call(sparse_dead_free, dead);
        } else
            free(dead);
    }
    plan_free(&p);
    free(ib);
    free(ob);
    free(fresh);
    free(info);
    free(rec);
    free(rec_a);
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
                int have = s->slot[base + ((u64)z * sh->by[level] + y) * sh->bx[level] + x] != 0;
                if (have != all) return have;
            }
    return all;
}

void vk_sparse_stats(u64* binds, u64* bind_ns) {
    *binds = g_sparse_binds;
    *bind_ns = g_sparse_bind_ns;
}

u32 vk_image_bound_blocks(VkImage img) {
    const Sparse* s = sparse_find(img);
    const Pieces* p = s ? NULL : pieces_of(img);
    u32 n = 0;
    if (s)
        for (u64 b = 0; b < sparse_nblocks(s) + s->tails; b++) n += s->slot[b] != 0;
    else if (p && p->per_layer)
        for (u32 i = 0; i < p->n; i++) n += p->pages[i] != 0;
    return n;
}

u32 vk_image_bound_memory(VkImage img) {
    const Sparse* s = sparse_find(img);
    const Pieces* p = s ? NULL : pieces_of(img);
    u32 n = 0;
    if (s) {
        for (u32 i = 0; i < s->nmem; i++) n += s->mem[i].refs != 0;
        for (u64 b = sparse_nblocks(s); b < sparse_nblocks(s) + s->tails; b++) n += s->slot[b] != 0;
        return n;
    }
    if (!p || !p->per_layer || !p->key) return vk_image_bound_blocks(img);
    /* the pages of one key have one allocation: each key once, and each page with none */
    KeyBlock* keyed = malloc((p->n + 1) * sizeof *keyed);
    if (!keyed) return vk_image_bound_blocks(img);
    u32 nkeyed = 0;
    for (u32 i = 0; i < p->n; i++) {
        if (!p->pages[i]) continue;
        if (p->key[i])
            keyed[nkeyed++] = (KeyBlock){p->key[i], i};
        else
            n++;
    }
    qsort(keyed, nkeyed, sizeof *keyed, key_block_cmp);
    for (u32 i = 0; i < nkeyed; i++) n += !i || keyed[i - 1].key != keyed[i].key;
    free(keyed);
    return n;
}

static int sparse_destroy(VkImage img) {
    for (u32 i = 0; i < g_nsparse; i++) {
        Sparse* s = g_sparse[i];
        if (s->img != img) continue;
        vkDestroyImage(g_vk.dev, img, NULL);
        for (u32 k = 0; k < s->nmem; k++) /* each allocation once, however many blocks it has */
            if (s->mem[k].refs) {
                vk_mem_untrack(s->mem[k].a);
                vk_devmem_free(s->mem[k].a);
            }
        for (u64 k = sparse_nblocks(s); k < sparse_nblocks(s) + s->tails; k++)
            if (s->slot[k]) {
                vk_mem_untrack(s->slot[k]);
                vk_devmem_free(s->slot[k]);
            }
        free(s->mem);
        free(s->of);
        free(s);
        g_sparse[i] = g_sparse[--g_nsparse];
        return 1;
    }
    return 0;
}

static void image_destroy(VkImage img, DevMem mem) {
    if (mem || !img) {
        vk_mem_untrack(mem);
        vk_devmem_image_destroy(img, mem);
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
void vk_image_destroy(VkImage img, DevMem mem) {
    VkResourceScope scope = vk_resource_begin();
    image_destroy(img, mem);
    vk_resource_end(VK_RESOURCE_IMAGE_DESTROY, scope);
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
    const int alias = sparse_alias_supported(ii);
    si.flags |= VK_IMAGE_CREATE_SPARSE_BINDING_BIT | VK_IMAGE_CREATE_SPARSE_RESIDENCY_BIT |
                (alias ? VK_IMAGE_CREATE_SPARSE_ALIASED_BIT : 0);
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
    if (alias) p->key = calloc(n, sizeof *p->key); /* none: each page its own memory, as without the feature */
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

/* What page i of p does in a bind of want and keys: whether its binding changes */
static int page_changes(const Pieces* p, const u8* want, const u64* keys, u32 i) {
    return !want[i] != !p->pages[i] || (p->key && keys && want[i] && keys[i] != p->key[i]);
}
/* The pages of a key in one vk_image_resident: those that keep their memory (kept, sorted by key), those given
   memory in it so far (made, in the order made), and how many are still to leave each key's memory (leave, sorted by
   key, the count in block) */
typedef struct {
    KeyBlock* kept;
    KeyBlock* made;
    KeyBlock* leave;
    u32 nkept, nmade, nleave;
} PageKeys;
/* page i of p bound where want[i] is set, released where it isn't, with key its key (0: like no other page). The
   memory it binds in *memory at *offset: an allocation made now (want[i] becomes 2: its texels are undefined), or the
   one the pages of its key have, whose texels are theirs (want[i] stays 1). *had: the memory it lets go that no page
   has any more, VK_NULL_HANDLE when it had none or other pages of its old key keep it */
static VkResult page_change(Pieces* p, u32 i, u8* want, u64 key, PageKeys* k, const VkMemoryRequirements* mem,
                            VkDeviceMemory* memory, VkDeviceSize* offset, DevMem* had) {
    const DevMemWant aci = {.at = DM_DEVICE, .image = 1};
    DevMemInfo info;
    const DevMem old = p->pages[i];
    const u64 old_key = p->key ? p->key[i] : 0;
    DevMem use = 0;
    if (want[i] && key) { /* the memory of its key: a page that keeps it, or one given it in this bind */
        u32 at = key_find(k->kept, k->nkept, key);
        if (at < k->nkept)
            use = p->pages[k->kept[at].block];
        else
            for (u32 j = 0; j < k->nmade && !use; j++)
                if (k->made[j].key == key) use = p->pages[k->made[j].block];
    }
    if (want[i] && !use) {
        VkResult res = vk_devmem_alloc(mem, &aci, &use, &info);
        if (!VK_OK(res)) return res;
        vk_mem_track(use, VK_OWN_TEXTURE);
        p->bound += mem->size;
        want[i] = 2;
        if (key) k->made[k->nmade++] = (KeyBlock){key, i};
    } else if (want[i]) {
        vk_devmem_info(use, &info);
        want[i] = 1;
    }
    /* what it had goes when no page has it any more: its own, or its old key's once the last page has left it and
       none keeps it */
    *had = 0;
    if (old) {
        int shared = 0;
        if (old_key) {
            u32 at = key_find(k->leave, k->nleave, old_key);
            shared = (at < k->nleave && --k->leave[at].block) || key_find(k->kept, k->nkept, old_key) < k->nkept;
        }
        if (!shared) {
            *had = old;
            p->bound -= mem->size;
        }
    }
    p->pages[i] = use;
    if (p->key) p->key[i] = use ? key : 0;
    if (use) {
        *memory = info.memory;
        *offset = info.offset;
    }
    return VK_SUCCESS;
}

u64 vk_image_resident_need(VkImage img, const u8* want, const u64* keys, u32 n) {
    const Pieces* p = pieces_of(img);
    if (!p || !p->per_layer || n != p->n) return 0;
    if (!p->key) keys = NULL;
    const u32 bricks = p->per_layer * p->layers;
    u64 need = 0;
    u32 nkept = 0, ntake = 0;
    KeyBlock* kept = keys ? malloc((n + 1) * sizeof *kept) : NULL; /* the keys that keep memory, then those that take */
    KeyBlock* take = keys ? malloc((n + 1) * sizeof *take) : NULL;
    if (keys && (!kept || !take)) keys = NULL; /* counted as if each took its own: more than it needs */
    for (u32 i = 0; i < n; i++) {
        if (!page_changes(p, want, keys, i)) {
            if (keys && p->pages[i] && p->key[i]) kept[nkept++] = (KeyBlock){p->key[i], i};
            continue;
        }
        if (!want[i]) continue;
        if (keys && keys[i])
            take[ntake++] = (KeyBlock){keys[i], i};
        else
            need += i < bricks ? p->req.alignment : p->tail_size;
    }
    if (keys) {
        qsort(kept, nkept, sizeof *kept, key_block_cmp);
        qsort(take, ntake, sizeof *take, key_block_cmp);
        for (u32 i = 0; i < ntake; i++) /* a key's first page, when no page keeps that key's memory */
            if ((!i || take[i - 1].key != take[i].key) && key_find(kept, nkept, take[i].key) == nkept)
                need += p->req.alignment;
    }
    free(kept);
    free(take);
    return need;
}

int vk_image_resident(VkImage img, u8* want, const u64* keys, u32 n, u64* bytes) {
    Pieces* p = pieces_of(img);
    *bytes = p ? p->bound : 0;
    if (!p || !p->per_layer || n != p->n) return 0;
    if (!p->key) keys = NULL;
    u32 changes = 0;
    for (u32 i = 0; i < n; i++) changes += page_changes(p, want, keys, i);
    if (!changes) return 1;
    if (!bind_ready()) return 0;
    PageKeys k = {0};
    if (keys) { /* the pages that stay as they are, by key: what the others of their keys bind */
        k.kept = malloc((n + 1) * sizeof *k.kept);
        k.made = malloc((changes + 1) * sizeof *k.made);
        k.leave = malloc((changes + 1) * sizeof *k.leave);
        if (!k.kept || !k.made || !k.leave) {
            free(k.kept);
            free(k.made);
            free(k.leave);
            return 0;
        }
        for (u32 i = 0; i < n; i++) {
            if (!p->pages[i] || !p->key[i]) continue;
            if (page_changes(p, want, keys, i))
                k.leave[k.nleave++] = (KeyBlock){p->key[i], 1};
            else
                k.kept[k.nkept++] = (KeyBlock){p->key[i], i};
        }
        qsort(k.kept, k.nkept, sizeof *k.kept, key_block_cmp);
        qsort(k.leave, k.nleave, sizeof *k.leave, key_block_cmp);
        u32 to = 0; /* one entry a key, with how many leave it */
        for (u32 i = 0; i < k.nleave; i++) {
            if (to && k.leave[to - 1].key == k.leave[i].key)
                k.leave[to - 1].block++;
            else
                k.leave[to++] = k.leave[i];
        }
        k.nleave = to;
    }
    /* the commands recorded so far read the blocks as they are: submitted, so the batch can wait for them. One of
       this image's binds still in the batch goes before, as a batch may bind a block only once */
    vk_submit_recorded();
    if (p->batch > g_bind.issued) bind_flush();
    const VkExtent3D g = p->block;
    const VkMemoryRequirements one = {p->req.alignment, p->req.alignment, p->req.memoryTypeBits},
                               tail = {p->tail_size, p->req.alignment, p->req.memoryTypeBits};
    VkResult res = VK_SUCCESS;
    DevMem had;
    u32 nb = 0, i = 0;
    for (u32 layer = 0; layer < p->layers && VK_OK(res); layer++)
        for (u32 l = 0; l < p->tail_first && VK_OK(res); l++) {
            const u32 w = level_dim(p->extent.width, l), h = level_dim(p->extent.height, l),
                      d = level_dim(p->extent.depth, l);
            for (u32 z = 0; z < d && VK_OK(res); z += g.depth)
                for (u32 y = 0; y < h && VK_OK(res); y += g.height)
                    for (u32 x = 0; x < w && VK_OK(res); x += g.width, i++) {
                        if (!page_changes(p, want, keys, i)) {
                            if (want[i]) want[i] = 1;
                            continue;
                        }
                        VkSparseImageMemoryBind b = {.subresource = {VK_IMAGE_ASPECT_COLOR_BIT, l, layer},
                                                     .offset = {(i32)x, (i32)y, (i32)z},
                                                     .extent = {w - x < g.width ? w - x : g.width,
                                                                h - y < g.height ? h - y : g.height,
                                                                d - z < g.depth ? d - z : g.depth}};
                        res = page_change(p, i, want, keys ? keys[i] : 0, &k, &one, &b.memory, &b.memoryOffset, &had);
                        if (!VK_OK(res)) break;
                        p->batch = bind_add(img, 0, &b, sizeof b, had, one.size, 1);
                        nb++;
                    }
        }
    /* the mip tails, each bound whole: one range of the image's memory a tail, and its own */
    for (u32 t = 0; t < p->tails && VK_OK(res); t++, i++) {
        if (!want[i] == !p->pages[i]) continue;
        VkSparseMemoryBind b = {.resourceOffset = p->tail_offset + t * p->tail_stride, .size = p->tail_size};
        res = page_change(p, i, want, 0, &k, &tail, &b.memory, &b.memoryOffset, &had);
        if (!VK_OK(res)) break;
        p->batch = bind_add(img, 1, &b, sizeof b, had, tail.size, 1);
        nb++;
    }
    free(k.kept);
    free(k.made);
    free(k.leave);
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
   measures: an eviction leaves a hole in a block of device memory or waits for a frame in flight before the system
   gets its memory back, and a budget measured again at every trim would take that for room still missing and evict again each
   present */
static struct {
    u64 reserve; /* 0: the driver's budget alone */
    u64 ns;      /* when it was measured, 0 never */
    u64 budget;
} g_uma_budget;
#endif

static void heap_find(void) {
    const VkPhysicalDeviceMemoryProperties* mp;
    mp = vk_devmem_props();
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
        DevMemBudget b[VK_MAX_MEMORY_HEAPS];
        vk_devmem_budgets(b);
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
u64 vk_mem_budget_cap(u64 bytes) {
    u64 had;
    if (g_heap < 0) heap_find(); /* which reads vk_budget_mb once */
    had = g_budget_cap;
    g_budget_cap = bytes;
    return had;
}

/* The driver's use of the device-local heap and its budget for it, through VK_EXT_memory_budget. Without the extension
   devmem.c's budget is its guess (80% of the heap), so what is known goes instead, *ours set: the memory this process
   took from the heap (devmem.c's blocks) and the heap's size */
void vk_mem_driver_use(u64* usage, u64* budget, int* ours) {
    DevMemBudget b[VK_MAX_MEMORY_HEAPS];
    int heap = vk_mem_heap();
    vk_devmem_budgets(b); /* the driver's, asked at frame_recycle's vk_devmem_frame */
    *ours = !g_vk.has_budget;
    if (g_vk.has_budget) {
        *usage = b[heap].usage;
        *budget = b[heap].budget;
    } else {
        *usage = b[heap].block_bytes;
        *budget = vk_devmem_props()->memoryHeaps[heap].size;
    }
}

/* the heap's use as the caches see it, and its budget. The use leaves out the free space inside devmem.c's blocks,
   which the next allocations fill before it asks the driver for more, and the memory of evicted entries the frames in
   flight still hold (vk_deferred_bytes): it goes once they are done */
void vk_mem_use(u64* usage, u64* budget) {
    DevMemBudget b[VK_MAX_MEMORY_HEAPS];
    if (g_heap < 0) heap_find();
    vk_devmem_budgets(b);
    u64 u = b[g_heap].usage, holes = b[g_heap].block_bytes - b[g_heap].allocated_bytes,
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
