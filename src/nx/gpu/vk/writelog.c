/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The write logs of compute programs where there is no GPU mirror (mirror.c), a dispatch's own: the bytes each store
   wrote (shader.h's SNX_VKB_WPAGES, SH_WLOG_*), which the renderer lands in guest memory, and no others. A log is host
   memory the GPU writes, kept in a pool: it is cleared when it goes back, where it was written (the windows its flags
   give), so that a dispatch costs what its stores wrote, not the 16 MiB an Ounce program's storage buffer is bound as.
   All under vk_lock, or the GPU thread's before it shares any */
#include <stdlib.h>
#include <string.h>
#include "common/utilities/bits.h"
#include "nx/gpu/shader/shader.h"
#include "nx/runtime/platform/log.h"
#include "nx/gpu/vk/vk.h"
#include "nx/gpu/vk/descriptors.h"

struct VkWriteLog {
    VkBuffer buf;
    VmaAllocation mem;
    u32* w;
    u32 parts, refs; /* parts: its global buffers' masks, SH_WLOG_PART words each */
    int seen;  /* invalidated since the GPU wrote it: its words are the GPU's to the host */
    u32 freed; /* the present it went back to the pool at, for vk_wlog_trim */
    struct VkWriteLog* next;
};

/* the logs free, by their parts less one; a log is never smaller than the program needs, nor bigger */
static VkWriteLog* g_free[16];
static struct {
    u64 made, in_use, bytes, windows, cleared;
} g_wlog;

static u64 log_bytes(u32 parts) { return (u64)parts * SH_WLOG_PART * 4; }

VkWriteLog* vk_wlog_get(u32 parts) {
    if (!parts || parts > 16) return NULL;
    VkWriteLog* l = g_free[parts - 1];
    if (l) {
        g_free[parts - 1] = l->next;
    } else {
        l = calloc(1, sizeof *l);
        if (!l) return NULL;
        u32 fam[2] = {g_vk.gfx_family, g_vk.compute_family};
        VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                  .size = log_bytes(parts),
                                  .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                  /* both queues write logs, one at a time, without ownership transfers */
                                  .sharingMode = fam[0] != fam[1] ? VK_SHARING_MODE_CONCURRENT : VK_SHARING_MODE_EXCLUSIVE,
                                  .queueFamilyIndexCount = fam[0] != fam[1] ? 2 : 0,
                                  .pQueueFamilyIndices = fam};
        /* host memory, cached where it can be: the CPU reads the flags of every part and the masks of the windows
           written, and writes zeros over them */
        VmaAllocationCreateInfo aci = {
            .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT,
            .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST};
        VmaAllocationInfo info;
        if (!VK_OK(vk_buffer_create(g_vk.vma, &bci, &aci, &l->buf, &l->mem, &info))) {
            free(l);
            return NULL;
        }
        vk_mem_track(l->mem);
        l->w = info.pMappedData;
        l->parts = parts;
        memset(l->w, 0, log_bytes(parts)); /* the only time it is cleared whole */
        if (!VK_OK(vmaFlushAllocation(g_vk.vma, l->mem, 0, VK_WHOLE_SIZE))) fatal("vulkan: a write log stayed unflushed");
        g_wlog.made++;
        g_wlog.bytes += log_bytes(parts);
    }
    l->refs = 1;
    l->seen = 0;
    l->next = NULL;
    g_wlog.in_use++;
    return l;
}
VkBuffer vk_wlog_buffer(const VkWriteLog* l) { return l->buf; }
u64 vk_wlog_size(const VkWriteLog* l) { return log_bytes(l->parts); }
void vk_wlog_ref(VkWriteLog* l) { l->refs++; }

