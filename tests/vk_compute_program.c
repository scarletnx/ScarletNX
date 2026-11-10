/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* What the frontends' compute programs share beyond their pipelines (common/gpu/vk/vk_common.h), through fakes of the
   Vulkan calls it makes. snx_vk_compute_layouts makes a set layout of the storage buffers, then the storage images,
   one descriptor each for the compute stage, and a pipeline layout of that set with the push constants asked (no
   range for none); it leaves neither layout when one is refused or the bindings are too many.
   snx_vk_storage_buffers writes each buffer to its own binding, 0 on, as a storage buffer of one descriptor, with
   the offset and range it was given, whatever their number */
#include "common/gpu/vk/vk_common.h"
#include <stdio.h>
#include <string.h>

#define MOST 40
typedef struct {
    int calls, writes, wrong; /* wrong: writes that are not one storage buffer of the set, or of a binding past MOST */
    int seen[MOST];           /* how many times each binding was written */
    VkDescriptorBufferInfo info[MOST];
    VkDevice device;
} FakeWrites;
/* a device and a set that are not null, never dereferenced */
#define DEVICE ((VkDevice)(uintptr_t)0xd0)
#define SET    ((VkDescriptorSet)(uintptr_t)0x5e7)
/* the layouts the fakes hand out, and what the outputs hold before a call */
#define SET_LAYOUT ((VkDescriptorSetLayout)(uintptr_t)0x1a0)
#define LAYOUT     ((VkPipelineLayout)(uintptr_t)0x2b0)
#define STALE_SET  ((VkDescriptorSetLayout)(uintptr_t)0xbad)
#define STALE      ((VkPipelineLayout)(uintptr_t)0xbad)

typedef struct {
    int sets, layouts, destroyed, wrong; /* wrong: a create info that is not the one the program asked for */
    VkResult set_result, layout_result;
    uint32_t buffers, images, push; /* what the call under test asked */
} FakeLayouts;
static struct {
    FakeLayouts layouts;
    FakeWrites writes;
} g_fake;

static VKAPI_ATTR VkResult VKAPI_CALL fake_set_layout(VkDevice dev, const VkDescriptorSetLayoutCreateInfo* info,
                                                      const VkAllocationCallbacks* alloc, VkDescriptorSetLayout* out) {
    (void)alloc;
    g_fake.layouts.sets++;
    if (dev != DEVICE || info->sType != VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO || info->pNext ||
        info->flags || info->bindingCount != g_fake.layouts.buffers + g_fake.layouts.images)
        g_fake.layouts.wrong++;
    else
        for (uint32_t i = 0; i < info->bindingCount; i++) {
            const VkDescriptorSetLayoutBinding* b = &info->pBindings[i];
            VkDescriptorType type =
                i < g_fake.layouts.buffers ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            if (b->binding != i || b->descriptorType != type || b->descriptorCount != 1 ||
                b->stageFlags != VK_SHADER_STAGE_COMPUTE_BIT || b->pImmutableSamplers)
                g_fake.layouts.wrong++;
        }
    *out = SET_LAYOUT;
    return g_fake.layouts.set_result;
}
static VKAPI_ATTR VkResult VKAPI_CALL fake_layout(VkDevice dev, const VkPipelineLayoutCreateInfo* info,
                                                  const VkAllocationCallbacks* alloc, VkPipelineLayout* out) {
    (void)alloc;
    g_fake.layouts.layouts++;
    if (dev != DEVICE || info->sType != VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO || info->pNext || info->flags ||
        info->setLayoutCount != 1 || info->pSetLayouts[0] != SET_LAYOUT ||
        info->pushConstantRangeCount != (g_fake.layouts.push ? 1u : 0u))
        g_fake.layouts.wrong++;
    else if (g_fake.layouts.push
                 ? info->pPushConstantRanges[0].stageFlags != VK_SHADER_STAGE_COMPUTE_BIT ||
                       info->pPushConstantRanges[0].offset || info->pPushConstantRanges[0].size != g_fake.layouts.push
                 : info->pPushConstantRanges != NULL)
        g_fake.layouts.wrong++;
    *out = LAYOUT;
    return g_fake.layouts.layout_result;
}
static VKAPI_ATTR void VKAPI_CALL fake_destroy_set_layout(VkDevice dev, VkDescriptorSetLayout layout,
                                                          const VkAllocationCallbacks* alloc) {
    (void)alloc;
    g_fake.layouts.destroyed++;
    if (dev != DEVICE || layout != SET_LAYOUT) g_fake.layouts.wrong++;
}

