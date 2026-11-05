/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The transfer engine on the GPU (gpu/vk/transfer.c) against its CPU reference (gpu/transfer.c), byte for byte, in
   guest RAM: memory fills of each width, display transfers between every pair of formats with each flag and scaling,
   texture copies with gaps. The bytes a command does not write must keep their values too. Each command counts the
   bytes it reads and writes of the RAM in the renderer's traffic, and a CPU read of a fill not done yet waits for it
   through one trap. */
#include "3ds/gpu/vk/vk.h"
#include "3ds/gpu/vk/vk_render.h"
#include "3ds/gpu/vk/vk_transfer.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define IN  0x20100000u /* FCRAM */
#define OUT 0x18000000u /* VRAM */
#define SPAN 0x80000u

static uint32_t seed = 0x3d5u;
static uint32_t next(void) {
    seed = seed * 1664525u + 1013904223u;
    return seed >> 8;
}
static void randomize(SnxCtrMemory* m, unsigned char* in, unsigned char* out) {
    for (uint32_t i = 0; i < SPAN; ++i) in[i] = (unsigned char)next(), out[i] = (unsigned char)next();
    assert(snx_ctr_phys_write(m, IN, in, SPAN) == SNX_CTR_MEM_OK);
    assert(snx_ctr_phys_write(m, OUT, out, SPAN) == SNX_CTR_MEM_OK);
}
/* the bytes the renderer counted as read and written since before, which must be read and written */
static void counted(SnxCtrVkTraffic before, uint64_t read, uint64_t written, const char* what) {
    SnxCtrVkTraffic now = snx_ctr_vk_traffic();
    if (now.read - before.read != read || now.written - before.written != written) {
        fprintf(stderr, "%s: the traffic counted %llu bytes read and %llu written, %llu and %llu expected\n", what,
                (unsigned long long)(now.read - before.read), (unsigned long long)(now.written - before.written),
                (unsigned long long)read, (unsigned long long)written);
        abort();
    }
}
static void same(SnxCtrMemory* m, const unsigned char* expected, const char* what) {
    static unsigned char actual[SPAN];
    assert(snx_ctr_vk_finish());
    assert(snx_ctr_phys_read(m, OUT, actual, SPAN) == SNX_CTR_MEM_OK);
    for (uint32_t i = 0; i < SPAN; ++i)
        if (actual[i] != expected[i]) {
            fprintf(stderr, "%s: byte 0x%x is 0x%02x, the reference's 0x%02x\n", what, i, actual[i], expected[i]);
            abort();
        }
}
int main(void) {
    static unsigned char in[SPAN], out[SPAN];
    SnxCtrMemory* m = snx_ctr_memory_new(SNX_CTR_OLD);
    char why[256];
    assert(m);
    if (!snx_ctr_vk_init(m, why, sizeof why)) {
        printf("no Vulkan device (%s): GPU transfer checks skipped\n", why);
        snx_ctr_memory_free(m);
        return 77;
    }
    unsigned checked = 0;
    for (unsigned i = 0; i < 24; ++i) { /* fills: 16-, 24- and 32-bit, odd sizes */
        uint32_t control = i % 3 == 0 ? 0 : i % 3 == 1 ? SNX_CTR_FILL_24BIT : SNX_CTR_FILL_32BIT;
        uint32_t at = next() % 0x1000 * 8, size = next() % 0x8000 + 1, value = next() ^ next() << 8;
        randomize(m, in, out);
        snx_ctr_fill(out + at, size, value, control | 1);
        SnxCtrVkTraffic before = snx_ctr_vk_traffic();
        assert(snx_ctr_vk_fill(OUT + at, size, value, control | 1) == SNX_CTR_XFER_OK);
        counted(before, 0, size, "fill");
        same(m, out, "fill");
        ++checked;
    }
    /* the CPU reading what a fill not done yet writes: one trap, which waits for it */
    SnxCtrVkTraffic before = snx_ctr_vk_traffic();
    uint32_t word = 0;
    assert(snx_ctr_vk_fill(OUT + 0x2000, 0x1000, 0x5a6b7c8du, SNX_CTR_FILL_32BIT | 1) == SNX_CTR_XFER_OK);
    assert(snx_ctr_phys_read(m, OUT + 0x2100, &word, sizeof word) == SNX_CTR_MEM_OK);
    if (word != 0x5a6b7c8du || snx_ctr_vk_traffic().traps != before.traps + 1) {
        fprintf(stderr, "a CPU read of a fill not done: 0x%08x after %llu traps, 0x5a6b7c8d after 1 expected\n", word,
                (unsigned long long)(snx_ctr_vk_traffic().traps - before.traps));
        abort();
    }
    static const uint32_t flags[] = {0,
                                     SNX_CTR_XFER_FLIP,
                                     SNX_CTR_XFER_LINEAR,
                                     SNX_CTR_XFER_LINEAR | SNX_CTR_XFER_NO_SWIZZLE,
                                     SNX_CTR_XFER_NO_SWIZZLE,
                                     SNX_CTR_XFER_FLIP | SNX_CTR_XFER_CROP,
                                     1u << SNX_CTR_XFER_SCALE_SHIFT,
                                     2u << SNX_CTR_XFER_SCALE_SHIFT,
                                     SNX_CTR_XFER_FLIP | (2u << SNX_CTR_XFER_SCALE_SHIFT)};
    for (unsigned in_fmt = 0; in_fmt < 5; ++in_fmt)
        for (unsigned out_fmt = 0; out_fmt < 5; ++out_fmt)
            for (unsigned f = 0; f < sizeof flags / sizeof *flags; ++f) {
                uint32_t width = 8 * (1 + next() % 12), height = 8 * (1 + next() % 8);
                uint32_t out_width = flags[f] & SNX_CTR_XFER_CROP ? width - 8 : width;
                SnxCtrDisplayTransfer t = {width | height << 16, out_width | height << 16,
                                           flags[f] | in_fmt << SNX_CTR_XFER_IN_FORMAT |
                                               out_fmt << SNX_CTR_XFER_OUT_FORMAT};
                size_t in_bytes, out_bytes;
                randomize(m, in, out);
                if (!snx_ctr_display_spans(&t, &in_bytes, &out_bytes)) continue;
                assert(snx_ctr_display_transfer(&t, in, SPAN, out, SPAN) == SNX_CTR_XFER_OK);
                before = snx_ctr_vk_traffic();
                assert(snx_ctr_vk_display_transfer(&t, IN, OUT) == SNX_CTR_XFER_OK);
                char what[64];
                snprintf(what, sizeof what, "display transfer %u to %u, flags 0x%x", in_fmt, out_fmt, flags[f]);
                counted(before, in_bytes, out_bytes, what);
                same(m, out, what);
                ++checked;
            }
    for (unsigned i = 0; i < 32; ++i) { /* texture copies: lines and gaps of 16-byte units, contiguous sides */
        uint32_t in_line = (1 + next() % 8) | (next() % 4) << 16, out_line = (1 + next() % 8) | (next() % 4) << 16;
        if (i % 4 == 0) in_line = 0;
        if (i % 4 == 1) out_line &= 0xffff;
        SnxCtrTextureCopy t = {(next() % 0x2000 + 1) * 16 + next() % 16, in_line, out_line};
        size_t in_bytes, out_bytes;
        randomize(m, in, out);
        assert(snx_ctr_texture_copy(&t, in, SPAN, out, SPAN) == SNX_CTR_XFER_OK);
        assert(snx_ctr_texture_spans(&t, &in_bytes, &out_bytes));
        before = snx_ctr_vk_traffic();
        assert(snx_ctr_vk_texture_copy(&t, IN, OUT) == SNX_CTR_XFER_OK);
        counted(before, in_bytes, out_bytes, "texture copy");
        same(m, out, "texture copy");
        ++checked;
    }
    /* a span leaving its region is refused before anything is recorded */
    SnxCtrTextureCopy beyond = {0x1000, 0, 0};
    assert(snx_ctr_vk_texture_copy(&beyond, IN, 0x18600000u - 0x800) == SNX_CTR_XFER_BOUNDS);
    snx_ctr_vk_transfer_free();
    snx_ctr_vk_free();
    snx_ctr_memory_free(m);
    printf("GPU transfer engine: %u fills, display transfers and texture copies equal to the CPU reference\n",
           checked);
    return 0;
}