/* the GPU's writes visible to the host, once: every reader runs after the work that wrote it is done */
static void log_see(VkWriteLog* l) {
    if (l->seen) return;
    if (!VK_OK(vmaInvalidateAllocation(g_vk.vma, l->mem, 0, VK_WHOLE_SIZE)))
        fatal("vulkan: a write log could not be read after its work completed");
    l->seen = 1;
}
static void log_free(VkWriteLog* l) {
    l->next = g_free[l->parts - 1];
    l->freed = g_vk_present;
    g_free[l->parts - 1] = l;
    g_wlog.in_use--;
}
/* The logs free for WLOG_IDLE presents go: the pool keeps what the dispatches in flight lately took, which one game
   peaked at 290 MB of host memory for while loading. The free lists are newest first, so the ones that go are at
   their ends */
#define WLOG_IDLE 600
void vk_wlog_trim(void) {
    for (int p = 0; p < 16; p++) {
        VkWriteLog** pp = &g_free[p];
        while (*pp && (u32)(g_vk_present - (*pp)->freed) < WLOG_IDLE) pp = &(*pp)->next;
        while (*pp) {
            VkWriteLog* l = *pp;
            *pp = l->next;
            vmaDestroyBuffer(g_vk.vma, l->buf, l->mem); /* free: no work uses it */
            g_wlog.bytes -= log_bytes(l->parts);
            free(l);
        }
    }
}
void vk_wlog_put(VkWriteLog* l) {
    if (l) log_free(l); /* no work recorded with it: clear still */
}
/* the last reference gone, the work that wrote it done: its windows written cleared, and back to the pool */
void vk_wlog_unref(VkWriteLog* l) {
    if (!l || --l->refs) return;
    log_see(l);
    int cleared = 0;
    for (u32 p = 0; p < l->parts; p++) {
        u32* flags = l->w + (size_t)p * SH_WLOG_PART;
        u32* masks = flags + SH_WPAGE_SLOT;
        for (u32 j = 0; j < SH_WPAGE_SLOT; j++)
            if (flags[j]) {
                flags[j] = 0;
                memset(masks + (size_t)j * SH_WLOG_MASK, 0, SH_WLOG_MASK * 4);
                cleared = 1;
                g_wlog.cleared++;
            }
    }
    if (cleared && !VK_OK(vmaFlushAllocation(g_vk.vma, l->mem, 0, VK_WHOLE_SIZE)))
        fatal("vulkan: a write log stayed unflushed");
    log_free(l);
}

u32 vk_wlog_runs(VkWriteLog* l, u32 part, u32 size, VkRunFn fn, void* user) {
    log_see(l);
    const u32* flags = l->w + (size_t)part * SH_WLOG_PART;
    const u32* masks = flags + SH_WPAGE_SLOT;
    u32 windows = (u32)(((u64)size + 4095) / 4096), seen = 0;
    if (windows > SH_WPAGE_SLOT) windows = SH_WPAGE_SLOT;
    u64 from = 0, to = 0; /* the run being gathered, empty when equal */
    for (u32 j = 0; j < windows; j++) {
        if (!flags[j]) continue;
        seen++;
        const u32* m = masks + (size_t)j * SH_WLOG_MASK;
        for (u32 k = 0; k < SH_WLOG_MASK; k++) {
            u32 bits = m[k];
            u64 base = (u64)j * 4096 + (u64)k * 32;
            while (bits) {
                u32 s = snx_ctz32(bits), rest = bits >> s;
                u32 n = snx_ctz32(~rest); /* 32 when rest is all ones, s being 0 then */
                if (n > 32 - s) n = 32 - s;
                u64 a = base + s, b = a + n;
                if (a == to && to != from)
                    to = b; /* goes on from the last */
                else {
                    if (to != from && from < size) fn(user, (u32)from, (u32)(to < size ? to : size));
                    from = a;
                    to = b;
                }
                bits = n + s >= 32 ? 0 : bits & ~(((1u << n) - 1) << s);
            }
        }
    }
    if (to != from && from < size) fn(user, (u32)from, (u32)(to < size ? to : size));
    g_wlog.windows += seen;
    return seen;
}

