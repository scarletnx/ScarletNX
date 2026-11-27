/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The texture table of SM86 programs (ShCaps.tex_table): one descriptor set, bound after the stages' sets
   (ShCaps.table_set), with SNX_TABLE_IMAGES sampled images at binding 0 and SNX_TABLE_SAMPLERS samplers at binding 1.
   A draw gives each plain texture slot of its vertex and fragment programs a word, view slot | sampler slot << 20
   (render.c stage_set_), in place of a set of combined image samplers that it made, hashed and kept for each new
   combination.

   A view takes a slot for each layout it is sampled in, the first time a draw samples it so, and keeps it until the view
   is destroyed (vk_image_view_destroy). A view that a draw samples is destroyed only through vk_defer_image, when the
   frames that used it are done, so a slot that comes back free is used by no pending command buffer. Where every slot
   is taken, the slots no frame in flight used go back (table_reclaim): textures stay alive while memory lasts, and a
   game's thousands of them would otherwise run the table out, and skip each draw that samples one more. A sampler takes a slot for good, as vk_sampler keeps
   its samplers. The set is UPDATE_AFTER_BIND with its slots partially bound: a new slot is written while command
   buffers that bind the set are pending, and those do not read it (UPDATE_UNUSED_WHILE_PENDING).

   With descriptor buffers the set is a descriptor buffer of its own, which lasts the run, where each frame's arena
   holds the stages' sets only for the frame (descriptors.c). A new slot's descriptor goes into its bytes at once
   (vkGetDescriptorEXT), under the same rules: no pending command buffer reads a slot that is written. Each frame's
   arena binds with it, at buffer index 1 (vk_descriptors_table).

   With descriptor heaps the table is also a run of each heap that lasts the run (vk_descriptor_heaps_table), which the
   table set's bindings map to with constant offsets: each slot is written into both, as a draw whose heaps ran out
   takes descriptor buffers. */
#include <string.h>
#include "common/platform/thread.h"
#include "common/utilities/bits.h"
#include "nx/gpu/shader/shader.h"
#include "nx/gpu/vk/vk.h"
#include "nx/gpu/vk/descriptors.h"
#include "nx/gpu/vk/descriptor_heaps.h"
#include "nx/gpu/vk/devmem.h"
#include "nx/gpu/vk/ledger.h"
#include "nx/gpu/vk/pool_map.h"
#include "nx/runtime/platform/log.h"

/* a view in a layout and its slot. view 0 is a free entry */
typedef struct {
    VkImageView view;
    VkImageLayout layout;
    u32 slot;
} TableView;
typedef struct {
    VkSampler sampler;
    u32 slot;
} TableSampler;

#define TABLE_VIEWS    (2 * SNX_TABLE_IMAGES) /* open addressing, at most half full */
#define TABLE_SAMPLERS (2 * SNX_TABLE_SAMPLERS)

static struct {
    VkDescriptorSetLayout layout;
    VkDescriptorPool pool;
    VkDescriptorSet set; /* with descriptor buffers, 1: the table is there (dmap) */
    /* with descriptor buffers: the table's own descriptor buffer, mapped, and each binding's offset and descriptor size
       */
    VkBuffer dbuf;
    DevMem dmem;
    u8* dmap;
    VkDeviceSize dbytes; /* its size, which the descriptor arena's address space leaves room for */
    VkDeviceSize at[2], size[2];
    VkBuffer scratch; /* gbuf_address's scratch words */
    DevMem scratch_mem;
    u64 scratch_address;
    PMutex lock;
    TableView views[TABLE_VIEWS];
    TableSampler samplers[TABLE_SAMPLERS];
    u32 free[SNX_TABLE_IMAGES]; /* the image slots given back, a stack */
    u32 nfree, next_image, next_sampler;
    /* each image slot's view and layout, and the frame (vk_frame_id) a draw last took it in: table_reclaim's */
    TableView slot_view[SNX_TABLE_IMAGES];
    u64 slot_frame[SNX_TABLE_IMAGES];
    /* the entries a draw found last, direct-mapped: a draw's few views and samplers stay in the CPU's nearest caches,
       where the tables above are megabytes. view 0 or sampler 0 is no entry */
    TableView near_views[1024];
    TableSampler near_samplers[256];
} g_table = {.lock = PMUTEX_INIT};
static u32 near_view_index(VkImageView v, VkImageLayout l) {
    const u64 h = (u64)(uintptr_t)v * 0x9E3779B97F4A7C15ull;
    return (u32)((h >> 40) ^ (u32)l) & 1023u;
}
static u32 near_sampler_index(VkSampler s) { return (u32)(((u64)(uintptr_t)s * 0x9E3779B97F4A7C15ull) >> 56); }

