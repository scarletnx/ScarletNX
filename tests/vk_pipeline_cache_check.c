/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Which pipeline cache files go to the driver (common/gpu/vk/vk_common.h's snx_vk_pipeline_cache_check), for the
   Switch's renderer and the 3DS's alike: the data a driver gave, whose header names this device and driver build, and
   nothing else, each refusal naming what the file holds against the device's: a file cut inside its header, a header
   whose size is short or past the file, another header version, another GPU, another build of the driver */
#include "common/gpu/vk/vk_common.h"
#include <stdio.h>
#include <string.h>

static int failures;

static void put32(unsigned char* p, uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = (unsigned char)(v >> 8 * i);
}
/* a pipeline cache's data as a driver of props gives it: the header, least significant byte first, then its own */
static void cache_of(unsigned char* out, size_t size, const VkPhysicalDeviceProperties* props) {
    memset(out, 0x5a, size);
    put32(out, 32);
    put32(out + 4, VK_PIPELINE_CACHE_HEADER_VERSION_ONE);
    put32(out + 8, props->vendorID);
    put32(out + 12, props->deviceID);
    memcpy(out + 16, props->pipelineCacheUUID, VK_UUID_SIZE);
}
/* data of size bytes taken (0) or refused, and a refusal's reason holding each of the words in says */
static void check(const char* name, const unsigned char* data, size_t size, const VkPhysicalDeviceProperties* props,
                  int expect, const char* const* says) {
    char why[256];
    int got = snx_vk_pipeline_cache_check(data, size, props, why, sizeof why);
    int ok = got == expect && (!got) == !why[0];
    for (; ok && says && *says; ++says) ok = strstr(why, *says) != NULL;
    printf("%s: %s%s%s\n", name, got ? "refused, it " : "taken", why, ok ? "" : ": FAIL");
    failures += !ok;
}

int main(void) {
    VkPhysicalDeviceProperties props = {.vendorID = 0x1002, .deviceID = 0x7550};
    for (unsigned i = 0; i < VK_UUID_SIZE; ++i) props.pipelineCacheUUID[i] = (uint8_t)(0x10 + i);
    unsigned char data[64];
    cache_of(data, sizeof data, &props);

    check("the driver's own data", data, sizeof data, &props, 0, NULL);
    check("its header alone, an empty cache", data, 32, &props, 0, NULL);
    check("no data", NULL, 0, &props, -1, (const char* const[]){"holds 0 bytes", "the 32", NULL});
    check("cut inside its header", data, 16, &props, -1, (const char* const[]){"holds 16 bytes", NULL});

    unsigned char other[64];
    memcpy(other, data, sizeof other);
    put32(other, 16);
    check("a header of 16 bytes", other, sizeof other, &props, -1,
          (const char* const[]){"header of 16 bytes", "file's 64", NULL});
    put32(other, 65);
    check("a header past the file", other, sizeof other, &props, -1,
          (const char* const[]){"header of 65 bytes", "file's 64", NULL});
    put32(other, 48);
    check("a longer header, within the file", other, sizeof other, &props, 0, NULL);

    memcpy(other, data, sizeof other);
    put32(other + 4, 2);
    check("header version 2", other, sizeof other, &props, -1, (const char* const[]){"version 2", NULL});

    memcpy(other, data, sizeof other);
    put32(other + 8, 0x10de);
    put32(other + 12, 0x2684);
    check("another GPU", other, sizeof other, &props, -1,
          (const char* const[]){"10de:00002684", "1002:00007550", NULL});

    memcpy(other, data, sizeof other);
    other[16 + 15] ^= 0xff;
    check("another build of the driver", other, sizeof other, &props, -1,
          (const char* const[]){"101112131415161718191a1b1c1d1ee0", "101112131415161718191a1b1c1d1e1f", NULL});

    /* the bytes are read least significant first whatever the host's order: a vendor of 0x02100000 is not 0x1002 */
    memcpy(other, data, sizeof other);
    other[8] = 0x00, other[9] = 0x00, other[10] = 0x10, other[11] = 0x02;
    check("its vendor in the other order", other, sizeof other, &props, -1,
          (const char* const[]){"2100000:00007550", NULL});

    if (failures) fprintf(stderr, "vk_pipeline_cache_check: %d failed\n", failures);
    return failures != 0;
}
