/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Compute: the compute class's launches (QMD), on a compute queue of their own when the device has one (a queue of a
   compute-only family, or a second queue of the graphics family), else on the graphics queue. Each dispatch gets
   copies of its constant and storage buffers in a host-visible arena and is recorded into a batch. The batch is
   submitted and its results land in guest memory before anything could observe them: a read of that memory
   (gmem_read and gmem_ptr call cs_mem_need), anything after it in the stream the game can observe (semaphore releases
   and reports: gpu_signal_point), a GPU-side write of that memory, a syncpoint increment or a wait. A byte is written
   only where the shader changed it and guest memory still holds what the shader read, since on the console the GPU
   stores while it runs and a later CPU write wins.

   A queue of its own only waits for compute, so holding signals until results land costs a dispatch's run time,
   not the rendering backlog's (which made that ordering too slow under the old OpenGL renderer, ROADMAP.md Phase 2).
   Dispatches that read textures run on the graphics queue, after everything recorded before them. */
#include <stdlib.h>
#include <string.h>
#include "nx/gpu/gpu.h"
#include "nx/gpu/shader/shader.h"
#include "nx/runtime/platform/log.h"
#include "nx/runtime/platform/plat.h"
#include "nx/gpu/vk/vk.h"
#include "nx/gpu/renderer.h"
#include "nx/gpu/vk/backing.h"

typedef struct CsPipe {
    u64 key;
    VkPipeline p;
    ShaderInfo si;
    VkCbReads cbr; /* the constant buffer words translation read */
    int failed;
    struct CsPipe* next;
    struct CsPipe* keyed; /* the program translated for the integer-ness of the 2D images last bound to it */
} CsPipe;
static CsPipe* g_csp[256];

/* the arena: inputs and outputs of the batch's dispatches, and what the outputs held before (org, host only) */
static VkBuffer g_ar;
static VmaAllocation g_ar_mem;
static u8* g_ar_ptr;
static u8* g_org;
static u64 g_ar_cap, g_ar_used;
typedef struct {
    u64 addr;
    u64 off;
    u32 size;
} CsOut;
static CsOut g_out[256];
static int g_nout;
static u64 g_out_lo = ~0ull, g_out_hi;

static VkCommandPool g_cpool;
static VkCommandBuffer g_ccb;
static VkDescriptorPool g_dpool;
static int g_recording, g_landing;
static int g_inflight; /* submitted and waited for in cs_land, results not landed yet */
/* Submitted batches whose results haven't landed, oldest first. A batch is submitted when it is full, or when a
   release, a draw or a reader needs its results, and lands when something needs them: the release thread once the
   batch is done, or a reader now. Landing each at its submit (with a fence wait) was 4 waits a frame in TotK. Each
   keeps its command buffer, descriptor pool, arena and original bytes until it lands; the current batch (the globals
   above) records into others meanwhile. */
typedef struct {
    VkCommandBuffer cb;
    VkDescriptorPool dpool;
    VkBuffer ar;
    VmaAllocation ar_mem;
    u8* ar_ptr;
    u8* org;
    u64 ar_cap, org_cap, cval, lo, hi;
    CsOut out[256];
    int nout, gfx;
} CsBatch;
#define CS_PENDING 3
static CsBatch g_pend[CS_PENDING];
static u32 g_phead, g_ptail; /* batches put aside, landed */
/* draws can read results on the GPU before they land (vk_compute_gpu_src): the batch signals g_vk_csem, the next
   graphics submit waits for g_vk_cs_wait_value, and the arena they read stays alive until that frame is done */
VkSemaphore g_vk_csem;
u64 g_vk_cs_wait_value;
static u64 g_cval;
static int g_ar_gfx;
static u64 g_org_cap;
u64 g_vk_dispatches, g_vk_cs_batches, g_vk_cs_skipped, g_vk_cs_wait_ns;

static u8 g_cb[8][0x10000];
static u32 g_cbn[8];
static int cs_cb_read(u32 idx, u32 off, u32* out) {
    if (idx < 8 && off + 4 <= g_cbn[idx]) {
        memcpy(out, g_cb[idx] + off, 4);
        return 1;
    }
    return 0;
}

static void cs_init(void) {
    VkDescriptorPoolSize sz[VK_POOL_TYPES];
    VkDescriptorPoolCreateInfo dci = {0};
    if (g_cpool) return;
    VkCommandPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                   .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                   .queueFamilyIndex = g_vk.compute_family};
    VK_OK(vkCreateCommandPool(g_vk.dev, &pci, NULL, &g_cpool));
    VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                      .commandPool = g_cpool,
                                      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                      .commandBufferCount = 1};
    VK_OK(vkAllocateCommandBuffers(g_vk.dev, &ai, &g_ccb));
    VkSemaphoreTypeCreateInfo st = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
                                    .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE};
    VkSemaphoreCreateInfo sci = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &st};
    VK_OK(vkCreateSemaphore(g_vk.dev, &sci, NULL, &g_vk_csem));
    /* the dispatches of a batch: a batch that runs out lands, which resets the pool */
    vk_pool_sizes(sz, 1024);
    dci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dci.maxSets = 1024;
    dci.poolSizeCount = VK_POOL_TYPES;
    dci.pPoolSizes = sz;
    VK_OK(vkCreateDescriptorPool(g_vk.dev, &dci, NULL, &g_dpool));
}

/* a bigger arena; only between batches, nothing points into it then */
static u64 g_ar_want = 8ull << 20; /* doubles when a batch fills the arena, up to 64 MB */
/* arenas draws read from: reused once the frames that read them are done */
static struct {
    VkBuffer b;
    VmaAllocation m;
    u8* p;
    u64 cap, frame;
} g_retired[8];
static int g_nretired;
static void arena_retire(void) {
    if (!g_ar) return;
    if (g_nretired == 8) { /* full: the oldest goes for good */
        vk_defer_buffer(g_retired[0].b, g_retired[0].m);
        memmove(g_retired, g_retired + 1, sizeof g_retired[0] * 7);
        g_nretired--;
    }
    g_retired[g_nretired++] = (typeof(g_retired[0])){g_ar, g_ar_mem, g_ar_ptr, g_ar_cap, vk_frame_id()};
    g_ar = VK_NULL_HANDLE;
}
/* the original bytes beside the arena, as big as it: batches put aside swap theirs with the current one's */
static int org_fit(void) {
    if (g_org_cap >= g_ar_cap) return 1;
    free(g_org);
    g_org = malloc(g_ar_cap);
    g_org_cap = g_org ? g_ar_cap : 0;
    return g_org != NULL;
}
static int arena_fit(u64 need) {
    if (g_ar && g_ar_used <= g_ar_cap && need <= g_ar_cap - g_ar_used && g_ar_cap >= g_ar_want) return org_fit();
    vk_cmd(); /* frames older than VK_FRAMES are done once the current one has started */
    for (int i = 0; i < g_nretired; i++)
        if (g_retired[i].frame + VK_FRAMES <= vk_frame_id() && g_retired[i].cap >= need &&
            g_retired[i].cap >= g_ar_want) {
            if (g_ar) vk_defer_buffer(g_ar, g_ar_mem);
            g_ar = g_retired[i].b;
            g_ar_mem = g_retired[i].m;
            g_ar_ptr = g_retired[i].p;
            g_ar_cap = g_retired[i].cap;
            g_retired[i] = g_retired[--g_nretired];
            return org_fit();
        }
    if (g_ar) vk_defer_buffer(g_ar, g_ar_mem); /* draws may still read it */
    u64 cap = g_ar_want;
    while (cap < need) cap *= 2;
    u32 fam[2] = {g_vk.gfx_family, g_vk.compute_family};
    VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                              .size = cap,
                              .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                       VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                                       VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT,
                              /* both queues use it, one at a time, without ownership transfers */
                              .sharingMode = fam[0] != fam[1] ? VK_SHARING_MODE_CONCURRENT : VK_SHARING_MODE_EXCLUSIVE,
                              .queueFamilyIndexCount = fam[0] != fam[1] ? 2 : 0,
                              .pQueueFamilyIndices = fam};
    VmaAllocationCreateInfo aci = {.flags =
                                       VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT,
                                   .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST};
    VmaAllocationInfo info;
    g_ar = VK_NULL_HANDLE;
    g_ar_cap = 0;
    if (!VK_OK(vmaCreateBuffer(g_vk.vma, &bci, &aci, &g_ar, &g_ar_mem, &info))) return 0;
    g_ar_ptr = info.pMappedData;
    g_ar_cap = cap;
    return org_fit();
}
static u64 arena_alloc(u64 size) {
    u64 al = vk_stage_align(), at = (g_ar_used + al - 1) & ~(al - 1);
    g_ar_used = at + size;
    return at;
}

