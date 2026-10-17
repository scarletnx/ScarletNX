/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Frames in flight. A frame runs from one present to the next and may submit many times (a readback, results the
   guest needs): each submission gets its own command buffer and signals the next value of the graphics timeline
   semaphore. A frame has a staging ring for uploads and for data draws read directly (vertices, indices, constant
   buffers), a ring its storage results are copied back into for the CPU, descriptor pools, and the resources to
   destroy once its work is done; none of them is reused before its last submission has finished. Ending a frame at a
   submit would reset them mid-draw, and wait for the GPU at every submit. */
#include <stdlib.h>
#include "nx/runtime/platform/log.h"
#include "nx/gpu/vk/vk.h"
#include "nx/runtime/platform/plat.h"

typedef struct {
    VkImage img;
    VmaAllocation mem;
    VkImageView v1, v2;
    VkBuffer buf;
    VkBufferView view;
} Dead;

/* A per-frame ring of one mapped buffer, reused once the frame's previous work is done. */
typedef struct {
    VkBuffer buf;
    VmaAllocation mem;
    u8* ptr;
    u64 cap, used;
    u32 small; /* frames in a row that used a quarter of it or less */
} Ring;

typedef struct {
    VkCommandPool pool;
    VkCommandBuffer* cbs; /* one per submission in this use of the frame */
    u32 ncb, icb;         /* icb: the one recording, or the next to begin */
    u64 last;             /* the timeline value of the frame's last submission, 0 when it has none */
    int recording, fresh; /* fresh: just ended up current, not yet reset for its new use */
    Ring stage;           /* uploads and draw data, written by the CPU and read by the GPU */
    Ring back;            /* storage results copied back by the GPU for the CPU to read (vk_stage_back) */
    /* destroyed once this frame's work is done */
    Dead* dead;
    int ndead, capdead;
    /* descriptor pools, reset when the frame is reused and kept */
    VkDescriptorPool pool_d[64];
    int npool, ipool; /* pool_d[ipool] is the one allocating; the ones after it are reset and waiting */
} Frame;

static Frame g_fr[VK_FRAMES];
static u32 g_cur;
static u64 g_frame_id;
static VkSemaphore g_gsem; /* graphics timeline: submission n signals n */
static u64 g_gval;         /* the last submission */
static u64 g_landed;       /* submissions whose storage results vk_buf_frame_done has landed */
u32 g_vk_epoch, g_vk_present;

u64 vk_hash(const void* p, u64 size) {
    const u64* q = p;
    u64 n = size / 8, i = 0;
    u64 a = 0x9E3779B97F4A7C15ull, b = 0xC2B2AE3D27D4EB4Full, c = 0x165667B19E3779F9ull, d = 0x27D4EB2F165667C5ull;
    for (; i + 3 < n; i += 4) {
        a = (a ^ q[i]) * 0x100000001B3ull;
        b = (b ^ q[i + 1]) * 0x100000001B3ull;
        c = (c ^ q[i + 2]) * 0x100000001B3ull;
        d = (d ^ q[i + 3]) * 0x100000001B3ull;
    }
    for (; i < n; i++) a = (a ^ q[i]) * 0x100000001B3ull;
    for (u64 k = n * 8; k < size; k++) b = (b ^ ((const u8*)p)[k]) * 0x100000001B3ull;
    return a ^ (b << 1) ^ (c << 2) ^ (d << 3) ^ size;
}

u64 vk_sample_hash(u64 gva, u64 len) {
    u8 tmp[2048 + 128];
    if (len <= sizeof tmp) {
        gmem_read(gva, tmp, len);
        return vk_hash(tmp, len);
    }
    u32 n = 0;
    gmem_read(gva, tmp, 64);
    n = 64;
    gmem_read(gva + len - 64, tmp + n, 64);
    n += 64;
    u64 step = (len - 32) / 62;
    for (int i = 0; i < 62; i++) {
        gmem_read(gva + (u64)i * step, tmp + n, 32);
        n += 32;
    }
    return vk_hash(tmp, n);
}

