/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A raster copy supports quarter-turn presentation, which VkImageBlit cannot express. Each call uses a fresh
   descriptor set from the frame pool. Image views retire with that frame, so threaded recording retains them. */
#include "nx/gpu/vk/descriptor_objects.h"
#include "nx/gpu/vk/present_quad.h"
#include "nx/gpu/vk/present_quad_spv.h"
#include "common/gpu/vk/present_fx.h"
#include "nx/gpu/hdr_output.h"
#include "nx/runtime/platform/log.h"
#include "snx_settings.h"
#include <stdlib.h>

static struct {
    VkDescriptorSetLayout descriptors;
    VkPipelineLayout layout;
    VkShaderModule vertex, fragment;
    VkSampler sampler[2];
    struct { VkFormat format; VkPipeline pipeline; }* pipelines;
    u32 count, capacity;
    /* the window's present's sets, one for each frame in flight, written each present (vk_present_quad_source): a
       set from the frame's pool cost more than the rest of the present */
    VkDescriptorPool pool;
    VkDescriptorSet frames[VK_FRAMES];
    u32 holds[VK_FRAMES]; /* the serial of the view each holds (Surf.present_serial), 0 for none */
    u32 serials;          /* the views made */
    /* the upscaler and the anti aliasing (vk_present_quad_effects): what runs for this run (atomic: a shortcut steps
       them on the frontend's thread), the sharpness, the effects once either is on, and an HDR frame said once */
    int upscaler, aa, sharpness;
    SnxFx* fx;
    int fx_failed, hdr_said;
} g_present_quad;