/* submits the batch without waiting; it signals g_vk_csem with the next value */
static void cs_submit(void) {
    if (!g_recording) return;
    vmaFlushAllocation(g_vk.vma, g_ar_mem, 0, VK_WHOLE_SIZE); /* the arena may not be host coherent */
    VK_OK(vkEndCommandBuffer(g_ccb));
    VkCommandBufferSubmitInfo c = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = g_ccb};
    VkSemaphoreSubmitInfo sig = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
                                 .semaphore = g_vk_csem,
                                 .value = ++g_cval,
                                 .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT};
    VkSubmitInfo2 si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
                        .commandBufferInfoCount = 1,
                        .pCommandBufferInfos = &c,
                        .signalSemaphoreInfoCount = 1,
                        .pSignalSemaphoreInfos = &sig};
    VkResult r = vkQueueSubmit2(g_vk.compute, 1, &si, VK_NULL_HANDLE);
    if (r == VK_ERROR_DEVICE_LOST) fatal("vulkan: the GPU device was lost (driver reset or crash)");
    g_recording = 0;
    g_inflight = 1;
    if (!VK_OK(r)) { /* never signaled by the GPU: the host signals it, so waits for it end */
        VkSemaphoreSignalInfo ss = {
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO, .semaphore = g_vk_csem, .value = g_cval};
        VK_OK(vkSignalSemaphore(g_vk.dev, &ss));
    }
}
static void cs_wait(u64 value) {
    VkSemaphoreWaitInfo wi = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
                              .semaphoreCount = 1,
                              .pSemaphores = &g_vk_csem,
                              .pValues = &value};
    u64 t0 = plat_ns();
    VK_OK(vkWaitSemaphores(g_vk.dev, &wi, UINT64_MAX));
    __atomic_add_fetch(&g_vk_cs_wait_ns, plat_ns() - t0, __ATOMIC_RELAXED);
}
/* the arena's results that changed, into guest memory, after its batch is done */
static void cs_write_back(VmaAllocation mem, const u8* ar, const u8* org, const CsOut* out, int nout) {
    vmaInvalidateAllocation(g_vk.vma, mem, 0, VK_WHOLE_SIZE);
    for (int i = 0; i < nout; i++) vk_write_changed(out[i].addr, ar + out[i].off, org + out[i].off, out[i].size);
}
/* an arena whose batch has landed: draws that read it may still run (gfx), so it waits for their frame like the
   arenas draws read; one they didn't read is free now */
static void cs_arena_done(VkBuffer b, VmaAllocation m, u8* p, u64 cap, int gfx) {
    if (g_nretired == 8) { /* full: the oldest goes for good */
        vk_defer_buffer(g_retired[0].b, g_retired[0].m);
        memmove(g_retired, g_retired + 1, sizeof g_retired[0] * 7);
        g_nretired--;
    }
    g_retired[g_nretired++] = (typeof(g_retired[0])){
        b, m, p, cap, gfx || vk_frame_id() < VK_FRAMES ? vk_frame_id() : vk_frame_id() - VK_FRAMES};
}
static void cs_any_update(void) {
    int any = g_nout > 0;
    for (u32 i = g_ptail; i != g_phead && !any; i++) any = g_pend[i % CS_PENDING].nout > 0;
    g_cs_any = any;
}
/* the oldest batch put aside lands: it is done once g_vk_csem reaches its value */
static void cs_land_oldest(void) {
    CsBatch* b = &g_pend[g_ptail % CS_PENDING];
    int was = g_landing;
    cs_wait(b->cval);
    g_landing = 1;
    cs_write_back(b->ar_mem, b->ar_ptr, b->org, b->out, b->nout);
    g_landing = was;
    vkResetDescriptorPool(g_vk.dev, b->dpool, 0);
    cs_arena_done(b->ar, b->ar_mem, b->ar_ptr, b->ar_cap, b->gfx);
    b->ar = VK_NULL_HANDLE;
    b->nout = 0;
    g_ptail++;
    g_vk_cs_batches++;
    cs_any_update();
}
/* submits the current batch and puts it aside to land later; the next batch records into the slot's command buffer
   and pool and a fresh arena */
static void cs_put_aside(void) {
    CsBatch* b;
    VkCommandBuffer cb;
    VkDescriptorPool dp;
    u8* org;
    u64 org_cap;
    if (!g_recording) return;
    cs_submit();
    if (g_phead - g_ptail == CS_PENDING) cs_land_oldest();
    b = &g_pend[g_phead % CS_PENDING];
    if (!b->cb) { /* first use of the slot: its own command buffer and pool for the batch that follows */
        VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                          .commandPool = g_cpool,
                                          .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                          .commandBufferCount = 1};
        VkDescriptorPoolSize sz[VK_POOL_TYPES];
        VkDescriptorPoolCreateInfo dci = {0};
        VK_OK(vkAllocateCommandBuffers(g_vk.dev, &ai, &b->cb));
        vk_pool_sizes(sz, 1024);
        dci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        dci.maxSets = 1024;
        dci.poolSizeCount = VK_POOL_TYPES;
        dci.pPoolSizes = sz;
        VK_OK(vkCreateDescriptorPool(g_vk.dev, &dci, NULL, &b->dpool));
    }
    cb = b->cb;
    dp = b->dpool;
    org = b->org;
    org_cap = b->org_cap;
    b->cb = g_ccb;
    b->dpool = g_dpool;
    b->ar = g_ar;
    b->ar_mem = g_ar_mem;
    b->ar_ptr = g_ar_ptr;
    b->ar_cap = g_ar_cap;
    b->org = g_org;
    b->org_cap = g_org_cap;
    b->cval = g_cval;
    b->gfx = g_ar_gfx;
    memcpy(b->out, g_out, (size_t)g_nout * sizeof g_out[0]);
    b->nout = g_nout;
    b->lo = g_out_lo;
    b->hi = g_out_hi;
    g_phead++;
    g_ccb = cb;
    g_dpool = dp;
    g_org = org;
    g_org_cap = org_cap;
    g_ar = VK_NULL_HANDLE; /* arena_fit gives the next batch one */
    g_ar_cap = g_ar_used = 0;
    g_nout = 0;
    g_out_lo = ~0ull;
    g_out_hi = 0;
    g_ar_gfx = 0;
    g_inflight = 0;
    cs_any_update();
}
/* every batch lands, the current one submitted and waited for */
static void cs_land(void) {
    if (g_landing) return;
    while (g_ptail != g_phead) cs_land_oldest();
    if (!g_recording && !g_inflight) return;
    g_landing = 1;
    cs_submit();
    cs_wait(g_cval);
    g_inflight = 0;
    cs_write_back(g_ar_mem, g_ar_ptr, g_org, g_out, g_nout);
    /* only now: until the results are in memory, readers on other threads wait for this landing in
       vk_compute_mem_need */
    g_nout = 0;
    g_out_lo = ~0ull;
    g_out_hi = 0;
    g_ar_used = 0;
    vkResetDescriptorPool(g_vk.dev, g_dpool, 0);
    if (g_ar_gfx) { /* draws read this arena: it waits until their frame is done, the next batch gets another */
        arena_retire();
        g_ar_gfx = 0;
    }
    g_vk_cs_batches++;
    g_landing = 0;
    cs_any_update();
}