static u32 view_home(VkImageView v) {
    return (u32)(snx_fnv_step(SNX_FNV_BASIS, (u64)(uintptr_t)v) & (TABLE_VIEWS - 1));
}
static u32 sampler_home(VkSampler s) {
    return (u32)(snx_fnv_step(SNX_FNV_BASIS, (u64)(uintptr_t)s) & (TABLE_SAMPLERS - 1));
}

/* the table in a descriptor buffer of its own (descriptors.c binds it with each frame's arena): a layout of descriptor
   buffers has no update-after-bind, and its counts go against the plain limits */
static int table_init_buffer(char* why, size_t size, u32 images, u32 samplers) {
    const VkPhysicalDeviceLimits* l = &g_vk.props.limits;
    const VkPhysicalDeviceDescriptorBufferPropertiesEXT* p = &g_vk.descriptor_props;
    const char* no = !g_vk.has_table_features ? "the device lacks a feature of descriptor indexing"
                     : l->maxPerStageDescriptorSampledImages < images || l->maxDescriptorSetSampledImages < images
                         ? "the device binds fewer sampled images"
                     : l->maxPerStageDescriptorSamplers < samplers || l->maxDescriptorSetSamplers < samplers
                         ? "the device binds fewer samplers"
                     : l->maxPerStageResources < images + samplers ? "the device binds fewer resources a stage"
                     : p->maxDescriptorBufferBindings < 2 || p->maxResourceDescriptorBufferBindings < 2 ||
                             p->maxSamplerDescriptorBufferBindings < 2
                         ? "the device binds fewer than two descriptor buffers"
                         : NULL;
    if (no) {
        snprintf(why, size, "%s", no);
        return 0;
    }
    const VkDescriptorBindingFlags binding_flags[2] = {VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT,
                                                       VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT};
    const VkShaderStageFlags stages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    const VkDescriptorSetLayoutBinding b[2] = {{0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, SNX_TABLE_IMAGES, stages, NULL},
                                               {1, VK_DESCRIPTOR_TYPE_SAMPLER, SNX_TABLE_SAMPLERS, stages, NULL}};
    const VkDescriptorSetLayoutBindingFlagsCreateInfo bf = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO,
        .bindingCount = 2,
        .pBindingFlags = binding_flags};
    const VkDescriptorSetLayoutCreateInfo dl = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                                .pNext = &bf,
                                                .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_DESCRIPTOR_BUFFER_BIT_EXT,
                                                .bindingCount = 2,
                                                .pBindings = b};
    if (!VK_OK(vkCreateDescriptorSetLayout(g_vk.dev, &dl, NULL, &g_table.layout))) {
        snprintf(why, size, "the device made no layout for it");
        return 0;
    }
    VkDeviceSize bytes = 0;
    vkGetDescriptorSetLayoutSizeEXT(g_vk.dev, g_table.layout, &bytes);
    /* one buffer holds both bindings: its bytes are within what a binding of either kind reaches */
    if (bytes > p->maxResourceDescriptorBufferRange || bytes > p->maxSamplerDescriptorBufferRange) {
        snprintf(why, size, "its %llu bytes are past the device's descriptor buffer range", (unsigned long long)bytes);
        vkDestroyDescriptorSetLayout(g_vk.dev, g_table.layout, NULL);
        g_table.layout = VK_NULL_HANDLE;
        return 0;
    }
    for (u32 i = 0; i < 2; i++) vkGetDescriptorSetLayoutBindingOffsetEXT(g_vk.dev, g_table.layout, i, &g_table.at[i]);
    g_table.size[0] = p->sampledImageDescriptorSize;
    g_table.size[1] = p->samplerDescriptorSize;
    /* in the GPU's memory where the CPU maps it, as the arenas are: the CPU only writes it */
    const VkBufferCreateInfo bi = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = bytes,
        .usage = VK_BUFFER_USAGE_RESOURCE_DESCRIPTOR_BUFFER_BIT_EXT |
                 VK_BUFFER_USAGE_SAMPLER_DESCRIPTOR_BUFFER_BIT_EXT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    const DevMemWant want = {.at = vk_devmem_tables_at(),
                             .mapped = 1,
                             .need = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT};
    DevMemInfo details;
    if (!bytes || !VK_OK(vk_devmem_buffer(&bi, &want, &g_table.dbuf, &g_table.dmem, &details)) || !details.ptr) {
        snprintf(why, size, "no memory for its descriptor buffer");
        if (g_table.dbuf) vk_devmem_buffer_destroy(g_table.dbuf, g_table.dmem); /* made, but not mapped */
        g_table.dbuf = VK_NULL_HANDLE;
        g_table.dmem = 0;
        vkDestroyDescriptorSetLayout(g_vk.dev, g_table.layout, NULL); /* no table: no layout that names one */
        g_table.layout = VK_NULL_HANDLE;
        return 0;
    }
    vk_mem_track(g_table.dmem, VK_OWN_DESCRIPTORS);
    g_table.dmap = details.ptr;
    g_table.dbytes = bytes;
    vk_descriptors_table(vk_buffer_address(g_table.dbuf));
    g_table.set = (VkDescriptorSet)(uintptr_t)1;
    why[0] = 0;
    return 1;
}
/* slot of binding (0 the images, 1 the samplers) names ii's view in its layout, or its sampler. 0 where the heaps'
   run could not take it: a heap draw would read what the slot held before, so the slot is not given out. A slot of
   the descriptor buffers or sets alone would reach heap draws through the pool map (vk_pool_gpu_take) */
