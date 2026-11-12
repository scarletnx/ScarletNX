/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported from Azahar video_core/renderer_software/sw_blitter.cpp (GPL-2.0-or-later, Copyright 2023 Citra Emulator
   Project), the pixel codecs of common/color.h and the Morton order of video_core/utils.h (GPL-2.0-or-later,
   Copyright 2014 Citra Emulator Project), at azahar-emu/azahar a4b335e. Their loops and arithmetic are kept,
   including the 2x/4x box filters' integer truncation; each access is checked against the staged buffers here. */
#include "3ds/gpu/transfer.h"
#include <string.h>

unsigned snx_ctr_pixel_bytes(unsigned format) {
    switch (format) {
        case SNX_CTR_PIX_RGBA8: return 4;
        case SNX_CTR_PIX_RGB8: return 3;
        case SNX_CTR_PIX_RGB565:
        case SNX_CTR_PIX_RGB5A1:
        case SNX_CTR_PIX_RGBA4: return 2;
        default: return 0;
    }
}

void snx_ctr_fill(unsigned char* dst, size_t size, uint32_t value, uint32_t control) {
    if (control & SNX_CTR_FILL_24BIT) {
        /* the blitter writes three bytes per step to the end (and past it by up to two bytes when the span is no
           multiple of 3); here the last value stops at the end */
        for (size_t i = 0; i < size; ++i) dst[i] = (unsigned char)(value >> ((i % 3) * 8));
    } else if (control & SNX_CTR_FILL_32BIT) {
        for (size_t i = 0; i + 4 <= size; i += 4) {
            dst[i] = (unsigned char)value;
            dst[i + 1] = (unsigned char)(value >> 8);
            dst[i + 2] = (unsigned char)(value >> 16);
            dst[i + 3] = (unsigned char)(value >> 24);
        }
    } else {
        for (size_t i = 0; i + 2 <= size; i += 2) {
            dst[i] = (unsigned char)value;
            dst[i + 1] = (unsigned char)(value >> 8);
        }
    }
}

uint32_t snx_ctr_morton_offset(uint32_t x, uint32_t y, uint32_t bytes_per_pixel) {
    static const uint32_t xlut[8] = {0x00, 0x01, 0x04, 0x05, 0x10, 0x11, 0x14, 0x15};
    static const uint32_t ylut[8] = {0x00, 0x02, 0x08, 0x0a, 0x20, 0x22, 0x28, 0x2a};
    uint32_t coarse_x = x & ~7u;
    return (xlut[x % 8] + ylut[y % 8] + coarse_x * 8) * bytes_per_pixel;
}

/* ---- pixel codecs: components are stored most significant first (common/color.h) ---- */
typedef struct {
    unsigned r, g, b, a;
} Color;
static unsigned c5to8(unsigned v) { return (v << 3) | (v >> 2); }
static unsigned c6to8(unsigned v) { return (v << 2) | (v >> 4); }
static unsigned c4to8(unsigned v) { return (v << 4) | v; }
static Color decode(unsigned format, const unsigned char* p) {
    Color c = {0, 0, 0, 0};
    unsigned v = (unsigned)p[0] | ((unsigned)p[1] << 8);
    switch (format) {
        case SNX_CTR_PIX_RGBA8: c.r = p[3], c.g = p[2], c.b = p[1], c.a = p[0]; break;
        case SNX_CTR_PIX_RGB8: c.r = p[2], c.g = p[1], c.b = p[0], c.a = 255; break;
        case SNX_CTR_PIX_RGB565:
            c.r = c5to8((v >> 11) & 31), c.g = c6to8((v >> 5) & 63), c.b = c5to8(v & 31), c.a = 255;
            break;
        case SNX_CTR_PIX_RGB5A1:
            c.r = c5to8((v >> 11) & 31), c.g = c5to8((v >> 6) & 31), c.b = c5to8((v >> 1) & 31), c.a = (v & 1) * 255;
            break;
        case SNX_CTR_PIX_RGBA4:
            c.r = c4to8((v >> 12) & 15), c.g = c4to8((v >> 8) & 15), c.b = c4to8((v >> 4) & 15), c.a = c4to8(v & 15);
            break;
        default: break;
    }
    return c;
}
static void encode(unsigned format, Color c, unsigned char* p) {
    unsigned v;
    switch (format) {
        case SNX_CTR_PIX_RGBA8:
            p[3] = (unsigned char)c.r, p[2] = (unsigned char)c.g, p[1] = (unsigned char)c.b, p[0] = (unsigned char)c.a;
            return;
        case SNX_CTR_PIX_RGB8: p[2] = (unsigned char)c.r, p[1] = (unsigned char)c.g, p[0] = (unsigned char)c.b; return;
        case SNX_CTR_PIX_RGB565: v = ((c.r >> 3) << 11) | ((c.g >> 2) << 5) | (c.b >> 3); break;
        case SNX_CTR_PIX_RGB5A1: v = ((c.r >> 3) << 11) | ((c.g >> 3) << 6) | ((c.b >> 3) << 1) | (c.a >> 7); break;
        case SNX_CTR_PIX_RGBA4: v = ((c.r >> 4) << 12) | ((c.g >> 4) << 8) | ((c.b >> 4) << 4) | (c.a >> 4); break;
        default: return;
    }
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
}
void snx_ctr_pixel_decode(unsigned format, const unsigned char* bytes, uint8_t rgba[4]) {
    Color c = decode(format, bytes);
    rgba[0] = (uint8_t)c.r, rgba[1] = (uint8_t)c.g, rgba[2] = (uint8_t)c.b, rgba[3] = (uint8_t)c.a;
}
void snx_ctr_pixel_encode(unsigned format, const uint8_t rgba[4], unsigned char* bytes) {
    Color c = {rgba[0], rgba[1], rgba[2], rgba[3]};
    encode(format, c, bytes);
}

