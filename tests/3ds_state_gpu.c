/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The 3DS's save states with the GPU's work in them (src/3ds/runtime/state.c): a process's RAM saved and loaded through
   write tracking, on each path the device takes to the RAM (--vk_memory=1 a copy of it, 2 in place). The transfer
   engine copies a buffer from FCRAM into VRAM and fills another, and the state is saved before that work is waited
   for: the state holds what the GPU wrote. The GPU then fills over the buffer the state holds, which nothing on the CPU
   reads, and the CPU writes over the rest. A load gives every byte back, and the GPU's next copy of the buffer reads
   what the load wrote, not what the GPU kept of its own fill; a fresh process's load, on a device of its own, too.
   Skips (77) without a Vulkan device or without the path asked for */
#include "3ds/runtime/state.h"
#include "3ds/runtime/internal.h"
#include "3ds/gpu/transfer.h"
#include "3ds/gpu/vk/vk.h"
#include "3ds/gpu/vk/vk_transfer.h"
#include "common/utilities/common.h"
#include "support/3ds_guest.h"
#include "support/3ds_vk_path.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SOURCE 0x20200000u /* FCRAM */
#define DEST   0x18100000u /* VRAM */
#define FILL   0x18200000u
#define LATER  0x18300000u
#define SPAN   0x10000u /* 16 pages */
#define CHECK(expression)                                                                                              \
    do {                                                                                                               \
        if (!(expression)) {                                                                                           \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expression);                                           \
            exit(1);                                                                                                   \
        }                                                                                                              \
    } while (0)

/* the buffer's words, and the words a check reads or the CPU writes */
static struct {
    uint32_t pattern[SPAN / 4], words[SPAN / 4];
} g_state_gpu;
static void copy(uint32_t from, uint32_t to) {
    SnxCtrTextureCopy t = {SPAN, 0, 0};
    CHECK(snx_ctr_vk_texture_copy(&t, from, to) == SNX_CTR_XFER_OK);
}
static void fill(uint32_t at, uint32_t value) {
    CHECK(snx_ctr_vk_fill(at, SPAN, value, SNX_CTR_FILL_32BIT | 1) == SNX_CTR_XFER_OK);
}
static void cpu_fill(SnxCtrProcess* p, uint32_t at, uint32_t value) {
    uint32_t* words = g_state_gpu.words;
    for (uint32_t i = 0; i < SPAN / 4; ++i) words[i] = value;
    CHECK(snx_ctr_phys_write(p->memory, at, words, SPAN) == SNX_CTR_MEM_OK);
}
/* the words at pa that are not the pattern (value 0) or value: 0 when all are */
static uint32_t wrong(SnxCtrProcess* p, uint32_t pa, uint32_t value, const char* what) {
    uint32_t* got = g_state_gpu.words;
    const uint32_t* pattern = g_state_gpu.pattern;
    CHECK(snx_ctr_phys_read(p->memory, pa, got, SPAN) == SNX_CTR_MEM_OK);
    uint32_t n = 0, first = 0;
    for (uint32_t i = 0; i < SPAN / 4; ++i)
        if (got[i] != (value ? value : pattern[i]) && !n++) first = i;
    if (n)
        fprintf(stderr, "%s: %u words wrong at 0x%08x, the first 0x%08x where 0x%08x was expected\n", what, n, pa,
                got[first], value ? value : pattern[first]);
    return n;
}
static SnxCtrProcess* process(const SnxCtrImage* image, int vk_memory) {
    char why[512];
    SnxCtrProcess* p = snx_ctr_process_new(image, SNX_CTR_OLD);
    CHECK(p);
    if (!snx_ctr_vk_init(p->memory, true, why, sizeof why)) {
        printf("3ds_state_gpu: skipped, no Vulkan device (%s)\n", why);
        exit(77);
    }
    test_ctr_vk_memory_taken(vk_memory);
    return p;
}

int main(int argc, char** argv) {
    int vk_memory = test_ctr_vk_memory(&argc, argv);
    char *root = snx_temp_dir(NULL), *saved = root ? snx_path_join(root, "gpu.snxstate") : NULL, why[512];
    CHECK(saved);
    unsigned char code[0x3000] = {0};
    SnxCtrImage image = ctr_image(code, sizeof code, 0x3000);
    SnxCtrProcess* p = process(&image, vk_memory);
    const char* path = snx_ctr_vk_copies() || snx_ctr_vk_shadow() ? "on a copy of the RAM" : "in place";
    uint32_t* pattern = g_state_gpu.pattern;
    for (uint32_t i = 0; i < SPAN / 4; ++i) pattern[i] = 0x31000000u + i * 2654435761u % 0xffffffu;
    CHECK(snx_ctr_phys_write(p->memory, SOURCE, pattern, SPAN) == SNX_CTR_MEM_OK);
    /* the GPU's work, not waited for: the buffer copied into VRAM, another span filled */
    copy(SOURCE, DEST);
    fill(FILL, 0x600d600du);
    CHECK(!snx_ctr_state_save(p, saved, NULL, why, sizeof why));
    /* the GPU over the buffer, nothing on the CPU reading it; the CPU over the rest */
    fill(SOURCE, 0xbadbadbau);
    CHECK(snx_ctr_vk_finish());
    cpu_fill(p, DEST, 0xdeadbeefu);
    cpu_fill(p, FILL, 0xdeadbeefu);
    if (snx_ctr_state_load(p, saved, why, sizeof why)) {
        fprintf(stderr, "3ds_state_gpu: %s\n", why);
        return 1;
    }
    /* the GPU's next copy reads the buffer the load wrote, before the CPU looks at it */
    copy(SOURCE, LATER);
    CHECK(snx_ctr_vk_finish());
    uint32_t failures = wrong(p, LATER, 0, "the GPU's copy after the load") +
                        wrong(p, SOURCE, 0, "the buffer after the load") +
                        wrong(p, DEST, 0, "the GPU's copy the save waited for") +
                        wrong(p, FILL, 0x600d600du, "the GPU's fill the save waited for");
    snx_ctr_vk_transfer_free();
    snx_ctr_vk_free();
    snx_ctr_process_free(p);
    /* a fresh process on a device of its own: the load makes its RAM, which its GPU reads */
    SnxCtrProcess* q = process(&image, vk_memory);
    if (snx_ctr_state_load(q, saved, why, sizeof why)) {
        fprintf(stderr, "3ds_state_gpu: a fresh process: %s\n", why);
        return 1;
    }
    copy(DEST, LATER);
    CHECK(snx_ctr_vk_finish());
    failures +=
        wrong(q, LATER, 0, "a fresh process's GPU copy") + wrong(q, FILL, 0x600d600du, "a fresh process's fill");
    snx_ctr_vk_transfer_free();
    snx_ctr_vk_free();
    snx_ctr_process_free(q);
    remove(saved);
    CHECK(!snx_remove_tree(root));
    free(saved);
    free(root);
    if (failures) return 1;
    printf("3ds_state_gpu: ok (the work %s)\n", path);
    return 0;
}