static int table_write(u32 binding, u32 slot, const VkDescriptorImageInfo* ii) {
    const VkDescriptorType type = binding ? VK_DESCRIPTOR_TYPE_SAMPLER : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    /* with heaps, into their run too: a draw whose heaps ran out takes descriptor buffers (render.c) */
    if (g_vk.has_descriptor_heap_support && vk_descriptor_heaps_table() &&
        !vk_descriptor_heaps_table_write(binding, slot, ii)) {
        LOG_LIMIT(4, "vulkan: the texture table's slot %u of binding %u was not written into the heaps\n", slot,
                  binding);
        return 0;
    }
    if (g_table.dmap) {
        VkDescriptorGetInfoEXT get = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_GET_INFO_EXT, .type = type};
        if (binding)
            get.data.pSampler = &ii->sampler;
        else
            get.data.pSampledImage = ii;
        vkGetDescriptorEXT(g_vk.dev, &get, g_table.size[binding],
                           g_table.dmap + g_table.at[binding] + (VkDeviceSize)slot * g_table.size[binding]);
        return 1;
    }
    const VkWriteDescriptorSet w = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                    .dstSet = g_table.set,
                                    .dstBinding = binding,
                                    .dstArrayElement = slot,
                                    .descriptorCount = 1,
                                    .descriptorType = type,
                                    .pImageInfo = ii};
    vkUpdateDescriptorSets(g_vk.dev, 1, &w, 0, NULL);
    return 1;
}
VkDeviceSize vk_table_descriptor_bytes(void) { return g_table.dbuf ? g_table.dbytes : 0; }
int vk_table_init(char* why, size_t size) {
    VkPhysicalDeviceVulkan12Properties p12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES};
    VkPhysicalDeviceProperties2 pp2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &p12};
    vkGetPhysicalDeviceProperties2(g_vk.phys, &pp2);
    /* a pipeline layout with the table counts every set against the update-after-bind limits: the stages' own
       textures and arrays come on top of the table (1024 of room) */
    const u32 images = SNX_TABLE_IMAGES + 1024, samplers = SNX_TABLE_SAMPLERS + 1024;
    if (g_vk.has_descriptor_buffers) return table_init_buffer(why, size, images, samplers);
    const char* no = !g_vk.has_table_features ? "the device lacks a feature of descriptor indexing"
                     : p12.maxPerStageDescriptorUpdateAfterBindSampledImages < images ||
                             p12.maxDescriptorSetUpdateAfterBindSampledImages < images
                         ? "the device binds fewer sampled images after bind"
                     : p12.maxPerStageDescriptorUpdateAfterBindSamplers < samplers ||
                             p12.maxDescriptorSetUpdateAfterBindSamplers < samplers
                         ? "the device binds fewer samplers after bind"
                     : p12.maxPerStageUpdateAfterBindResources < images + samplers
                         ? "the device binds fewer resources a stage after bind"
                         : NULL;
    if (no) {
        snprintf(why, size, "%s", no);
        return 0;
    }
    const VkDescriptorBindingFlags flags = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
                                           VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT |
                                           VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT;
    const VkDescriptorBindingFlags binding_flags[2] = {flags, flags};
    const VkShaderStageFlags stages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    const VkDescriptorSetLayoutBinding b[2] = {{0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, SNX_TABLE_IMAGES, stages, NULL},
                                               {1, VK_DESCRIPTOR_TYPE_SAMPLER, SNX_TABLE_SAMPLERS, stages, NULL}};
    const VkDescriptorSetLayoutBindingFlagsCreateInfo bf = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO,
        .bindingCount = 2,
        .pBindingFlags = binding_flags};
    const VkDescriptorSetLayoutCreateInfo dl = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                                .pNext = &bf,
                                                .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT,
                                                .bindingCount = 2,
                                                .pBindings = b};
    const VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, SNX_TABLE_IMAGES},
                                           {VK_DESCRIPTOR_TYPE_SAMPLER, SNX_TABLE_SAMPLERS}};
    const VkDescriptorPoolCreateInfo pc = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                           .flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT,
                                           .maxSets = 1,
                                           .poolSizeCount = 2,
                                           .pPoolSizes = sizes};
    if (!VK_OK(vkCreateDescriptorSetLayout(g_vk.dev, &dl, NULL, &g_table.layout)) ||
        !VK_OK(vkCreateDescriptorPool(g_vk.dev, &pc, NULL, &g_table.pool))) {
        snprintf(why, size, "the device made no set for it");
        return 0;
    }
    const VkDescriptorSetAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                            .descriptorPool = g_table.pool,
                                            .descriptorSetCount = 1,
                                            .pSetLayouts = &g_table.layout};
    if (!VK_OK(vkAllocateDescriptorSets(g_vk.dev, &ai, &g_table.set))) {
        snprintf(why, size, "the device made no set for it");
        return 0;
    }
    why[0] = 0;
    return 1;
}
VkDescriptorSetLayout vk_table_layout(void) { return g_table.layout; }
u64 vk_buffer_address(VkBuffer b) {
    const VkBufferDeviceAddressInfo info = {.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, .buffer = b};
    return (u64)vkGetBufferDeviceAddress(g_vk.dev, &info);
}
int vk_table_scratch_init(void) {
    const VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                   .size = 256,
                                   .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                   .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    const DevMemWant want = {.at = DM_DEVICE};
    if (!VK_OK(vk_buffer_create(&bi, &want, &g_table.scratch, &g_table.scratch_mem, NULL))) return 0;
    g_table.scratch_address = vk_buffer_address(g_table.scratch);
    return g_table.scratch_address != 0;
}
u64 vk_table_scratch(void) { return g_table.scratch_address; }
VkDescriptorSet vk_table_set(void) { return g_table.set; }