u64 vk_stage_align(void) {
    static u64 al;
    if (!al) {
        const VkPhysicalDeviceLimits* l = &g_vk.props.limits;
        al = 16;
        if (l->minUniformBufferOffsetAlignment > al) al = l->minUniformBufferOffsetAlignment;
        if (l->minStorageBufferOffsetAlignment > al) al = l->minStorageBufferOffsetAlignment;
        if (l->minTexelBufferOffsetAlignment > al) al = l->minTexelBufferOffsetAlignment; /* compute buffer images */
    }
    return al;
}

static void dead_push(Frame* f, Dead d) {
    if (f->ndead == f->capdead) {
        f->capdead = f->capdead ? f->capdead * 2 : 64;
        f->dead = realloc(f->dead, sizeof *f->dead * f->capdead);
    }
    f->dead[f->ndead++] = d;
}
static void dead_free(Frame* f) {
    for (int i = 0; i < f->ndead; i++) {
        Dead* d = &f->dead[i];
        if (d->v1) vkDestroyImageView(g_vk.dev, d->v1, NULL);
        if (d->v2) vkDestroyImageView(g_vk.dev, d->v2, NULL);
        if (d->view) vkDestroyBufferView(g_vk.dev, d->view, NULL);
        if (d->img) vmaDestroyImage(g_vk.vma, d->img, d->mem);
        if (d->buf) vmaDestroyBuffer(g_vk.vma, d->buf, d->mem);
    }
    f->ndead = 0;
}

void vk_cmd_init(void) {
    VkCommandPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                   .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
                                   .queueFamilyIndex = g_vk.gfx_family};
    VkSemaphoreTypeCreateInfo tci = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
                                     .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE};
    VkSemaphoreCreateInfo sci = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &tci};
    VK_OK(vkCreateSemaphore(g_vk.dev, &sci, NULL, &g_gsem));
    for (int i = 0; i < VK_FRAMES; i++) {
        VK_OK(vkCreateCommandPool(g_vk.dev, &pci, NULL, &g_fr[i].pool));
        g_fr[i].fresh = 1;
    }
}

#define STAGE_MIN (16ull << 20)

/* a ring grown for a burst goes once it has been mostly unused for a while (the frame's work is done, nothing reads
   it); the next allocation makes one of the usual size */
static void ring_reset(Ring* r) {
    r->small = r->cap > STAGE_MIN && r->used <= r->cap / 4 ? r->small + 1 : 0;
    if (r->small >= 300) {
        vmaDestroyBuffer(g_vk.vma, r->buf, r->mem);
        *r = (Ring){0};
    }
    r->used = 0;
}

static void timeline_wait(u64 value) {
    VkSemaphoreWaitInfo wi = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, .semaphoreCount = 1, .pSemaphores = &g_gsem, .pValues = &value};
    VK_OK(vkWaitSemaphores(g_vk.dev, &wi, UINT64_MAX));
}
/* the storage results of every submission up to value, which has finished, land in guest memory, in order */
static void land_through(u64 value) {
    while (g_landed < value) vk_buf_frame_done(++g_landed);
}

/* the frame's previous use is done, its results landed and what it held freed, before it records again */
static void frame_recycle(Frame* f) {
    if (f->last) {
        timeline_wait(f->last);
        land_through(f->last);
        f->last = 0;
    }
    dead_free(f);
    vkResetCommandPool(g_vk.dev, f->pool, 0);
    /* the pools stay, reset: a frame's draws fill several, and making them again every frame costs */
    for (int i = 0; i < f->npool; i++) vkResetDescriptorPool(g_vk.dev, f->pool_d[i], 0);
    f->ipool = 0;
    ring_reset(&f->stage);
    ring_reset(&f->back);
    f->icb = 0;
    f->fresh = 0;
}

VkCommandBuffer vk_cmd(void) {
    Frame* f = &g_fr[g_cur];
    if (!f->recording) {
        if (f->fresh) frame_recycle(f);
        if (f->icb == f->ncb) {
            VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                              .commandPool = f->pool,
                                              .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                              .commandBufferCount = 1};
            VkCommandBuffer* cbs = realloc(f->cbs, sizeof *cbs * (f->ncb + 1));
            if (!cbs) fatal("vulkan: out of memory for command buffers");
            f->cbs = cbs;
            VK_OK(vkAllocateCommandBuffers(g_vk.dev, &ai, &f->cbs[f->ncb]));
            f->ncb++;
        }
        VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                       .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
        VK_OK(vkBeginCommandBuffer(f->cbs[f->icb], &bi));
        f->recording = 1;
    }
    return f->cbs[f->icb];
}

