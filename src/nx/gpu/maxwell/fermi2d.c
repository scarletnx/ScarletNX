/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported from Eden (src/video_core/engines/fermi_2d.cpp, src/video_core/engines/fermi_2d.h):
   SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
   SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project */
/* The 2D engine (FERMI_TWOD_A, 0x902D): a blit from one surface to another, scaled, started by the last word of the
   source origin (FERMI2D_PIXELS_FROM_MEMORY_SRC_Y0_INT). gpu.c's puller hands it its methods (fermi2d_method). The
   blit's configuration is worked out from the registers as Eden's Fermi2D::Blit does (F2dConfig), and the renderer
   does the blit on the GPU: as an image blit when the source is a render target (r3d_blit), else from memory
   (blit_mem, vk/blit.c), with the source pixels the blit samples worked out here, nearest sampled. Register names
   are fermi2d_regs.h's */
#include "nx/gpu/maxwell/engines_int.h"
#include "nx/gpu/maxwell/fermi2d_regs.h"
#include "nx/gpu/maxwell/gob.h"

/* A blit's configuration from the pixels_from_memory registers (Eden's Fermi2D::Config): the destination rectangle,
   and the source's origin and its step per destination pixel, in source pixels */
typedef struct {
    int dst_x0, dst_y0, dst_width, dst_height;
    double src_x0, src_y0, du_dx, dv_dy;
} F2dConfig;

/* a Surface's registers from b (FERMI2D_DST or FERMI2D_SRC) */
static void f2d_surface(const u32* r, u32 b, Surf2D* s) {
    s->format = r[b + FERMI2D_SURFACE_FORMAT];
    s->linear = r[b + FERMI2D_SURFACE_LINEAR] & 1;
    s->bh = fermi2d_block_height(r[b + FERMI2D_SURFACE_BLOCK_SIZE]);
    s->depth = r[b + FERMI2D_SURFACE_DEPTH];
    s->layer = r[b + FERMI2D_SURFACE_LAYER];
    s->pitch = r[b + FERMI2D_SURFACE_PITCH];
    s->width = r[b + FERMI2D_SURFACE_WIDTH];
    s->height = r[b + FERMI2D_SURFACE_HEIGHT];
    s->addr = gpu_packed_va(r[b + FERMI2D_SURFACE_ADDR_UPPER], r[b + FERMI2D_SURFACE_ADDR_LOWER]);
}
/* A surface's slice as a plane: a 3D surface's layer is its z, with the block depth of its block size register
   (blk); a 2D array's layer is a whole number of blocks further: its row bytes rounded up to a GOB's width, times its
   rows rounded up to a block's height (8 << bh rows), per layer */
static GpuDmaSide f2d_plane(const Surf2D* s, u32 blk, u32 bpp) {
    GpuDmaSide pl;
    memset(&pl, 0, sizeof pl);
    pl.addr = s->addr;
    pl.bl = s->linear != FERMI2D_MEMORY_LAYOUT_PITCH;
    pl.pitch = s->pitch;
    pl.wb = s->width * bpp; /* a row's bytes; pitch linear rows are pitch apart */
    pl.h = s->height;
    pl.bh = s->bh;
    if (pl.bl) {
        if (s->depth > 1) {
            pl.z = s->layer;
            pl.bd = fermi2d_block_depth(blk);
        } else if (s->layer) {
            u32 block_shift = MAXWELL_GOB_SIZE_Y_SHIFT + s->bh;
            u64 row_bytes = ((u64)pl.wb + MAXWELL_GOB_SIZE_X - 1) & ~(u64)(MAXWELL_GOB_SIZE_X - 1);
            u64 rows = (((u64)s->height + (1u << block_shift) - 1) >> block_shift) << block_shift;
            pl.addr += row_bytes * rows * s->layer;
        }
    }
    return pl;
}
/* the source pixel a destination pixel's center falls on, clamped to the surface's n pixels */
static u32 f2d_sample(double at, u32 n) { return at <= 0 ? 0 : at >= (double)n ? n - 1 : (u32)at; }

/* a float format's components: their count, and 4 or 2 bytes each. 0 when it isn't one. The X formats' fourth
   component is padding */
