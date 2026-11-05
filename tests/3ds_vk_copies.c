/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The GPU's copy of the board's RAM (gpu/vk/render.c's page states), page by page, through the transfer engine and
   the CPU's accesses by physical address, as write tracking brings them to the renderer: what the CPU wrote is copied
   in once, before the GPU's first read, and only the pages it wrote; what the GPU wrote is copied back once, at the
   CPU's first access, with the pages the GPU wrote around it, and the CPU's next reads trap no more; a GPU write to
   part of a page keeps the CPU's bytes in the rest of it; and what the GPU wrote is in the RAM once the device is
   gone, which nothing on the CPU read before. In place (--vk_memory=2) nothing is copied, and the
   bytes are the same. */
#include "3ds/gpu/vk/vk.h"
#include "3ds/gpu/vk/vk_render.h"
#include "3ds/gpu/vk/vk_transfer.h"
#include "support/3ds_vk_path.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SOURCE 0x20200000u /* FCRAM */
#define DEST   0x18100000u /* VRAM */
#define PARTLY 0x18200000u
#define LATER  0x18300000u
#define SPAN   0x10000u /* 16 pages */

static struct {
    int failures;
    bool copies;          /* the work is on a copy of the RAM */
    SnxCtrVkTraffic last; /* the traffic at the last check */
    uint32_t pattern[SPAN / 4];
} g_copies;
/* what the renderer copied in and out, and the traps, since the last check: each checked on a copy of the RAM */
static void moved(const char* what, uint64_t in, uint64_t out, int traps) {
    SnxCtrVkTraffic now = snx_ctr_vk_traffic();
    const SnxCtrVkTraffic* last = &g_copies.last;
    uint64_t got_in = now.copied_in - last->copied_in, got_out = now.copied_out - last->copied_out;
    uint64_t got_traps = now.traps - last->traps;
    g_copies.last = now;
    if (!g_copies.copies) in = out = 0;
    if (got_in != in || got_out != out || (g_copies.copies && traps >= 0 && got_traps != (uint64_t)traps)) {
        fprintf(stderr, "%s: 0x%llx bytes copied in, 0x%llx out, %llu traps; 0x%llx, 0x%llx and %d expected\n", what,
                (unsigned long long)got_in, (unsigned long long)got_out, (unsigned long long)got_traps,
                (unsigned long long)in, (unsigned long long)out, traps);
        ++g_copies.failures;
    }
}
static void word_is(SnxCtrMemory* m, uint32_t pa, uint32_t want, const char* what) {
    uint32_t got = 0;
    if (snx_ctr_phys_read(m, pa, &got, 4) != SNX_CTR_MEM_OK || got != want) {
        fprintf(stderr, "%s: 0x%08x at 0x%08x, 0x%08x expected\n", what, got, pa, want);
        ++g_copies.failures;
    }
}
/* the transfer engine's copy of size bytes from one address to another, a line of them, waited for */
static void gpu_copy(uint32_t from, uint32_t to, uint32_t size) {
    SnxCtrTextureCopy t = {size, 0, 0};
    if (snx_ctr_vk_texture_copy(&t, from, to) != SNX_CTR_XFER_OK || !snx_ctr_vk_finish()) {
        fprintf(stderr, "the GPU's copy of 0x%x bytes from 0x%08x to 0x%08x failed\n", size, from, to);
        exit(1);
    }
}
int main(int argc, char** argv) {
    int vk_memory = test_ctr_vk_memory(&argc, argv);
    uint32_t* pattern = g_copies.pattern;
    SnxCtrMemory* m = snx_ctr_memory_new(SNX_CTR_OLD);
    char why[512];
    if (!m) return 1;
    if (!snx_ctr_vk_init(m, why, sizeof why)) {
        printf("no Vulkan device (%s): the RAM's copy unchecked\n", why);
        snx_ctr_memory_free(m);
        return 77;
    }
    test_ctr_vk_memory_taken(vk_memory);
    g_copies.copies = snx_ctr_vk_copies();
    for (uint32_t i = 0; i < SPAN / 4; ++i) pattern[i] = 0x10000000u + i;
    if (snx_ctr_phys_write(m, SOURCE, pattern, SPAN) != SNX_CTR_MEM_OK) return 1;
    g_copies.last = snx_ctr_vk_traffic();
    /* the source's 16 pages copied in, and the destination's, which a write need not cover whole */
    gpu_copy(SOURCE, DEST, SPAN);
    moved("the first GPU copy", 2 * SPAN, 0, 0);
    /* the CPU's first read of what the GPU wrote: one trap, the 16 pages it wrote copied back */
    word_is(m, DEST + 5 * 4096 + 8, pattern[5 * 1024 + 2], "the GPU's copy, read on the CPU");
    moved("the CPU's first read", 0, SPAN, 1);
    word_is(m, DEST + 9 * 4096, pattern[9 * 1024], "another page of it");
    moved("the CPU's second read", 0, 0, 0);
    /* the CPU writes one page of the source: one trap, and only that page is copied in again */
    pattern[3 * 1024 + 7] = 0xfeedf00du;
    if (snx_ctr_phys_write(m, SOURCE + 3 * 4096 + 28, &pattern[3 * 1024 + 7], 4) != SNX_CTR_MEM_OK) return 1;
    moved("the CPU's write", 0, 0, 1);
    gpu_copy(SOURCE, DEST, SPAN);
    moved("the second GPU copy", 4096, 0, 0);
    word_is(m, DEST + 3 * 4096 + 28, 0xfeedf00du, "the CPU's new word, through the GPU");
    moved("the CPU's read after it", 0, SPAN, 1);
    /* a fill of part of a page the CPU wrote: the rest of the page keeps the CPU's bytes */
    if (snx_ctr_phys_write(m, PARTLY, pattern, 4096) != SNX_CTR_MEM_OK) return 1;
    if (snx_ctr_vk_fill(PARTLY + 0x100, 0x40, 0xabcdef01u, SNX_CTR_FILL_32BIT | 1) != SNX_CTR_XFER_OK) return 1;
    word_is(m, PARTLY + 0xfc, pattern[0x3f], "the word before the fill");
    word_is(m, PARTLY + 0x100, 0xabcdef01u, "the fill");
    word_is(m, PARTLY + 0x140, pattern[0x50], "the word after the fill");
    word_is(m, PARTLY + 0xffc, pattern[0x3ff], "the page's last word");
    moved("a fill of part of a page", 4096, 4096, -1);
    /* a fill nothing on the CPU reads, done, then the device gone: the RAM holds it */
    if (snx_ctr_vk_fill(LATER, 0x2000, 0x600d600du, SNX_CTR_FILL_32BIT | 1) != SNX_CTR_XFER_OK || !snx_ctr_vk_finish())
        return 1;
    snx_ctr_vk_transfer_free();
    snx_ctr_vk_free();
    const unsigned char* ram = NULL;
    const SnxCtrProfile* profile = snx_ctr_memory_profile(m);
    for (size_t i = 0; i < profile->region_count; ++i)
        if (profile->regions[i].base == 0x18000000u) ram = snx_ctr_memory_region(m, i);
    uint32_t after = 0;
    if (ram) memcpy(&after, ram + (LATER - 0x18000000u) + 0x1ffc, 4);
    if (after != 0x600d600du) {
        fprintf(stderr, "the RAM after the device: 0x%08x at 0x%08x, the fill's 0x600d600d expected\n", after,
                LATER + 0x1ffc);
        ++g_copies.failures;
    }
    snx_ctr_memory_free(m);
    if (g_copies.failures) return 1;
    printf("3ds_vk_copies: the work %s, every byte where it belongs\n",
           g_copies.copies ? "on a copy of the RAM, copied in and out by page" : "in place, nothing copied");
    return 0;
}