static u8* ring_alloc(Frame* f, Ring* r, u64 size, u64 align, VkBufferUsageFlags usage, VmaAllocationCreateFlags flags,
                      VmaMemoryUsage where, VkBuffer* buf, u64* off, VmaAllocation* mem) {
    if (align < 16) align = 16;
    u64 at = (r->used + align - 1) & ~(align - 1);
    if (!r->buf || at + size > r->cap) {
        /* a bigger buffer; the old one stays alive until this frame's work is done */
        if (r->buf) {
            /* The submit only flushes the current staging ring. Flush a retired one too: earlier copies and ASTC
               dispatches still read its host writes (the readback ring has none) */
            if (r == &f->stage) vmaFlushAllocation(g_vk.vma, r->mem, 0, VK_WHOLE_SIZE);
            dead_push(f, (Dead){.buf = r->buf, .mem = r->mem});
        }
        u64 cap = r->cap ? r->cap * 2 : STAGE_MIN;
        while (cap < size) cap *= 2;
        VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                  .size = cap,
                                  .usage = usage,
                                  .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
        VmaAllocationCreateInfo aci = {.flags = flags | VMA_ALLOCATION_CREATE_MAPPED_BIT, .usage = where};
        VmaAllocationInfo info;
        if (!VK_OK(vmaCreateBuffer(g_vk.vma, &bci, &aci, &r->buf, &r->mem, &info))) {
            r->buf = VK_NULL_HANDLE;
            r->cap = 0;
            return NULL;
        }
        r->ptr = info.pMappedData;
        r->cap = cap;
        at = 0;
    }
    r->used = at + size;
    *buf = r->buf;
    *off = at;
    if (mem) *mem = r->mem;
    return r->ptr + at;
}

u8* vk_stage_output(u64 size, u64 align, VkBuffer* buf, u64* off, VmaAllocation* mem) {
    vk_cmd(); /* the ring is only reused once this frame's previous work is done */
    /* written once from the CPU, read by the GPU: device-local when the CPU can map it (resizable BAR) */
    return ring_alloc(&g_fr[g_cur], &g_fr[g_cur].stage, size, align,
                      VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                          VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                          VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                          VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT,
                      VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT, VMA_MEMORY_USAGE_AUTO, buf, off, mem);
}
u8* vk_stage(u64 size, u64 align, VkBuffer* buf, u64* off) { return vk_stage_output(size, align, buf, off, NULL); }

/* The staging ring can be device-local memory behind the BAR, where every CPU read crosses PCIe uncached: comparing
   a frame's storage results there cost Tears of the Kingdom most of its GPU thread. The frame's results are copied
   on the GPU into host-cached memory instead, and read from there. */
u8* vk_stage_back(u64 size, VkBuffer* buf, u64* off, VmaAllocation* mem) {
    vk_cmd();
    return ring_alloc(&g_fr[g_cur], &g_fr[g_cur].back, size, g_vk.props.limits.nonCoherentAtomSize,
                      VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT,
                      VMA_MEMORY_USAGE_AUTO_PREFER_HOST, buf, off, mem);
}