/* A descriptor can fail after its inputs were copied into the arena. No command references those inputs yet. */
static void cs_abandon_binding(u8* originals[8]) {
    vk_buf_writable_abort(); /* a deferred dispatch's outputs, not recorded */
    for (int i = 0; i < 8; i++) free(originals[i]);
    if (g_recording || g_inflight) return;
    g_ar_used = 0;
    vkResetDescriptorPool(g_vk.dev, g_dpool, 0);
}
/* a draw reads [gva, gva + len), which one pending result covers: it reads it from the batch's arena, after the
   batch. Any other overlap lands the results now, so the caller's caches see them in memory */
int vk_compute_gpu_src(u64 gva, u64 len, VkBuffer* buf, u64* off) {
    u32 k;
    if (!g_cs_any || g_landing) return 0;
    for (int i = 0; i < g_nout; i++) {
        CsOut* o = &g_out[i];
        if (gva >= o->addr + o->size || o->addr >= gva + len) {
            if (vk_backing_overlap(gva, len, o->addr, o->size))
                cs_land();
            else
                continue;
            return 0;
        }
        /* partly covered, or at an offset that suits no use of it (index, storage and uniform buffers): it lands */
        if (gva < o->addr || gva + len > o->addr + o->size || ((o->off + (gva - o->addr)) & (vk_stage_align() - 1))) {
            cs_land();
            return 0;
        }
        g_ar_gfx = 1;
        cs_put_aside();
        o = &g_pend[(g_phead - 1) % CS_PENDING].out[i];
        g_vk_cs_wait_value = g_cval;
        *buf = g_pend[(g_phead - 1) % CS_PENDING].ar;
        *off = o->off + (gva - o->addr);
        return 1;
    }
    for (k = g_ptail; k != g_phead; k++) {
        CsBatch* b = &g_pend[k % CS_PENDING];
        for (int i = 0; i < b->nout; i++) {
            CsOut* o = &b->out[i];
            if (gva >= o->addr + o->size || o->addr >= gva + len) {
                if (vk_backing_overlap(gva, len, o->addr, o->size))
                    cs_land();
                else
                    continue;
                return 0;
            }
            if (gva < o->addr || gva + len > o->addr + o->size ||
                ((o->off + (gva - o->addr)) & (vk_stage_align() - 1))) {
                cs_land();
                return 0;
            }
            b->gfx = 1;
            if (g_vk_cs_wait_value < b->cval) g_vk_cs_wait_value = b->cval;
            *buf = b->ar;
            *off = o->off + (gva - o->addr);
            return 1;
        }
    }
    return 0;
}
void vk_compute_land(void) {
    vk_lock();
    cs_land();
    vk_unlock();
}
u64 vk_compute_put_aside(void) {
    u64 gate = 0;
    vk_lock();
    if (g_nout) cs_put_aside();
    if (g_ptail != g_phead) gate = g_pend[(g_phead - 1) % CS_PENDING].cval;
    vk_unlock();
    return gate;
}
void vk_compute_wait(u64 value) {
    if (value) cs_wait(value);
}
void vk_compute_land_through(u64 value) {
    vk_lock();
    while (g_ptail != g_phead && g_pend[g_ptail % CS_PENDING].cval <= value) cs_land_oldest();
    vk_unlock();
}
/* the front end writes guest memory (size > 0) or reaches a point the game can observe (size 0) */
void vk_compute_before_write(u64 gva, u64 size) {
    if (!size)
        vk_compute_land();
    else if (g_cs_any)
        vk_compute_mem_need(gva, size);
}
/* a read of guest memory: results for it have to be there first. Called from any thread that reads guest memory while
   g_cs_any is set: the pending results are looked at under vk_lock, which recording and landing them hold, so a
   reader waits for a landing in progress on another thread. The landing thread itself passes (g_landing). A result of
   a batch put aside lands that batch and those before it */
void vk_compute_mem_need(u64 a, u64 n) {
    u32 k, last;
    int found = 0;
    vk_lock();
    last = g_ptail;
    if (!g_landing) {
        for (k = g_ptail; k != g_phead; k++) {
            const CsBatch* b = &g_pend[k % CS_PENDING];
            for (int i = 0; i < b->nout && !(found && last == k + 1); i++)
                if (vk_backing_overlap(a, n, b->out[i].addr, b->out[i].size)) {
                    found = 1;
                    last = k + 1;
                }
        }
        while (found && g_ptail != last) cs_land_oldest();
        for (int i = 0; i < g_nout; i++)
            if (vk_backing_overlap(a, n, g_out[i].addr, g_out[i].size)) {
                cs_land();
                break;
            }
    }
    vk_unlock();
}

/* the pipeline of a program (80 zero bytes, then the code), by its code and launch parameters, and by the constant
   buffer words its translation read */
/* A program is translated for the integer textures it is given, as fragment shaders are keyed (an integer texture
   bound where the shader declares a float one has no view), and for its 2D storage images whose store kind is the
   other one than a bindless image's default, unsigned (ShaderInfo.rt_int_mask bits 8 and up). */
