/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The stand-in for a game's tensor-core network (setting tensor_standin), which is what a title's AI upscaler is: the
   passes of such a network are HMMA and LDSM, its inputs and outputs are pictures at an image's size, and the ten of
   one title's upscaler are 40 ms of a frame that the exact path emulates with the device's 8x8x8 cooperative matrices
   (hmma_coop). Here each pass is replaced by one dispatch of tensor_standin.comp: the largest picture the network has
   sampled so far, filtered into the image the pass would save. The descriptor sets get their two views each, a ring
   that the pool gives back when it has gone round.

   The one thing this cannot do is make the network's arithmetic: the picture is the network's input, not what it
   would have made of it. A setting, off by default, so nothing a game shows changes unless it is asked for. */
#include <stdlib.h>
#include <string.h>
#include "nx/gpu/gpu.h"
#include "nx/gpu/vk/vk.h"
#include "nx/gpu/vk/tensor_standin_spv.h"
#include "nx/runtime/platform/log.h"

#define TS_SETS 256 /* sets the ring holds before its pool goes round: about 25 frames of a network's ten passes */

/* The network's input picture by where it lies (a surface's address and bytes, and its format): the surface itself may
   be let go while the network still runs, and the cache makes it again at the same address */
typedef struct {
    u64 gva, size;
    VkFormat vk;
} TsInput;
/* the copies recorded this frame, by the picture they came from and the image they went to (TS_MADE of them): a
   later pass of the same network over the same picture asks for bytes that are already there */
#define TS_MADE 64
typedef struct {
    u64 in, out;
    u32 frame;
} TsMade;
static struct {
    VkDescriptorSetLayout lay;
    VkPipelineLayout play;
    VkPipeline pipe;
    VkDescriptorPool pool;
    VkSampler smp;
    TsInput in;
    TsMade made[TS_MADE];
    u32 made_next;
    u32 n;      /* sets allocated in the pool */
    int failed; /* the pipeline or the pool could not be made: every pass runs as it is */
} g_ts;

static int ts_init(void) {
    if (g_ts.failed || g_ts.pipe) return !g_ts.failed;
    VkDescriptorSetLayoutBinding b[2] = {
        {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
    };
    VkDescriptorSetLayoutCreateInfo li = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 2, .pBindings = b};
    VkPipelineLayoutCreateInfo pl = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                     .setLayoutCount = 1,
                                     .pSetLayouts = &g_ts.lay};
    const float max_lod = 0.0f;
    (void)max_lod;
    VkSamplerCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                              .magFilter = VK_FILTER_LINEAR,
                              .minFilter = VK_FILTER_LINEAR,
                              .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
                              .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              .minLod = 0.0f,
                              .maxLod = 0.0f};
    VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, TS_SETS},
                                  {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, TS_SETS}};
    VkDescriptorPoolCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                     .maxSets = TS_SETS,
                                     .poolSizeCount = 2,
                                     .pPoolSizes = ps};
    if (!VK_OK(vkCreateDescriptorSetLayout(g_vk.dev, &li, NULL, &g_ts.lay)) ||
        !VK_OK(vkCreatePipelineLayout(g_vk.dev, &pl, NULL, &g_ts.play)) ||
        !VK_OK(vkCreateSampler(g_vk.dev, &si, NULL, &g_ts.smp)) ||
        !VK_OK(vkCreateDescriptorPool(g_vk.dev, &pi, NULL, &g_ts.pool)) ||
        !VK_OK(snx_vk_compute_pipeline(g_vk.dev, vk_pipeline_cache(), g_ts.play, tensor_standin_spv,
                                       tensor_standin_spv_size, &g_ts.pipe))) {
        g_ts.failed = 1;
        g_ts.pipe = VK_NULL_HANDLE;
        WARN("vulkan: the tensor stand-in's pipeline could not be made: the networks' passes run as they are\n");
        return 0;
    }
    return 1;
}

/* the picture the network sampled in this pass: the pass that saves it copies that one, not what the network would */
void vk_tensor_remember(const Surf* in) {
    if (!g_settings.tensor_standin || !in || in->f.depth || in->f.is_int) return;
    g_ts.in = (TsInput){in->gva, in->size, in->f.vk};
}

