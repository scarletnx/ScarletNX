/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A mock encoder proves cache-key and byte-copy behavior, not Vulkan descriptor validity or GPU rendering. */
#include "nx/gpu/vk/descriptor_encoding.h"
#include <stdio.h>
#include <stdlib.h>

static unsigned checks, failures;
static void check(int pass, const char* what) {
    checks++;
    if (!pass && failures++ < 12) fprintf(stderr, "FAIL: %s\n", what);
}
/* A deterministic opaque byte oracle depends on every semantic key field. Epoch and device do not change this
   mock format, but must still invalidate reuse; a real driver may encode different device-specific bytes. */
static void encode(const VkBufferEncodingKey* key, unsigned char* output) {
    uint64_t fields[] = {key->buffer, key->address, key->offset, key->range, key->format, key->type, key->bytes};
    for (uint32_t byte = 0; byte < key->bytes; byte++) {
        uint64_t value = byte + 17;
        for (size_t field = 0; field < sizeof fields / sizeof *fields; field++)
            value = (value ^ fields[field]) * 0x9e3779b185ebca87ull + field;
        output[byte] = (unsigned char)(value >> ((byte % 8) * 8));
    }
}
static void test_cache(void) {
    const size_t stride = (size_t)snx_align_up(sizeof(VkBufferEncodingKey) + 128, 8);
    unsigned char* storage = calloc(2, stride); /* one bucket forces exact-key collisions */
    check(storage != NULL, "mock cache allocation");
    if (!storage) return;
    VkBufferEncodingKey key = {.frame = 1, .device = 2, .buffer = 3, .address = 0x100000, .offset = 256,
                                .range = 16, .format = 98, .type = 6, .bytes = 16};
    unsigned char expected[128];
    int hit, evicted;
    void* payload = vk_buffer_encoding_slot(storage, stride, 1, &key, &hit, &evicted);
    check(!hit && !evicted, "first exact key misses");
    encode(&key, payload);
    payload = vk_buffer_encoding_slot(storage, stride, 1, &key, &hit, &evicted);
    encode(&key, expected);
    check(hit && !evicted && !memcmp(payload, expected, key.bytes), "same live key reuses identical opaque bytes");
    for (uint32_t field = 0; field < 9; field++) {
        VkBufferEncodingKey changed = key;
        switch (field) {
            case 0: changed.frame++; break;
            case 1: changed.device++; break;
            case 2: changed.buffer++; break;
            case 3: changed.address++; break;
            case 4: changed.offset++; break;
            case 5: changed.range++; break;
            case 6: changed.format++; break;
            case 7: changed.type++; break;
            default: changed.bytes = 32; break;
        }
        payload = vk_buffer_encoding_slot(storage, stride, 1, &changed, &hit, &evicted);
        check(!hit, "every key field participates in reuse and invalidation");
        encode(&changed, payload);
        encode(&changed, expected);
        payload = vk_buffer_encoding_slot(storage, stride, 1, &changed, &hit, &evicted);
        check(hit && !memcmp(payload, expected, changed.bytes), "changed key's payload is independently encoded");
    }
    unsigned hits = 0, misses = 0, evictions = 0;
    for (uint32_t iteration = 0; iteration < 2048; iteration++) {
        VkBufferEncodingKey varied = key;
        varied.frame += iteration / 128;
        varied.offset += (iteration % 11) * 64;
        varied.range += iteration % 7;
        varied.bytes = iteration & 1 ? 16 : 64;
        payload = vk_buffer_encoding_slot(storage, stride, 1, &varied, &hit, &evicted);
        hits += hit;
        misses += !hit;
        evictions += evicted;
        if (!hit) encode(&varied, payload);
        encode(&varied, expected);
        check(!memcmp(payload, expected, varied.bytes), "collision replacement preserves exact bytes");
    }
    check(hits + misses == 2048 && evictions && misses, "collision fixture exercises replacement");
    free(storage);
}
static void test_replication(void) {
    unsigned char bytes[128 * 257 + 32], pattern[128];
    for (size_t stride = 1; stride <= 128; stride++) {
        for (size_t byte = 0; byte < stride; byte++) pattern[byte] = (unsigned char)(byte * 29 + stride);
        for (uint32_t count = 1; count <= 256; count++) {
            memset(bytes, 0xa5, sizeof bytes);
            memcpy(bytes + 16, pattern, stride);
            vk_descriptor_repeat(bytes + 16, stride, count);
            int exact = 1;
            for (uint32_t element = 0; element < count; element++)
                if (memcmp(bytes + 16 + (size_t)element * stride, pattern, stride)) exact = 0;
            for (size_t byte = 0; byte < 16; byte++) if (bytes[byte] != 0xa5) exact = 0;
            for (size_t byte = 16 + (size_t)count * stride; byte < sizeof bytes; byte++)
                if (bytes[byte] != 0xa5) exact = 0;
            check(exact, "opaque runs reproduce every byte and preserve both guards");
        }
    }
}
static void test_ranges(void) {
    uint64_t end = UINT64_MAX;
    check(vk_descriptor_active_end(1024, 128, 8, 2, 3, 16, &end) && end == 208,
          "partial array writes include the first-element offset");
    check(!vk_descriptor_active_end(1024, 128, 8, 6, 3, 16, &end), "array write cannot cross binding end");
    check(!vk_descriptor_active_end(1024, 1000, 8, 0, 1, 16, &end), "full binding must remain inside layout");
    check(!vk_descriptor_active_end(1024, UINT64_MAX, 1, 0, 1, 16, &end), "offset overflow is rejected");
    check(!vk_descriptor_active_end(1024, 0, UINT32_MAX, 0, UINT32_MAX, SIZE_MAX, &end),
          "descriptor count multiplication cannot overflow");
    check(!vk_descriptor_active_end(1024, 0, 1, 0, 1, 0, &end), "zero descriptor size is rejected");
    /* Bindings are deliberately out of numerical order. Later active prefixes may overwrite only the old tables'
       unused tails, while every full layout remains in bounds of the underlying arena. */
    unsigned char arena[4096];
    memset(arena, 0x5a, sizeof arena);
    const uint64_t offsets[] = {192, 16, 80};
    const uint32_t active_binding[] = {1, 2, 1, 0, 2, 1};
    uint64_t bases[6], used = 0;
    for (uint32_t table = 0; table < 6; table++) {
        uint32_t binding = active_binding[table];
        check(used + 256 <= sizeof arena, "full fixed layout fits at compact table base");
        check(vk_descriptor_active_end(256, offsets[binding], 1, 0, 1, 16, &end), "queried binding offset is valid");
        bases[table] = used;
        memset(arena + used + offsets[binding], (int)table + 1, 16);
        used += snx_align_up(end, 16);
    }
    for (uint32_t table = 0; table < 6; table++)
        for (uint32_t byte = 0; byte < 16; byte++)
            check(arena[bases[table] + offsets[active_binding[table]] + byte] == table + 1,
                  "later compact allocations preserve every earlier active descriptor");
    check(used < 6 * 256, "inactive layout tails consume no cursor space");
}
int main(void) {
    test_cache();
    test_replication();
    test_ranges();
    printf("descriptor encoding mock bytes: %u checks, %u failures\n", checks, failures);
    return failures != 0;
}