/* ---- display transfer ---- */

typedef struct {
    uint32_t in_w, in_h, out_w, out_h; /* out_* after scaling */
    unsigned in_fmt, out_fmt, in_bpp, out_bpp, scaling, hs, vs, flip, linear, no_swizzle;
    int64_t skew; /* bytes the output starts after its address: FLIP with CROP */
} Display;

static SnxCtrXferResult display_setup(const SnxCtrDisplayTransfer* t, Display* d) {
    uint32_t out_w = t->output_size & 0xffff, out_h = t->output_size >> 16;
    *d = (Display){0};
    d->in_w = t->input_size & 0xffff;
    d->in_h = t->input_size >> 16;
    if (!d->in_w || !d->in_h || !out_w || !out_h) return SNX_CTR_XFER_EMPTY;
    d->in_fmt = (t->flags >> SNX_CTR_XFER_IN_FORMAT) & 7;
    d->out_fmt = (t->flags >> SNX_CTR_XFER_OUT_FORMAT) & 7;
    d->in_bpp = snx_ctr_pixel_bytes(d->in_fmt);
    d->out_bpp = snx_ctr_pixel_bytes(d->out_fmt);
    d->scaling = (t->flags >> SNX_CTR_XFER_SCALE_SHIFT) & 3;
    d->flip = !!(t->flags & SNX_CTR_XFER_FLIP);
    d->linear = !!(t->flags & SNX_CTR_XFER_LINEAR);
    d->no_swizzle = !!(t->flags & SNX_CTR_XFER_NO_SWIZZLE);
    /* scaling mode 3 and scaled linear input are unimplemented in the reference too; a format the hardware does
       not define has no size */
    if (d->scaling > 2 || (d->linear && d->scaling) || !d->in_bpp || !d->out_bpp) return SNX_CTR_XFER_UNSUPPORTED;
    d->hs = d->scaling != 0;
    d->vs = d->scaling == 2;
    /* "flip_vertically alongside crop_input_lines produces skewed output on hardware": the output address moves */
    if (d->flip && (t->flags & SNX_CTR_XFER_CROP))
        d->skew = ((int64_t)d->in_w - (int64_t)out_w) * (int64_t)(out_h - 1) * d->out_bpp;
    if (d->skew < 0) return SNX_CTR_XFER_UNSUPPORTED; /* the reference would wrap its pointer before the output */
    d->out_w = out_w >> d->hs;
    d->out_h = out_h >> d->vs;
    if (!d->out_w || !d->out_h) return SNX_CTR_XFER_EMPTY;
    return SNX_CTR_XFER_OK;
}