/* the entry of view in layout leaves the table, its slot back to the free stack: the entries after it in their run
   move back where their home lets them (deletion in linear probing without tombstones), so that a lookup stops at the
   first free entry still. Under the lock */
static void forget_entry(u32 i) {
    const TableView gone = g_table.views[i];
    vk_pool_map_view_gone(gone.view); /* the pool map's records that name its slot */
    g_table.free[g_table.nfree++] = gone.slot;
    g_table.slot_view[gone.slot] = (TableView){0};
    TableView* hit = &g_table.near_views[near_view_index(gone.view, gone.layout)];
    if (hit->view == gone.view && hit->layout == gone.layout) *hit = (TableView){0};
    u32 hole = i;
    for (u32 j = (i + 1) & (TABLE_VIEWS - 1); g_table.views[j].view; j = (j + 1) & (TABLE_VIEWS - 1)) {
        const u32 home = view_home(g_table.views[j].view);
        /* j may fill the hole when its home is not in (hole, j] going round */
        const int stays = hole <= j ? (home > hole && home <= j) : (home > hole || home <= j);
        if (stays) continue;
        g_table.views[hole] = g_table.views[j];
        hole = j;
    }
    g_table.views[hole] = (TableView){0};
}
/* every slot no frame in flight took back to the free stack: a slot a draw took in frame f is read until that frame
   is done, f + VK_FRAMES <= vk_frame_id(), as the compute arenas are kept (compute.c). Under the lock */
