/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The error a RAM region's import gives when the driver refuses it (gpu/vk/device.c), at each step: the call that
   failed and its VkResult, the region's range, host address and alignment, the memory types asked for. The values are
   made up, none of them a default. Then the device of this host, if any: the error its start gave is the reason
   snx_ctr_vk_error keeps, then the device and driver it chose; none is kept once it starts. */
#include "3ds/gpu/vk/vk.h"
#include <assert.h>
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

    /* this host's device: the error its start gave is the reason kept, then the device it chose; none once it starts */
    SnxCtrMemory* m = snx_ctr_memory_new(SNX_CTR_OLD);
    char why[1024];
    assert(m);
    if (snx_ctr_vk_init(m, why, sizeof why)) {
        assert(!snx_ctr_vk_error()[0]);
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
