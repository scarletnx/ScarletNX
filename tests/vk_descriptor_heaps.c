/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Pure geometry and mapping proofs use the same planning helpers as the Vulkan backend, with independent range,
   alignment and index-address assertions. No Vulkan device or descriptor contents are assumed. */
#include "nx/gpu/vk/descriptor_heaps.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t checks, failures;
static void check(int pass, const char* message) {
    checks++;
    if (!pass && failures++ < 12) fprintf(stderr, "FAIL descriptor heap: %s\n", message);
}
static uint32_t random_word(uint32_t* state) { *state = *state * 1664525u + 1013904223u; return *state; }
/* Mock indices are ordinary CPU values, not Vulkan descriptor encodings. Compare production run detection and
   write-only index filling with an independent per-element, byte-by-byte table oracle. */
static void image_runs(void) {
    VkDescriptorImageInfo images[257];
    for (uint32_t i = 0; i < 257; i++) {
        memset(&images[i], (int)(i & 255), sizeof images[i]);
        images[i].imageView = (VkImageView)(uintptr_t)0x1357;
        images[i].sampler = (VkSampler)(uintptr_t)0x2468;
        images[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
    check(!vk_descriptor_heap_image_run(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, NULL, 256) && !vk_descriptor_heap_image_run(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, images, 0),
          "empty or missing image ranges contain no reusable entry");
    check(vk_descriptor_heap_image_run(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, images, 256) == 256,
          "all equal descriptor fields form one run regardless of object padding");
    images[7].sampler = (VkSampler)(uintptr_t)0x2469;
    check(vk_descriptor_heap_image_run(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, images, 256) == 7, "a changed sampler ends the run");
    check(vk_descriptor_heap_image_run(VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, images, 256) == 256 &&
              vk_descriptor_heap_image_run(VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, images, 256) == 256,
          "separate images ignore differing sampler fields and reuse the complete run");
    check(!vk_descriptor_heap_image_run(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, images, 256),
          "a non-image descriptor cannot reuse image metadata");
    images[7].sampler = images[0].sampler;
    images[7].imageView = (VkImageView)(uintptr_t)0x1358;
    check(vk_descriptor_heap_image_run(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, images, 256) == 7, "a changed image view ends the run");
    images[7].imageView = images[0].imageView;
    images[7].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    check(vk_descriptor_heap_image_run(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, images, 256) == 7, "a changed image layout ends the run");
    images[7].imageLayout = images[0].imageLayout;
    images[256].imageView = VK_NULL_HANDLE;
    check(vk_descriptor_heap_image_run(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, images, 256) == 256 && vk_descriptor_heap_image_run(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, images, 17) == 17 &&
              vk_descriptor_heap_image_run(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, images + 249, 7) == 7,
          "run detection respects the remaining array range and cannot inspect the next element");
    vk_descriptor_heap_write_index_run(NULL, 0, 0);
    enum { BYTES = 2 * (256 + 16) * 4 + 38, RESOURCE = 19 + 3 * 4, SAMPLER = 19 + (256 + 11) * 4 };
    uint8_t actual[BYTES], expected[BYTES];
    for (uint32_t pattern = 0; pattern < 4; pattern++)
        for (uint32_t count = 1; count <= 256; count++) {
            for (uint32_t i = 0; i < count; i++) {
                images[i].imageView = (VkImageView)(uintptr_t)(0x1357 + (pattern == 1 ? i & 1 : pattern == 3 ? i / 13 : 0));
                images[i].sampler = (VkSampler)(uintptr_t)(0x2468 + (pattern == 2 ? i / 7 : 0));
                images[i].imageLayout = pattern == 3 && i >= count / 2
                    ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            }
            memset(actual, 0xa5, sizeof actual);
            memset(expected, 0xa5, sizeof expected);
            uint32_t lookups = 0, replicas = 0;
            for (uint32_t i = 0; i < count;) {
                uint32_t run = vk_descriptor_heap_image_run(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, images + i, count - i);
                uint32_t resource = (uint32_t)(uintptr_t)images[i].imageView ^ (uint32_t)images[i].imageLayout << 24;
                uint32_t sampler = (uint32_t)(uintptr_t)images[i].sampler;
                check(run && run <= count - i, "run length remains within the validated binding");
                vk_descriptor_heap_write_index_run(actual + RESOURCE + 4 * i, run, resource);
                vk_descriptor_heap_write_index_run(actual + SAMPLER + 4 * i, run, sampler);
                lookups++;
                replicas += run - 1;
                i += run;
            }
            for (uint32_t i = 0; i < count; i++) {
                uint32_t resource = (uint32_t)(uintptr_t)images[i].imageView ^ (uint32_t)images[i].imageLayout << 24;
                uint32_t sampler = (uint32_t)(uintptr_t)images[i].sampler;
                for (uint32_t byte = 0; byte < 4; byte++) {
                    expected[RESOURCE + 4 * i + byte] = (uint8_t)(resource >> (8 * byte));
                    expected[SAMPLER + 4 * i + byte] = (uint8_t)(sampler >> (8 * byte));
                }
            }
            check(!memcmp(actual, expected, sizeof actual),
                  "resource/sampler runs match every independent little-endian index and preserve gaps/guards");
            check(lookups + replicas == count, "logical requests count every element while lookups count actual runs");
            if (!pattern) check(lookups == 1 && replicas == count - 1, "one repeated array resolves just one descriptor pair");
            if (pattern == 1) check(lookups == count && !replicas, "alternating fields retain independent lookups");
            if (pattern == 2) check(lookups == (count + 6) / 7, "sampler changes split shared-image runs exactly");
        }
}
static void descriptor_alignment(void) {
    static const VkDescriptorType images[] = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
        VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER};
    static const VkDeviceSize alignments[][3] = {{32, 16, 8}, {4, 4, 4}, {16, 64, 32}};
    for (uint32_t test = 0; test < sizeof alignments / sizeof *alignments; test++) {
        VkPhysicalDeviceDescriptorHeapPropertiesEXT props = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_HEAP_PROPERTIES_EXT,
            .imageDescriptorAlignment = alignments[test][0], .bufferDescriptorAlignment = alignments[test][1],
            .samplerDescriptorAlignment = alignments[test][2]};
        for (uint32_t image = 0; image < sizeof images / sizeof *images; image++)
            check(vk_descriptor_heap_descriptor_alignment(&props, images[image]) == alignments[test][0],
                  "images and formatted texel buffers use image alignment even when buffer alignment differs");
        check(vk_descriptor_heap_descriptor_alignment(&props, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER) == alignments[test][1] &&
                  vk_descriptor_heap_descriptor_alignment(&props, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) == alignments[test][1],
              "unformatted buffers use their separate alignment");
        check(vk_descriptor_heap_descriptor_alignment(&props, VK_DESCRIPTOR_TYPE_SAMPLER) == alignments[test][2],
              "samplers use their independent heap alignment");
    }
    VkPhysicalDeviceDescriptorHeapPropertiesEXT props = {.imageDescriptorAlignment = 32, .bufferDescriptorAlignment = 16};
    VkDescriptorHeapMemoryPlan plan;
    check(vk_descriptor_heap_memory_plan(&plan, 256, 2, 32, 32, 32, 1024, 1024),
          "mixed typed-buffer allocation has two image-aligned frame partitions");
    for (uint32_t slot = 0; slot < 2; slot++)
        for (VkDeviceSize preceding = 0; preceding < 64; preceding++) {
            VkDeviceSize used = preceding;
            uint32_t offset;
            check(vk_descriptor_heap_allocate(&plan, &used, slot, 32,
                      vk_descriptor_heap_descriptor_alignment(&props, VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER), &offset) &&
                      offset == slot * 256 + ((preceding + 31) / 32) * 32 && offset % 32 == 0,
                  "typed-buffer placement remains image-aligned after arbitrary preceding index/buffer bytes");
        }
    check(!vk_descriptor_heap_descriptor_alignment(NULL, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) &&
              !vk_descriptor_heap_descriptor_alignment(&props, VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT),
          "missing properties and unsupported descriptor classes are rejected");
}
static void plans(void) {
    uint32_t state = 0x367263;
    for (uint32_t i = 0; i < 10000; i++) {
        VkDeviceSize alignment = 1ull << (random_word(&state) % 13);
        VkDeviceSize descriptor_alignment = 1ull << (random_word(&state) % 8);
        VkDeviceSize requested = 1 + random_word(&state) % (2u << 20);
        VkDeviceSize reserved = random_word(&state) % (256u << 10);
        VkDescriptorHeapMemoryPlan plan;
        check(vk_descriptor_heap_memory_plan(&plan, requested, 2, reserved, alignment, descriptor_alignment,
                                              16ull << 20, 16ull << 20), "valid bounded plan succeeds");
        check(plan.frame_size >= requested && plan.frame_size - requested < descriptor_alignment &&
                  plan.frame_size % descriptor_alignment == 0, "frame range uses exact descriptor alignment");
        check(plan.reserved_offset == 2 * plan.frame_size && plan.reserved_size == reserved &&
                  plan.range_size == plan.reserved_offset + reserved, "reserved bytes follow both live frame ranges");
        check(plan.allocation_size == plan.range_size + alignment - 1, "only worst-case address padding is added");
        for (VkDeviceSize address = 1; address <= alignment; address++) {
            VkDeviceSize padded = (address + alignment - 1) / alignment * alignment;
            check(padded % alignment == 0 && padded - address + plan.range_size <= plan.allocation_size,
                  "all buffer-address residues contain the complete aligned heap range");
        }
        VkDescriptorHeapMemoryPlan rejected;
        check(!vk_descriptor_heap_memory_plan(&rejected, requested, 2, reserved, alignment, descriptor_alignment,
                                               plan.range_size - 1, plan.allocation_size), "heap-size cap is exact");
        check(!vk_descriptor_heap_memory_plan(&rejected, requested, 2, reserved, alignment, descriptor_alignment,
                                               plan.range_size, plan.allocation_size - 1), "allocation cap includes address padding");
    }
    VkDescriptorHeapMemoryPlan plan;
    check(!vk_descriptor_heap_memory_plan(&plan, 0, 2, 0, 1, 1, UINT64_MAX, UINT64_MAX), "zero frame range rejected");
    check(!vk_descriptor_heap_memory_plan(&plan, 1, 0, 0, 1, 1, UINT64_MAX, UINT64_MAX), "zero frame count rejected");
    check(!vk_descriptor_heap_memory_plan(&plan, 1, 2, 0, 0, 1, UINT64_MAX, UINT64_MAX), "zero heap alignment rejected");
    check(!vk_descriptor_heap_memory_plan(&plan, 1, 2, 0, 1, 0, UINT64_MAX, UINT64_MAX), "zero descriptor alignment rejected");
    check(!vk_descriptor_heap_memory_plan(&plan, UINT64_MAX, 2, 0, 8, 8, UINT64_MAX, UINT64_MAX), "alignment overflow rejected");
    check(!vk_descriptor_heap_memory_plan(&plan, UINT32_MAX, 2, 0, 1, 1, UINT64_MAX, UINT64_MAX), "32-bit index overflow rejected");
    check(!vk_descriptor_heap_memory_plan(&plan, 16, 2, UINT64_MAX, 8, 8, UINT64_MAX, UINT64_MAX), "reserved-range overflow rejected");
    check(!vk_descriptor_heap_memory_plan(&plan, 1, 1, UINT64_MAX - 1, 8, 1, UINT64_MAX, UINT64_MAX), "padding overflow rejected");
    check(vk_descriptor_heap_memory_plan(&plan, 1u << 20, 2, 0x17a00, 256, 32, 32u << 20, 64u << 20) &&
              plan.reserved_offset == 2u << 20 && plan.range_size == (2u << 20) + 0x17a00,
          "resource reservation is additional and disjoint from user data");
}
static void constrained_heaps(void) {
    VkDescriptorHeapMemoryPlan sampler, resource;
    check(vk_descriptor_heap_memory_plan_fit(&sampler, 128u << 10, 2, 512, 32, 32, 131072, 64u << 20),
          "reported small sampler heap remains usable");
    check(sampler.frame_size == 65280 && sampler.reserved_offset == 130560 && sampler.range_size == 131072 &&
              sampler.reserved_size == 512, "sampler maximum includes its reservation and both frame partitions");
    check(vk_descriptor_heap_memory_plan_fit(&resource, 1u << 20, 2, 96768, 64, 64, 32u << 20, 64u << 20) &&
              resource.frame_size == 1u << 20 && resource.reserved_offset == 2u << 20 &&
              resource.range_size == (2u << 20) + 96768, "resource heap respects its separate limit and reservation");
    VkDeviceSize cursors[2] = {0};
    uint32_t offsets[2][2040];
    for (uint32_t slot = 0; slot < 2; slot++) {
        for (uint32_t i = 0; i < 2040; i++) {
            check(vk_descriptor_heap_allocate(&sampler, &cursors[slot], slot, 32, 32, &offsets[slot][i]),
                  "every live sampler slot fits exactly once");
            check(offsets[slot][i] == slot * 65280 + i * 32 && offsets[slot][i] + 32 <= sampler.reserved_offset,
                  "frame slots and reserved bytes never overlap");
        }
        uint32_t untouched = 0x12345678;
        check(!vk_descriptor_heap_allocate(&sampler, &cursors[slot], slot, 32, 32, &untouched) &&
                  cursors[slot] == 65280 && untouched == 0x12345678,
              "exhaustion preserves live offsets and cursor without a frame transition");
    }
    /* Only slot0's fence/reset permits reuse. Slot1's live indices remain unchanged. */
    cursors[0] = 0;
    uint32_t reused = UINT32_MAX;
    check(vk_descriptor_heap_allocate(&sampler, &cursors[0], 0, 32, 32, &reused) && reused == offsets[0][0] &&
              cursors[1] == 65280 && offsets[1][2039] == 130528, "fenced-slot reuse does not change the other live frame");
    check(!vk_descriptor_heap_memory_plan_fit(&sampler, 4096, 2, 512, 32, 32, 543, 4096),
          "reservation-only sampler range cannot masquerade as usable descriptor space");
    check(vk_descriptor_heap_memory_plan_fit(&sampler, 4096, 2, 512, 32, 32, 576, 4096) && sampler.frame_size == 32,
          "one descriptor per frame is a valid compact arena");
    check(vk_descriptor_heap_memory_plan_fit(&sampler, UINT64_MAX, 2, 512, 32, 32, 131072, 131103) &&
              sampler.frame_size == 65280, "oversized target clamps without overflow");
    check(vk_descriptor_heap_memory_plan_fit(&sampler, 4096, 2, 512, 32, 32, 131072, 1567) &&
              sampler.frame_size == 512 && sampler.allocation_size == 1567,
          "backing allocation cap can be stricter than the device heap limit");
    uint32_t state = 0x61578;
    for (uint32_t i = 0; i < 10000; i++) {
        VkDeviceSize alignment = 1ull << (random_word(&state) % 9);
        VkDeviceSize reserved = random_word(&state) % 4096;
        VkDeviceSize heap_limit = reserved + 2 * alignment + random_word(&state) % 65536;
        VkDeviceSize allocation_limit = reserved + 2 * alignment + random_word(&state) % 65536 + 255;
        check(vk_descriptor_heap_memory_plan_fit(&sampler, 1u << 20, 2, reserved, alignment, alignment,
                                                  heap_limit, allocation_limit), "random bounded heap can retain live frame ranges");
        check(sampler.range_size <= heap_limit && sampler.allocation_size <= allocation_limit &&
                  sampler.frame_size % alignment == 0 && sampler.reserved_offset == 2 * sampler.frame_size &&
                  sampler.reserved_offset + reserved == sampler.range_size,
              "clamped plans preserve exact capacity, alignment and reservation geometry");
    }
}
static void table_runs(void) {
    uint32_t words = 0, resource, sampler;
    check(vk_descriptor_heap_table_append(&words, 34, 0, &resource, &sampler) && words == 34 && resource == 0,
          "scalar buffer bindings occupy only resource words");
    check(vk_descriptor_heap_table_append(&words, 1, 1, &resource, &sampler) &&
              words == 36 && resource == 34 && sampler == 35,
          "one sampled texture occupies36 words/144 bytes after34 buffers");
    check(vk_descriptor_heap_table_append(&words, 1, 1, &resource, &sampler) &&
              words == 38 && resource == 36 && sampler == 37, "each scalar sampler stays adjacent to its image index");
    check(vk_descriptor_heap_table_append(&words, 256, 1, &resource, &sampler) &&
              words == 550 && resource == 38 && sampler == 294, "array resource and sampler runs remain separately contiguous");
    VkDescriptorSetAndBindingMappingEXT map;
    check(vk_descriptor_heap_binding_mapping(&map, 0, 99, resource, sampler, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER),
          "array run maps without an extra pointer or packed index");
    for (uint32_t i = 0; i < 256; i++)
        check(map.sourceData.indirectIndexArray.addressOffset / 4 + i == 38 + i &&
                  map.sourceData.indirectIndexArray.samplerAddressOffset / 4 + i == 294 + i,
              "interleaving preserves all array elements and independent sampler indices");
    check(vk_descriptor_heap_table_append(&words, 1, 0, &resource, &sampler) && words == 551 && resource == 550,
          "the next binding starts beyond both array runs");
    uint32_t old = words;
    check(!vk_descriptor_heap_table_append(&words, UINT32_MAX, 1, &resource, &sampler) && words == old,
          "table overflow leaves the previous mapping unchanged");
}
static void mappings(void) {
    static const VkDescriptorType types[] = {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER};
    static const VkSpirvResourceTypeFlagsEXT masks[] = {VK_SPIRV_RESOURCE_TYPE_UNIFORM_BUFFER_BIT_EXT,
        VK_SPIRV_RESOURCE_TYPE_READ_ONLY_STORAGE_BUFFER_BIT_EXT | VK_SPIRV_RESOURCE_TYPE_READ_WRITE_STORAGE_BUFFER_BIT_EXT,
        VK_SPIRV_RESOURCE_TYPE_COMBINED_SAMPLED_IMAGE_BIT_EXT,
        VK_SPIRV_RESOURCE_TYPE_READ_ONLY_IMAGE_BIT_EXT | VK_SPIRV_RESOURCE_TYPE_READ_WRITE_IMAGE_BIT_EXT,
        VK_SPIRV_RESOURCE_TYPE_READ_ONLY_IMAGE_BIT_EXT | VK_SPIRV_RESOURCE_TYPE_READ_WRITE_IMAGE_BIT_EXT};
    uint32_t resources[1024], samplers[1024];
    for (uint32_t i = 0; i < 1024; i++) { resources[i] = 0x100000 + i * 32; samplers[i] = 0x10000 + i * 16; }
    for (uint32_t set = 0; set < 5; set++)
        for (uint32_t type = 0; type < sizeof types / sizeof *types; type++) {
            VkDescriptorSetAndBindingMappingEXT mapping;
            check(vk_descriptor_heap_binding_mapping(&mapping, set, 73, 127, 383, types[type]), "mapping constructor succeeds");
            check(mapping.sType == VK_STRUCTURE_TYPE_DESCRIPTOR_SET_AND_BINDING_MAPPING_EXT && !mapping.pNext &&
                      mapping.descriptorSet == set && mapping.firstBinding == 73 && mapping.bindingCount == 1 &&
                      mapping.resourceMask == masks[type], "mapping identifies the complete original SPIR-V resource class");
            const VkDescriptorMappingSourceIndirectIndexArrayEXT* s = &mapping.sourceData.indirectIndexArray;
            check(mapping.source == VK_DESCRIPTOR_MAPPING_SOURCE_HEAP_WITH_INDIRECT_INDEX_ARRAY_EXT &&
                      s->pushOffset == 128 + 8 * set && s->samplerPushOffset == s->pushOffset &&
                      s->heapIndexStride == 1 && s->samplerHeapIndexStride == 1 && !s->useCombinedImageSamplerIndex &&
                      !s->pEmbeddedSampler && !s->heapOffset && !s->samplerHeapOffset,
                  "independent byte indices avoid packed-index limits and additional push data");
            for (uint32_t array = 0; array < 256; array++) {
                uint32_t resource_word = s->addressOffset / 4 + array;
                uint32_t sampler_word = s->samplerAddressOffset / 4 + array;
                check(resources[resource_word] * s->heapIndexStride == 0x100000 + (127 + array) * 32 &&
                          samplers[sampler_word] * s->samplerHeapIndexStride == 0x10000 + (383 + array) * 16,
                      "mapping preserves every descriptor-array index and independent image/sampler offset");
            }
        }
    VkDescriptorSetAndBindingMappingEXT mapping;
    check(!vk_descriptor_heap_binding_mapping(&mapping, 5, 0, 0, 0, types[0]), "invalid stage set rejected");
    check(!vk_descriptor_heap_binding_mapping(&mapping, 0, 0, UINT32_MAX, 0, types[0]), "resource word overflow rejected");
    check(!vk_descriptor_heap_binding_mapping(&mapping, 0, 0, 0, UINT32_MAX, types[0]), "sampler word overflow rejected");
    check(!vk_descriptor_heap_binding_mapping(&mapping, 0, 0, 0, 0, VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT), "unhandled resource class rejected");
}
/* CPU arrays stand in for immutable GPU partitions. This proves cache lifetime and normalized shadow bounds,
   not a native descriptor encoding. Only the production epoch and allocation helpers control reuse. */
