/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Exact descriptor-buffer encoding keys and byte-range helpers. No Vulkan calls or object-lifetime ownership. */
#ifndef SNX_VK_DESCRIPTOR_ENCODING_H
#define SNX_VK_DESCRIPTOR_ENCODING_H
#include "common/utilities/bits.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef struct {
    uint64_t frame, device, buffer, address, offset, range;
    uint32_t format, type, bytes;
} VkBufferEncodingKey;

static inline int vk_buffer_encoding_equal(const VkBufferEncodingKey* a, const VkBufferEncodingKey* b) {
    return a->frame == b->frame && a->device == b->device && a->buffer == b->buffer && a->address == b->address &&
           a->offset == b->offset && a->range == b->range && a->format == b->format && a->type == b->type &&
           a->bytes == b->bytes;
}
/* storage has two key+payload slots per power-of-two bucket. stride is aligned for VkBufferEncodingKey. A miss
   installs the exact key; the caller must encode its bytes before another lookup. frame is nonzero and monotonic. */
static inline void* vk_buffer_encoding_slot(void* storage, size_t stride, uint32_t buckets,
                                             const VkBufferEncodingKey* key, int* hit, int* evicted) {
    uint64_t hash = snx_fnv_step(SNX_FNV_BASIS, key->buffer);
    hash = snx_fnv_step(hash, key->address);
    hash = snx_fnv_step(hash, key->offset);
    hash = snx_fnv_step(hash, key->range);
    hash = snx_fnv_step(hash, key->format | (uint64_t)key->type << 32);
    hash = snx_fnv_step(hash, key->bytes);
    size_t first = (size_t)((hash ^ (hash >> 32)) & (buckets - 1)) * 2;
    VkBufferEncodingKey* a = (VkBufferEncodingKey*)((uint8_t*)storage + first * stride);
    VkBufferEncodingKey* b = (VkBufferEncodingKey*)((uint8_t*)a + stride);
    VkBufferEncodingKey* entry;
    *hit = 1;
    *evicted = 0;
    if (vk_buffer_encoding_equal(a, key)) return a + 1;
    if (vk_buffer_encoding_equal(b, key)) return b + 1;
    *hit = 0;
    if (a->frame != key->frame || a->device != key->device) entry = a;
    else if (b->frame != key->frame || b->device != key->device) entry = b;
    else {
        entry = hash >> 63 ? a : b;
        *evicted = 1;
    }
    *entry = *key;
    return entry + 1;
}
/* The actual payload ends here. The table's full queried size must still fit the underlying arena at its base,
   even when its unused tail overlaps another table: Vulkan requires every layout binding's address in bounds. */
static inline int vk_descriptor_active_end(uint64_t layout_size, uint64_t binding_offset, uint32_t slots,
                                            uint32_t first, uint32_t count, size_t stride, uint64_t* end) {
    if (!stride || first > slots || count > slots - first || binding_offset > layout_size ||
        (uint64_t)slots > (layout_size - binding_offset) / stride)
        return 0;
    *end = binding_offset + ((uint64_t)first + count) * stride;
    return 1;
}
/* The first opaque pattern is already present. Double from initialized, disjoint ranges; no descriptor bytes
   are interpreted, and the last copy is bounded by the remaining number of elements. */
static inline void vk_descriptor_repeat(void* destination, size_t stride, uint32_t count) {
    for (uint32_t filled = 1; filled < count;) {
        uint32_t take = filled < count - filled ? filled : count - filled;
        memcpy((uint8_t*)destination + (size_t)filled * stride, destination, (size_t)take * stride);
        filled += take;
    }
}
#endif