#include "nx/gpu/vk/wlog_merge_spv.h" /* compiled from wlog_merge.comp by the build (cmake/glsl.cmake) */
typedef struct {
    u32 first, from, to, sadj, dadj;
} MergeParams;
static VkDescriptorSetLayout g_mg_ds;
static VkPipelineLayout g_mg_layout;
static VkPipeline g_mg_pipe;
static void merge_init(void) {
    if (g_mg_pipe) return;
    VkDescriptorSetLayoutBinding b[3];
    for (u32 i = 0; i < 3; i++)
        b[i] = (VkDescriptorSetLayoutBinding){.binding = i,
                                              .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                              .descriptorCount = 1,
                                              .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
    VkDescriptorSetLayoutCreateInfo di = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 3, .pBindings = b};
    if (!VK_OK(vkCreateDescriptorSetLayout(g_vk.dev, &di, NULL, &g_mg_ds)))
        fatal("vulkan: cannot create the write-back merge's descriptor layout");
    VkPushConstantRange push = {VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(MergeParams)};
    VkPipelineLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                     .setLayoutCount = 1,
                                     .pSetLayouts = &g_mg_ds,
                                     .pushConstantRangeCount = 1,
                                     .pPushConstantRanges = &push};
    if (!VK_OK(vkCreatePipelineLayout(g_vk.dev, &li, NULL, &g_mg_layout)))
        fatal("vulkan: cannot create the write-back merge's pipeline layout");
    VkShaderModuleCreateInfo mi = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = wlog_merge_spv_size, .pCode = wlog_merge_spv};
    VkShaderModule module;
    if (!VK_OK(vkCreateShaderModule(g_vk.dev, &mi, NULL, &module))) fatal("vulkan: cannot load the write-back merge");
    VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                                                .module = module,
                                                .pName = "main"},
                                      .layout = g_mg_layout};
    if (!VK_OK(vkCreateComputePipelines(g_vk.dev, vk_pipeline_cache(), 1, &ci, NULL, &g_mg_pipe)))
        fatal("vulkan: cannot create the write-back merge's pipeline");
    vkDestroyShaderModule(g_vk.dev, module, NULL);
}
int vk_wlog_merge(VkWriteLog* l, u32 part, u32 from, u32 to, VkBuffer src, u64 src_off, VkBuffer dst, u64 dst_off) {
    if (from >= to) return 1;
    if (part >= l->parts || to > (u64)SH_WPAGE_SLOT * 4096 || ((from | to | src_off | dst_off) & 3)) return 0;
    merge_init();
    u64 a = g_vk.props.limits.minStorageBufferOffsetAlignment;
    if (a < 4) a = 4;
    /* the descriptors start at offsets the device takes: sadj and dadj skip the words before the part's first */
    u64 sb = (src_off + from) & ~(a - 1), db = dst_off & ~(a - 1);
    VkDescriptorSet set = vk_desc_alloc(g_mg_ds);
    if (!set) return 0;
    VkDescriptorBufferInfo info[3] = {{src, sb, src_off + to - sb},
                                      {dst, db, dst_off + (to - from) - db},
                                      {l->buf, (u64)part * SH_WLOG_PART * 4, (u64)SH_WLOG_PART * 4}};
    VkWriteDescriptorSet w[3];
    for (u32 k = 0; k < 3; k++)
        w[k] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                      .dstSet = set,
                                      .dstBinding = k,
                                      .descriptorCount = 1,
                                      .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                      .pBufferInfo = &info[k]};
    vkUpdateDescriptorSets(g_vk.dev, 3, w, 0, NULL);
    MergeParams p = {from / 4096, from, to, (u32)((src_off + from - sb) / 4), (u32)((dst_off - db) / 4)};
    VkCommandBuffer cb = vk_cmd();
    rec_CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_mg_pipe);
    rec_CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_mg_layout, 0, 1, &set, 0, NULL);
    rec_CmdPushConstants(cb, g_mg_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof p, &p);
    rec_CmdDispatch(cb, (to - 1) / 4096 - from / 4096 + 1, 1, 1);
    return 1;
}

void vk_wlog_stats(u64* logs, u64* bytes, u64* in_use, u64* windows) {
    *logs = g_wlog.made;
    *bytes = g_wlog.bytes;
    *in_use = g_wlog.in_use;
    *windows = g_wlog.windows;
}