/* snx_vk_compute_layouts with what the fakes answer: its result, and the calls it made */
static int layouts_case(uint32_t buffers, uint32_t images, uint32_t push, VkResult set_result, VkResult layout_result,
                        VkResult expected, int sets, int layouts, int destroyed) {
    VkDescriptorSetLayout set = STALE_SET;
    VkPipelineLayout layout = STALE;
    g_fake.layouts = (FakeLayouts){
        .set_result = set_result, .layout_result = layout_result, .buffers = buffers, .images = images, .push = push};
    VkResult r = snx_vk_compute_layouts(DEVICE, buffers, images, push, &set, &layout);
    int made = expected == VK_SUCCESS;
    int ok = r == expected && g_fake.layouts.sets == sets && g_fake.layouts.layouts == layouts &&
             g_fake.layouts.destroyed == destroyed && !g_fake.layouts.wrong &&
             set == (made ? SET_LAYOUT : (VkDescriptorSetLayout)VK_NULL_HANDLE) &&
             layout == (made ? LAYOUT : (VkPipelineLayout)VK_NULL_HANDLE);
    printf("snx_vk_compute_layouts of %u buffers, %u images, %u bytes pushed (the set layout answers %d, the pipeline "
           "layout %d): result %d, %d set layouts made and %d destroyed, %d pipeline layouts, %d wrong, the layouts "
           "%s%s\n",
           buffers, images, push, (int)set_result, (int)layout_result, (int)r, g_fake.layouts.sets,
           g_fake.layouts.destroyed, g_fake.layouts.layouts, g_fake.layouts.wrong,
           set == SET_LAYOUT && layout == LAYOUT ? "both given"
           : !set && !layout                     ? "both null"
                                                 : "not both or neither",
           ok ? "" : ": FAIL");
    if (!ok)
        fprintf(stderr,
                "  result %d expected, %d set layouts made and %d destroyed, %d pipeline layouts, the layouts %s\n",
                (int)expected, sets, destroyed, layouts, made ? "both given" : "both null");
    return !ok;
}

static VKAPI_ATTR void VKAPI_CALL fake_update(VkDevice dev, uint32_t writes, const VkWriteDescriptorSet* w,
                                              uint32_t copies, const VkCopyDescriptorSet* c) {
    (void)c;
    g_fake.writes.calls++;
    g_fake.writes.device = dev;
    g_fake.writes.wrong += copies != 0;
    for (uint32_t i = 0; i < writes; i++) {
        g_fake.writes.writes++;
        if (w[i].sType != VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET || w[i].pNext || w[i].dstSet != SET ||
            w[i].dstBinding >= MOST || w[i].dstArrayElement || w[i].descriptorCount != 1 ||
            w[i].descriptorType != VK_DESCRIPTOR_TYPE_STORAGE_BUFFER || !w[i].pBufferInfo || w[i].pImageInfo ||
            w[i].pTexelBufferView) {
            g_fake.writes.wrong++;
            continue;
        }
        g_fake.writes.seen[w[i].dstBinding]++;
        g_fake.writes.info[w[i].dstBinding] = *w[i].pBufferInfo;
    }
}