typedef struct {
    u64 tex_int, tex_sint;
    u32 image;
} CsKey;
static CsPipe* cs_pipe(const u8* code, u32 lx, u32 ly, u32 lz, u32 shm, CsKey k) {
    static CsPipe allocation_failed = {.failed = 1};
    u32 blen;
    u64 key = vk_shader_hash(code, 0x10000 + 80, &blen, cs_cb_read) ^ ((u64)lx << 40) ^ ((u64)ly << 48) ^
              ((u64)lz << 56) ^ ((u64)shm << 20) ^ ((u64)k.image << 32) ^ (u64)k.tex_int * 0x9E3779B97F4A7C15ull ^
              (u64)k.tex_sint * 0xC2B2AE3D27D4EB4Full;
    CsPipe* p = g_csp[key & 255];
    while (p && (p->key != key || !vk_cb_reads_hold(&p->cbr, cs_cb_read))) p = p->next;
    if (p) return p;
    p = calloc(1, sizeof *p);
    if (!p) return &allocation_failed;
    p->key = key;
    p->next = g_csp[key & 255];
    g_csp[key & 255] = p;
    p->si.cs_local[0] = lx;
    p->si.cs_local[1] = ly;
    p->si.cs_local[2] = lz;
    p->si.shared_size = (int)shm;
    p->si.rt_int_mask = k.image << 8;
    p->si.tex_int_mask = k.tex_int;
    p->si.tex_sint_mask = k.tex_sint;
    const char* corpus = vk_corpus_dir();
    if (corpus) shader_corpus_save(corpus, code, blen, 0x10000 + 80, ST_CS, &p->si, key);
    size_t nw = 0;
    u32* spv = vk_translate_spirv(code, 0x10000 + 80, ST_CS, &p->si, &nw, cs_cb_read, &p->cbr);
    if (!spv || p->si.unsupported) {
        p->failed = 1;
        LOG("vulkan: compute %016llx not translated: %s\n", (unsigned long long)key,
            p->si.first_unsupported[0] ? p->si.first_unsupported : "unsupported feature or shader");
        return p;
    }
    u32 image_slots = g_vk.props.limits.maxPerStageDescriptorStorageImages;
    if (image_slots > g_vk.props.limits.maxDescriptorSetStorageImages)
        image_slots = g_vk.props.limits.maxDescriptorSetStorageImages;
    if (p->si.nimg > (int)image_slots) {
        LOG_LIMIT(4, "vulkan: compute image count exceeds this GPU's descriptor limit (skipped)\n");
        free(spv);
        p->failed = 1;
        return p;
    }
    VkShaderModuleCreateInfo mi = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = nw * 4, .pCode = spv};
    VkShaderModule m;
    int ok = VK_OK(vkCreateShaderModule(g_vk.dev, &mi, NULL, &m));
    free(spv);
    if (!ok) {
        p->failed = 1;
        return p;
    }
    VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                                                .module = m,
                                                .pName = "main"},
                                      .layout = g_vk_playout};
    if (!VK_OK(vkCreateComputePipelines(g_vk.dev, vk_pipeline_cache(), 1, &ci, NULL, &p->p))) p->failed = 1;
    vkDestroyShaderModule(g_vk.dev, m, NULL);
    if (!p->failed)
        LOG_LIMIT(12, "vulkan: compute %016llx ready: block %ux%ux%u, %d global buffers, %d textures, %d images\n",
                  (unsigned long long)key, lx, ly, lz, p->si.ngbuf, p->si.ntex, p->si.nimg);
    return p;
}

/* A buffer image: the texels a buffer header describes, bound as a storage texel buffer over their copy in the
   arena. Raw loads and atomics view it as R32_UINT, as the console reads it for them whatever its format. Formatted
   stores are typeless in the shader, unsigned for bindless images: an integer format is viewed as itself, a 32-bit
   float one as the integer format of the same bits, which the shader's stores of register bits match. */
typedef struct {
    u64 gva;
    u32 bytes;
    VkFormat fmt;
} CsBufImg;
static int cs_buffer_image(const u32* tic, int kind, CsBufImg* out) {
    u32 hv = (tic[2] >> 21) & 7, type = (tic[4] >> 23) & 0xF;
    u64 gva;
    u64 texels;
    VkFmt f;
    VkFormat fmt;
    u64 bytes;
    u32 element;
    VkFormatProperties fp;
    VkFormatFeatureFlags want;
    if (hv != 0 || type != 6) return 0;
    gva = ((u64)(tic[2] & 0xFFFF) << 32) | tic[1];
    texels = (((u64)(tic[3] & 0xFFFF) << 16) | (tic[4] & 0xFFFF)) + 1;
    f = vk_tic_format(tic[0], 0);
    if (!gva || !f.bpp || f.bw != 1 || !f.raw || f.depth) return 0;
    fmt = f.vk;
    if (kind == 1)
        fmt = VK_FORMAT_R32_UINT;
    else if (kind == 2 && !f.is_int)
        fmt = fmt == VK_FORMAT_R32_SFLOAT            ? VK_FORMAT_R32_UINT
              : fmt == VK_FORMAT_R32G32_SFLOAT       ? VK_FORMAT_R32G32_UINT
              : fmt == VK_FORMAT_R32G32B32A32_SFLOAT ? VK_FORMAT_R32G32B32A32_UINT
                                                     : VK_FORMAT_UNDEFINED;
    else if (kind == 2 && f.sint) /* the shader's image is unsigned: the same bits viewed as such */
        fmt = fmt == VK_FORMAT_R32_SINT            ? VK_FORMAT_R32_UINT
              : fmt == VK_FORMAT_R32G32_SINT       ? VK_FORMAT_R32G32_UINT
              : fmt == VK_FORMAT_R32G32B32A32_SINT ? VK_FORMAT_R32G32B32A32_UINT
              : fmt == VK_FORMAT_R16G16B16A16_SINT ? VK_FORMAT_R16G16B16A16_UINT
              : fmt == VK_FORMAT_R16G16_SINT       ? VK_FORMAT_R16G16_UINT
              : fmt == VK_FORMAT_R16_SINT          ? VK_FORMAT_R16_UINT
              : fmt == VK_FORMAT_R8G8B8A8_SINT     ? VK_FORMAT_R8G8B8A8_UINT
              : fmt == VK_FORMAT_R8G8_SINT         ? VK_FORMAT_R8G8_UINT
              : fmt == VK_FORMAT_R8_SINT           ? VK_FORMAT_R8_UINT
                                                   : VK_FORMAT_UNDEFINED;
    else if (kind == 0 && f.is_int)
        fmt = VK_FORMAT_UNDEFINED;
    if (fmt == VK_FORMAT_UNDEFINED) return 0;
    bytes = texels * f.bpp;
    element = kind == 1 ? 4 : f.bpp;
    if (bytes > (64ull << 20) || bytes / element > g_vk.props.limits.maxTexelBufferElements) return 0;
    vkGetPhysicalDeviceFormatProperties(g_vk.phys, fmt, &fp);
    want = VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT |
           (kind == 1 ? VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_ATOMIC_BIT : 0);
    if ((fp.bufferFeatures & want) != want) return 0;
    out->gva = gva;
    out->bytes = (u32)(bytes / element * element);
    out->fmt = fmt;
    return 1;
}

/* Both the initial translation and format specialization use the same bounded program. */
static void cs_read_code(u64 pa, u8* dst) {
    u32 code_size = 0x10000;
    /* NVN puts a header with its program size 0x100 bytes before compute code. Bounded by it, a BRX whose
       targets aren't resolved can't scan into the unrelated programs that follow in the same window. Other
       clients keep the whole window. */
    if (pa >= 0x100) {
        const u8* header = gmem_ptr_untracked(pa - 0x100, 8);
        if (header) {
            u32 magic, declared;
            memcpy(&magic, header, sizeof magic);
            memcpy(&declared, header + 4, sizeof declared);
            if (magic == 0x12345679 && declared >= 0x80 && declared <= 0x10000 && !(declared & 7) &&
                gmem_ptr_untracked(pa, declared))
                code_size = declared;
        }
    }
    memset(dst, 0, 0x10000 + 80);
    gmem_read_code(pa, dst + 80, code_size);
}

