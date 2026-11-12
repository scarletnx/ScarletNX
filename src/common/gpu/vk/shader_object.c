/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* VK_EXT_shader_object's shared parts (shader_object.h) */
#include "common/gpu/vk/shader_object.h"
#include "common/platform/file.h"
#include "common/utilities/bits.h"
#include "common/utilities/endian.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct SnxVkShaderBinary {
    uint64_t key;
    size_t at, size; /* in the blob */
};

/* the file: this magic, the device's shaderBinaryUUID, its shaderBinaryVersion and the count, then each binary's key
   and size, and its bytes up to a multiple of 8. Little-endian, as the bytes are this machine's device's anyway */
static const char magic[8] = {'S', 'N', 'X', 'V', 'K', 'S', 'B', '1'};
enum { HEAD = 8 + VK_UUID_SIZE + 4 + 4, ENTRY = 8 + 4 + 4 };

uint64_t snx_vk_shader_key(VkShaderStageFlagBits stage, const uint32_t* words, size_t count) {
    uint32_t s = (uint32_t)stage;
    uint64_t h = snx_fnv1a(SNX_FNV_BASIS, &s, sizeof s);
    return snx_fnv1a(h, words, count * sizeof *words);
}

void snx_vk_shader_binaries_init(SnxVkShaderBinaries* b, const uint8_t uuid[VK_UUID_SIZE], uint32_t version) {
    *b = (SnxVkShaderBinaries){0};
    memcpy(b->uuid, uuid, VK_UUID_SIZE);
    b->version = version;
}
void snx_vk_shader_binaries_free(SnxVkShaderBinaries* b) {
    free(b->entries);
    free(b->slots);
    free(b->blob);
    *b = (SnxVkShaderBinaries){0};
}

static size_t slot_of(const SnxVkShaderBinaries* b, uint64_t key) {
    size_t mask = b->slot_count - 1, i = (size_t)(key ^ key >> 32) & mask;
    while (b->slots[i] && b->entries[b->slots[i] - 1].key != key) i = (i + 1) & mask;
    return i;
}
const void* snx_vk_shader_binary(const SnxVkShaderBinaries* b, uint64_t key, size_t* size) {
    if (!b || !b->slot_count) return NULL;
    uint32_t e = b->slots[slot_of(b, key)];
    if (!e) return NULL;
    *size = b->entries[e - 1].size;
    return b->blob + b->entries[e - 1].at;
}
/* room for one more entry and its slot, the table kept at most half full */
static bool room(SnxVkShaderBinaries* b, size_t bytes) {
    if (b->count == b->capacity) {
        size_t n = b->capacity ? b->capacity * 2 : 64;
        struct SnxVkShaderBinary* e = realloc(b->entries, n * sizeof *e);
        if (!e) return false;
        b->entries = e;
        b->capacity = n;
    }
    if ((b->count + 1) * 2 > b->slot_count) {
        size_t n = b->slot_count ? b->slot_count * 2 : 128;
        uint32_t* slots = calloc(n, sizeof *slots);
        if (!slots) return false;
        free(b->slots);
        b->slots = slots;
        b->slot_count = n;
        for (size_t i = 0; i < b->count; ++i) b->slots[slot_of(b, b->entries[i].key)] = (uint32_t)(i + 1);
    }
    if (b->blob_size + bytes > b->blob_capacity) {
        size_t n = b->blob_capacity ? b->blob_capacity : 1u << 20;
        while (n < b->blob_size + bytes) n *= 2;
        unsigned char* blob = realloc(b->blob, n);
        if (!blob) return false;
        b->blob = blob;
        b->blob_capacity = n;
    }
    return true;
}
static bool add(SnxVkShaderBinaries* b, uint64_t key, const void* data, size_t size) {
    /* a binary's code is 16-byte aligned (VkShaderCreateInfoEXT), as is malloc's memory on the hosts this runs on */
    size_t at = (size_t)snx_align_up(b->blob_size, 16);
    if (!size || size > UINT32_MAX || snx_vk_shader_binary(b, key, &(size_t){0}) || !room(b, at - b->blob_size + size))
        return false;
    memcpy(b->blob + at, data, size);
    b->entries[b->count] = (struct SnxVkShaderBinary){key, at, size};
    b->slots[slot_of(b, key)] = (uint32_t)(b->count + 1);
    b->count++;
    b->blob_size = at + size;
    return true;
}

