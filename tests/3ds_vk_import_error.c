/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The error a RAM region's import gives when the driver refuses it (gpu/vk/device.c), at each step: the call that
   failed and its VkResult, the region's range, host address and alignment, the memory types asked for. Then the error
   the device's own memory gives when it cannot hold the RAM either, at each step. The values are made up, none of them
   a default. Then the device of this host, if any: the error its start gave is the reason snx_ctr_vk_error keeps, then
   the device and driver it chose; none is kept once it starts. */
#include "3ds/gpu/vk/vk.h"
#include "common/utilities/common.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void expect(const SnxCtrVkImport* f, const char* region, const char* want) {
    char got[512];
    snx_ctr_vk_import_error(f, region, got, sizeof got);
    if (strcmp(got, want)) {
        fprintf(stderr, "got:  %s\nwant: %s\n", got, want);
        abort();
    }
}
static void expect_ram(const SnxCtrVkDeviceRam* f, const char* region, const char* want) {
    char got[512];
    snx_ctr_vk_device_ram_error(f, region, got, sizeof got);
    if (strcmp(got, want)) {
        fprintf(stderr, "got:  %s\nwant: %s\n", got, want);
        abort();
    }
}
int main(void) {
    const char* const where = "the driver cannot import the VRAM region (0x18000000, 0x600000 bytes at host "
                              "0x7f01db900000, alignment 4096): ";
    char want[512];
    SnxCtrVkImport f = {.step = SNX_CTR_VK_IMPORT_ALLOCATE,
                        .result = VK_ERROR_INVALID_EXTERNAL_HANDLE,
                        .base = 0x18000000,
                        .size = 0x600000,
                        .host = 0x7f01db900000ull,
                        .alignment = 4096,
                        .pointer_types = 0x20,
                        .buffer_types = 0x2f,
                        .type = 5};
    snprintf(want, sizeof want,
             "%svkAllocateMemory gave VK_ERROR_INVALID_EXTERNAL_HANDLE (-1000072003) for memory type 5 (the host "
             "pointer's types 0x20, the buffer's 0x2f)",
             where);
    expect(&f, "VRAM", want);
    f.step = SNX_CTR_VK_IMPORT_BIND;
    f.result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
    snprintf(want, sizeof want,
             "%svkBindBufferMemory gave VK_ERROR_OUT_OF_DEVICE_MEMORY (-2) for memory type 5 (the host pointer's types "
             "0x20, the buffer's 0x2f)",
             where);
    expect(&f, "VRAM", want);
    f.step = SNX_CTR_VK_IMPORT_POINTER;
    f.result = VK_ERROR_INVALID_EXTERNAL_HANDLE;
    snprintf(want, sizeof want,
             "%svkGetMemoryHostPointerPropertiesEXT gave VK_ERROR_INVALID_EXTERNAL_HANDLE (-1000072003)", where);
    expect(&f, "VRAM", want);
    f.step = SNX_CTR_VK_IMPORT_BUFFER;
    f.result = VK_ERROR_OUT_OF_HOST_MEMORY;
    snprintf(want, sizeof want, "%svkCreateBuffer gave VK_ERROR_OUT_OF_HOST_MEMORY (-1)", where);
    expect(&f, "VRAM", want);
    f.step = SNX_CTR_VK_IMPORT_TYPE;
    f.pointer_types = 0x10;
    f.buffer_types = 0x0f;
    snprintf(want, sizeof want,
             "%sno host-coherent memory type is both one of the host pointer's (0x10) and one of the buffer's (0xf)",
             where);
    expect(&f, "VRAM", want);
    f = (SnxCtrVkImport){.step = SNX_CTR_VK_IMPORT_ALIGNMENT,
                         .base = 0x1ff80000,
                         .size = 0x80000,
                         .host = 0x7f0000001000ull,
                         .alignment = 65536};
    expect(&f, "AXI WRAM",
           "the driver cannot import the AXI WRAM region (0x1ff80000, 0x80000 bytes at host 0x7f0000001000, alignment "
           "65536): its host address or its size is not a multiple of the alignment");
    f.result = (VkResult)-77; /* a result snx_vk_result_str has no name for still says its value */
    f.step = SNX_CTR_VK_IMPORT_POINTER;
    expect(&f, "FCRAM",
           "the driver cannot import the FCRAM region (0x1ff80000, 0x80000 bytes at host 0x7f0000001000, alignment "
           "65536): vkGetMemoryHostPointerPropertiesEXT gave VkResult (-77)");
    printf("import errors: 7 messages say their values\n");

    /* the device's memory that could not hold the RAM either, at each step */
    SnxCtrVkDeviceRam d = {.step = SNX_CTR_VK_DEVICE_RAM_EXTENSIONS, .size = 0x6e00000};
    expect_ram(&d, "IO", "the device has no VK_KHR_external_memory_fd and VK_EXT_external_memory_dma_buf to export it");
    d.step = SNX_CTR_VK_DEVICE_RAM_EXPORT;
    d.features = VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT;
    expect_ram(&d, "IO", "its buffers export a dma-buf never (external memory features 0x4)");
    d.features = VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT | VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT;
    expect_ram(&d, "IO",
               "its buffers export a dma-buf only from a dedicated allocation (external memory features 0x3)");
    d.step = SNX_CTR_VK_DEVICE_RAM_BUFFER;
    d.result = VK_ERROR_OUT_OF_HOST_MEMORY;
    d.offset = 0x700000;
    expect_ram(&d, "FCRAM",
               "vkCreateBuffer gave VK_ERROR_OUT_OF_HOST_MEMORY (-1) for the FCRAM region at offset 0x700000");
    d.step = SNX_CTR_VK_DEVICE_RAM_ALIGNMENT;
    d.offset = 0x680000;
    d.alignment = 1048576;
    expect_ram(&d, "AXI WRAM",
               "the AXI WRAM region's offset 0x680000 is not a multiple of its buffer's alignment 1048576");
    d.step = SNX_CTR_VK_DEVICE_RAM_TYPE;
    d.buffer_types = 0x7ad;
    d.visible_types = 0x84;
    expect_ram(&d, "IO",
               "no host-visible, coherent, cached memory type is one of its buffers' (0x7ad; the host-visible coherent "
               "ones, uncached: 0x84)");
    d.step = SNX_CTR_VK_DEVICE_RAM_ALLOCATE;
    d.result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
    d.allocation = 0x6e01000;
    d.type = 6;
    expect_ram(&d, "IO",
               "vkAllocateMemory gave VK_ERROR_OUT_OF_DEVICE_MEMORY (-2) for 0x6e01000 bytes of memory type 6");
    d.step = SNX_CTR_VK_DEVICE_RAM_BIND;
    d.result = VK_ERROR_INVALID_EXTERNAL_HANDLE;
    d.offset = 0x600000;
    expect_ram(
        &d, "DSP RAM",
        "vkBindBufferMemory gave VK_ERROR_INVALID_EXTERNAL_HANDLE (-1000072003) for the DSP RAM region at offset "
        "0x600000");
    d.step = SNX_CTR_VK_DEVICE_RAM_EXPORT_FD;
    d.result = VK_ERROR_TOO_MANY_OBJECTS;
    expect_ram(&d, "IO", "vkGetMemoryFdKHR gave VK_ERROR_TOO_MANY_OBJECTS (-10) for its dma-buf");
    d.step = SNX_CTR_VK_DEVICE_RAM_VIEWS;
    d.error = ENOMEM;
    char reason[128];
    errno = ENOMEM;
    snprintf(want, sizeof want, "the guest's views of the RAM do not map its dma-buf: %s",
             snx_os_error(reason, sizeof reason));
    expect_ram(&d, "IO", want);
    printf("the device's memory's errors: 10 messages say their values\n");

    /* this host's device: the error its start gave is the reason kept, then the device it chose; none once it starts */
    SnxCtrMemory* m = snx_ctr_memory_new(SNX_CTR_OLD);
    char why[1024];
    assert(m);
    if (snx_ctr_vk_init(m, why, sizeof why)) {
        const SnxCtrVk* v = snx_ctr_vk();
        assert(!snx_ctr_vk_error()[0]);
        if (v->ram_memory)
            printf("the device started, the RAM its memory (type %u, a dma-buf)\n", v->ram_type);
        else
            printf("the device started, every RAM region imported\n");
        snx_ctr_vk_free();
    } else {
        const char* reason = snx_ctr_vk_error();
        size_t n = strlen(reason);
        assert(n && !strncmp(why, reason, n) && (!why[n] || !strncmp(why + n, "; device ", 9)));
        assert(!strstr(reason, "; device "));
        printf("no device here: %s\n", why);
    }
    snx_ctr_memory_free(m);
    return 0;
}