static void table_reclaim(void) {
    const u64 now = vk_frame_id();
    u32 taken = 0;
    for (u32 slot = 0; slot < g_table.next_image; slot++) {
        const TableView v = g_table.slot_view[slot];
        if (!v.view || g_table.slot_frame[slot] + VK_FRAMES > now) continue;
        u32 i = view_home(v.view);
        while (g_table.views[i].view && (g_table.views[i].view != v.view || g_table.views[i].layout != v.layout))
            i = (i + 1) & (TABLE_VIEWS - 1);
        if (!g_table.views[i].view) continue;
        forget_entry(i);
        taken++;
    }
    LOG_LIMIT(4, "vulkan: the texture table was full: %u slots no frame in flight used came back\n", taken);
}
/* the slot of view in layout, or of sampler, where its near entry holds it: ~0u where not */
static u32 near_view(VkImageView view, VkImageLayout layout) {
    const TableView* hit = &g_table.near_views[near_view_index(view, layout)];
    return hit->view == view && hit->layout == layout ? hit->slot : ~0u;
}
static u32 near_sampler(VkSampler sampler) {
    const TableSampler* hit = &g_table.near_samplers[near_sampler_index(sampler)];
    return hit->sampler == sampler ? hit->slot : ~0u;
}
/* the slot of view in layout, new where it has none: under the lock. ~0u where the table is full, or where the heaps'
   run could not take the view: the slot is then given back, so no draw (with heaps, buffers or sets, or through the
   pool map's learned slots) can name it */