void snx_vk_shader_binaries_load(SnxVkShaderBinaries* b, const char* path, size_t* loaded) {
    *loaded = 0;
    PFileMap file = {0};
    if (!path || plat_file_map(&file, path) || !file.data) return;
    const unsigned char* p = file.data;
    uint64_t size = file.size;
    if (size >= HEAD && !memcmp(p, magic, sizeof magic) && !memcmp(p + 8, b->uuid, VK_UUID_SIZE) &&
        snx_ld_le32(p + 8 + VK_UUID_SIZE) == b->version) {
        uint32_t count = snx_ld_le32(p + 8 + VK_UUID_SIZE + 4);
        /* every entry whole, or none taken: a cut file is a first run's */
        bool whole = true;
        uint64_t at = HEAD;
        for (uint32_t i = 0; i < count && whole; ++i) {
            whole = at + ENTRY <= size && at + ENTRY + snx_ld_le32(p + at + 8) <= size;
            if (whole) at = snx_align_up(at + ENTRY + snx_ld_le32(p + at + 8), 8);
        }
        at = HEAD;
        for (uint32_t i = 0; i < count && whole; ++i) {
            const unsigned char* e = p + at;
            uint32_t bytes = snx_ld_le32(e + 8);
            if (add(b, snx_ld_le64(e), e + ENTRY, bytes)) *loaded += bytes;
            at = snx_align_up(at + ENTRY + bytes, 8);
        }
    }
    plat_file_unmap(&file);
    b->dirty = false;
}

bool snx_vk_shader_binaries_keep(SnxVkShaderBinaries* b, VkDevice dev, uint64_t key, VkShaderEXT shader) {
    size_t size = 0;
    if (snx_vk_shader_binary(b, key, &size)) return true;
    if (vkGetShaderBinaryDataEXT(dev, shader, &size, NULL) != VK_SUCCESS || !size) return false;
    void* data = malloc(size);
    bool kept = data && vkGetShaderBinaryDataEXT(dev, shader, &size, data) == VK_SUCCESS && add(b, key, data, size);
    free(data);
    b->dirty |= kept;
    return kept;
}

int snx_vk_shader_binaries_save(SnxVkShaderBinaries* b, const char* path, size_t* saved, char* why, size_t n) {
    *saved = 0;
    size_t total = HEAD;
    for (size_t i = 0; i < b->count; ++i) total = snx_align_up(total + ENTRY + b->entries[i].size, 8);
    unsigned char* out = calloc(1, total);
    if (!out) {
        snprintf(why, n, "%zu bytes for the binaries cannot be allocated", total);
        return -1;
    }
    memcpy(out, magic, sizeof magic);
    memcpy(out + 8, b->uuid, VK_UUID_SIZE);
    snx_st_le32(out + 8 + VK_UUID_SIZE, b->version);
    snx_st_le32(out + 8 + VK_UUID_SIZE + 4, (uint32_t)b->count);
    size_t at = HEAD;
    for (size_t i = 0; i < b->count; ++i) {
        const struct SnxVkShaderBinary* e = &b->entries[i];
        snx_st_le64(out + at, e->key);
        snx_st_le32(out + at + 8, (uint32_t)e->size);
        memcpy(out + at + ENTRY, b->blob + e->at, e->size);
        at = snx_align_up(at + ENTRY + e->size, 8);
    }
    int failed = snx_vk_write_replace(path, out, total, why, n);
    free(out);
    if (!failed) {
        *saved = total;
        b->dirty = false;
    }
    return failed;
}

VkResult snx_vk_shader_object(VkDevice dev, SnxVkShaderBinaries* b, uint64_t key, const VkShaderCreateInfoEXT* info,
                              VkShaderEXT* out, bool* compiled) {
    *out = VK_NULL_HANDLE;
    *compiled = false;
    size_t size = 0;
    const void* binary = snx_vk_shader_binary(b, key, &size);
    if (binary) {
        VkShaderCreateInfoEXT from = *info;
        from.codeType = VK_SHADER_CODE_TYPE_BINARY_EXT;
        from.codeSize = size;
        from.pCode = binary;
        /* VK_INCOMPATIBLE_SHADER_BINARY_EXT, or any other refusal: the SPIR-V below */
        if (vkCreateShadersEXT(dev, 1, &from, NULL, out) == VK_SUCCESS && *out) return VK_SUCCESS;
        *out = VK_NULL_HANDLE;
    }
    VkResult r = vkCreateShadersEXT(dev, 1, info, NULL, out);
    if (r != VK_SUCCESS || !*out) {
        *out = VK_NULL_HANDLE;
        return r != VK_SUCCESS ? r : VK_ERROR_INITIALIZATION_FAILED;
    }
    *compiled = true;
    if (b) (void)snx_vk_shader_binaries_keep(b, dev, key, *out);
    return VK_SUCCESS;
}

void snx_vk_shader_object_defaults(VkCommandBuffer cb) {
    vkCmdSetRasterizerDiscardEnable(cb, VK_FALSE);
    vkCmdSetPolygonModeEXT(cb, VK_POLYGON_MODE_FILL);
    vkCmdSetRasterizationSamplesEXT(cb, VK_SAMPLE_COUNT_1_BIT);
    VkSampleMask mask = ~(VkSampleMask)0;
    vkCmdSetSampleMaskEXT(cb, VK_SAMPLE_COUNT_1_BIT, &mask);
    vkCmdSetAlphaToCoverageEnableEXT(cb, VK_FALSE);
    vkCmdSetDepthClampEnableEXT(cb, VK_FALSE);
    vkCmdSetDepthBiasEnable(cb, VK_FALSE);
    vkCmdSetDepthBoundsTestEnable(cb, VK_FALSE);
    vkCmdSetPrimitiveRestartEnable(cb, VK_FALSE);
    vkCmdSetLineWidth(cb, 1.0f);
}