static int initialize(void) {
    if (g_present_quad.layout) return 1;
    VkDescriptorSetLayoutBinding binding = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                                             VK_SHADER_STAGE_FRAGMENT_BIT, NULL};
    VkDescriptorSetLayoutCreateInfo descriptors = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &binding};
    if (!VK_OK(vkCreateDescriptorSetLayout(g_vk.dev, &descriptors, NULL, &g_present_quad.descriptors))) return 0;
    VkPushConstantRange push = {VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32};
    VkPipelineLayoutCreateInfo layout = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                          .setLayoutCount = 1, .pSetLayouts = &g_present_quad.descriptors,
                                          .pushConstantRangeCount = 1, .pPushConstantRanges = &push};
    if (!VK_OK(vkCreatePipelineLayout(g_vk.dev, &layout, NULL, &g_present_quad.layout))) return 0;
    if (!VK_OK(
            snx_vk_shader_module(g_vk.dev, present_quad_vert_spv, present_quad_vert_spv_size, &g_present_quad.vertex)))
        g_present_quad.vertex = VK_NULL_HANDLE;
    if (!VK_OK(snx_vk_shader_module(g_vk.dev, present_quad_frag_spv, present_quad_frag_spv_size,
                                    &g_present_quad.fragment)))
        g_present_quad.fragment = VK_NULL_HANDLE;
    for (u32 i = 0; i < 2; i++) {
        VkSamplerCreateInfo sampler = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                                        .magFilter = i ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
                                        .minFilter = i ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
                                        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
                                        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
        if (!VK_OK(vk_sampler_create(g_vk.dev, &sampler, NULL, &g_present_quad.sampler[i]))) return 0;
    }
    return g_present_quad.vertex && g_present_quad.fragment;
}
static VkPipeline pipeline(VkFormat format) {
    for (u32 i = 0; i < g_present_quad.count; i++)
        if (g_present_quad.pipelines[i].format == format) return g_present_quad.pipelines[i].pipeline;
    if (!initialize()) return VK_NULL_HANDLE;
    VkPipelineShaderStageCreateInfo stages[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT,
         .module = g_present_quad.vertex, .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
         .module = g_present_quad.fragment, .pName = "main"}};
    VkPipelineVertexInputStateCreateInfo vertex = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo assembly = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                       .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo viewport = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
                                                  .viewportCount = 1, .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo raster = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                    .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE,
                                                    .lineWidth = 1.0f};
    VkPipelineMultisampleStateCreateInfo samples = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                                    .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState attachment = {.colorWriteMask = 15};
    VkPipelineColorBlendStateCreateInfo blend = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                                 .attachmentCount = 1, .pAttachments = &attachment};
    VkDynamicState states[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                                 .dynamicStateCount = 2, .pDynamicStates = states};
    VkPipelineRenderingCreateInfo rendering = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
                                                 .colorAttachmentCount = 1, .pColorAttachmentFormats = &format};
    VkGraphicsPipelineCreateInfo info = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &rendering,
                                          .stageCount = 2, .pStages = stages, .pVertexInputState = &vertex,
                                          .pInputAssemblyState = &assembly, .pViewportState = &viewport,
                                          .pRasterizationState = &raster, .pMultisampleState = &samples,
                                          .pColorBlendState = &blend, .pDynamicState = &dynamic,
                                          .layout = g_present_quad.layout};
    VkPipeline result;
    if (!VK_OK(vkCreateGraphicsPipelines(g_vk.dev, vk_pipeline_cache(), 1, &info, NULL, &result))) return VK_NULL_HANDLE;
    if (g_present_quad.count == g_present_quad.capacity) {
        u32 capacity = g_present_quad.capacity ? g_present_quad.capacity * 2 : 4;
        void* grown = capacity > UINT32_MAX / sizeof *g_present_quad.pipelines ? NULL
                      : realloc(g_present_quad.pipelines, capacity * sizeof *g_present_quad.pipelines);
        if (!grown) { vkDestroyPipeline(g_vk.dev, result, NULL); return VK_NULL_HANDLE; }
        g_present_quad.pipelines = grown;
        g_present_quad.capacity = capacity;
    }
    g_present_quad.pipelines[g_present_quad.count].format = format;
    g_present_quad.pipelines[g_present_quad.count++].pipeline = result;
    return result;
}
static VkImageView view(VkImage image, VkFormat format, VkImageUsageFlags usage) {
    VkImageViewUsageCreateInfo use = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO, .usage = usage};
    VkImageViewCreateInfo info = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .pNext = &use, .image = image,
                                  .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = format,
                                  .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkImageView result = VK_NULL_HANDLE;
    return VK_OK(vk_image_view_create(g_vk.dev, &info, NULL, &result)) ? result : VK_NULL_HANDLE;
}
int vk_present_quad_source(VkCommandBuffer cb, Surf* source, int linear, int raw, SnxPresentSource* out) {
    VkImageView from;
    VkDescriptorSet set;
    if (!source || !initialize()) return 0;
    if (raw && !g_present_quad.pool) { /* the window's: a set of its own for each frame in flight */
        VkDescriptorPoolSize size = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_FRAMES};
        VkDescriptorPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                          .maxSets = VK_FRAMES,
                                          .poolSizeCount = 1,
                                          .pPoolSizes = &size};
        VkDescriptorSetLayout layouts[VK_FRAMES];
        VkDescriptorSetAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                          .descriptorSetCount = VK_FRAMES,
                                          .pSetLayouts = layouts};
        for (u32 i = 0; i < VK_FRAMES; i++) layouts[i] = g_present_quad.descriptors;
        if (!VK_OK(vkCreateDescriptorPool(g_vk.dev, &pci, NULL, &g_present_quad.pool))) {
            g_present_quad.pool = VK_NULL_HANDLE;
            return 0;
        }
        ai.descriptorPool = g_present_quad.pool;
        if (!VK_OK(vkAllocateDescriptorSets(g_vk.dev, &ai, g_present_quad.frames))) {
            vkDestroyDescriptorPool(g_vk.dev, g_present_quad.pool, NULL);
            g_present_quad.pool = VK_NULL_HANDLE;
            return 0;
        }
    }
    /* the frame's own set, which the frame VK_FRAMES back used and vk_cmd waited for; else one from the frame's pool */
    set = raw ? g_present_quad.frames[vk_frame_id() % VK_FRAMES] : vk_desc_alloc(g_present_quad.descriptors);
    if (!set) return 0;
    if (raw) { /* kept on the surface for the next present of it */
        if (!source->present) {
            source->present = view(source->img, vk_srgb_as(source->f.vk, 0), VK_IMAGE_USAGE_SAMPLED_BIT);
            source->present_serial = ++g_present_quad.serials ? g_present_quad.serials : ++g_present_quad.serials;
        }
        from = source->present;
    } else
        from = view(source->img, source->f.vk, VK_IMAGE_USAGE_SAMPLED_BIT);
    if (!from) return 0;
    vk_surf_use(cb, source, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    VkDescriptorImageInfo image = {g_present_quad.sampler[!!linear], from, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet write = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 0,
                                   .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                   .pImageInfo = &image};
    if (raw) { /* the frame's set: written only when it holds another view, or the other sampler */
        u32* holds = &g_present_quad.holds[vk_frame_id() % VK_FRAMES], tag = source->present_serial << 1 | !!linear;
        if (*holds != tag) vkUpdateDescriptorSets(g_vk.dev, 1, &write, 0, NULL);
        *holds = tag;
    } else {
        vkUpdateDescriptorSets(g_vk.dev, 1, &write, 0, NULL);
        vk_defer_image(VK_NULL_HANDLE, 0, from, VK_NULL_HANDLE);
    }
    *out = (SnxPresentSource){.set = set, .view = from, .width = source->sw, .height = source->sh};
    return 1;
}
int vk_present_quad_draw(VkCommandBuffer cb, const SnxPresentSource* s, VkFormat format, VkRect2D source_rect,
                         VkRect2D destination_rect, u32 transform) {
    VkPipeline p = pipeline(format);
    if (!p || !source_rect.extent.width || !source_rect.extent.height || !destination_rect.extent.width ||
        !destination_rect.extent.height)
        return 0;
    VkViewport viewport = {(float)destination_rect.offset.x, (float)destination_rect.offset.y,
                            (float)destination_rect.extent.width, (float)destination_rect.extent.height, 0, 1};
    struct { float crop[4]; u32 flags[4]; } push = {
        {(float)source_rect.offset.x / s->width, (float)source_rect.offset.y / s->height,
         (float)source_rect.extent.width / s->width, (float)source_rect.extent.height / s->height},
        {transform & 7, 0, 0, 0}};
    rec_CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    rec_CmdSetViewport(cb, 0, 1, &viewport);
    rec_CmdSetScissor(cb, 0, 1, &destination_rect);
    rec_CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_present_quad.layout, 0, 1, &s->set, 0, NULL);
    rec_CmdPushConstants(cb, g_present_quad.layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof push, &push);
    rec_CmdDraw(cb, 3, 1, 0, 0);
    return 1;
}
int vk_present_quad(Surf* source, VkImage destination, VkFormat format, VkExtent2D extent,
                    VkRect2D source_rect, VkRect2D destination_rect, u32 transform, int linear) {
    SnxPresentSource from;
    if (!source || !source_rect.extent.width || !source_rect.extent.height ||
        !destination_rect.extent.width || !destination_rect.extent.height || !pipeline(format)) return 0;
    VkImageView to = view(destination, format, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    if (!to) return 0;
    vk_render_end();
    VkCommandBuffer cb = vk_cmd();
    if (!vk_present_quad_source(cb, source, linear, 0, &from)) {
        vk_image_view_destroy(g_vk.dev, to, NULL);
        return 0;
    }
    vk_barrier(cb, destination, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
               VK_ACCESS_2_MEMORY_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
               VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT |
               VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo attachment = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = to,
                                            .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                            .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
    VkRenderingInfo rendering = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, extent},
                                  .layerCount = 1, .colorAttachmentCount = 1, .pColorAttachments = &attachment};
    rec_CmdBeginRendering(cb, &rendering);
    vk_present_quad_draw(cb, &from, format, source_rect, destination_rect, transform);
    rec_CmdEndRendering(cb);
    vk_render_forget_state();
    vk_barrier(cb, destination, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
               VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
               VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    vk_defer_image(VK_NULL_HANDLE, 0, VK_NULL_HANDLE, to);
    return 1;
}

static void effects_say(const char* text) { LOG("vulkan: %s\n", text); }
void vk_present_quad_effects_init(void) {
    SnxFxConfig c;
    char said[512];
    if (snx_fx_settings(&c, g_settings.upscaler, g_settings.upscaler_sharpness, g_settings.anti_aliasing, said,
                        sizeof said))
        LOG("settings: %s\n", said);
    __atomic_store_n(&g_present_quad.upscaler, c.upscaler, __ATOMIC_RELAXED);
    __atomic_store_n(&g_present_quad.aa, c.aa, __ATOMIC_RELAXED);
    g_present_quad.sharpness = c.sharpness;
    LOG("vulkan: upscaler %s, anti aliasing %s\n", snx_fx_upscale_label(c.upscaler), snx_fx_aa_label(c.aa));
}
const char* vk_present_quad_effects_step(int which) {
    int* at = which ? &g_present_quad.aa : &g_present_quad.upscaler;
    int was = __atomic_load_n(at, __ATOMIC_RELAXED);
    int next = which ? snx_fx_aa_next(was) : snx_fx_upscale_next(was);
    while (!__atomic_compare_exchange_n(at, &was, next, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED))
        next = which ? snx_fx_aa_next(was) : snx_fx_upscale_next(was);
    return which ? snx_fx_aa_label(next) : snx_fx_upscale_label(next);
}
int vk_present_quad_effects(VkCommandBuffer cb, SnxPresentSource* s, VkRect2D* crop, VkRect2D to, u32 transform) {
    int filter = __atomic_load_n(&g_present_quad.upscaler, __ATOMIC_RELAXED);
    int aa = __atomic_load_n(&g_present_quad.aa, __ATOMIC_RELAXED);
    u32 w = transform & 4 ? to.extent.height : to.extent.width, h = transform & 4 ? to.extent.width : to.extent.height;
    SnxFxImage out;
    if ((filter == SNX_FX_UPSCALE_OFF && aa == SNX_FX_AA_OFF) || g_present_quad.fx_failed || !w || !h) return 0;
    /* an HDR frame (a game's own, gpu/vk/hdr_swapchain.c) is shown as it is: the passes are for SDR's range */
    if (g_vk.sc_space != SNX_HDR_SPACE_SRGB) {
        if (!g_present_quad.hdr_said++)
            LOG("vulkan: upscaler %s and anti aliasing %s: the game's HDR frames are shown without them\n",
                snx_fx_upscale_label(filter), snx_fx_aa_label(aa));
        return 0;
    }
    g_present_quad.hdr_said = 0;
    if (!g_present_quad.fx) {
        g_present_quad.fx = snx_fx_create(g_vk.phys, g_vk.dev, vk_pipeline_cache(), VK_FRAMES, effects_say);
        if (!g_present_quad.fx) {
            g_present_quad.fx_failed = 1;
            LOG("vulkan: upscaler %s and anti aliasing %s: their passes could not be made on this device, so the window "
                "shows the frame with both off\n",
                snx_fx_upscale_label(filter), snx_fx_aa_label(aa));
            return 0;
        }
        snx_fx_set_sharpness(g_present_quad.fx, g_present_quad.sharpness);
        snx_fx_set_fp16(g_present_quad.fx, g_shader_float16);
    }
    /* the passes record on cb at once, where the commands queued for the recording thread (vk_rec_call) come after
       what they read: the ring drained, every one of those is in cb */
    vk_rec_sync();
    int ran = snx_fx_render(g_present_quad.fx, cb, vk_frame_id() % VK_FRAMES, s->view, s->width, s->height,
                            (u32)crop->offset.x, (u32)crop->offset.y, crop->extent.width, crop->extent.height, 0, w, h,
                            filter, aa, &out);
    vk_render_forget_state(); /* their pipelines and sets are bound now */
    if (!ran) return 0;
    VkDescriptorSet set = vk_desc_alloc(g_present_quad.descriptors);
    if (!set) return 0;
    vk_barrier(cb, out.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT, 0,
               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
               VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    VkDescriptorImageInfo image = {g_present_quad.sampler[1], out.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet write = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                  .dstSet = set,
                                  .dstBinding = 0,
                                  .descriptorCount = 1,
                                  .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                  .pImageInfo = &image};
    vkUpdateDescriptorSets(g_vk.dev, 1, &write, 0, NULL);
    *s = (SnxPresentSource){.set = set, .view = out.view, .width = out.width, .height = out.height};
    *crop = (VkRect2D){{0, 0}, {out.width, out.height}};
    return 1;
}