/* the surface the network's picture lies in now, or NULL: another at the address, or none */
static Surf* ts_input(void) {
    if (!g_ts.in.gva || !g_ts.in.size) return NULL;
    for (Surf* t = vk_surf_first(g_ts.in.gva); t; t = t->next)
        if (t->gva == g_ts.in.gva && t->size == g_ts.in.size && t->f.vk == g_ts.in.vk &&
            t->type == VK_IMAGE_VIEW_TYPE_2D && !t->msaa)
            return t;
    return NULL;
}

/* a set of the ring with the two views: its pool goes round once every set is in the GPU's hands */
static VkDescriptorSet ts_set(VkImageView src, VkImageView dst) {
    if (g_ts.n == TS_SETS) {
        vk_finish();
        vkResetDescriptorPool(g_vk.dev, g_ts.pool, 0);
        g_ts.n = 0;
    }
    VkDescriptorSetAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                      .descriptorPool = g_ts.pool,
                                      .descriptorSetCount = 1,
                                      .pSetLayouts = &g_ts.lay};
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (!VK_OK(vkAllocateDescriptorSets(g_vk.dev, &ai, &set))) return VK_NULL_HANDLE;
    g_ts.n++;
    VkDescriptorImageInfo ii[2] = {{.sampler = g_ts.smp, .imageView = src, .imageLayout = VK_IMAGE_LAYOUT_GENERAL},
                                   {.sampler = VK_NULL_HANDLE, .imageView = dst, .imageLayout = VK_IMAGE_LAYOUT_GENERAL}};
    VkWriteDescriptorSet w[2] = {
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
         .dstSet = set,
         .dstBinding = 0,
         .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
         .pImageInfo = &ii[0]},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
         .dstSet = set,
         .dstBinding = 1,
         .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
         .pImageInfo = &ii[1]},
    };
    vkUpdateDescriptorSets(g_vk.dev, 2, w, 0, NULL);
    return set;
}

/* The picture the network sampled so far into out, where out is a 2D image the pass it stands in for would save: 1
   when the dispatch is recorded. 0 for a pass whose pictures are not that (a depth image, an integer one, a buffer), or
   one with no picture sampled before it */
int vk_tensor_standin(VkCommandBuffer cb, Surf* out) {
    if (!g_settings.tensor_standin || !out || !ts_init()) return 0;
    if (out->type != VK_IMAGE_VIEW_TYPE_2D || out->msaa || out->f.depth || out->f.is_int || out->f.stencil) return 0;
    Surf* in = ts_input();
    if (!in || in == out) return 0;
    /* A picture this frame's earlier pass already put in this image from this one: the same bytes again, and a full
       frame of copies a pass at a time is what stood the ten of a network for 9 ms of a frame. The picture changes from
       one frame to the next, so the next frame copies again */
    for (u32 k = 0; k < TS_MADE; k++)
        if (g_ts.made[k].frame == g_vk_present && g_ts.made[k].in == in->gva && g_ts.made[k].out == out->gva) return 2;
    const u32 ow = out->sw ? out->sw : out->w, oh = out->sh ? out->sh : out->h;
    if (!ow || !oh) return 0;
    const VkComponentMapping identity = {0};
    VkImageView sv = vk_tex_view(in, VK_IMAGE_VIEW_TYPE_2D, 0, &identity, 0);
    VkImageView dv = vk_tex_storage_view(out, VK_IMAGE_VIEW_TYPE_2D);
    if (!sv || !dv) return 0;
    VkDescriptorSet set = ts_set(sv, dv);
    if (!set) return 0;
    vk_render_end();
    vk_surf_use(cb, in, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    vk_surf_use(cb, out, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    rec_CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_ts.pipe);
    rec_CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_ts.play, 0, 1, &set, 0, NULL);
    rec_CmdDispatchKey(cb, (ow + 7) / 8, (oh + 7) / 8, 1, out->gva, 0);
    g_ts.made[g_ts.made_next] = (TsMade){in->gva, out->gva, g_vk_present};
    g_ts.made_next = (g_ts.made_next + 1) % TS_MADE;
    return 1;
}