static u32 view_slot(VkImageView view, VkImageLayout layout) {
    TableView* hit = &g_table.near_views[near_view_index(view, layout)];
    const u32 seen = near_view(view, layout);
    if (seen != ~0u) {
        g_table.slot_frame[seen] = vk_frame_id();
        return seen;
    }
    u32 i = view_home(view);
    while (g_table.views[i].view && (g_table.views[i].view != view || g_table.views[i].layout != layout))
        i = (i + 1) & (TABLE_VIEWS - 1);
    TableView* e = &g_table.views[i];
    if (!e->view) {
        if (!g_table.nfree && g_table.next_image == SNX_TABLE_IMAGES) {
            table_reclaim();
            /* entries moved back: view's free place is found again */
            i = view_home(view);
            while (g_table.views[i].view) i = (i + 1) & (TABLE_VIEWS - 1);
            e = &g_table.views[i];
        }
        const u32 slot = g_table.nfree                           ? g_table.free[--g_table.nfree]
                         : g_table.next_image < SNX_TABLE_IMAGES ? g_table.next_image++
                                                                 : ~0u;
        if (slot == ~0u) {
            LOG_LIMIT(1,
                      "vulkan: the texture table is full (%u views, each used by a frame in flight): draws that "
                      "sample more views are skipped\n",
                      SNX_TABLE_IMAGES);
            return ~0u;
        }
        const VkDescriptorImageInfo ii = {VK_NULL_HANDLE, view, layout};
        if (!table_write(0, slot, &ii)) {
            g_table.free[g_table.nfree++] = slot;
            return ~0u;
        }
        *e = (TableView){view, layout, slot};
        g_table.slot_view[slot] = *e;
    }
    *hit = *e;
    g_table.slot_frame[e->slot] = vk_frame_id();
    return e->slot;
}
static u32 sampler_slot(VkSampler sampler) {
    TableSampler* hit = &g_table.near_samplers[near_sampler_index(sampler)];
    const u32 seen = near_sampler(sampler);
    if (seen != ~0u) return seen;
    u32 i = sampler_home(sampler);
    while (g_table.samplers[i].sampler && g_table.samplers[i].sampler != sampler) i = (i + 1) & (TABLE_SAMPLERS - 1);
    TableSampler* e = &g_table.samplers[i];
    if (!e->sampler) {
        /* with heaps, the slots of their run (vk_descriptor_heaps_table_samplers) */
        const u32 most = g_vk.has_descriptor_heap_support && vk_descriptor_heaps_table()
                             ? vk_descriptor_heaps_table_samplers()
                             : SNX_TABLE_SAMPLERS;
        if (g_table.next_sampler == most) {
            LOG_LIMIT(1, "vulkan: the texture table is full (%u samplers)\n", most);
            return ~0u;
        }
        const VkDescriptorImageInfo ii = {sampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
        if (!table_write(1, g_table.next_sampler, &ii)) return ~0u;
        *e = (TableSampler){sampler, g_table.next_sampler++};
    }
    *hit = *e;
    return e->slot;
}
int vk_table_words(const VkImageView* views, const VkImageLayout* layouts, const VkSampler* samplers, u32 n,
                   u32* words) {
    int ok = 1;
    pm_lock(&g_table.lock);
    for (u32 i = 0; i < n && ok; i++) {
        if (!views[i]) {
            words[i] = 0;
            continue;
        }
        const u32 v = view_slot(views[i], layouts[i]), sm = sampler_slot(samplers[i]);
        ok = v != ~0u && sm != ~0u;
        words[i] = v | sm << SNX_TABLE_SAMPLER_SHIFT;
    }
    pm_unlock(&g_table.lock);
    return ok;
}
/* vk_table_words where each view and sampler is in its near entry: the table makes no entry and forgets none, so the
   call touches nothing outside the table and its own lock. A draw of draw_batches=2 calls it with no share lock
   (share_wrap.h), where vk_table_words took it at each draw of the table. 0 where one is not there, with words not
   all written: vk_table_words then */
int vk_table_words_near(const VkImageView* views, const VkImageLayout* layouts, const VkSampler* samplers, u32 n,
                        u32* words) {
    int ok = 1;
    const u64 frame = vk_frame_id();
    pm_lock(&g_table.lock);
    for (u32 i = 0; i < n && ok; i++) {
        if (!views[i]) {
            words[i] = 0;
            continue;
        }
        const u32 v = near_view(views[i], layouts[i]), s = near_sampler(samplers[i]);
        ok = v != ~0u && s != ~0u;
        if (ok) {
            g_table.slot_frame[v] = frame;
            words[i] = v | s << SNX_TABLE_SAMPLER_SHIFT;
        }
    }
    pm_unlock(&g_table.lock);
    return ok;
}
/* each entry of view goes (forget_entry) */
void vk_table_forget_view(VkImageView view) {
    if (!g_table.set || !view) return;
    pm_lock(&g_table.lock);
    for (;;) { /* an entry for each layout the view was sampled in */
        u32 i = view_home(view);
        while (g_table.views[i].view && g_table.views[i].view != view) i = (i + 1) & (TABLE_VIEWS - 1);
        if (!g_table.views[i].view) break;
        forget_entry(i);
    }
    pm_unlock(&g_table.lock);
}
void vk_table_reserve_null(void) {
    pm_lock(&g_table.lock);
    if (!g_table.next_image) g_table.next_image = 1;
    if (!g_table.next_sampler) g_table.next_sampler = 1;
    pm_unlock(&g_table.lock);
}
int vk_table_null(VkImageView view, VkSampler sampler) {
    if (!view || !sampler || !g_table.set) return 0;
    const VkDescriptorImageInfo image = {VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_GENERAL}, smp = {sampler, VK_NULL_HANDLE,
                                                                                                VK_IMAGE_LAYOUT_UNDEFINED};
    pm_lock(&g_table.lock);
    table_write(0, 0, &image);
    table_write(1, 0, &smp);
    pm_unlock(&g_table.lock);
    return 1;
}
void vk_table_touch(u32 image_slot) {
    if (image_slot < SNX_TABLE_IMAGES) g_table.slot_frame[image_slot] = vk_frame_id();
}