void vk_submit(VkSemaphore wait, VkPipelineStageFlags2 wait_stage, VkSemaphore signal) {
    vk_render_end();
    Frame* f = &g_fr[g_cur];
    VkCommandBuffer cb = vk_cmd(); /* a submit with nothing recorded still has to wait and signal */
    vk_query_flush(cb);            /* the samples-passed counts of the passes ended (query.c) */
    vk_buf_host_barrier(cb, g_gval + 1);
    /* staging memory may not be host coherent: the host writes are made visible to the GPU here */
    if (f->stage.mem) vmaFlushAllocation(g_vk.vma, f->stage.mem, 0, VK_WHOLE_SIZE);
    VK_OK(vkEndCommandBuffer(cb));
    VkSemaphoreSubmitInfo w[2] = {
        {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = wait, .stageMask = wait_stage}};
    u32 nw = wait ? 1 : 0;
    if (g_vk_cs_wait_value) { /* draws read compute results on the GPU: after that batch */
        w[nw++] = (VkSemaphoreSubmitInfo){.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
                                          .semaphore = g_vk_csem,
                                          .value = g_vk_cs_wait_value,
                                          .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT};
        g_vk_cs_wait_value = 0;
    }
    VkSemaphoreSubmitInfo s[2] = {{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
                                   .semaphore = g_gsem,
                                   .value = g_gval + 1,
                                   .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT},
                                  {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
                                   .semaphore = signal,
                                   .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT}};
    VkCommandBufferSubmitInfo c = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = cb};
    VkSubmitInfo2 si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
                        .waitSemaphoreInfoCount = nw,
                        .pWaitSemaphoreInfos = w,
                        .commandBufferInfoCount = 1,
                        .pCommandBufferInfos = &c,
                        .signalSemaphoreInfoCount = signal ? 2u : 1u,
                        .pSignalSemaphoreInfos = s};
    VkResult r = vkQueueSubmit2(g_vk.gfx, 1, &si, VK_NULL_HANDLE);
    if (r == VK_ERROR_DEVICE_LOST) fatal("vulkan: the GPU device was lost (driver reset or crash)");
    VK_OK(r);
    g_gval++;
    if (r != VK_SUCCESS) { /* never signaled by the GPU: the host signals it, so waits for it and later ones end */
        VkSemaphoreSignalInfo ss = {
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO, .semaphore = g_gsem, .value = g_gval};
        VK_OK(vkSignalSemaphore(g_vk.dev, &ss));
    }
    f->recording = 0;
    f->icb++;
    f->last = g_gval;
    /* results of submissions the GPU has finished land now, so a later release doesn't find them pending */
    u64 done = 0;
    if (VK_OK(vkGetSemaphoreCounterValue(g_vk.dev, g_gsem, &done))) land_through(done < g_gval ? done : g_gval);
}

u64 g_vk_finishes, g_vk_finish_ns;
void vk_finish(void) {
    u64 t0 = plat_ns();
    vk_submit(VK_NULL_HANDLE, 0, VK_NULL_HANDLE);
    timeline_wait(g_gval);
    land_through(g_gval);
    g_vk_finishes++;
    g_vk_finish_ns += plat_ns() - t0;
}

/* A present ends the frame: the next one records in the other slot, once that slot's previous use is done */
void vk_frame_next(void) {
    if (g_fr[g_cur].recording) vk_submit(VK_NULL_HANDLE, 0, VK_NULL_HANDLE);
    g_cur = (g_cur + 1) % VK_FRAMES;
    g_fr[g_cur].fresh = 1;
    g_frame_id++;
}
/* A frame that goes long without a present (a loading screen) still has to hand its pools and rings back: one with
   many submissions or descriptor pools ends here, at the start of a draw or dispatch, before anything is set up */
void vk_frame_check(void) {
    const Frame* f = &g_fr[g_cur];
    if (f->icb >= 64 || f->ipool >= 32) vk_frame_next();
}

u64 vk_submit_number(void) { return g_gval + 1; }
u64 vk_last_submit(void) { return g_gval; }
void vk_wait_submit(u64 value) { timeline_wait(value); }
void vk_land_through(u64 value) { land_through(value < g_gval ? value : g_gval); }
u64 vk_frame_id(void) { return g_frame_id; }

static VkDescriptorPool desc_pool_new(void) {
    /* a thousand sets of the stage layout (gpu/vk/pipeline.c), each with all its descriptors: a frame chains more
       pools when it needs them */
    VkDescriptorPoolSize sz[VK_POOL_TYPES];
    VkDescriptorPoolCreateInfo ci = {0};
    vk_pool_sizes(sz, 1024);
    ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    ci.maxSets = 1024;
    ci.poolSizeCount = VK_POOL_TYPES;
    ci.pPoolSizes = sz;
    VkDescriptorPool p = VK_NULL_HANDLE;
    VK_OK(vkCreateDescriptorPool(g_vk.dev, &ci, NULL, &p));
    return p;
}
VkDescriptorSet vk_desc_alloc(VkDescriptorSetLayout layout) {
    vk_cmd(); /* the pools are reset when the frame starts */
    Frame* f = &g_fr[g_cur];
    for (int tries = 0; tries < 2; tries++) {
        if (tries) f->ipool++;
        if (f->ipool == f->npool) {
            if (f->npool == (int)(sizeof f->pool_d / sizeof f->pool_d[0])) {
                LOG_LIMIT(1, "vulkan: a frame used all its descriptor pools (draws skipped)\n");
                return VK_NULL_HANDLE;
            }
            VkDescriptorPool p = desc_pool_new();
            if (!p) return VK_NULL_HANDLE;
            f->pool_d[f->npool++] = p;
        }
        VkDescriptorSetAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                          .descriptorPool = f->pool_d[f->ipool],
                                          .descriptorSetCount = 1,
                                          .pSetLayouts = &layout};
        VkDescriptorSet set;
        VkResult r = vkAllocateDescriptorSets(g_vk.dev, &ai, &set);
        if (r == VK_SUCCESS) return set;
        if (r != VK_ERROR_OUT_OF_POOL_MEMORY && r != VK_ERROR_FRAGMENTED_POOL) {
            VK_OK(r);
            return VK_NULL_HANDLE;
        }
    }
    return VK_NULL_HANDLE;
}