static void display_offsets(const Display* d, uint32_t x, uint32_t y, uint64_t* src, uint64_t* dst) {
    uint32_t input_x = x << d->hs, input_y = y << d->vs, output_y = d->flip ? d->out_h - y - 1 : y;
    if (d->linear) {
        *src = ((uint64_t)input_x + (uint64_t)input_y * d->in_w) * d->in_bpp;
        if (!d->no_swizzle) /* linear in, tiled out */
            *dst = snx_ctr_morton_offset(x, output_y, d->out_bpp) + (uint64_t)(output_y & ~7u) * d->out_w * d->out_bpp;
        else
            *dst = ((uint64_t)x + (uint64_t)output_y * d->out_w) * d->out_bpp;
    } else {
        *src = snx_ctr_morton_offset(input_x, input_y, d->in_bpp) + (uint64_t)(input_y & ~7u) * d->in_w * d->in_bpp;
        if (!d->no_swizzle) /* tiled in, linear out */
            *dst = ((uint64_t)x + (uint64_t)output_y * d->out_w) * d->out_bpp;
        else
            *dst = snx_ctr_morton_offset(x, output_y, d->out_bpp) + (uint64_t)(output_y & ~7u) * d->out_w * d->out_bpp;
    }
    *dst += (uint64_t)d->skew;
}

/* the source bytes one output pixel reads: itself, and the 1 or 3 next ones of the box filter */
static uint64_t source_reach(const Display* d) {
    return (uint64_t)d->in_bpp * (d->scaling == 2 ? 4 : d->scaling ? 2 : 1);
}

static bool display_spans(const SnxCtrDisplayTransfer* t, size_t* input_bytes, size_t* output_bytes) {
    Display d;
    uint64_t in_end = 0, out_end = 0, src, dst;
    if (display_setup(t, &d) != SNX_CTR_XFER_OK) return false;
    for (uint32_t y = 0; y < d.out_h; ++y)
        for (uint32_t x = 0; x < d.out_w; ++x) {
            display_offsets(&d, x, y, &src, &dst);
            if (src + source_reach(&d) > in_end) in_end = src + source_reach(&d);
            if (dst + d.out_bpp > out_end) out_end = dst + d.out_bpp;
        }
    if (in_end > SIZE_MAX || out_end > SIZE_MAX) return false;
    *input_bytes = (size_t)in_end;
    *output_bytes = (size_t)out_end;
    return true;
}
/* The spans walk every output pixel (a frame's 96000 for a 400x240 screen), and depend on the transfer's sizes and
   flags alone: a game's few transfers, made each frame, are remembered */
#define SPANS_KEPT 8
bool snx_ctr_display_spans(const SnxCtrDisplayTransfer* t, size_t* input_bytes, size_t* output_bytes) {
    static _Thread_local struct {
        SnxCtrDisplayTransfer key;
        size_t input, output;
        bool ok, used;
    } kept[SPANS_KEPT];
    static _Thread_local unsigned next;
    for (unsigned i = 0; i < SPANS_KEPT; ++i)
        if (kept[i].used && kept[i].key.input_size == t->input_size && kept[i].key.output_size == t->output_size &&
            kept[i].key.flags == t->flags) {
            if (!kept[i].ok) return false;
            *input_bytes = kept[i].input;
            *output_bytes = kept[i].output;
            return true;
        }
    size_t input = 0, output = 0;
    bool ok = display_spans(t, &input, &output);
    unsigned i = next++ % SPANS_KEPT;
    kept[i].key = *t;
    kept[i].input = input;
    kept[i].output = output;
    kept[i].ok = ok;
    kept[i].used = true;
    if (ok) {
        *input_bytes = input;
        *output_bytes = output;
    }
    return ok;
}

