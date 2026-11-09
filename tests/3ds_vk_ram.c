/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The board's RAM as the GPU has it (gpu/vk/device.c), whichever way the device took it: imported, or moved into the
   device's memory where the driver will not import it (RADV). What the guest wrote before the device started reaches
   the GPU; a GPU fill shows in every guest view of the page (the flat space's two aliases, the API, the host's view);
   a guest store through the flat space reaches the GPU; once the device is gone the RAM still holds it all, and a
   device made again takes the RAM again. A host whose device starts, then cannot take the RAM, fails this test: on
   main, RADV did (#1116). No device at all is a skip. */
#include "3ds/gpu/vk/vk.h"
#include "support/3ds_vk_path.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PAGE  0x20004000u /* FCRAM */
#define OTHER 0x18001000u /* VRAM */
static SnxCtrMemory* g_m;

/* where the host holds the region at physical address pa */
static unsigned char* region(uint32_t pa) {
    const SnxCtrProfile* p = snx_ctr_memory_profile(g_m);
    for (size_t i = 0; i < p->region_count; ++i)
        if (p->regions[i].base == pa) return snx_ctr_memory_region(g_m, i);
    return NULL;
}
static VkBuffer buffer_of(uint32_t pa, uint32_t size, VkDeviceSize* offset) {
    const SnxCtrVkRam* ram = snx_ctr_vk_ram(pa, size, offset);
    assert(ram);
    return ram->buffer;
}
/* [from, from + size) copied to [to, to + size) by the GPU, waited for */
static void gpu_copy(uint32_t from, uint32_t to, uint32_t size) {
    VkBufferCopy c = {.size = size};
    VkBuffer src = buffer_of(from, size, &c.srcOffset), dst = buffer_of(to, size, &c.dstOffset);
    vkCmdCopyBuffer(snx_ctr_vk_commands(), src, dst, 1, &c);
    assert(snx_ctr_vk_finish());
}
static void gpu_fill(uint32_t pa, uint32_t size, uint32_t value) {
    VkDeviceSize offset;
    VkBuffer b = buffer_of(pa, size, &offset);
    vkCmdFillBuffer(snx_ctr_vk_commands(), b, offset, size, value);
    assert(snx_ctr_vk_finish());
}
static void expect(const unsigned char* got, const unsigned char* want, size_t size, const char* what) {
    for (size_t i = 0; i < size; ++i)
        if (got[i] != want[i]) {
            fprintf(stderr, "%s: byte %zu is 0x%02x, 0x%02x expected\n", what, i, got[i], want[i]);
            exit(1);
        }
}
/* the device started, with the RAM; 77 (a skip) when there is no device, 1 when it started and could not take the
   RAM */
static int start(int vk_memory) {
    char why[1024];
    if (snx_ctr_vk_init(g_m, why, sizeof why)) {
        test_ctr_vk_memory_taken(vk_memory);
        if (snx_ctr_vk_shadow()) { /* the guest's bytes are not the buffer's: this test's premise is that they are */
            printf("the RAM is a copy in the device's host-visible memory: the buffer is not the guest's memory\n");
            snx_ctr_vk_free();
            return 77;
        }
        return 0; /* how it took the RAM: device.c's line on the standard error */
    }
    const char* reason = snx_ctr_vk_error();
    if (!strncmp(reason, "the driver cannot import", 24) || !strncmp(reason, "the device has no", 17)) {
        fprintf(stderr, "the device started and cannot take the RAM: %s\n", why);
        return 1;
    }
    printf("no Vulkan device (%s): the RAM's checks skipped\n", why);
    return 77;
}
int main(int argc, char** argv) {
    int vk_memory = test_ctr_vk_memory(&argc, argv);
    static unsigned char before[256], fill[64], stored[64], got[256];
    g_m = snx_ctr_memory_new(SNX_CTR_OLD);
    SnxCtrAddressSpace* s = snx_ctr_space_new(g_m);
    assert(g_m && s);
    unsigned char* base = snx_ctr_space_base(s); /* NULL without a flat space: the API alone */
    assert(snx_ctr_map(s, 0x100000, PAGE, 4096, SNX_CTR_READ | SNX_CTR_WRITE) == SNX_CTR_MEM_OK);
    assert(snx_ctr_map(s, 0x200000, PAGE, 4096, SNX_CTR_READ) == SNX_CTR_MEM_OK);
    for (unsigned i = 0; i < sizeof before; ++i) before[i] = (unsigned char)(i * 13 + 5);
    assert(snx_ctr_write(s, 0x100000, before, sizeof before) == SNX_CTR_MEM_OK);
    int started = start(vk_memory);
    if (started) {
        snx_ctr_space_free(s);
        snx_ctr_memory_free(g_m);
        return started;
    }
    /* what the guest wrote before the device started */
    gpu_copy(PAGE, OTHER, sizeof before);
    assert(snx_ctr_phys_read(g_m, OTHER, got, sizeof before) == SNX_CTR_MEM_OK);
    expect(got, before, sizeof before, "the GPU's copy of the guest's bytes");
    /* a GPU fill, in every view of the page */
    gpu_fill(PAGE + 0x200, sizeof fill, 0xc0dec0deu);
    for (unsigned i = 0; i < sizeof fill; ++i) fill[i] = (unsigned char)(0xc0dec0deu >> (i % 4 * 8));
    assert(snx_ctr_read(s, 0x100200, got, sizeof fill) == SNX_CTR_MEM_OK);
    expect(got, fill, sizeof fill, "the fill through the API");
    assert(snx_ctr_read(s, 0x200200, got, sizeof fill) == SNX_CTR_MEM_OK);
    expect(got, fill, sizeof fill, "the fill through the read-only alias");
    expect(region(0x20000000u) + (PAGE - 0x20000000u) + 0x200, fill, sizeof fill,
           "the fill in the host's view of FCRAM");
    unsigned checks = 4;
    if (base) {
        expect(base + 0x100200, fill, sizeof fill, "the fill in the flat space");
        expect(base + 0x200200, fill, sizeof fill, "the fill in the flat space's alias");
        /* a store through the flat space, as compiled code makes it, reaches the GPU */
        for (unsigned i = 0; i < sizeof stored; ++i) stored[i] = (unsigned char)(0x80 + i);
        memcpy(base + 0x100300, stored, sizeof stored);
        gpu_copy(PAGE + 0x300, OTHER + 0x400, sizeof stored);
        assert(snx_ctr_phys_read(g_m, OTHER + 0x400, got, sizeof stored) == SNX_CTR_MEM_OK);
        expect(got, stored, sizeof stored, "the GPU's copy of a store through the flat space");
        checks += 3;
    }
    snx_ctr_vk_free();
    /* the device gone, the RAM still holds what it held, and every view is still one memory */
    assert(snx_ctr_read(s, 0x200200, got, sizeof fill) == SNX_CTR_MEM_OK);
    expect(got, fill, sizeof fill, "the fill after the device went");
    assert(snx_ctr_write(s, 0x100000, "again", 5) == SNX_CTR_MEM_OK);
    assert(snx_ctr_read(s, 0x200000, got, 5) == SNX_CTR_MEM_OK && !memcmp(got, "again", 5));
    memcpy(before, "again", 5);
    ++checks;
    /* a device made again takes the RAM again */
    started = start(vk_memory);
    assert(!started);
    gpu_copy(PAGE, OTHER + 0x800, sizeof before);
    assert(snx_ctr_phys_read(g_m, OTHER + 0x800, got, sizeof before) == SNX_CTR_MEM_OK);
    expect(got, before, sizeof before, "the second device's copy of the guest's bytes");
    ++checks;
    snx_ctr_vk_free();
    snx_ctr_space_free(s);
    snx_ctr_memory_free(g_m);
    printf("3ds_vk_ram: %u checks\n", checks);
    return 0;
}