/* vk_cmd() first: after a frame advance g_cur is the next slot, still holding an older frame's work. Starting it waits for
   that work and frees its list, so what is pushed now waits for the frame recorded from now on, which is submitted
   after every frame that can still use it */
void vk_defer_image(VkImage img, VmaAllocation a, VkImageView v1, VkImageView v2) {
    vk_cmd();
    dead_push(&g_fr[g_cur], (Dead){.img = img, .mem = a, .v1 = v1, .v2 = v2});
}
void vk_defer_buffer(VkBuffer b, VmaAllocation a) {
    vk_cmd();
    dead_push(&g_fr[g_cur], (Dead){.buf = b, .mem = a});
}
void vk_defer_buffer_view(VkBufferView v) {
    vk_cmd();
    dead_push(&g_fr[g_cur], (Dead){.view = v});
}

void vk_barrier(VkCommandBuffer cb, VkImage img, VkImageAspectFlags aspect, VkPipelineStageFlags2 src_stage,
                VkAccessFlags2 src_access, VkImageLayout from, VkPipelineStageFlags2 dst_stage,
                VkAccessFlags2 dst_access, VkImageLayout to) {
    VkImageMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                               .srcStageMask = src_stage,
                               .srcAccessMask = src_access,
                               .dstStageMask = dst_stage,
                               .dstAccessMask = dst_access,
                               .oldLayout = from,
                               .newLayout = to,
                               .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                               .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                               .image = img,
                               .subresourceRange = {aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS}};
    VkDependencyInfo d = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &b};
    vkCmdPipelineBarrier2(cb, &d);
}

/* one buffer the GPU copies into for the host to read: vk_readback_buffer before recording the copy, vk_finish,
   then vk_readback_ready before reading the pointer */
static VkBuffer g_rb;
static VmaAllocation g_rb_mem;
static u8* g_rb_ptr;
static u64 g_rb_cap;
static u32 g_rb_small; /* readbacks in a row of an eighth of it or less */
u8* vk_readback_buffer(u64 size, VkBuffer* buf) {
    /* it grows to the biggest readback, and shrinks back after many small ones */
    g_rb_small = g_rb_cap > STAGE_MIN && size <= g_rb_cap / 8 ? g_rb_small + 1 : 0;
    if (size > g_rb_cap || g_rb_small >= 64) {
        if (g_rb) vk_defer_buffer(g_rb, g_rb_mem);
        u64 cap = size > g_rb_cap ? (g_rb_cap ? g_rb_cap : STAGE_MIN) : STAGE_MIN;
        g_rb_small = 0;
        while (cap < size) cap *= 2;
        VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                  .size = cap,
                                  .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                  .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
        VmaAllocationCreateInfo aci = {.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT |
                                                VMA_ALLOCATION_CREATE_MAPPED_BIT,
                                       .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST};
        VmaAllocationInfo info;
        if (!VK_OK(vmaCreateBuffer(g_vk.vma, &bci, &aci, &g_rb, &g_rb_mem, &info))) {
            g_rb = VK_NULL_HANDLE;
            g_rb_cap = 0;
            return NULL;
        }
        g_rb_ptr = info.pMappedData;
        g_rb_cap = cap;
    }
    *buf = g_rb;
    return g_rb_ptr;
}
void vk_readback_ready(void) { vmaInvalidateAllocation(g_vk.vma, g_rb_mem, 0, VK_WHOLE_SIZE); }