static int f2d_float_format(u32 f, u32* n, u32* size) {
    *n = f == 0xC0 || f == 0xC3 || f == 0xCA || f == 0xCE ? 4
         : f == 0xCB || f == 0xDE                         ? 2
         : f == 0xE5 || f == 0xF2                         ? 1
                                                          : 0;
    *size = f == 0xC0 || f == 0xC3 || f == 0xCB || f == 0xE5 ? 4 : 2;
    return *n != 0;
}

/* a color format's pixel as the blit's conversion reads and writes it (pixel_format.h), 0 for one it doesn't take
   (integers, the packed ones it has no layout for). The names are the components from the most significant: A8B8G8R8
   is R in the first byte, A8R8G8B8 B in it, and R5G6B5 R in the top five bits of its word */
static u32 f2d_pixel_format(u32 f) {
    switch (f) {
        case 0xC0: return GPU_PIXEL_DESC(2, 4, GPU_PIXEL_FLOAT);                 /* R32G32B32A32_FLOAT */
        case 0xC3: return GPU_PIXEL_DESC(2, 4, GPU_PIXEL_FLOAT) | GPU_PIXEL_X;   /* R32G32B32X32_FLOAT */
        case 0xC6: return GPU_PIXEL_DESC(1, 4, GPU_PIXEL_UNORM);                 /* R16G16B16A16_UNORM */
        case 0xC7: return GPU_PIXEL_DESC(1, 4, GPU_PIXEL_SNORM);                 /* R16G16B16A16_SNORM */
        case 0xCA: return GPU_PIXEL_DESC(1, 4, GPU_PIXEL_FLOAT);                 /* R16G16B16A16_FLOAT */
        case 0xCB: return GPU_PIXEL_DESC(2, 2, GPU_PIXEL_FLOAT);                 /* R32G32_FLOAT */
        case 0xCE: return GPU_PIXEL_DESC(1, 4, GPU_PIXEL_FLOAT) | GPU_PIXEL_X;   /* R16G16B16X16_FLOAT */
        case 0xCF: return GPU_PIXEL_DESC(0, 4, GPU_PIXEL_UNORM) | GPU_PIXEL_BGR; /* A8R8G8B8_UNORM */
        case 0xD0: return GPU_PIXEL_DESC(0, 4, GPU_PIXEL_UNORM) | GPU_PIXEL_BGR | GPU_PIXEL_SRGB;
        case 0xD1: return GPU_PIXEL_DESC(2, 4, GPU_PIXEL_1010102); /* A2B10G10R10_UNORM */
        case 0xD5: return GPU_PIXEL_DESC(0, 4, GPU_PIXEL_UNORM);   /* A8B8G8R8_UNORM */
        case 0xD6: return GPU_PIXEL_DESC(0, 4, GPU_PIXEL_UNORM) | GPU_PIXEL_SRGB;
        case 0xD7: return GPU_PIXEL_DESC(0, 4, GPU_PIXEL_SNORM);
        case 0xDA: return GPU_PIXEL_DESC(1, 2, GPU_PIXEL_UNORM); /* R16G16_UNORM */
        case 0xDB: return GPU_PIXEL_DESC(1, 2, GPU_PIXEL_SNORM);
        case 0xDE: return GPU_PIXEL_DESC(1, 2, GPU_PIXEL_FLOAT);
        case 0xE0: return GPU_PIXEL_DESC(2, 3, GPU_PIXEL_111110);                              /* B10G11R11_FLOAT */
        case 0xE5: return GPU_PIXEL_DESC(2, 1, GPU_PIXEL_FLOAT);                               /* R32_FLOAT */
        case 0xE6: return GPU_PIXEL_DESC(0, 4, GPU_PIXEL_UNORM) | GPU_PIXEL_BGR | GPU_PIXEL_X; /* X8R8G8B8_UNORM */
        case 0xE7: return GPU_PIXEL_DESC(0, 4, GPU_PIXEL_UNORM) | GPU_PIXEL_BGR | GPU_PIXEL_X | GPU_PIXEL_SRGB;
        case 0xE8: return GPU_PIXEL_DESC(1, 3, GPU_PIXEL_565) | GPU_PIXEL_BGR; /* R5G6B5_UNORM */
        case 0xEA: return GPU_PIXEL_DESC(0, 2, GPU_PIXEL_UNORM);               /* R8G8_UNORM */
        case 0xEB: return GPU_PIXEL_DESC(0, 2, GPU_PIXEL_SNORM);
        case 0xEE: return GPU_PIXEL_DESC(1, 1, GPU_PIXEL_UNORM); /* R16_UNORM */
        case 0xEF: return GPU_PIXEL_DESC(1, 1, GPU_PIXEL_SNORM);
        case 0xF2: return GPU_PIXEL_DESC(1, 1, GPU_PIXEL_FLOAT); /* R16_FLOAT */
        case 0xF3: return GPU_PIXEL_DESC(0, 1, GPU_PIXEL_UNORM); /* R8_UNORM */
        case 0xF4: return GPU_PIXEL_DESC(0, 1, GPU_PIXEL_SNORM);
        case 0xFA: return GPU_PIXEL_DESC(0, 4, GPU_PIXEL_UNORM) | GPU_PIXEL_X; /* X8B8G8R8_UNORM */
        case 0xFB: return GPU_PIXEL_DESC(0, 4, GPU_PIXEL_UNORM) | GPU_PIXEL_X | GPU_PIXEL_SRGB;
    }
    return 0;
}

