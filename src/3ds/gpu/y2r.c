/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   The Y2R engine's conversion on the CPU: the reference vk/y2r.c's shader is tested against. Ported from Azahar
   core/hw/y2r.cpp and the coefficients of core/hle/service/cam/y2r_u.cpp (GPL-2.0-or-later, Copyright 2015-2024
   Citra Emulator Project / Azahar Emulator Project, azahar-emu/azahar a4b335e). The engine takes the picture in
   strips of 8 lines: each 8x8 tile converted to RGB, rotated (each strip on its own, as the hardware does) and laid
   out linearly or in the PICA's 8x8 Morton order, then written out. */
#include "3ds/gpu/y2r.h"
#include "3ds/gpu/transfer.h"
#include <string.h>

const int16_t snx_ctr_y2r_standard[4][8] = {
    {0x100, 0x166, 0xB6, 0x58, 0x1C5, -0x166F, 0x10EE, -0x1C5B}, /* ITU_Rec601 */
    {0x100, 0x193, 0x77, 0x2F, 0x1DB, -0x1933, 0xA7C, -0x1D51},  /* ITU_Rec709 */
    {0x12A, 0x198, 0xD0, 0x64, 0x204, -0x1BDE, 0x10F2, -0x229B}, /* ITU_Rec601_Scaling */
    {0x12A, 0x1CA, 0x88, 0x36, 0x21C, -0x1F04, 0x99C, -0x2421},  /* ITU_Rec709_Scaling */
};
/* the bytes a sample takes in its stream (16-bit inputs carry 8 bits in the first byte) and a pixel in the output */
static unsigned sample_bytes(const SnxCtrY2rConfig* c) {
    return c->input_format == SNX_CTR_Y2R_422_16 || c->input_format == SNX_CTR_Y2R_420_16 ? 2u : 1u;
}
static unsigned pixel_format(const SnxCtrY2rConfig* c) {
    static const unsigned formats[4] = {0, 1, 3, 2}; /* RGBA8, RGB8, RGB5A1, RGB565 as the PICA numbers them */
    return formats[c->output_format];
}
/* the samples stream s takes in a strip of rows lines */
static uint32_t strip_samples(const SnxCtrY2rConfig* c, unsigned s, uint32_t rows) {
    uint32_t pixels = rows * c->input_line_width;
    if (s == SNX_CTR_Y2R_Y) return c->input_format == SNX_CTR_Y2R_YUYV ? pixels * 2 : pixels;
    if (c->input_format == SNX_CTR_Y2R_YUYV) return 0;
    return c->input_format == SNX_CTR_Y2R_420_8 || c->input_format == SNX_CTR_Y2R_420_16 ? pixels / 4 : pixels / 2;
}
bool snx_ctr_y2r_check(const SnxCtrY2rConfig* c, const SnxCtrY2rBuffer streams[SNX_CTR_Y2R_STREAMS],
                       uint64_t spans[SNX_CTR_Y2R_STREAMS]) {
    uint32_t w = c->input_line_width, lines = c->input_lines;
    if (c->input_format > SNX_CTR_Y2R_YUYV || c->output_format > SNX_CTR_Y2R_RGB565 || c->rotation > 3 ||
        c->block_alignment > 1 || !w || w > 1024 || w % 8 || !lines || lines > 1024 ||
        (c->block_alignment && lines % 8))
        return false;
    unsigned bpp = snx_ctr_pixel_bytes(pixel_format(c));
    for (unsigned s = 0; s < SNX_CTR_Y2R_STREAMS; ++s) {
        const SnxCtrY2rBuffer* b = &streams[s];
        spans[s] = 0;
        if (s != SNX_CTR_Y2R_DST && !strip_samples(c, s, 8)) continue;
        /* each unit holds whole samples or pixels, and every strip moves whole units */
        unsigned size = s == SNX_CTR_Y2R_DST ? bpp : s == SNX_CTR_Y2R_Y && c->input_format == SNX_CTR_Y2R_YUYV
                                                         ? 1u
                                                         : sample_bytes(c);
        if (!b->transfer_unit || b->transfer_unit % size) return false;
        uint32_t per_unit = b->transfer_unit / size, units = 0;
        for (uint32_t y = 0; y < lines; y += 8) {
            uint32_t rows = lines - y < 8 ? lines - y : 8;
            uint32_t n = s == SNX_CTR_Y2R_DST ? rows * w : strip_samples(c, s, rows);
            if (n % per_unit) return false;
            units += n / per_unit;
        }
        spans[s] = (uint64_t)units * (b->transfer_unit + b->gap) - b->gap;
    }
    return true;
}
/* the byte at offset i of a stream's data, as its DMA reads or writes it */
static uint64_t stream_offset(const SnxCtrY2rBuffer* b, uint64_t i) {
    return i / b->transfer_unit * (b->transfer_unit + b->gap) + i % b->transfer_unit;
}
void snx_ctr_y2r_convert(const SnxCtrY2rConfig* c, const SnxCtrY2rBuffer streams[SNX_CTR_Y2R_STREAMS],
                         const unsigned char* const src[3], unsigned char* dst) {
    uint32_t w = c->input_line_width, tiles = w / 8;
    unsigned n = sample_bytes(c), bpp = snx_ctr_pixel_bytes(pixel_format(c));
    uint64_t taken[3] = {0, 0, 0}, sent = 0; /* samples read from each input, pixels written */
    static uint32_t tile[128][64], strip[1024 * 8];
    static uint8_t in_y[1024 * 8 * 2], in_u[1024 * 4], in_v[1024 * 4];
    for (uint32_t y0 = 0; y0 < c->input_lines; y0 += 8) {
        uint32_t rows = c->input_lines - y0 < 8 ? c->input_lines - y0 : 8;
        /* ReceiveData: each input's samples for the strip, the first byte of each */
        uint8_t* inputs[3] = {in_y, in_u, in_v};
        for (unsigned s = 0; s < 3; ++s) {
            uint32_t count = strip_samples(c, s, rows);
            unsigned size = s == SNX_CTR_Y2R_Y && c->input_format == SNX_CTR_Y2R_YUYV ? 1u : n;
            for (uint32_t i = 0; i < count; ++i)
                inputs[s][i] = src[s][stream_offset(&streams[s], (taken[s] + i) * size)];
            taken[s] += count;
        }
        /* ConvertYUVToRGB into tiles, each pixel R, G, B from the most significant byte */
        for (uint32_t y = 0; y < rows; ++y)
            for (uint32_t x = 0; x < w; ++x) {
                int32_t Y, U, V;
                if (c->input_format == SNX_CTR_Y2R_YUYV) {
                    Y = in_y[(y * w + x) * 2];
                    U = in_y[(y * w + x / 2 * 2) * 2 + 1];
                    V = in_y[(y * w + x / 2 * 2) * 2 + 3];
                } else if (c->input_format == SNX_CTR_Y2R_420_8 || c->input_format == SNX_CTR_Y2R_420_16) {
                    Y = in_y[y * w + x];
                    U = in_u[(y / 2 * w + x) / 2];
                    V = in_v[(y / 2 * w + x) / 2];
                } else {
                    Y = in_y[y * w + x];
                    U = in_u[(y * w + x) / 2];
                    V = in_v[(y * w + x) / 2];
                }
                const int16_t* k = c->coefficients;
                int32_t cy = k[0] * Y, r = cy + k[1] * V, g = cy - k[2] * V - k[3] * U, b = cy + k[4] * U;
                r = (r >> 3) + k[5] + 0x18;
                g = (g >> 3) + k[6] + 0x18;
                b = (b >> 3) + k[7] + 0x18;
                r >>= 5, g >>= 5, b >>= 5;
                r = r < 0 ? 0 : r > 255 ? 255 : r;
                g = g < 0 ? 0 : g > 255 ? 255 : g;
                b = b < 0 ? 0 : b > 255 ? 255 : b;
                tile[x / 8][y * 8 + x % 8] = (uint32_t)r << 24 | (uint32_t)g << 16 | (uint32_t)b << 8;
            }
        /* each tile rotated into place and laid out in the strip */
        uint32_t* out = strip;
        for (uint32_t i = 0; i < tiles; ++i) {
            uint32_t rotated[64];
            const uint32_t* t = tile[c->rotation >= 2 ? tiles - 1 - i : i];
            int k = 0;
            for (int j = 0; j < (int)rows * 8; ++j) {
                int from;
                switch (c->rotation) {
                    case 0: from = j; break;
                    case 1: from = ((int)rows - 1 - j % (int)rows) * 8 + j / (int)rows; break;
                    case 2: from = (int)rows * 8 - 1 - j; break;
                    default: from = j % (int)rows * 8 + 7 - j / (int)rows; break;
                }
                /* the tile's pixel k (row k / 8) at its place in the PICA's Morton order */
                uint32_t at =
                    c->block_alignment ? snx_ctr_morton_offset((uint32_t)k % 8, (uint32_t)k / 8, 1) : (uint32_t)k;
                rotated[at] = t[from];
                ++k;
            }
            if (c->block_alignment) { /* whole strips only: every slot was written */
                memcpy(out, rotated, sizeof rotated);
                out += 64;
            } else {
                uint32_t width = c->rotation & 1 ? 8 : w, stride = c->rotation & 1 ? 8 * rows : 8;
                for (uint32_t y = 0; y < rows; ++y)
                    for (uint32_t x = 0; x < 8; ++x) out[y * width + x] = rotated[y * 8 + x];
                out += stride;
            }
        }
        /* SendData: the strip's pixels in the output format, the alpha the configuration gives */
        for (uint32_t p = 0; p < rows * w; ++p) {
            uint8_t rgba[4] = {(uint8_t)(strip[p] >> 24), (uint8_t)(strip[p] >> 16), (uint8_t)(strip[p] >> 8),
                               (uint8_t)c->alpha};
            unsigned char bytes[4];
            snx_ctr_pixel_encode(pixel_format(c), rgba, bytes);
            for (unsigned i = 0; i < bpp; ++i)
                dst[stream_offset(&streams[SNX_CTR_Y2R_DST], (sent + p) * bpp + i)] = bytes[i];
        }
        sent += rows * w;
    }
}