void vk_dispatch(GpuChannel* ch, const u32* q, u64 code) {
    u32* rc = ch->rcomp;
    u32 gx = q[12] & 0x7FFFFFFF, gy = q[13] & 0xFFFF, gz = q[13] >> 16, lx = q[18] >> 16, ly = q[19] & 0xFFFF,
        lz = q[19] >> 16, shm = q[17] & 0x3FFFF;
    CsKey ck = {0, 0, 0};
    u32 tcb_key;
    u64 tic_key_base;
    CsBufImg bimg[8] = {0};
    int i;
    vk_frame_check();
    if (!gx || !gy || !gz || !lx || !ly || !lz) return;
    const VkPhysicalDeviceLimits* lim = &g_vk.props.limits;
    if (gx > lim->maxComputeWorkGroupCount[0] || gy > lim->maxComputeWorkGroupCount[1] ||
        gz > lim->maxComputeWorkGroupCount[2] || lx > lim->maxComputeWorkGroupSize[0] ||
        ly > lim->maxComputeWorkGroupSize[1] || lz > lim->maxComputeWorkGroupSize[2] ||
        (u64)lx * ly * lz > lim->maxComputeWorkGroupInvocations || shm > lim->maxComputeSharedMemorySize) {
        LOG_LIMIT(1,
                  "vulkan: a dispatch of %ux%ux%u blocks of %ux%ux%u with %u bytes shared is past this GPU's limits "
                  "(skipped)\n",
                  gx, gy, gz, lx, ly, lz, shm);
        g_vk_cs_skipped++;
        return;
    }
    cs_init();
    /* constant buffers from the QMD (reading them lands results they overlap) */
    u64 cba[8] = {0};
    for (int i = 0; i < 8; i++) {
        g_cbn[i] = 0;
        if (!((q[20] >> i) & 1)) continue;
        cba[i] = ((u64)(q[30 + 2 * i] & 0xFF) << 32) | q[29 + 2 * i];
        u32 sz = (q[30 + 2 * i] >> 15) & 0x1FFFF;
        if (!sz || sz > 0x10000) sz = 0x10000;
        g_cbn[i] = sz;
        gmem_read(cba[i], g_cb[i], sz);
    }
    /* the program at an address is taken as the same until the next submit, as vk_shader_get does: the code isn't
       read and hashed again for every dispatch of it */
    static struct {
        u64 addr;
        u32 epoch, lx, ly, lz, shm;
        CsPipe* p;
    } fc[64];
    u64 pa = code + q[8];
    typeof(fc[0])* fe = &fc[((pa >> 8) ^ (pa >> 16)) & 63];
    CsPipe* p = fe->p;
    if (!p || fe->addr != pa || fe->epoch != g_vk_epoch || fe->lx != lx || fe->ly != ly || fe->lz != lz ||
        fe->shm != shm || !vk_cb_reads_hold(&p->cbr, cs_cb_read)) {
        static u8 code_buf[0x10000 + 80]; /* compute programs have no header: 80 zero bytes stand in for one */
        CsKey unkeyed = {0, 0, 0};
        cs_read_code(pa, code_buf);
        p = cs_pipe(code_buf, lx, ly, lz, shm, unkeyed);
        *fe = (typeof(fc[0])){pa, g_vk_epoch, lx, ly, lz, shm, p};
    }
    /* A 2D store is translated unsigned for a bindless image and float for a bound one. An image of the other
       class (a bindless RGBA8 target), or an integer texture, needs the program translated again, keyed on it. */
    tcb_key = rc[0x982] & 7;
    tic_key_base = ((u64)rc[0x55D] << 32) | rc[0x55E];
    if (!p->failed)
        for (i = 0; i < p->si.ntex && i < SH_MAXTEX; i++) {
            ShTex* st = &p->si.tex[i];
            u32 h = 0, h2 = 0, tic[8];
            VkFmt f;
            if (st->shadow || st->flags) continue; /* raw and multisample textures are declared on their own */
            if (!cs_cb_read(st->bindless ? (st->bl_cbuf & 7) : tcb_key, st->cbuf_off * 4, &h)) continue;
            if (st->bindless && st->bl2 && cs_cb_read(st->bl2_cbuf & 7, st->bl2_off * 4, &h2)) h |= h2;
            gmem_read(tic_key_base + (h & 0xFFFFF) * 32ull, tic, sizeof tic);
            f = vk_tic_format(tic[0], 0);
            if (f.is_int || vk_tic_reads_stencil(tic)) ck.tex_int |= 1ull << i;
            if (f.is_int && f.sint) ck.tex_sint |= 1ull << i;
        }
    if (!p->failed)
        for (i = 0; i < p->si.nimg && i < 8; i++) {
            u32 h = 0, tic[8];
            if (p->si.img[i].type != 3 || p->si.img[i].kind == 1) continue;
            if (!cs_cb_read(p->si.img[i].bindless ? (p->si.img[i].bl_cbuf & 7) : tcb_key, p->si.img[i].cbuf_off * 4,
                            &h))
                continue;
            gmem_read(tic_key_base + (h & 0xFFFFF) * 32ull, tic, sizeof tic);
            if (!!vk_tic_format(tic[0], 0).is_int != !!p->si.img[i].bindless) ck.image |= 1u << i;
        }
    if (ck.tex_int || ck.image) {
        if (!p->keyed || (p->keyed->si.rt_int_mask >> 8) != ck.image || p->keyed->si.tex_int_mask != ck.tex_int ||
            p->keyed->si.tex_sint_mask != ck.tex_sint) {
            static u8 keyed_code[0x10000 + 80];
            cs_read_code(pa, keyed_code);
            p->keyed = cs_pipe(keyed_code, lx, ly, lz, shm, ck);
        }
        p = p->keyed;
    }
    if (p->failed) {
        g_vk_cs_skipped++;
        return;
    }
    int needs_unformatted_image = 0;
    for (i = 0; i < p->si.nimg; i++)
        needs_unformatted_image |= p->si.img[i].kind == 0 || (p->si.img[i].type == 1 && p->si.img[i].kind == 2);
    if (needs_unformatted_image && !g_vk.feat.shaderStorageImageWriteWithoutFormat) {
        LOG_LIMIT(1, "vulkan: storage image writes need shaderStorageImageWriteWithoutFormat (skipped)\n");
        g_vk_cs_skipped++;
        return;
    }
    /* storage buffers: {address, size} in constant buffer 0 */
    u64 sa[16];
    u32 ss[16];
    int ng = p->si.ngbuf > 16 ? 16 : p->si.ngbuf;
    u64 need = 0;
    for (int i = 0; i < ng; i++) {
        u32 v[3] = {0, 0, 0};
        cs_cb_read(0, p->si.gbuf_cbuf_off[i], &v[0]);
        cs_cb_read(0, p->si.gbuf_cbuf_off[i] + 4, &v[1]);
        cs_cb_read(0, p->si.gbuf_cbuf_off[i] + 8, &v[2]);
        sa[i] = ((u64)v[1] << 32) | v[0];
        ss[i] = v[2] ? v[2] : 16;
        if (ss[i] > (16u << 20)) ss[i] = 16u << 20;
        ss[i] = (ss[i] + 3) & ~3u;
        need += ss[i] + vk_stage_align();
    }
    for (int i = 0; i < 8; i++)
        if (p->si.cbuf_mask & (1u << i)) need += g_cbn[i] + vk_stage_align();
    /* buffer images: their headers from the compute engine's texture pool, handles from its texture bank */
    for (i = 0; i < p->si.nimg && i < 8; i++) {
        u64 tic_base;
        u32 h = 0, tic[8];
        if (p->si.img[i].type != 1) continue;
        tic_base = ((u64)rc[0x55D] << 32) | rc[0x55E];
        if ((u32)i >= g_vk_buffer_image_slots ||
            !cs_cb_read(p->si.img[i].bindless ? (p->si.img[i].bl_cbuf & 7) : (rc[0x982] & 7), p->si.img[i].cbuf_off * 4,
                        &h)) {
            g_vk_cs_skipped++;
            return;
        }
        gmem_read(tic_base + (h & 0xFFFFF) * 32ull, tic, sizeof tic);
        if (!cs_buffer_image(tic, p->si.img[i].kind, &bimg[i])) {
            LOG_LIMIT(4, "vulkan: unsupported compute buffer image (header word 0 %08x, skipped)\n", tic[0]);
            g_vk_cs_skipped++;
            return;
        }
        need += bimg[i].bytes + vk_stage_align();
    }
    need += sizeof(i32) * 320 + vk_stage_align(); /* optional format data */
    /* dispatches that read textures run on the graphics queue, after what was recorded before them. Inputs that
       pending compute results overlap land those first, before anything is put in the arena */
    int gfx = p->si.ntex > 0 || p->si.nimg > 0, overlap = 0, storage_images = 0, retries = 0;
    /* so does one whose storage buffers a draw's pending result covers: on the graphics queue, after that draw, it
       reads the result on the GPU instead of the compute queue waiting for it to land (TotK: three a frame) */
    for (i = 0; i < ng && !gfx; i++) gfx = sa[i] && vk_buf_pending(sa[i], ss[i]);
    for (i = 0; i < p->si.nimg; i++) storage_images |= p->si.img[i].type != 1;
    /* A graphics-queue dispatch without storage images binds like a draw: staged inputs, results pending in staging
       (buffer.c's GfxOut), landed when the guest needs them or once their submit is done. Landing each one at once
       took a vk_finish per dispatch, about 9 a frame in TotK. Storage images still land at once (vk_surf_flush) */
    int deferred = gfx && !storage_images;
    /* a read-only input inside a pending result of this batch is read from the arena on the GPU, after a barrier
       (multi-pass skinning reads what the pass before wrote); any other overlap lands the results first */
    i64 chain[16];
    for (int i = 0; i < ng; i++) {
        chain[i] = -1;
        for (int k = 0; k < g_nout && !overlap; k++) {
            CsOut* o = &g_out[k];
            if (sa[i] >= o->addr + o->size || o->addr >= sa[i] + ss[i]) {
                if (vk_backing_overlap(sa[i], ss[i], o->addr, o->size)) overlap = 1;
                continue;
            }
            if (!((p->si.gbuf_wmask >> i) & 1) && g_recording && sa[i] >= o->addr && sa[i] + ss[i] <= o->addr + o->size)
                chain[i] = (i64)(o->off + (sa[i] - o->addr));
            else
                overlap = 1;
        }
    }
    if (deferred)
        for (int i = 0; i < ng; i++) chain[i] = -1; /* inputs come through vk_buf_get, which finds pending results */
    else {
        if (g_recording && g_ar_used + need > g_ar_cap && g_ar_want < (64ull << 20))
            g_ar_want *= 2; /* fewer, bigger batches */
        if (gfx || overlap || g_inflight) {
            cs_land();
            for (int i = 0; i < ng; i++) chain[i] = -1; /* the results are in memory now */
        } else if (g_nout + ng > 256 || g_ar_used + need > g_ar_cap) {
            cs_put_aside(); /* full: it runs while the next one records, and lands when its results are needed */
            for (int i = 0; i < ng; i++) chain[i] = -1; /* a new arena */
        }
        if (!g_recording && !arena_fit(need)) return;
    }

    VkDescriptorSet set;
    u64 setup_sub;
rebind:
    setup_sub = vk_submit_number();
    if (deferred) {
        vk_buf_writable_abort();
        set = vk_desc_alloc(g_vk_dslayout[shader_vk_set(ST_CS)]);
        if (!set) {
            g_vk_cs_skipped++;
            return;
        }
    } else {
        VkDescriptorSetAllocateInfo dai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                           .descriptorPool = g_dpool,
                                           .descriptorSetCount = 1,
                                           .pSetLayouts = &g_vk_dslayout[shader_vk_set(ST_CS)]};
        if (vkAllocateDescriptorSets(g_vk.dev, &dai, &set) != VK_SUCCESS) {
            cs_land();
            if (!VK_OK(vkAllocateDescriptorSets(g_vk.dev, &dai, &set))) return;
        }
    }
    VkWriteDescriptorSet w[SNX_VKB_END];
    VkDescriptorBufferInfo bi[80];
    VkDescriptorImageInfo ii[SH_MAXTEX + 8];
    Surf* images[8] = {0};
    u8* originals[8] = {0};
    VkBufferView bufimg_view[8] = {0};
    VkBuffer bufimg_buf[8] = {0};
    u64 bufimg_off[8] = {0};
    u32 nw = 0;
    for (int i = 0; i < 8; i++)
        if (p->si.cbuf_mask & (1u << i)) {
            /* only the bytes the shader reads (robust buffer access keeps reads past them in bounds, of undefined
               value) */
            u32 range = g_cbn[i];
            if (p->si.cbuf_max[i] && range > p->si.cbuf_max[i]) range = p->si.cbuf_max[i];
            range = (range + 15) & ~15u;
            if (!range) range = 16;
            if (range > g_vk.props.limits.maxUniformBufferRange) range = g_vk.props.limits.maxUniformBufferRange;
            u32 n = g_cbn[i] < range ? g_cbn[i] : range;
            VkBuffer cbuf = g_ar;
            u64 o;
            u8* dst;
            if (deferred) {
                dst = vk_stage(range, vk_stage_align(), &cbuf, &o);
                if (!dst) {
                    g_vk_cs_skipped++;
                    return;
                }
            } else {
                o = arena_alloc(range);
                dst = g_ar_ptr + o;
            }
            memcpy(dst, g_cb[i], n);
            memset(dst + n, 0, range - n);
            bi[nw] = (VkDescriptorBufferInfo){cbuf, o, range};
            w[nw] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                           .dstSet = set,
                                           .dstBinding = SNX_VKB_CB + i,
                                           .descriptorCount = 1,
                                           .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                           .pBufferInfo = &bi[nw]};
            nw++;
        }
    u64 so[16];
    int chained = 0;
    for (int i = 0; i < ng; i++) {
        VkBuffer sbuf = g_ar;
        u64 o;
        if (deferred) {
            /* written: a pending result of its own, from its original bytes; read: the bytes draws would read,
               which may be an earlier draw's or dispatch's result on the GPU */
            int ok = (p->si.gbuf_wmask >> i) & 1 ? vk_buf_copy_writable(sa[i], ss[i], &sbuf, &o)
                     : sa[i]                     ? vk_buf_get_strict(sa[i], ss[i], &sbuf, &o)
                                                 : vk_buf_copy(0, ss[i], &sbuf, &o);
            if (!ok) {
                vk_buf_writable_abort();
                g_vk_cs_skipped++;
                return;
            }
            so[i] = o;
        } else {
            if (chain[i] >= 0) chained = 1;
            o = so[i] = chain[i] >= 0 ? (u64)chain[i] : arena_alloc(ss[i]);
            if (chain[i] >= 0)
                ; /* written by an earlier dispatch of the batch, read in place */
            else if (sa[i])
                gmem_read(sa[i], g_ar_ptr + o, ss[i]);
            else
                memset(g_ar_ptr + o, 0, ss[i]);
        }
        bi[nw] = (VkDescriptorBufferInfo){sbuf, o, ss[i]};
        w[nw] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                       .dstSet = set,
                                       .dstBinding = SNX_VKB_SSBO + i,
                                       .descriptorCount = 1,
                                       .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                       .pBufferInfo = &bi[nw]};
        nw++;
    }
    VkCommandBuffer cb;
    if (gfx) {
        /* textures from the compute engine's headers; texture handles from its texture constant buffer bank. Compute
           shaders declare every texture as float (they aren't keyed on integer textures), and depth textures aren't
           sampled here */
        u64 tic_base = ((u64)rc[0x55D] << 32) | rc[0x55E], tsc_base = ((u64)rc[0x557] << 32) | rc[0x558];
        u32 tcb = rc[0x982] & 7;
        int fmt_used = 0;
        i32 fmt[320] = {0}; /* SNX_FMT, std140: snx_ic[8] snx_tc[32] snx_ib[8] snx_tb[32], 16 bytes each */
        for (int i = 0; i < p->si.ntex && i < SH_MAXTEX; i++) {
            ShTex* st = &p->si.tex[i];
            u32 h = 0, h2 = 0;
            cs_cb_read(st->bindless ? (st->bl_cbuf & 7) : tcb, st->cbuf_off * 4, &h);
            if (st->bindless && st->bl2 && cs_cb_read(st->bl2_cbuf & 7, st->bl2_off * 4, &h2)) h |= h2;
            u32 tic[8], tsc[8];
            gmem_read(tic_base + (h & 0xFFFFF) * 32ull, tic, 32);
            gmem_read(tsc_base + ((h >> 20) & 0xFFF) * 32ull, tsc, 32);
            VkImageViewType want = vk_tex_view_type(st);
            if (st->flags & 2) {
                LOG_LIMIT(4, "vulkan: multisample texture upload is unavailable (skipped)\n");
                g_vk_cs_skipped++;
                cs_abandon_binding(originals);
                return;
            }
            Surf* t = st->flags & 1 ? vk_tex_from_tic_raw(tic) : vk_tex_from_tic(tic);
            if ((st->flags & 1) && !t) {
                LOG_LIMIT(4, "vulkan: raw surface load requires an RGBA8 texture (skipped)\n");
                g_vk_cs_skipped++;
                cs_abandon_binding(originals);
                return;
            }
            int num = st->flags & 1                      ? 1
                      : !((p->si.tex_int_mask >> i) & 1) ? 0
                      : ((p->si.tex_sint_mask >> i) & 1) ? 2
                                                         : 1;
            VkImageView v = t && (!t->f.depth || vk_tic_reads_stencil(tic)) ? vk_tex_bind_view(t, want, tic, tsc, num)
                                                                            : VK_NULL_HANDLE;
            VkImageLayout sampled_layout =
                p->si.nimg ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            const Surf* sampled = v ? t : NULL; /* the texture whose own view is bound, for the sampler */
            if (v)
                vk_surf_use(vk_cmd(), t, sampled_layout, VK_SAMPLED_STAGES, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
            else {
                v = vk_null_view(want, num);
                sampled_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            }
            VkSampler smp = vk_tex_bind_sampler(tsc, st, num, sampled, want);
            if (!v || !smp) { /* before any output is recorded: nothing of this dispatch lands */
                g_vk_cs_skipped++;
                cs_abandon_binding(originals);
                return;
            }
            ii[i] = (VkDescriptorImageInfo){smp, v, sampled_layout};
            w[nw++] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                             .dstSet = set,
                                             .dstBinding = SNX_VKB_TEX + i,
                                             .descriptorCount = 1,
                                             .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                             .pImageInfo = &ii[i]};
            fmt[(8 + i) * 4] = t ? (t->f.vk == VK_FORMAT_R16G16B16A16_SFLOAT || t->f.vk == VK_FORMAT_R16G16_SFLOAT ||
                                            t->f.vk == VK_FORMAT_R16_SFLOAT
                                        ? 1
                                    : t->f.bpp == 4 && !t->f.is_int ? 2
                                                                    : 0)
                                 : 0;
            fmt[(48 + i) * 4] = t && t->f.bpp ? t->f.bpp : 4;
            fmt_used = 1;
        }
        for (int i = 0; i < p->si.nimg; i++) {
            if (p->si.img[i].type == 1) continue; /* buffer images are bound below */
            const u32 off = p->si.img[i].cbuf_off;
            u32 h = 0, tic[8];
            if (!cs_cb_read(p->si.img[i].bindless ? (p->si.img[i].bl_cbuf & 7) : tcb, off * 4, &h)) {
                g_vk_cs_skipped++;
                cs_abandon_binding(originals);
                return;
            }
            gmem_read(tic_base + (h & 0xFFFFF) * 32ull, tic, sizeof tic);
            Surf* t = vk_tex_from_tic(tic);
            u64 image_bytes = vk_surf_guest_size(t);
            VkComponentMapping identity = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                                           VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
            VkImageView view = t && vk_surf_can_writeback(t) && t->storage_capable && t->f.raw && !t->f.depth &&
                                       (p->si.img[i].kind ? t->f.vk == VK_FORMAT_R32_UINT : !t->f.is_int) &&
                                       t->levels == 1 && t->depth3d == 1 && p->si.img[i].type == 3 && image_bytes &&
                                       image_bytes <= UINT32_MAX
                                   ? vk_tex_view(t, VK_IMAGE_VIEW_TYPE_2D, 0, &identity, 0)
                                   : VK_NULL_HANDLE;
            if (!view) {
                LOG_LIMIT(4, "vulkan: unsupported compute storage image (header word 0 %08x, kind %d, skipped)\n",
                          tic[0], p->si.img[i].kind);
                g_vk_cs_skipped++;
                cs_abandon_binding(originals);
                return;
            }
            int seen = 0;
            for (int k = 0; k < i; k++)
                if (images[k] == t) {
                    seen = 1;
                    break;
                }
            if (!seen) {
                originals[i] = malloc(image_bytes);
                if (!originals[i]) {
                    g_vk_cs_skipped++;
                    cs_abandon_binding(originals);
                    return;
                }
                gmem_read(t->gva, originals[i], image_bytes);
            }
            vk_surf_use(vk_cmd(), t, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                        VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
            images[i] = t;
            ii[p->si.ntex + i] = (VkDescriptorImageInfo){VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_GENERAL};
            w[nw++] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                             .dstSet = set,
                                             .dstBinding = SNX_VKB_IMG + i,
                                             .descriptorCount = 1,
                                             .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                             .pImageInfo = &ii[p->si.ntex + i]};
        }
        for (i = 0; i < p->si.nimg && i < 8; i++) {
            int alias;
            int k;
            VkBufferViewCreateInfo vci;
            if (p->si.img[i].type != 1) continue;
            /* the slots of one buffer (its loads and atomics, and its stores) view one copy, so each sees the
               others' writes as the console's would */
            alias = -1;
            for (k = 0; k < i && alias < 0; k++)
                if (bufimg_view[k] && bimg[k].gva == bimg[i].gva) alias = k;
            if (alias >= 0) {
                bufimg_off[i] = bufimg_off[alias];
                bufimg_buf[i] = bufimg_buf[alias];
                if (bimg[i].bytes > bimg[alias].bytes) bimg[i].bytes = bimg[alias].bytes;
            } else if (deferred) { /* a pending result, like a written storage buffer */
                if (!vk_buf_copy_writable(bimg[i].gva, (u32)bimg[i].bytes, &bufimg_buf[i], &bufimg_off[i])) {
                    for (k = 0; k < i; k++)
                        if (bufimg_view[k]) vk_defer_buffer_view(bufimg_view[k]);
                    g_vk_cs_skipped++;
                    cs_abandon_binding(originals);
                    return;
                }
            } else {
                bufimg_off[i] = arena_alloc(bimg[i].bytes);
                bufimg_buf[i] = g_ar;
                gmem_read(bimg[i].gva, g_ar_ptr + bufimg_off[i], bimg[i].bytes);
            }
            memset(&vci, 0, sizeof vci);
            vci.sType = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO;
            vci.buffer = bufimg_buf[i];
            vci.format = bimg[i].fmt;
            vci.offset = bufimg_off[i];
            vci.range = bimg[i].bytes;
            if (!VK_OK(vkCreateBufferView(g_vk.dev, &vci, NULL, &bufimg_view[i]))) {
                for (k = 0; k < i; k++)
                    if (bufimg_view[k]) vk_defer_buffer_view(bufimg_view[k]); /* a draw may still use the set */
                g_vk_cs_skipped++;
                cs_abandon_binding(originals);
                return;
            }
            memset(&w[nw], 0, sizeof w[nw]);
            w[nw].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[nw].dstSet = set;
            w[nw].dstBinding = SNX_VKB_IMGBUF + i;
            w[nw].descriptorCount = 1;
            w[nw].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER;
            w[nw].pTexelBufferView = &bufimg_view[i];
            nw++;
        }
        if (fmt_used) {
            VkBuffer fbuf = g_ar;
            u64 o;
            u8* dst;
            if (deferred) {
                dst = vk_stage(sizeof fmt, vk_stage_align(), &fbuf, &o);
                if (!dst) {
                    for (i = 0; i < p->si.nimg && i < 8; i++)
                        if (bufimg_view[i]) vk_defer_buffer_view(bufimg_view[i]);
                    g_vk_cs_skipped++;
                    cs_abandon_binding(originals);
                    return;
                }
            } else {
                o = arena_alloc(sizeof fmt);
                dst = g_ar_ptr + o;
            }
            memcpy(dst, fmt, sizeof fmt);
            bi[nw] = (VkDescriptorBufferInfo){fbuf, o, sizeof fmt};
            w[nw] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                           .dstSet = set,
                                           .dstBinding = SNX_VKB_FMT,
                                           .descriptorCount = 1,
                                           .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                           .pBufferInfo = &bi[nw]};
            nw++;
        }
    }
    /* reading an input landed results and submitted: what was read before may be stale, as in a draw's setup */
    if (deferred && setup_sub != vk_submit_number()) {
        for (i = 0; i < p->si.nimg && i < 8; i++)
            if (bufimg_view[i]) {
                vk_defer_buffer_view(bufimg_view[i]);
                bufimg_view[i] = VK_NULL_HANDLE;
            }
        vk_buf_writable_abort();
        if (++retries < 4) goto rebind;
        g_vk_cs_skipped++;
        return;
    }
    /* the outputs, once every input is read (an input can alias another storage buffer's output) */
    for (int i = 0; i < ng && !deferred; i++)
        if (((p->si.gbuf_wmask >> i) & 1) && sa[i]) {
            memcpy(g_org + so[i], g_ar_ptr + so[i], ss[i]);
            g_out[g_nout++] = (CsOut){sa[i], so[i], ss[i]};
            if (sa[i] < g_out_lo) g_out_lo = sa[i];
            if (sa[i] + ss[i] > g_out_hi) g_out_hi = sa[i] + ss[i];
        }
    /* buffer images land like storage buffers: only the bytes the dispatch changed are written back */
    for (i = 0; i < p->si.nimg && i < 8 && !deferred; i++) {
        int alias = 0;
        int k;
        for (k = 0; k < i; k++) alias |= bufimg_view[k] && bufimg_off[k] == bufimg_off[i];
        if (bufimg_view[i] && !alias && g_nout < 256) {
            memcpy(g_org + bufimg_off[i], g_ar_ptr + bufimg_off[i], bimg[i].bytes);
            g_out[g_nout].addr = bimg[i].gva;
            g_out[g_nout].off = bufimg_off[i];
            g_out[g_nout].size = bimg[i].bytes;
            g_nout++;
            if (bimg[i].gva < g_out_lo) g_out_lo = bimg[i].gva;
            if (bimg[i].gva + bimg[i].bytes > g_out_hi) g_out_hi = bimg[i].gva + bimg[i].bytes;
        }
    }
    if (gfx) {
        vk_render_end();
        cb = vk_cmd();
    } else {
        if (!g_recording) {
            VkCommandBufferBeginInfo b = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                          .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
            VK_OK(vkBeginCommandBuffer(g_ccb, &b));
            g_recording = 1;
        }
        cb = g_ccb;
    }
    vkUpdateDescriptorSets(g_vk.dev, nw, w, 0, NULL);
    if (chained) {
        VkMemoryBarrier2 cmb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                                .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                                .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT};
        VkDependencyInfo cdi = {
            .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &cmb};
        vkCmdPipelineBarrier2(cb, &cdi);
    }
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, p->p);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_vk_playout, 0, 1, &set, 0, NULL);
    vkCmdDispatch(cb, gx, gy, gz);
    if (deferred) {
        /* pending from here: the submit copies them where the host reads them (vk_buf_host_barrier) */
        vk_buf_writable_commit();
        for (i = 0; i < p->si.nimg && i < 8; i++)
            if (bufimg_view[i]) vk_defer_buffer_view(bufimg_view[i]);
        g_vk_dispatches++;
        return;
    }
    /* results visible to the host */
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                           .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                           .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                           .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT,
                           .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    vkCmdPipelineBarrier2(cb, &di);
    g_vk_dispatches++;
    if (gfx) {
        /* everything before it on the graphics queue runs first; then its results land at once */
        vmaFlushAllocation(g_vk.vma, g_ar_mem, 0, VK_WHOLE_SIZE); /* the arena may not be host coherent */
        vk_finish();
        vmaInvalidateAllocation(g_vk.vma, g_ar_mem, 0, VK_WHOLE_SIZE);
        g_landing = 1;
        for (int i = 0; i < g_nout; i++)
            vk_write_changed(g_out[i].addr, g_ar_ptr + g_out[i].off, g_org + g_out[i].off, g_out[i].size);
        for (i = 0; i < p->si.nimg && i < 8; i++)
            if (bufimg_view[i]) vkDestroyBufferView(g_vk.dev, bufimg_view[i], NULL);
        for (int i = 0; i < p->si.nimg; i++) {
            if (!images[i]) continue; /* a buffer image, landed with the outputs */
            int seen = 0;
            for (int k = 0; k < i; k++)
                if (images[k] == images[i]) {
                    seen = 1;
                    break;
                }
            if (seen) continue;
            images[i]->wgen++;
            vk_surf_flush(images[i], originals[i]);
        }
        for (int i = 0; i < p->si.nimg; i++) free(originals[i]);
        g_nout = 0;
        g_out_lo = ~0ull;
        g_out_hi = 0;
        g_ar_used = 0;
        vkResetDescriptorPool(g_vk.dev, g_dpool, 0);
        g_landing = 0;
        return;
    }
    cs_any_update();
}