/* The blit from memory, which the renderer does on the GPU (its destination kept there): which source column and row
   each destination column and row takes, nearest sampled, worked out here. Between two float formats each component is
   converted, as Eden's software blitter does (one game converts the RGBA32F tables its CPU fills into the RGBA16F
   texture its lighting reads, at each scene's start); between other color formats the conversion's shader build reads
   and writes each as f2d_pixel_format says (RGBA8 to BGRA8 swaps red and blue). Formats it doesn't take copy their
   bytes when their pixels are as wide, else nothing is done */
static void f2d_blit_memory(const u32* r, const Surf2D* s, const Surf2D* d, const F2dConfig* c) {
    u32 bs = gpu_color_bpp(s->format), bd = gpu_color_bpp(d->format), n, i, y0, y1, x0, x1, sn, ss, dn, ds, fmt = 0;
    u32 sp = f2d_pixel_format(s->format), dp = f2d_pixel_format(d->format);
    GpuBlit b;
    u32* tab;
    if (s->format != d->format && f2d_float_format(s->format, &sn, &ss) && f2d_float_format(d->format, &dn, &ds))
        fmt = GPU_BLIT_FLOAT(sn, ss, dn, ds, s->format == 0xC3 || s->format == 0xCE);
    else if (s->format != d->format && sp && dp)
        fmt = GPU_BLIT_CONVERT(sp, dp);
    else if (bs != bd) {
        WARN_LIMIT(4, "gpu: a 2D blit from format %x to %x isn't done\n", s->format, d->format);
        return;
    } else if (s->format != d->format)
        LOG_LIMIT(4, "gpu: a 2D blit from format %x to %x copies the bytes\n", s->format, d->format);
    if (!s->width || !s->height) return;
    /* the destination rectangle clipped to the destination surface */
    x0 = c->dst_x0 > 0 ? (u32)c->dst_x0 : 0;
    y0 = c->dst_y0 > 0 ? (u32)c->dst_y0 : 0;
    x1 = (i64)c->dst_x0 + c->dst_width < (i64)d->width ? (u32)((i64)c->dst_x0 + c->dst_width) : d->width;
    y1 = (i64)c->dst_y0 + c->dst_height < (i64)d->height ? (u32)((i64)c->dst_y0 + c->dst_height) : d->height;
    if (x0 >= x1 || y0 >= y1) return;
    n = x1 - x0;
    tab = (u32*)malloc(sizeof *tab * ((size_t)n + (y1 - y0)));
    if (!tab) {
        WARN_LIMIT(1, "gpu: no memory for a 2D blit %u pixels wide\n", n);
        return;
    }
    for (i = 0; i < n; i++)
        tab[i] = f2d_sample(c->src_x0 + ((double)(x0 + i - c->dst_x0) + 0.5) * c->du_dx, s->width);
    for (i = y0; i < y1; i++)
        tab[n + i - y0] = f2d_sample(c->src_y0 + ((double)(i - c->dst_y0) + 0.5) * c->dv_dy, s->height);
    b.src = f2d_plane(s, r[FERMI2D_SRC + FERMI2D_SURFACE_BLOCK_SIZE], bs);
    b.dst = f2d_plane(d, r[FERMI2D_DST + FERMI2D_SURFACE_BLOCK_SIZE], bd);
    b.bpp = bs;
    b.x0 = x0, b.x1 = x1, b.y0 = y0, b.y1 = y1;
    b.scol = tab;
    b.srow = tab + n;
    b.fmt = fmt;
    if (!g_renderer_up || !g_renderer->blit_mem)
        WARN_LIMIT(4, "gpu: a 2D blit of %ux%u pixels to %llx before the renderer started: nothing is copied\n", n,
                   y1 - y0, (unsigned long long)b.dst.addr);
    else if (!g_renderer->blit_mem(&b))
        WARN_LIMIT(4, "gpu: the GPU could not do a 2D blit of %ux%u pixels to %llx: nothing is copied\n", n, y1 - y0,
                   (unsigned long long)b.dst.addr);
    else if (g_renderer->rt_reload)
        g_renderer->rt_reload(d->addr);
    free(tab);
}
/* a signed 32.32 fixed point value from its two registers, the fraction's word first */
static double f2d_fixed(const u32* r, u32 lo) {
    return (double)(i64)(((u64)r[lo + 1] << 32) | r[lo]) / FERMI2D_FIXED_ONE;
}
/* The blit's configuration (Eden's Fermi2D::Blit). With the CORNER origin a destination pixel samples the start of
   its step and with CENTER its middle, as the renderer samples: a corner's start is half a step back (Eden takes off
   the step's integer half, (du_dx >> 33) << 32; the exact half is taken here) */
