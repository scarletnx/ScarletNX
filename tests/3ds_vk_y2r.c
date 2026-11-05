/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The Y2R engine on the GPU (gpu/vk/y2r.c) against its CPU reference (gpu/y2r.c), byte for byte: every input format,
   output format, rotation and alignment, with transfer units and gaps, each stream on pages of the guest's RAM laid
   out in another order than its own, as a heap's may be. The output's bytes the conversion skips keep their values. */
#include "3ds/gpu/vk/vk.h"
#include "3ds/gpu/vk/vk_y2r.h"
#include "support/3ds_vk_path.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define POOL     0x20100000u /* FCRAM pages handed out to the streams */
#define MAX_SPAN 0x500000u
static uint32_t seed = 0x2f1u;
static uint32_t next(void) {
    seed = seed * 1664525u + 1013904223u;
    return seed >> 8;
}
static uint32_t pool_at;
/* a stream of span bytes from offset first in its first page: its pages out of order, its bytes random */
static uint32_t* place(SnxCtrMemory* m, uint32_t first, uint64_t span, unsigned char* bytes, uint32_t* count) {
    uint32_t n = (uint32_t)((first + span + 4095) / 4096);
    uint32_t* pages = malloc(n * sizeof *pages);
    assert(pages);
    for (uint32_t i = 0; i < n; ++i) pages[i] = POOL + (pool_at + i) * 4096;
    pool_at += n;
    for (uint32_t i = n; i > 1; --i) { /* shuffled */
        uint32_t j = next() % i, t = pages[i - 1];
        pages[i - 1] = pages[j];
        pages[j] = t;
    }
    for (uint64_t i = 0; i < span; ++i) bytes[i] = (unsigned char)next();
    for (uint32_t i = 0; i < n; ++i) { /* the page's part of the stream, the rest of it random too */
        unsigned char page[4096];
        for (unsigned b = 0; b < 4096; ++b) {
            int64_t at = (int64_t)i * 4096 + b - first;
            page[b] = at >= 0 && (uint64_t)at < span ? bytes[at] : (unsigned char)next();
        }
        assert(snx_ctr_phys_write(m, pages[i], page, sizeof page) == SNX_CTR_MEM_OK);
    }
    *count = n;
    return pages;
}
int main(int argc, char** argv) {
    int vk_memory = test_ctr_vk_memory(&argc, argv);
    SnxCtrMemory* m = snx_ctr_memory_new(SNX_CTR_OLD);
    char why[256];
    assert(m);
    if (!snx_ctr_vk_init(m, why, sizeof why)) {
        printf("no Vulkan device (%s): GPU Y2R checks skipped\n", why);
        snx_ctr_memory_free(m);
        return 77;
    }
    test_ctr_vk_memory_taken(vk_memory);
    static unsigned char src[3][MAX_SPAN], expected[MAX_SPAN];
    unsigned checked = 0;
    for (unsigned input = 0; input <= SNX_CTR_Y2R_YUYV; ++input)
        for (unsigned output = 0; output <= SNX_CTR_Y2R_RGB565; ++output)
            for (unsigned rotation = 0; rotation < 4; ++rotation)
                for (unsigned block = 0; block < 2; ++block) {
                    SnxCtrY2rConfig c = {(uint8_t)input, (uint8_t)output, (uint8_t)rotation, (uint8_t)block,
                                         (uint16_t)(8 * (1 + next() % 40)), (uint16_t)(block ? 8 * (1 + next() % 12)
                                                                                             : 2 * (1 + next() % 40)),
                                         {0}, (uint16_t)(next() & 0xff)};
                    memcpy(c.coefficients, snx_ctr_y2r_standard[next() % 4], sizeof c.coefficients);
                    /* units of whole lines (a sample or pixel row of 8 lines takes an even count of them), gaps */
                    unsigned n = input == SNX_CTR_Y2R_422_16 || input == SNX_CTR_Y2R_420_16 ? 2 : 1;
                    unsigned y_unit = input == SNX_CTR_Y2R_YUYV ? c.input_line_width * 2 : c.input_line_width * n;
                    unsigned uv_unit = input == SNX_CTR_Y2R_420_8 || input == SNX_CTR_Y2R_420_16
                                           ? c.input_line_width / 4 * n
                                           : c.input_line_width / 2 * n;
                    unsigned bpp = output == SNX_CTR_Y2R_RGBA8 ? 4 : output == SNX_CTR_Y2R_RGB8 ? 3 : 2;
                    SnxCtrY2rBuffer streams[SNX_CTR_Y2R_STREAMS] = {
                        {0x1000u + next() % 4096, 0, (uint16_t)y_unit, (uint16_t)(next() % 3 * 16)},
                        {0x2000u + next() % 4096, 0, (uint16_t)uv_unit, (uint16_t)(next() % 3 * 8)},
                        {0x3000u + next() % 4096, 0, (uint16_t)uv_unit, (uint16_t)(next() % 3 * 8)},
                        {0x4000u + next() % 4096, 0, (uint16_t)(c.input_line_width * bpp), (uint16_t)(next() % 3 * 32)}};
                    uint64_t spans[SNX_CTR_Y2R_STREAMS];
                    if (!snx_ctr_y2r_check(&c, streams, spans)) {
                        fprintf(stderr, "a configuration the engine takes was refused (input %u, lines %u)\n", input,
                                c.input_lines);
                        abort();
                    }
                    assert(spans[SNX_CTR_Y2R_DST] <= MAX_SPAN);
                    pool_at = 0;
                    uint32_t* pages[SNX_CTR_Y2R_STREAMS] = {NULL, NULL, NULL, NULL};
                    uint32_t counts[SNX_CTR_Y2R_STREAMS] = {0, 0, 0, 0};
                    for (unsigned s = 0; s < 3; ++s)
                        if (spans[s]) pages[s] = place(m, streams[s].address & 4095u, spans[s], src[s], &counts[s]);
                    pages[SNX_CTR_Y2R_DST] = place(m, streams[SNX_CTR_Y2R_DST].address & 4095u,
                                                   spans[SNX_CTR_Y2R_DST], expected, &counts[SNX_CTR_Y2R_DST]);
                    const unsigned char* const inputs[3] = {src[0], src[1], src[2]};
                    snx_ctr_y2r_convert(&c, streams, inputs, expected);
                    const uint32_t* const pages_in[SNX_CTR_Y2R_STREAMS] = {pages[0], pages[1], pages[2], pages[3]};
                    assert(snx_ctr_vk_y2r(&c, streams, pages_in, counts) == SNX_CTR_XFER_OK);
                    uint32_t first = streams[SNX_CTR_Y2R_DST].address & 4095u;
                    for (uint64_t i = 0; i < spans[SNX_CTR_Y2R_DST]; ++i) {
                        uint64_t at = first + i;
                        unsigned char actual;
                        assert(snx_ctr_phys_read(m, pages[SNX_CTR_Y2R_DST][at / 4096] + (uint32_t)(at % 4096),
                                                 &actual, 1) == SNX_CTR_MEM_OK);
                        if (actual != expected[i]) {
                            fprintf(stderr,
                                    "input %u output %u rotation %u block %u (%ux%u): byte 0x%llx is 0x%02x, the "
                                    "reference's 0x%02x\n",
                                    input, output, rotation, block, c.input_line_width, c.input_lines,
                                    (unsigned long long)i, actual, expected[i]);
                            abort();
                        }
                    }
                    for (unsigned s = 0; s < SNX_CTR_Y2R_STREAMS; ++s) free(pages[s]);
                    ++checked;
                }
    /* refused: a width not of whole tiles, 8x8 output of a partial strip, a unit that splits a pixel */
    SnxCtrY2rConfig bad = {SNX_CTR_Y2R_422_8, SNX_CTR_Y2R_RGBA8, 0, 0, 12, 8, {0}, 0};
    SnxCtrY2rBuffer streams[SNX_CTR_Y2R_STREAMS] = {{0, 0, 96, 0}, {0, 0, 48, 0}, {0, 0, 48, 0}, {0, 0, 384, 0}};
    uint64_t spans[SNX_CTR_Y2R_STREAMS];
    assert(!snx_ctr_y2r_check(&bad, streams, spans));
    bad.input_line_width = 96, bad.input_lines = 12, bad.block_alignment = 1;
    assert(!snx_ctr_y2r_check(&bad, streams, spans));
    bad.input_lines = 8, bad.block_alignment = 0, streams[SNX_CTR_Y2R_DST].transfer_unit = 382;
    assert(!snx_ctr_y2r_check(&bad, streams, spans));
    snx_ctr_vk_y2r_free();
    snx_ctr_vk_free();
    snx_ctr_memory_free(m);
    printf("Y2R: %u conversions on the GPU match the CPU reference\n", checked);
    return 0;
}
