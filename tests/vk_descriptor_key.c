/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "nx/gpu/vk/descriptor_key.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define HANDLE(T, n) ((T)(uintptr_t)(n))
static uint64_t reference[64];
static size_t reference_n;
static void differs(VkDescriptorSetLayout layout, VkWriteDescriptorSet* w) {
    uint64_t key[64];
    size_t n = vk_descriptor_key(key, layout, w, 1);
    assert(n != reference_n || memcmp(key, reference, n * sizeof *key));
}
int main(void) {
    VkDescriptorSetLayout layout = HANDLE(VkDescriptorSetLayout, 0x100);
    VkDescriptorBufferInfo buffers[2] = {{HANDLE(VkBuffer, 0x200), 256, 4096}, {HANDLE(VkBuffer, 0x300), 512, 8192}};
    VkWriteDescriptorSet w = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = HANDLE(VkDescriptorSet, 0x400), .dstBinding = 3, .descriptorCount = 2,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = buffers};
    reference_n = vk_descriptor_key(reference, layout, &w, 1);
    assert(reference_n == VK_DESCRIPTOR_KEY_WORDS(1, 2));
    uint64_t key[64];
    VkDescriptorBufferInfo relocated[2];
    relocated[0] = buffers[0]; relocated[1] = buffers[1];
    w.pBufferInfo = relocated;
    w.dstSet = HANDLE(VkDescriptorSet, 0x500);
    size_t n = vk_descriptor_key(key, layout, &w, 1);
    assert(n == reference_n && !memcmp(key, reference, n * sizeof *key));
    differs(HANDLE(VkDescriptorSetLayout, 0x101), &w);
    relocated[1].range++; differs(layout, &w); relocated[1].range--;
    relocated[1].offset++; differs(layout, &w); relocated[1].offset--;
    relocated[1].buffer = HANDLE(VkBuffer, 0x301); differs(layout, &w); relocated[1] = buffers[1];
    w.dstBinding++; differs(layout, &w); w.dstBinding--;
    w.dstArrayElement = 1u << 24; differs(layout, &w); w.dstArrayElement = 0;
    w.descriptorCount--; differs(layout, &w); w.descriptorCount++;
    w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; differs(layout, &w);
    /* Image layout, sampler and view each affect identity, including late array elements. */
    VkDescriptorImageInfo images[256];
    for (unsigned i = 0; i < 256; i++)
        images[i] = (VkDescriptorImageInfo){HANDLE(VkSampler, 0x600 + i), HANDLE(VkImageView, 0x900 + i),
                                          VK_IMAGE_LAYOUT_GENERAL};
    w = (VkWriteDescriptorSet){.dstBinding = 7, .descriptorCount = 256,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = images};
    uint64_t full[VK_DESCRIPTOR_KEY_WORDS(2, 512) + 1];
    VkWriteDescriptorSet writes[2] = {w, w};
    writes[1].dstBinding++;
    full[VK_DESCRIPTOR_KEY_WORDS(2, 512)] = 0x123456789abcdef0ull;
    n = vk_descriptor_key(full, layout, writes, 2);
    assert(n == VK_DESCRIPTOR_KEY_WORDS(2, 512));
    assert(full[n] == 0x123456789abcdef0ull);
    uint64_t saved[VK_DESCRIPTOR_KEY_WORDS(1, 256)];
    vk_descriptor_key(saved, layout, &w, 1);
    images[255].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vk_descriptor_key(full, layout, &w, 1);
    assert(memcmp(saved, full, sizeof saved));
    images[255].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    images[255].sampler = HANDLE(VkSampler, 0xffff);
    vk_descriptor_key(full, layout, &w, 1);
    assert(memcmp(saved, full, sizeof saved));
    images[255].sampler = HANDLE(VkSampler, 0x600 + 255);
    images[255].imageView = HANDLE(VkImageView, 0xffff);
    vk_descriptor_key(full, layout, &w, 1);
    assert(memcmp(saved, full, sizeof saved));
    VkBufferView view = HANDLE(VkBufferView, 0x1234);
    w = (VkWriteDescriptorSet){.dstBinding = 9, .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, .pTexelBufferView = &view};
    vk_descriptor_key(reference, layout, &w, 1);
    reference_n = VK_DESCRIPTOR_KEY_WORDS(1, 1);
    view = HANDLE(VkBufferView, 0x1235); differs(layout, &w);
    puts("vk_descriptor_key: ok");
    return 0;
}