static void f2d_config(const u32* r, F2dConfig* c) {
    c->dst_x0 = (int)r[FERMI2D_PIXELS_FROM_MEMORY_DST_X0];
    c->dst_y0 = (int)r[FERMI2D_PIXELS_FROM_MEMORY_DST_Y0];
    c->dst_width = (int)r[FERMI2D_PIXELS_FROM_MEMORY_DST_WIDTH];
    c->dst_height = (int)r[FERMI2D_PIXELS_FROM_MEMORY_DST_HEIGHT];
    c->du_dx = f2d_fixed(r, FERMI2D_PIXELS_FROM_MEMORY_DU_DX);
    c->dv_dy = f2d_fixed(r, FERMI2D_PIXELS_FROM_MEMORY_DV_DY);
    c->src_x0 = f2d_fixed(r, FERMI2D_PIXELS_FROM_MEMORY_SRC_X0);
    c->src_y0 = f2d_fixed(r, FERMI2D_PIXELS_FROM_MEMORY_SRC_Y0);
    if (fermi2d_sample_mode_origin(r[FERMI2D_PIXELS_FROM_MEMORY_SAMPLE_MODE]) == FERMI2D_ORIGIN_CORNER) {
        c->src_x0 -= c->du_dx / 2;
        c->src_y0 -= c->dv_dy / 2;
    }
}
/* The blit (Eden's Fermi2D::Blit): the renderer's image blit when it takes it, else the blit from memory. Every
   operation is done as SRC_COPY */
static void f2d_blit(GpuChannel* ch) {
    const u32* r = ch->r2d;
    u32 op = r[FERMI2D_OPERATION];
    Surf2D s, d;
    F2dConfig c;
    f2d_config(r, &c);
    f2d_surface(r, FERMI2D_DST, &d);
    f2d_surface(r, FERMI2D_SRC, &s);
    if (c.dst_width <= 0 || c.dst_height <= 0) return;
    if (op != FERMI2D_OPERATION_SRC_COPY && op != FERMI2D_OPERATION_SRC_COPY_AND)
        LOG_LIMIT(4, "gpu: a 2D blit's operation %u is done as a copy\n", op);
    if (r3d_blit(&s, &d, c.dst_x0, c.dst_y0, c.dst_width, c.dst_height, c.src_x0, c.src_y0, c.du_dx, c.dv_dy)) return;
    f2d_blit_memory(r, &s, &d, &c);
}
/* a 2D engine method (Eden's Fermi2D::CallMethod): stored, and the blit started by its last register */
void fermi2d_method(GpuChannel* ch, u32 m, u32 v) {
    if (m >= FERMI2D_NUM_REGS) return;
    ch->r2d[m] = v;
    if (m == FERMI2D_PIXELS_FROM_MEMORY_SRC_Y0_INT) f2d_blit(ch);
}