static void current_frame_cache(void) {
    VkDescriptorHeapMemoryPlan plan;
    check(vk_descriptor_heap_memory_plan(&plan, 64, 2, 16, 8, 8, 1024, 1024), "cache model has two disjoint live ranges");
    VkDescriptorHeapCacheTag tag = {0};
    typedef struct { uint64_t resource_key, sampler_handle; uint32_t sampler_offset; } CacheEntry;
    CacheEntry cached = {0};
    unsigned char heap[128], shadow[64];
    memset(heap, 0xa5, sizeof heap);
    memset(shadow, 0x5a, sizeof shadow);
    VkDeviceSize cursors[2] = {0};
    int exhausted[2] = {0};
    for (uint64_t frame = 0; frame < 6; frame++) {
        uint32_t slot = (uint32_t)(frame % 2), other = 1 - slot;
        unsigned char pending[64];
        memcpy(pending, heap + other * 64, sizeof pending);
        if (frame >= 2) {
            /* This slot alone has completed. Its old GPU bytes may now be reused. */
            cursors[slot] = 0;
            exhausted[slot] = 0;
            vk_descriptor_heap_cache_reset(&tag);
        }
        check(vk_descriptor_heap_cache_frame(&tag, frame), "first write of frame0 and every later frame invalidates CPU metadata");
        cached = (CacheEntry){0};
        check(!cached.resource_key && !cached.sampler_handle, "old resource and handle offsets are unreachable after a frame change");
        check(!exhausted[slot], "exhaustion remains local to its retired frame slot");
        uint32_t offset = UINT32_MAX;
        check(vk_descriptor_heap_allocate(&plan, &cursors[slot], slot, 8, 8, &offset), "new frame encodes a fresh immutable sampler");
        check(offset == slot * 64 && offset + 8 <= (slot + 1) * 64, "cache miss selects the current GPU partition");
        size_t local = (size_t)offset - slot * 64;
        unsigned char pattern[8];
        memset(pattern, (int)frame + 17, sizeof pattern);
        memcpy(heap + offset, pattern, sizeof pattern);
        memcpy(shadow + local, pattern, sizeof pattern);
        cached.resource_key = cached.sampler_handle = 41;
        cached.sampler_offset = offset;
        check(!vk_descriptor_heap_cache_frame(&tag, frame) && cached.resource_key == 41 && cached.sampler_handle == 41 &&
                  !memcmp(shadow + ((size_t)cached.sampler_offset - slot * 64), pattern, sizeof pattern),
              "same-frame handle reuse compares the current frame's CPU shadow");
        check(!memcmp(heap + other * 64, pending, sizeof pending), "cache reuse and shadow writes preserve the other in-flight frame's bytes");
        VkDeviceSize before = cursors[slot];
        uint32_t unchanged = 0x12345678;
        check(!vk_descriptor_heap_allocate(&plan, &cursors[slot], slot, 64, 8, &unchanged) &&
                  unchanged == 0x12345678 && cursors[slot] == before,
              "a failed partial write preserves its earlier live offset and allocation cursor");
        exhausted[slot] = 1;
        check(!vk_descriptor_heap_cache_frame(&tag, frame) && cached.sampler_offset == offset &&
                  !memcmp(heap + offset, pattern, sizeof pattern), "fallback leaves completed encodings immutable and metadata valid");
    }
    vk_descriptor_heap_cache_reset(&tag);
    check(vk_descriptor_heap_cache_frame(&tag, 5), "same-frame fenced reset also forces a fresh CPU cache");
    check(!vk_descriptor_heap_cache_frame(&tag, 5), "reset does not cause repeated clears in the same frame");
    vk_descriptor_heap_cache_reset(&tag);
    check(vk_descriptor_heap_cache_frame(&tag, UINT64_MAX), "invalid tag cannot alias the maximum frame ID");
    check(!vk_descriptor_heap_cache_frame(&tag, UINT64_MAX), "maximum frame ID can reuse its own metadata");
    check(vk_descriptor_heap_cache_frame(&tag, 0), "frame counter wrap cannot reuse the previous frame's offsets");
    vk_descriptor_heap_cache_reset(&tag);
    check(vk_descriptor_heap_cache_frame(&tag, 0), "explicit invalidity also handles the first frame ID after reset");
}
/* The device's reason for no heaps names the first piece missing, in the order device.c needs them */
static void missing_pieces(void) {
    check(!strcmp(vk_descriptor_heap_missing(0, 1, 1, 1), "no VK_EXT_descriptor_heap"), "extension missing");
    check(!strcmp(vk_descriptor_heap_missing(1, 0, 1, 1), "its descriptorHeap feature is off"), "feature off");
    check(!strcmp(vk_descriptor_heap_missing(1, 1, 0, 1), "its bufferDeviceAddress feature is off"),
          "buffer device address off");
    check(!strcmp(vk_descriptor_heap_missing(1, 1, 1, 0), "neither extendedFlags nor maintenance5"),
          "extended flags and maintenance5 missing");
    check(!strcmp(vk_descriptor_heap_missing(0, 0, 0, 0), "no VK_EXT_descriptor_heap"), "the first piece first");
    check(!vk_descriptor_heap_missing(1, 1, 1, 1), "nothing missing");
}
int main(void) {
    missing_pieces();
    image_runs(); descriptor_alignment(); plans(); constrained_heaps(); table_runs(); mappings(); current_frame_cache();
    printf("descriptor heap geometry: %u checks, %u failures\n", checks, failures);
    return failures != 0;
}