SnxCtrXferResult snx_ctr_display_transfer(const SnxCtrDisplayTransfer* t, const unsigned char* in, size_t in_size,
                                          unsigned char* out, size_t out_size) {
    Display d;
    uint64_t src, dst;
    SnxCtrXferResult r = display_setup(t, &d);
    if (r != SNX_CTR_XFER_OK) return r;
    for (uint32_t y = 0; y < d.out_h; ++y)
        for (uint32_t x = 0; x < d.out_w; ++x) {
            Color c1;
            display_offsets(&d, x, y, &src, &dst);
            if (src + source_reach(&d) > in_size || dst + d.out_bpp > out_size) return SNX_CTR_XFER_BOUNDS;
            Color c = decode(d.in_fmt, in + src);
            if (d.scaling == 1) {
                c1 = decode(d.in_fmt, in + src + d.in_bpp);
                c.r = (c.r + c1.r) / 2, c.g = (c.g + c1.g) / 2, c.b = (c.b + c1.b) / 2, c.a = (c.a + c1.a) / 2;
            } else if (d.scaling == 2) {
                c1 = decode(d.in_fmt, in + src + d.in_bpp);
                Color c2 = decode(d.in_fmt, in + src + 2 * d.in_bpp);
                Color c3 = decode(d.in_fmt, in + src + 3 * d.in_bpp);
                c.r = (c.r + c1.r + c2.r + c3.r) / 4, c.g = (c.g + c1.g + c2.g + c3.g) / 4;
                c.b = (c.b + c1.b + c2.b + c3.b) / 4, c.a = (c.a + c1.a + c2.a + c3.a) / 4;
            }
            encode(d.out_fmt, c, out + dst);
        }
    return SNX_CTR_XFER_OK;
}

/* ---- texture copy ---- */

typedef struct {
    uint32_t remaining, in_width, in_gap, out_width, out_gap;
} Copy;

static SnxCtrXferResult copy_setup(const SnxCtrTextureCopy* t, Copy* c) {
    c->remaining = t->size & ~15u;
    if (!c->remaining) return SNX_CTR_XFER_EMPTY; /* "zero size. Real hardware freezes on this." */
    c->in_gap = (t->input_line >> 16) * 16;
    c->out_gap = (t->output_line >> 16) * 16;
    /* a zero gap means contiguous data even with a zero width */
    c->in_width = c->in_gap ? (t->input_line & 0xffff) * 16 : c->remaining;
    c->out_width = c->out_gap ? (t->output_line & 0xffff) * 16 : c->remaining;
    if (!c->in_width || !c->out_width) return SNX_CTR_XFER_EMPTY; /* "Real hardware freezes on this." */
    return SNX_CTR_XFER_OK;
}

/* the reference's pointer walk; with in == NULL it only measures how far each side reaches */
static SnxCtrXferResult copy_walk(const Copy* setup, const unsigned char* in, size_t in_size, unsigned char* out,
                                  size_t out_size, uint64_t* in_end, uint64_t* out_end) {
    Copy c = *setup;
    uint64_t src = 0, dst = 0;
    uint32_t left_in = c.in_width, left_out = c.out_width;
    *in_end = *out_end = 0;
    while (c.remaining) {
        uint32_t n = left_in < left_out ? left_in : left_out;
        if (c.remaining < n) n = c.remaining;
        if (in) {
            if (src + n > in_size || dst + n > out_size) return SNX_CTR_XFER_BOUNDS;
            memcpy(out + dst, in + src, n);
        }
        src += n, dst += n;
        if (src > *in_end) *in_end = src;
        if (dst > *out_end) *out_end = dst;
        left_in -= n, left_out -= n, c.remaining -= n;
        if (!left_in) left_in = c.in_width, src += c.in_gap;
        if (!left_out) left_out = c.out_width, dst += c.out_gap;
    }
    return SNX_CTR_XFER_OK;
}

bool snx_ctr_texture_spans(const SnxCtrTextureCopy* t, size_t* input_bytes, size_t* output_bytes) {
    Copy c;
    uint64_t in_end, out_end;
    if (copy_setup(t, &c) != SNX_CTR_XFER_OK) return false;
    copy_walk(&c, NULL, 0, NULL, 0, &in_end, &out_end);
    *input_bytes = (size_t)in_end;
    *output_bytes = (size_t)out_end;
    return true;
}

SnxCtrXferResult snx_ctr_texture_copy(const SnxCtrTextureCopy* t, const unsigned char* in, size_t in_size,
                                      unsigned char* out, size_t out_size) {
    Copy c;
    uint64_t in_end, out_end;
    SnxCtrXferResult r = copy_setup(t, &c);
    if (r != SNX_CTR_XFER_OK) return r;
    return copy_walk(&c, in, in_size, out, out_size, &in_end, &out_end);
}
