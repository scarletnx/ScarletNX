/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* What the frontends' compute programs share beyond their pipelines (common/gpu/vk/vk_common.h), through a fake of the
   Vulkan call it makes: snx_vk_storage_buffers writes each buffer to its own binding, 0 on, as a storage buffer of
   one descriptor, with the offset and range it was given, whatever their number */
#include "common/gpu/vk/vk_common.h"
#include <stdio.h>
#include <string.h>

#define MOST 40
typedef struct {
    int calls, writes, wrong; /* wrong: writes that are not one storage buffer of the set, or of a binding past MOST */
    int seen[MOST];           /* how many times each binding was written */
    VkDescriptorBufferInfo info[MOST];
    VkDevice device;
} Fake;
static Fake g_fake;
/* a device and a set that are not null, never dereferenced */
#define DEVICE ((VkDevice)(uintptr_t)0xd0)
#define SET    ((VkDescriptorSet)(uintptr_t)0x5e7)

static VKAPI_ATTR void VKAPI_CALL fake_update(VkDevice dev, uint32_t writes, const VkWriteDescriptorSet* w,
                                              uint32_t copies, const VkCopyDescriptorSet* c) {
    (void)c;
    g_fake.calls++;
    g_fake.device = dev;
    g_fake.wrong += copies != 0;
    for (uint32_t i = 0; i < writes; i++) {
        g_fake.writes++;
        if (w[i].sType != VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET || w[i].pNext || w[i].dstSet != SET ||
            w[i].dstBinding >= MOST || w[i].dstArrayElement || w[i].descriptorCount != 1 ||
            w[i].descriptorType != VK_DESCRIPTOR_TYPE_STORAGE_BUFFER || !w[i].pBufferInfo || w[i].pImageInfo ||
            w[i].pTexelBufferView) {
            g_fake.wrong++;
            continue;
        }
        g_fake.seen[w[i].dstBinding]++;
        g_fake.info[w[i].dstBinding] = *w[i].pBufferInfo;
    }
}

int main(void) {
    /* the buffers written, and the calls that takes: sixteen writes a call */
    static const struct {
        uint32_t count;
        int calls;
    } cases[] = {{0, 0}, {1, 1}, {2, 1}, {3, 1}, {16, 1}, {17, 2}, {MOST, 3}};
    int failures = 0;
    vkUpdateDescriptorSets = fake_update;
    for (size_t c = 0; c < sizeof cases / sizeof *cases; c++) {
        VkDescriptorBufferInfo infos[MOST];
        memset(infos, 0, sizeof infos);
        for (uint32_t i = 0; i < MOST; i++) { /* a buffer apart for each binding */
            infos[i].buffer = (VkBuffer)(uintptr_t)(0xb00 + i);
            infos[i].offset = 0x100u * (i + 1);
            infos[i].range = i % 3 ? 0x40u + i : VK_WHOLE_SIZE;
        }
        memset(&g_fake, 0, sizeof g_fake);
        snx_vk_storage_buffers(DEVICE, SET, infos, cases[c].count);
        int ok = g_fake.calls == cases[c].calls && g_fake.writes == (int)cases[c].count && !g_fake.wrong &&
                 (!cases[c].count || g_fake.device == DEVICE);
        for (uint32_t i = 0; i < MOST; i++) {
            int once = i < cases[c].count;
            if (g_fake.seen[i] != once) ok = 0;
            if (once && (g_fake.info[i].buffer != infos[i].buffer || g_fake.info[i].offset != infos[i].offset ||
                         g_fake.info[i].range != infos[i].range))
                ok = 0;
        }
        printf("snx_vk_storage_buffers of %u: %d calls, %d writes, %d wrong%s\n", cases[c].count, g_fake.calls,
               g_fake.writes, g_fake.wrong, ok ? "" : ": FAIL");
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
