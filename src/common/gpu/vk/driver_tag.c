/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   The names of each driver's cache files and the check of a pipeline cache's file (vk_common.h), apart from the rest
   of vk_common.c so that a test of the files links them without the Vulkan loader */
#include "common/gpu/vk/vk_common.h"
#include "common/utilities/ascii.h"
#include "common/utilities/endian.h"
#include <stdio.h>
#include <string.h>

void snx_vk_driver_tag(char* out, size_t n, uint32_t driver_id, uint32_t vendor, uint32_t device, const char* package) {
    int w = snprintf(out, n, "d%u-%04x-%08x", (unsigned)driver_id, (unsigned)vendor, (unsigned)device);
    if (w < 0 || (size_t)w >= n || !package || !*package) return;
    size_t at = (size_t)w;
    out[at++] = '-';
    /* a package's folder name as a file name keeps it: any other letter is an underscore */
    for (; *package && at + 1 < n; package++, at++) {
        char c = *package;
        out[at] = snx_is_alnum(c) || c == '.' || c == '_' || c == '-' ? c : '_';
    }
    out[at] = 0;
}
void snx_vk_driver_path(char* out, size_t n, const char* dir, const char* name, const char* tag, const char* ext) {
    snprintf(out, n, "%s/%s.%s%s", dir, name, tag, ext);
}

/* VkPipelineCacheHeaderVersionOne: headerSize, headerVersion, vendorID and deviceID, then pipelineCacheUUID */
#define CACHE_HEADER_ONE 32u
static void uuid_hex(char* out, const uint8_t* uuid) {
    for (unsigned i = 0; i < VK_UUID_SIZE; ++i) snprintf(out + 2 * i, 3, "%02x", (unsigned)uuid[i]);
}
int snx_vk_pipeline_cache_check(const void* data, size_t size, const VkPhysicalDeviceProperties* props, char* why,
                                size_t n) {
    const uint8_t* p = (const uint8_t*)data;
    if (n) *why = 0;
    if (!p || size < CACHE_HEADER_ONE) {
        snprintf(why, n, "holds %zu bytes, fewer than the %u of a pipeline cache's header", p ? size : 0,
                 CACHE_HEADER_ONE);
        return -1;
    }
    unsigned header = (unsigned)snx_ld_le32(p), version = (unsigned)snx_ld_le32(p + 4),
             vendor = (unsigned)snx_ld_le32(p + 8), device = (unsigned)snx_ld_le32(p + 12);
    if (header < CACHE_HEADER_ONE || header > size) {
        snprintf(why, n, "has a header of %u bytes, where a pipeline cache's is %u or more and within the file's %zu",
                 header, CACHE_HEADER_ONE, size);
        return -1;
    }
    if (version != VK_PIPELINE_CACHE_HEADER_VERSION_ONE) {
        snprintf(why, n, "has a header of version %u, where Vulkan's is %u", version,
                 (unsigned)VK_PIPELINE_CACHE_HEADER_VERSION_ONE);
        return -1;
    }
    if (vendor != props->vendorID || device != props->deviceID) {
        snprintf(why, n, "was written for the GPU %04x:%08x, and this one is %04x:%08x", vendor, device,
                 (unsigned)props->vendorID, (unsigned)props->deviceID);
        return -1;
    }
    if (memcmp(p + 16, props->pipelineCacheUUID, VK_UUID_SIZE)) {
        char was[2 * VK_UUID_SIZE + 1], is[2 * VK_UUID_SIZE + 1];
        uuid_hex(was, p + 16);
        uuid_hex(is, props->pipelineCacheUUID);
        snprintf(why, n, "was written by another build of the driver (pipeline cache UUID %s, and this one's is %s)",
                 was, is);
        return -1;
    }
    return 0;
}
