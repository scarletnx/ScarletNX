/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Forced hashes exercise pending four-way cache collisions independently of the runtime hash. Mock native
   encodings are ordinary CPU byte patterns: the production planner/publisher must retain every exact index,
   copy only successful encodings, and leave earlier chunks intact on exhaustion or partial native failure. */
#include "nx/gpu/vk/descriptor_heaps.h"
#include <stdio.h>
#include <string.h>

static unsigned checks, failures;
static void check(int pass, const char* message) {
    checks++;
    if (!pass && failures++ < 20) fprintf(stderr, "FAIL buffer batch: %s\n", message);
}
static VkDescriptorHeapMemoryPlan plan(void) {
    return (VkDescriptorHeapMemoryPlan){.frame_size = 1024, .reserved_offset = 2048, .reserved_size = 64,
        .range_size = 2112, .allocation_size = 2175, .alignment = 64};
}
static VkDescriptorHeapResourceEntry entry(uint64_t id, uint32_t offset) {
    return (VkDescriptorHeapResourceEntry){.object = 0x100 + id, .offset = id * 64, .range = 64,
        .tag = 0, .heap_offset = offset};
}
static int add(VkDescriptorHeapBufferBatch* batch, VkDescriptorHeapResourceEntry* cache,
                 const VkDescriptorHeapMemoryPlan* memory, uint32_t frame, uint64_t id,
                 uint64_t hash, uint32_t bytes, VkDeviceSize alignment) {
    VkDescriptorHeapResourceEntry in = entry(id, 0);
    return vk_descriptor_heap_buffer_plan(batch, cache, 8, memory, frame, &in, hash, bytes, alignment);
}
static uint8_t byte(uint64_t id, unsigned index) { return (uint8_t)(id * 37 + index * 11); }
static void encode(const VkDescriptorHeapBufferBatch* batch, uint8_t* scratch, size_t stride) {
    memset(scratch, 0xcc, VK_DESCRIPTOR_HEAP_BUFFER_BATCH * stride);
    for (uint32_t i = 0; i < batch->misses; i++)
        for (uint32_t j = 0; j < batch->miss[i].bytes; j++) scratch[i * stride + j] = byte(batch->miss[i].entry.object, j);
}
static void collisions(void) {
    const uint32_t ids[8] = {1, 2, 3, 4, 5, 6, 1, 6};
    for (uint32_t frame = 0; frame < 2; frame++) {
        VkDescriptorHeapMemoryPlan memory = plan();
        VkDescriptorHeapResourceEntry cache[8] = {0}, original[8] = {0};
        VkDeviceSize used = 7;
        VkDescriptorHeapBufferBatch batch = {.used = used};
        uint8_t heap[2176], expected[2176], scratch[8 * 64];
        memset(heap, 0xa5, sizeof heap); memset(expected, 0xa5, sizeof expected);
        uint32_t offsets[8];
        VkDeviceSize cursor = used;
        for (uint32_t i = 0; i < 8; i++) {
            uint32_t bytes = ids[i] & 1 ? 16 : 32;
            VkDeviceSize alignment = ids[i] & 1 ? 16 : 64;
            if (i < 6) {
                cursor = (cursor + alignment - 1) / alignment * alignment;
                offsets[i] = (uint32_t)(frame * 1024 + cursor);
                cursor += bytes;
            } else offsets[i] = offsets[i == 6 ? 0 : 5];
            check(add(&batch, cache, &memory, frame, ids[i], ids[i] == 6 ? 1ull << 32 : 0, bytes, alignment),
                  "forced pending collision can be planned");
            check(batch.offsets[i] == offsets[i], "each logical item retains its own exact aligned offset");
            check(!memcmp(cache, original, sizeof cache) && used == 7, "planning leaves global cache/cursor unchanged");
            if (i < 6)
                for (uint32_t j = 0; j < bytes; j++) expected[offsets[i] + j] = byte(0x100 + ids[i], j);
        }
        check(batch.misses == 6 && batch.hits == 2 && batch.changes == 4 && batch.evictions == 2,
              "six unique misses and two duplicates survive colliding pending replacements");
        check(batch.used == cursor, "local cursor includes only unique encodings and exact alignment gaps");
        encode(&batch, scratch, 64);
        check(!vk_descriptor_heap_buffer_publish(&batch, cache, &used, heap, scratch, 64, 0),
              "native failure refuses publication even with partially encoded CPU scratch");
        int untouched = 1;
        for (uint32_t i = 0; i < sizeof heap; i++) untouched &= heap[i] == 0xa5;
        check(!memcmp(cache, original, sizeof cache) && used == 7 && untouched,
              "native failure preserves cache, cursor and heap");
        check(vk_descriptor_heap_buffer_publish(&batch, cache, &used, heap, scratch, 64, 1), "successful batch publishes");
        check(used == cursor && !memcmp(heap, expected, sizeof heap), "all descriptor bytes and untouched padding match independently");
        check(cache[0].object == 0x105 && cache[0].heap_offset == offsets[4] &&
                  cache[1].object == 0x106 && cache[1].heap_offset == offsets[5] &&
                  cache[2].object == 0x103 && cache[3].object == 0x104,
              "final cache contains the last virtual replacement of each slot");
    }
}
static void old_hits(void) {
    VkDescriptorHeapMemoryPlan memory = plan();
    VkDescriptorHeapResourceEntry cache[8] = {0};
    for (uint32_t i = 0; i < 4; i++) cache[i] = entry(i + 1, 16 + i * 16);
    VkDescriptorHeapBufferBatch batch = {.used = 128};
    const uint32_t ids[] = {1, 5, 1, 6, 1, 5}, expected[] = {16, 128, 144, 160, 144, 128};
    for (uint32_t i = 0; i < 6; i++) {
        check(add(&batch, cache, &memory, 0, ids[i], 0, 16, 16), "old hit and collision sequence plans");
        check(batch.offsets[i] == expected[i], "old hit remains live after its metadata slot is replaced");
    }
    check(batch.misses == 3 && batch.hits == 3 && batch.evictions == 3,
          "pending-key reuse remains exact even after that key is evicted again");
    check(cache[0].heap_offset == 16 && cache[0].object == 0x101, "unpublished overlay cannot replace an old hit");
}
static void failed_later_chunk(void) {
    VkDescriptorHeapMemoryPlan memory = plan();
    VkDescriptorHeapResourceEntry cache[8] = {0}, saved[8];
    VkDeviceSize used = 0;
    uint8_t heap[2176], previous[2176], scratch[8 * 64];
    memset(heap, 0x5a, sizeof heap);
    VkDescriptorHeapBufferBatch first = {.used = used};
    check(add(&first, cache, &memory, 0, 1, 0, 16, 16), "first chunk plans");
    encode(&first, scratch, 64);
    check(vk_descriptor_heap_buffer_publish(&first, cache, &used, heap, scratch, 64, 1), "first chunk commits");
    memcpy(saved, cache, sizeof saved); memcpy(previous, heap, sizeof heap);
    VkDescriptorHeapBufferBatch next = {.used = used};
    check(add(&next, cache, &memory, 0, 2, 0, 32, 64), "later chunk plans without altering first");
    check(add(&next, cache, &memory, 0, 3, 0, 16, 16), "later chunk contains a second pending miss");
    memset(scratch, 0xcc, sizeof scratch);
    for (uint32_t i = 0; i < next.miss[0].bytes; i++) scratch[i] = byte(0x102, i);
    /* The mock native call wrote the first destination but left the second one unencoded before failure. */
    check(!vk_descriptor_heap_buffer_publish(&next, cache, &used, heap, scratch, 64, 0), "later native failure aborts only its chunk");
    check(used == 16 && !memcmp(saved, cache, sizeof cache) && !memcmp(previous, heap, sizeof heap),
          "failure preserves every earlier successful chunk and all its cache indices");
    VkDescriptorHeapMemoryPlan small = memory;
    small.frame_size = 64; small.reserved_offset = 128;
    VkDescriptorHeapBufferBatch full = {.used = 48};
    check(add(&full, cache, &small, 0, 2, 0, 16, 16), "last free descriptor can be planned");
    check(!add(&full, cache, &small, 0, 3, 0, 16, 16) && full.exhausted, "next descriptor detects exhaustion before encoding");
    check(!vk_descriptor_heap_buffer_publish(&full, cache, &used, heap, scratch, 64, 1), "exhausted plan cannot publish its prefix");
    check(used == 16 && !memcmp(saved, cache, sizeof cache) && !memcmp(previous, heap, sizeof heap),
          "allocation failure changes neither earlier cursor/cache nor bytes");
}
static void exact_keys_and_limit(void) {
    VkDescriptorHeapMemoryPlan memory = plan();
    VkDescriptorHeapResourceEntry cache[8] = {0};
    VkDescriptorHeapBufferBatch batch = {0};
    const VkDescriptorHeapResourceEntry key = {.object = 100, .offset = 64, .range = 32, .tag = 0};
    for (uint32_t i = 0; i < 8; i++) {
        VkDescriptorHeapResourceEntry changed = key;
        switch (i / 2) {
            case 0: changed.object += 1 + i % 2; break;
            case 1: changed.offset += 1 + i % 2; break;
            case 2: changed.range += 1 + i % 2; break;
            case 3: changed.tag += 1 + i % 2; break;
        }
        check(vk_descriptor_heap_buffer_plan(&batch, cache, 8, &memory, 0, &changed, 0, 16, 16),
              "a change in any consumed key word is a distinct miss");
    }
    check(batch.misses == 8 && !batch.hits, "type, buffer, offset and range all participate in exact equality");
    VkDescriptorHeapBufferBatch saved;
    memcpy(&saved, &batch, sizeof saved);
    check(!vk_descriptor_heap_buffer_plan(&batch, cache, 8, &memory, 0, &key, 0, 16, 16), "ninth logical element cannot overflow fixed planning arrays");
    check(!memcmp(&batch, &saved, sizeof batch), "capacity rejection leaves all eight valid entries intact");
}
static void compact_keys(void) {
    check(sizeof(VkDescriptorHeapResourceEntry) == 32 && sizeof(VkDescriptorHeapSamplerEntry) == 16,
          "resource and sampler entries retain all fields in exactly32 and16 bytes");
    VkDescriptorHeapResourceEntry a = {.object = UINT64_MAX, .offset = 0x1234567887654321ull,
        .range = 0xabcdef0123456789ull, .tag = 1, .heap_offset = 0};
    VkDescriptorHeapResourceEntry b = a;
    b.heap_offset = UINT32_MAX;
    check(vk_descriptor_heap_resource_equal(&a, &b), "heap allocation offset is outside the28-byte key");
    for (uint32_t byte = 0; byte < 28; byte++) {
        b = a;
        ((uint8_t*)&b)[byte] ^= 0x80;
        check(!vk_descriptor_heap_resource_equal(&a, &b), "every byte of object, buffer offset/range and tag participates");
    }
    uint32_t tags[5];
    check(vk_descriptor_heap_resource_tag(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_FORMAT_UNDEFINED, &tags[0]) && tags[0] == 0 &&
              vk_descriptor_heap_resource_tag(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_FORMAT_UNDEFINED, &tags[1]) && tags[1] == 1,
          "UBO and SSBO remain distinct even for the same exact address range");
    check(vk_descriptor_heap_resource_tag(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_FORMAT_UNDEFINED, &tags[2]) &&
              vk_descriptor_heap_resource_tag(VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_FORMAT_UNDEFINED, &tags[3]) &&
              vk_descriptor_heap_resource_tag(VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_FORMAT_UNDEFINED, &tags[4]) &&
              tags[2] == UINT32_MAX && tags[3] == UINT32_MAX - 1 && tags[4] == UINT32_MAX - 2,
          "combined, sampled and storage images retain three disjoint exact tags");
    const VkFormat formats[] = {VK_FORMAT_R8_UINT, VK_FORMAT_R32_UINT, VK_FORMAT_R32_SFLOAT,
        (VkFormat)INT32_MAX, (VkFormat)1000066001};
    for (uint32_t i = 0; i < sizeof formats / sizeof *formats; i++) {
        uint32_t tag;
        check(vk_descriptor_heap_resource_tag(VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, formats[i], &tag) &&
                  tag == (uint32_t)formats[i] + 2 && tag > 1 && tag < UINT32_MAX - 2,
              "every accepted format maps below image tags and above unformatted buffers");
    }
    const VkFormat invalid[] = {VK_FORMAT_UNDEFINED, (VkFormat)-1, (VkFormat)INT32_MIN, (VkFormat)0x80000001u};
    for (uint32_t i = 0; i < sizeof invalid / sizeof *invalid; i++) {
        uint32_t tag = 123;
        check(!vk_descriptor_heap_resource_tag(VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, invalid[i], &tag) && tag == 123,
              "undefined or negative texel formats cannot wrap into another descriptor tag");
    }
    uint32_t ignored;
    check(!vk_descriptor_heap_resource_tag(VK_DESCRIPTOR_TYPE_SAMPLER, VK_FORMAT_UNDEFINED, &ignored) &&
              !vk_descriptor_heap_resource_tag(VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, VK_FORMAT_R32_UINT, &ignored),
          "unsupported descriptor classes cannot share a supported tag");
    VkDescriptorHeapBufferBatch batch = {0};
    VkDescriptorHeapResourceEntry cache[8] = {0}, empty = {0};
    VkDescriptorHeapMemoryPlan memory = plan();
    check(!vk_descriptor_heap_buffer_plan(&batch, cache, 8, &memory, 0, &empty, 0, 16, 16) && !batch.count,
          "object0 is exclusively an empty slot, independent of the valid UBO tag0");
}
static void sampler_collisions(void) {
    uint8_t shadow[128] = {0}, encoding[32];
    for (uint32_t i = 0; i < sizeof encoding; i++) shadow[32 + i] = encoding[i] = (uint8_t)(i * 17);
    VkDescriptorHeapSamplerEntry sample = {.hash = 0x123456789abcdef0ull, .offset = 1024 + 32, .used = 1};
    check(vk_descriptor_heap_sampler_equal(&sample, sample.hash, shadow, 1024, encoding, sizeof encoding),
          "equal hash and complete CPU shadow bytes reuse one sampler encoding");
    for (uint32_t i = 0; i < sizeof encoding; i++) {
        encoding[i] ^= 1;
        check(!vk_descriptor_heap_sampler_equal(&sample, sample.hash, shadow, 1024, encoding, sizeof encoding),
              "forced64-bit hash collisions compare every opaque byte before reuse");
        encoding[i] ^= 1;
    }
    check(!vk_descriptor_heap_sampler_equal(&sample, sample.hash ^ 1, NULL, 1024, encoding, sizeof encoding),
          "unequal hashes reject before computing a shadow address");
    sample.used = 0; sample.offset = 0;
    check(!vk_descriptor_heap_sampler_equal(&sample, sample.hash, NULL, 1024, encoding, sizeof encoding),
          "unused entries never form an address outside the current frame's CPU shadow");
}
int main(void) {
    collisions(); old_hits(); failed_later_chunk(); exact_keys_and_limit(); compact_keys(); sampler_collisions();
    if (!failures) printf("PASS descriptor heap buffer batches: %u collision/dedup/alignment/failure checks\n", checks);
    return failures != 0;
}