int main(void) {
    /* the buffers written, and the calls that takes: sixteen writes a call */
    static const struct {
        uint32_t count;
        int calls;
    } cases[] = {{0, 0}, {1, 1}, {2, 1}, {3, 1}, {16, 1}, {17, 2}, {MOST, 3}};
    int failures = 0;
    const VkResult no_memory = VK_ERROR_OUT_OF_DEVICE_MEMORY, many = VK_ERROR_TOO_MANY_OBJECTS;
    vkCreateDescriptorSetLayout = fake_set_layout;
    vkCreatePipelineLayout = fake_layout;
    vkDestroyDescriptorSetLayout = fake_destroy_set_layout;
    /* the programs the frontends have: two or three buffers with their parameters, one buffer and none, buffers then
       images; then the most bindings there are */
    failures += layouts_case(2, 0, 64, VK_SUCCESS, VK_SUCCESS, VK_SUCCESS, 1, 1, 0);
    failures += layouts_case(3, 0, 16, VK_SUCCESS, VK_SUCCESS, VK_SUCCESS, 1, 1, 0);
    failures += layouts_case(1, 0, 0, VK_SUCCESS, VK_SUCCESS, VK_SUCCESS, 1, 1, 0);
    failures += layouts_case(3, 2, 8, VK_SUCCESS, VK_SUCCESS, VK_SUCCESS, 1, 1, 0);
    failures += layouts_case(0, 1, 4, VK_SUCCESS, VK_SUCCESS, VK_SUCCESS, 1, 1, 0);
    failures += layouts_case(SNX_VK_COMPUTE_BINDINGS, 0, 4, VK_SUCCESS, VK_SUCCESS, VK_SUCCESS, 1, 1, 0);
    failures += layouts_case(9, SNX_VK_COMPUTE_BINDINGS - 9, 4, VK_SUCCESS, VK_SUCCESS, VK_SUCCESS, 1, 1, 0);
    /* a binding too many, in either kind or in a sum that wraps: no call at all */
    failures += layouts_case(SNX_VK_COMPUTE_BINDINGS + 1, 0, 4, VK_SUCCESS, VK_SUCCESS, many, 0, 0, 0);
    failures += layouts_case(9, SNX_VK_COMPUTE_BINDINGS - 8, 4, VK_SUCCESS, VK_SUCCESS, many, 0, 0, 0);
    failures += layouts_case(UINT32_MAX, 2, 4, VK_SUCCESS, VK_SUCCESS, many, 0, 0, 0);
    failures += layouts_case(2, UINT32_MAX, 4, VK_SUCCESS, VK_SUCCESS, many, 0, 0, 0);
    /* a set layout the driver refuses: its error and no pipeline layout; a pipeline layout it refuses: its error, and
       the set layout destroyed */
    failures += layouts_case(2, 0, 64, no_memory, VK_SUCCESS, no_memory, 1, 0, 0);
    failures += layouts_case(2, 0, 64, VK_SUCCESS, no_memory, no_memory, 1, 1, 1);
    vkUpdateDescriptorSets = fake_update;
    for (size_t c = 0; c < sizeof cases / sizeof *cases; c++) {
        VkDescriptorBufferInfo infos[MOST];
        memset(infos, 0, sizeof infos);
        for (uint32_t i = 0; i < MOST; i++) { /* a buffer apart for each binding */
            infos[i].buffer = (VkBuffer)(uintptr_t)(0xb00 + i);
            infos[i].offset = 0x100u * (i + 1);
            infos[i].range = i % 3 ? 0x40u + i : VK_WHOLE_SIZE;
        }
        memset(&g_fake.writes, 0, sizeof g_fake.writes);
        snx_vk_storage_buffers(DEVICE, SET, infos, cases[c].count);
        int ok = g_fake.writes.calls == cases[c].calls && g_fake.writes.writes == (int)cases[c].count &&
                 !g_fake.writes.wrong && (!cases[c].count || g_fake.writes.device == DEVICE);
        for (uint32_t i = 0; i < MOST; i++) {
            int once = i < cases[c].count;
            if (g_fake.writes.seen[i] != once) ok = 0;
            if (once &&
                (g_fake.writes.info[i].buffer != infos[i].buffer || g_fake.writes.info[i].offset != infos[i].offset ||
                 g_fake.writes.info[i].range != infos[i].range))
                ok = 0;
        }
        printf("snx_vk_storage_buffers of %u: %d calls, %d writes, %d wrong%s\n", cases[c].count, g_fake.writes.calls,
               g_fake.writes.writes, g_fake.writes.wrong, ok ? "" : ": FAIL");
        if (!ok) {
            fprintf(stderr,
                    "  %d calls and %u writes expected, each binding from 0 to %u once, a storage buffer of the set "
                    "with its buffer, offset and range\n",
                    cases[c].calls, cases[c].count, cases[c].count ? cases[c].count - 1 : 0);
            failures++;
        }
    }
    return failures != 0;
}
