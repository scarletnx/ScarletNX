/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.apache and NOTICE;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
   This fork's changes are under GPL-3.0-or-later, the license of the fork as a whole (LICENSE). */
/* VIC: the video image compositor. Here it copies each enabled slot's NV12 surface (the picture NVDEC wrote there)
   into the game's surface, converted to its format, without scaling: a picture larger than its target rectangle or
   the surface is cropped.
   How the pixels are composed follows Eden's VIC (eden-emulator/mirror, src/video_core/host1x/vic.cpp): a slot is
   read into 10 bit pixels (its Y, U, V samples scaled to 10 bit, its alpha from the slot's planar alpha), the slot's
   color matrix and soft clamp are applied when the config enables them, and the result is written back at 8 bit per
   channel. Without a color matrix the samples are copied as they are (Y, U, V land in the R, G, B channels): VIC
   does no fixed YUV to RGB conversion of its own, the color conversion is whatever matrix the config carries.
   This file resolves the config into a job (the rectangles, the matrices, the planes); the composition itself runs
   as a compute shader (gpu/video/vic.comp, dispatched by gpu/vk/vic.c through the renderer's vic_compose), and on
   the CPU here only when the renderer composes nothing (the null renderer, or a job too large for the GPU). */
#include "nx/gpu/renderer.h"
#include "nx/gpu/video/host1x_int.h"
/* The config struct (its iova in register 0x708) as the game's TVMR and NvDdkVic libraries build it: 0x610 bytes,
   the output surface config at +0x20, 8 slots of 0xB0 bytes from +0x90.
   Surface config u64 (output at +0x20, a slot's at slot+0x40): 0-6 format (0x1F A8B8G8R8, 0x20 A8R8G8B8,
   0x23 X8B8G8R8, 0x43 Y8___U8V8_N420 (NV21: V in each chroma pair's even byte), 0x44 Y8___V8U8_N420 (NV12, U
   there): NVIDIA names the bytes from the high one down), 11-14 block kind (0 pitch linear), 15-18 block height
   log2, 32-45 width-1, 46-59 height-1. Its other u64s name the luma and chroma planes' padded sizes (width-1 in
   bits 0-13, height-1 in bits 14-27).
   The output config (u64 at +0x10) has the background colour, 10 bit each: alpha in bits 6-15, R 16-25, G 26-35,
   B 36-45 (libdrm's tests/tegra/vic40.h, edenprobe's vicref.c). The target rectangle follows, inclusive pixel
   coordinates: left 0-13 and right 16-29 of the u32 at +0x18, top 0-13 and bottom 16-29 of the one at +0x1C. The
   background fills the target rectangle, with or without a slot enabled (libdrm's vic40_fill clears a surface that
   way), the slots compose over it, and the surface outside it is left alone (libdrm's vic-flip fills half a surface
   with a half-height rectangle). The slots' destination rectangles are clipped to it.
   A slot's config u64 at slot+0: bit 0 enable. Its source rectangle is 16.16 fixed point, right and bottom inclusive:
   left bits 0-29 and right 32-61 of the u64 at slot+0x20, top and bottom of the one at +0x28. Its target rectangle,
   u64 at slot+0x30: left 0-13, right 16-29, top 32-45, bottom 48-61, also inclusive.
   The u64 at slot+0x10 has the soft clamp range the converted pixels are clamped to (low 0-9, high 10-19) and the
   planar alpha (32-41), the 10 bit alpha every pixel of the slot gets.
   A slot's color matrix, four u64s at slot+0x60, has three signed 20 bit coefficients per u64 (S12.8), the right
   shift in bits 60-63 of the first u64 and the enable bit at bit 63 of the second (Eden's MatrixStruct).
   A slot's surface 0 (the current frame) is at registers 0x400 + slot * 0x60 (luma) and +4 (chroma), iova >> 8 */
#define VIC_CFG_SIZE  0x610
#define VIC_NUM_SLOTS 8
#define VIC_MAX_ROW   0x4000 /* one row of 14 bit widths: the largest a surface config names */

/* a 10 bit pixel as Eden's Pixel: R, G, B hold the slot's Y, U, V samples scaled to 10 bit, A the planar alpha */
typedef struct {
    u16 r, g, b, a;
} VicPixel;

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

/* a signed 20 bit coefficient, as the config stores the matrix */
static int sext20(u64 v) {
    u32 x = (u32)v & 0xFFFFF;
    return (x & 0x80000) ? (int)x - 0x100000 : (int)x;
}
/* the slot's color matrix (Eden's MatrixStruct): out = (R*c0 + G*c1 + B*c2) >> r_shift, then + c3, then >> 8 */
static void matrix_read(VicSlot* sl, const u8* p) {
    u64 w0 = rd64(p, 0), w1 = rd64(p, 8), w2 = rd64(p, 16), w3 = rd64(p, 24);
    sl->mat_enable = (int)(w1 >> 63);
    sl->mat_shift = (u32)((w0 >> 60) & 15);
    sl->mat[0][0] = sext20(w0);
    sl->mat[1][0] = sext20(w0 >> 20);
    sl->mat[2][0] = sext20(w0 >> 40);
    sl->mat[0][1] = sext20(w1);
    sl->mat[1][1] = sext20(w1 >> 20);
    sl->mat[2][1] = sext20(w1 >> 40);
    sl->mat[0][2] = sext20(w2);
    sl->mat[1][2] = sext20(w2 >> 20);
    sl->mat[2][2] = sext20(w2 >> 40);
    sl->mat[0][3] = sext20(w3);
    sl->mat[1][3] = sext20(w3 >> 20);
    sl->mat[2][3] = sext20(w3 >> 40);
}

/* the row size a plane's padded width names, in bytes: the layout the picture was written with (NVDEC writes
   with the pitch the picture setup names). Zero or smaller than the surface needs: fall back to the surface's
   row, 256 byte aligned when the plane is pitch linear */
static u32 plane_rowbytes(u32 padded, u32 need, int blk) {
    if (padded >= need) return padded;
    return blk ? need : (need + 255) & ~255u;
}
static int plane_map(VicPlane* pl, u64 iova, u32 blk, u32 bh, u32 wbytes, u32 h) {
    pl->blk = blk;
    pl->bh = bh;
    pl->h = h;
    pl->pitch = wbytes; /* the row size as the config names it, no rounding: a width that is not a multiple of
                           16 can name rows the surface width alone would get wrong (the picture comes out
                           green). bl_offset rounds to whole GOBs itself for a block linear plane */
    if (blk) {
        /* whole GOBs: 64 bytes wide (bl_offset rounds the width up the same way), 8 << bh rows high */
        u32 gy = 8u << bh;
        pl->lim = (u64)((wbytes + 63) & ~63u) * ((h + gy - 1) / gy * gy);
    } else {
        pl->lim = (u64)wbytes * h;
    }
    pl->p = iova ? iova_ptr(iova, pl->lim) : NULL;
    if (!pl->p && iova && blk) {
        /* a buffer that ends inside the last block row: the rows past its end are left out */
        pl->lim = (u64)wbytes * h;
        pl->p = iova_ptr(iova, pl->lim);
    }
    return pl->p != NULL;
}

/* ------------------------------------------------------------ the CPU composition (the fallback) */
/* one row of 14 bit widths: the largest a surface config names */
static u8 g_rowy[VIC_MAX_ROW], g_rowc[VIC_MAX_ROW], g_rowo[VIC_MAX_ROW * 4];
/* the slot row as 10 bit pixels */
static VicPixel g_slotrow[VIC_MAX_ROW];

/* n bytes of row y from byte xb, between the plane and buf: block linear moves at most up to the next 16 byte
   boundary at a time, the run bl_offset keeps contiguous */
static void plane_row(const VicPlane* pl, u32 xb, u32 y, u8* buf, u32 n, int write) {
    for (u32 k = 0; k < n;) {
        u32 x = xb + k, run = pl->blk ? 16 - (x & 15) : n - k;
        if (run > n - k) run = n - k;
        u64 off = pl->blk ? bl_offset(x, y, 0, pl->pitch, pl->h, pl->bh, 0) : (u64)y * pl->pitch + x;
        if (off + run <= pl->lim) {
            if (write)
                memcpy(pl->p + off, buf + k, run);
            else
                memcpy(buf + k, pl->p + off, run);
        } else if (!write)
            memset(buf + k, 0, run);
        k += run;
    }
}
/* one slot row of pixels through the matrix and the soft clamp, as Eden's Blend does; without an enabled
   matrix the pixels are left alone (the samples are copied as they are). The alpha stays the planar alpha,
   out of the matrix and the clamp. Eden keeps it out of the matrix but clamps all four channels, in both its
   paths: which of the two the console does is unmeasured */
static void pixels_convert(const VicSlot* sl, VicPixel* px, u32 n) {
    u32 i;
    if (!sl->mat_enable) return;
    int lo = (int)sl->clamp_lo, hi = (int)sl->clamp_hi;
    for (i = 0; i < n; i++) {
        int r = (int)px[i].r * sl->mat[0][0] + (int)px[i].g * sl->mat[0][1] + (int)px[i].b * sl->mat[0][2];
        int g = (int)px[i].r * sl->mat[1][0] + (int)px[i].g * sl->mat[1][1] + (int)px[i].b * sl->mat[1][2];
        int b = (int)px[i].r * sl->mat[2][0] + (int)px[i].g * sl->mat[2][1] + (int)px[i].b * sl->mat[2][2];
        r = ((r >> sl->mat_shift) + sl->mat[0][3]) >> 8;
        g = ((g >> sl->mat_shift) + sl->mat[1][3]) >> 8;
        b = ((b >> sl->mat_shift) + sl->mat[2][3]) >> 8;
        px[i].r = (u16)clampi(r, lo, hi);
        px[i].g = (u16)clampi(g, lo, hi);
        px[i].b = (u16)clampi(b, lo, hi);
    }
}
static int fmt_yuv(u32 fmt) { return fmt == 0x43 || fmt == 0x44; }
/* w 10 bit pixels into row out_y of the output surface from column dx0, in its format */
static void row_write(const VicPixel* px, u32 w, u32 fmt, const VicPlane* oy, const VicPlane* oc, u32 dx0, u32 out_y) {
    if (!fmt_yuv(fmt)) {
        int swap = fmt == 0x20; /* A8R8G8B8 stores R and B the other way around than A8B8G8R8 does */
        /* the 10 bit channels back to 8 bit, as Eden's WriteABGR writes them */
        for (u32 k = 0; k < w; k++) {
            u8* o = g_rowo + k * 4;
            u8 rr = (u8)(px[k].r >> 2), gg = (u8)(px[k].g >> 2);
            u8 bb = (u8)(px[k].b >> 2), aa = (u8)(px[k].a >> 2);
            o[0] = swap ? bb : rr;
            o[1] = gg;
            o[2] = swap ? rr : bb;
            o[3] = aa;
        }
        plane_row(oy, dx0 * 4, out_y, g_rowo, w * 4, 1);
        return;
    }
    /* NV12 or NV21 out: luma from R, chroma from G and B of the pixel pairs */
    u32 wc = (w + 1) & ~1u, vu = fmt == 0x43; /* chroma bytes: whole U, V pairs */
    for (u32 k = 0; k < w; k++) g_rowo[k] = (u8)(px[k].r >> 2);
    plane_row(oy, dx0, out_y, g_rowo, w, 1);
    /* the chroma rows follow the luma pairs; the row keeps the chroma of its even row */
    if (oc->p && !(out_y & 1)) { /* no chroma plane mapped (vic_execute logs it) */
        for (u32 k = 0; k < wc; k += 2) {
            g_rowo[k + vu] = (u8)(px[k].g >> 2);
            g_rowo[k + 1 - vu] = (u8)(px[k].b >> 2);
        }
        plane_row(oc, dx0, out_y / 2, g_rowo, wc, 1);
    }
}
/* the job's target rectangle in its background colour, before the slots compose over it */
static void background_cpu(const VicJob* j) {
    for (u32 k = 0; k < j->tw; k++) {
        g_slotrow[k].r = (u16)j->bg[0];
        g_slotrow[k].g = (u16)j->bg[1];
        g_slotrow[k].b = (u16)j->bg[2];
        g_slotrow[k].a = (u16)j->bg[3];
    }
    for (u32 y = 0; y < j->th; y++) row_write(g_slotrow, j->tw, j->fmt, &j->oy, &j->oc, j->tx0, j->ty0 + y);
}
/* a prepared slot into the output surface: what compose_slot below does per row */
static void compose_cpu(const VicSlot* sl, u32 fmt, const VicPlane* oy, const VicPlane* oc) {
    u32 w = sl->w, h = sl->h, sx0 = sl->sx0, sy0 = sl->sy0, dx0 = sl->dx0, dy0 = sl->dy0;
    for (u32 y = 0; y < h; y++) {
        /* the slot's row as 10 bit pixels, as Eden's ReadProgressiveY8__V8U8_N420 builds them: the samples
           scaled to 10 bit (chroma samples duplicated over the pixel pairs), the planar alpha as the alpha */
        plane_row(&sl->iy, sx0, sy0 + y, g_rowy, w, 0);
        u32 csx = sx0 & ~1u, cend = (sx0 + w + 1) & ~1u;
        plane_row(&sl->ic, csx, (sy0 + y) / 2, g_rowc, cend - csx, 0);
        for (u32 k = 0; k < w; k++) {
            u32 cb = ((sx0 + k) & ~1u) - csx;
            g_slotrow[k].r = (u16)(g_rowy[k] << 2);
            g_slotrow[k].g = (u16)(g_rowc[cb + sl->vu] << 2);
            g_slotrow[k].b = (u16)(g_rowc[cb + 1 - sl->vu] << 2);
            g_slotrow[k].a = (u16)sl->alpha;
        }
        pixels_convert(sl, g_slotrow, w);
        row_write(g_slotrow, w, fmt, oy, oc, dx0, dy0 + y);
    }
}

/* ------------------------------------------------------------ the job */
/* slot s of config cs: its rectangles, color matrix and planes resolved, as Eden's Blend clips them. 1 when the
   slot composes anything */
static int slot_prepare(const u32* r, const u8* cs, int s, u32 sw, u32 sh, u32 fmt, VicSlot* out) {
    const u8* sl = cs + 0x90 + s * 0xB0;
    u64 sc = rd64(sl, 0x40);
    u32 ifmt = (u32)(sc & 0x7F), iblk = (u32)((sc >> 11) & 15), ibh = (u32)((sc >> 15) & 15);
    u32 iw = (u32)((sc >> 32) & 0x3FFF) + 1, ih = (u32)((sc >> 46) & 0x3FFF) + 1;
    if (!fmt_yuv(ifmt)) {
        LOG_LIMIT(4, "video: VIC slot %d input format %x isn't handled (NV12 and NV21 are), slot skipped\n", s, ifmt);
        return 0;
    }
    out->vu = ifmt == 0x43;
    u64 src_x = rd64(sl, 0x20), src_y = rd64(sl, 0x28), dst = rd64(sl, 0x30);
    u32 sx0 = (u32)(src_x & 0x3FFFFFFF) >> 16, sx1 = (u32)((src_x >> 32) & 0x3FFFFFFF) >> 16;
    u32 sy0 = (u32)(src_y & 0x3FFFFFFF) >> 16, sy1 = (u32)((src_y >> 32) & 0x3FFFFFFF) >> 16;
    u32 dx0 = (u32)(dst & 0x3FFF), dx1 = (u32)((dst >> 16) & 0x3FFF);
    u32 dy0 = (u32)((dst >> 32) & 0x3FFF), dy1 = (u32)((dst >> 48) & 0x3FFF);
    if (sx1 < sx0 || sy1 < sy0 || dx1 < dx0 || dy1 < dy0 || sx0 >= iw || sy0 >= ih || dx0 >= sw || dy0 >= sh) {
        LOG_LIMIT(4, "video: VIC slot %d rectangles are empty or outside the surfaces, slot skipped\n", s);
        return 0;
    }
    u32 w = sx1 - sx0 + 1, h = sy1 - sy0 + 1;   /* the source rectangle */
    u32 dw = dx1 - dx0 + 1, dh = dy1 - dy0 + 1; /* the destination rectangle */
    if (w != dw || h != dh)
        LOG_LIMIT(2, "video: VIC slot %d scales %ux%u to %ux%u, which isn't done: the picture is cropped\n", s, w, h,
                  dw, dh);
    if (w > dw) w = dw;
    if (h > dh) h = dh;
    /* the target rectangle of the output config clips the destination, like Eden's Blend, and the picture is
       cropped from the side the crop lands on */
    u32 tgt0 = rd32(cs, 0x18), tgt1 = rd32(cs, 0x1C);
    u32 tx0 = tgt0 & 0x3FFF, tx1 = (tgt0 >> 16) & 0x3FFF, ty0 = tgt1 & 0x3FFF, ty1 = (tgt1 >> 16) & 0x3FFF;
    if (tx1 < tx0 || ty1 < ty0) {
        LOG_LIMIT(4, "video: VIC slot %d target rectangle is empty, slot skipped\n", s);
        return 0;
    }
    if (dx0 > tx1 || dy0 > ty1) {
        LOG_LIMIT(4, "video: VIC slot %d rectangles are outside the target rectangle, slot skipped\n", s);
        return 0;
    }
    if (dx0 < tx0) {
        u32 cut = tx0 - dx0;
        if (cut >= w) {
            LOG_LIMIT(4, "video: VIC slot %d rectangles are outside the target rectangle, slot skipped\n", s);
            return 0;
        }
        sx0 += cut;
        dx0 += cut;
        w -= cut;
    }
    if (dy0 < ty0) {
        u32 cut = ty0 - dy0;
        if (cut >= h) {
            LOG_LIMIT(4, "video: VIC slot %d rectangles are outside the target rectangle, slot skipped\n", s);
            return 0;
        }
        sy0 += cut;
        dy0 += cut;
        h -= cut;
    }
    /* the crop moved the rectangles right and down: they can end up outside the surfaces now, where the clamps
       below would underflow */
    if (sx0 >= iw || sy0 >= ih || dx0 >= sw || dy0 >= sh) {
        LOG_LIMIT(4, "video: VIC slot %d rectangles are empty or outside the surfaces, slot skipped\n", s);
        return 0;
    }
    if (w > tx1 - dx0 + 1) w = tx1 - dx0 + 1;
    if (h > ty1 - dy0 + 1) h = ty1 - dy0 + 1;
    if (w > sw - dx0) w = sw - dx0;
    if (h > sh - dy0) h = sh - dy0;
    if (w > iw - sx0) w = iw - sx0;
    if (h > ih - sy0) h = ih - sy0;
    if (!w || !h) {
        LOG_LIMIT(4, "video: VIC slot %d rectangles are empty or outside the surfaces, slot skipped\n", s);
        return 0;
    }
    /* NV12 chroma is written by pixel pairs; RGBA keeps odd starts, or an odd target would shift */
    if (fmt_yuv(fmt)) {
        sx0 &= ~1u;
        sy0 &= ~1u;
        dx0 &= ~1u;
        dy0 &= ~1u;
    }
    u64 cfg2 = rd64(sl, 0x10);
    out->alpha = (u32)((cfg2 >> 32) & 0x3FF);
    out->clamp_lo = (u32)(cfg2 & 0x3FF);
    out->clamp_hi = (u32)((cfg2 >> 10) & 0x3FF);
    matrix_read(out, sl + 0x60);
    /* the planes' padded sizes (the u64 at slot+0x48: luma width-1 in bits 0-13, height-1 in 14-27, chroma
       width-1 in 32-45 and height-1 in 46-59): the rows are as wide as the config says, not as wide as the
       surface is. The chroma width counts U, V pairs */
    u64 pad = rd64(sl, 0x48);
    u32 ilw = (u32)(pad & 0x3FFF) + 1, ilh = (u32)((pad >> 14) & 0x3FFF) + 1;
    u32 icw = (u32)((pad >> 32) & 0x3FFF) + 1, ich = (u32)((pad >> 46) & 0x3FFF) + 1;
    u32 yrows = ilh > ih ? ilh : ih, crows = ich > (ih + 1) / 2 ? ich : (ih + 1) / 2;
    u64 ly = (u64)r[(0x400 + s * 0x60) / 4] << 8, lc = (u64)r[(0x404 + s * 0x60) / 4] << 8;
    if (!plane_map(&out->iy, ly, iblk, ibh, plane_rowbytes(ilw, iw, iblk), yrows) ||
        !plane_map(&out->ic, lc, iblk, ibh, plane_rowbytes(icw * 2, (iw + 1) & ~1u, iblk), crows)) {
        LOG_LIMIT(4, "video: VIC slot %d input (luma iova %llx, chroma %llx) not mapped, slot skipped\n", s,
                  (unsigned long long)ly, (unsigned long long)lc);
        return 0;
    }
    out->sx0 = sx0;
    out->sy0 = sy0;
    out->dx0 = dx0;
    out->dy0 = dy0;
    out->w = w;
    out->h = h;
    out->on = 1;
    LOG_LIMIT(3, "video: VIC slot %d: NV12 %ux%u %s (block height %u), source %u,%u, target %u,%u, %ux%u copied\n", s,
              iw, ih, iblk ? "block linear" : "pitch linear", 1u << ibh, sx0, sy0, dx0, dy0, w, h);
    return 1;
}

void vic_execute(const u32* r) {
    {
        static u32 nv;
        nv++;
        if (nv % 120 == 0) LOG("video: %u VIC jobs so far\n", nv);
    }
    u64 ly = (u64)r[0x720 / 4] << 8, lc = (u64)r[0x724 / 4] << 8, cfg_iova = (u64)r[0x708 / 4] << 8;
    u8* cs = iova_ptr(cfg_iova, VIC_CFG_SIZE);
    if (!cs) {
        /* without the config the surface's format, size and layout are unknown: writing a guess corrupts it */
        LOG_LIMIT(4, "video: VIC config at iova %llx not mapped, job skipped\n", (unsigned long long)cfg_iova);
        return;
    }
    u64 cfg = rd64(cs, 0x20);
    u32 fmt = (u32)(cfg & 0x7F), blk = (u32)((cfg >> 11) & 15), bh = (u32)((cfg >> 15) & 15);
    u32 sw = (u32)((cfg >> 32) & 0x3FFF) + 1, sh = (u32)((cfg >> 46) & 0x3FFF) + 1;
    /* the surface config's luma and chroma sizes bound what is written, as Eden's WriteABGR and WriteY8__V8U8_N420
       clamp their output to them */
    u64 planes = rd64(cs, 0x28);
    u32 lw = (u32)(planes & 0x3FFF) + 1, lh = (u32)((planes >> 14) & 0x3FFF) + 1;
    u32 ocw = (u32)((planes >> 32) & 0x3FFF) + 1, och = (u32)((planes >> 46) & 0x3FFF) + 1;
    if (lw < sw) sw = lw;
    if (lh < sh) sh = lh;
    /* SNX_VIC_FMT=n overrides the output format the config names, to try another one while debugging */
    static int fmt_env = -1;
    if (fmt_env == -1) {
        const char* e = getenv("SNX_VIC_FMT");
        fmt_env = e ? (int)(strtoul(e, 0, 0) & 0x7F) : -2;
    }
    if (fmt_env >= 0) fmt = (u32)fmt_env;
    if (fmt != 0x1F && fmt != 0x20 && fmt != 0x23 && !fmt_yuv(fmt)) {
        LOG_LIMIT(4, "video: VIC output format %x isn't handled, job skipped\n", fmt);
        return;
    }
    LOG_LIMIT(3, "video: VIC config %016llx: format %x, %s (block height %u), surface %ux%u\n", (unsigned long long)cfg,
              fmt, blk ? "block linear" : "pitch linear", 1u << bh, sw, sh);
    /* the output planes' rows are as wide as their padded widths name (the chroma width counts U, V pairs) */
    u32 bpp = fmt_yuv(fmt) ? 1 : 4;
    u32 oyrows = lh > sh ? lh : sh, ocrows = och > (sh + 1) / 2 ? och : (sh + 1) / 2;
    VicJob job = {0};
    job.fmt = fmt;
    job.sw = sw;
    job.sh = sh;
    if (!plane_map(&job.oy, ly, blk, bh, plane_rowbytes(lw * bpp, sw * bpp, blk), oyrows)) {
        LOG_LIMIT(4, "video: VIC output at iova %llx not mapped\n", (unsigned long long)ly);
        return;
    }
    if (fmt_yuv(fmt) && !plane_map(&job.oc, lc, blk, bh, plane_rowbytes(ocw * 2, (sw + 1) & ~1u, blk), ocrows))
        LOG_LIMIT(4, "video: VIC output chroma at iova %llx not mapped, the picture has no color\n",
                  (unsigned long long)lc);
    /* the background colour over the target rectangle, clipped to the surface (whole pixel pairs in NV12, as the
       slots start) */
    u64 ocfg = rd64(cs, 0x10);
    job.bg[0] = (u32)(ocfg >> 16) & 0x3FF;
    job.bg[1] = (u32)(ocfg >> 26) & 0x3FF;
    job.bg[2] = (u32)(ocfg >> 36) & 0x3FF;
    job.bg[3] = (u32)(ocfg >> 6) & 0x3FF;
    u32 tgt0 = rd32(cs, 0x18), tgt1 = rd32(cs, 0x1C);
    u32 tx0 = tgt0 & 0x3FFF, tx1 = (tgt0 >> 16) & 0x3FFF, ty0 = tgt1 & 0x3FFF, ty1 = (tgt1 >> 16) & 0x3FFF;
    if (tx0 <= tx1 && ty0 <= ty1 && tx0 < sw && ty0 < sh) {
        if (tx1 >= sw) tx1 = sw - 1;
        if (ty1 >= sh) ty1 = sh - 1;
        if (fmt_yuv(fmt)) {
            tx0 &= ~1u;
            ty0 &= ~1u;
        }
        job.tx0 = tx0;
        job.ty0 = ty0;
        job.tw = tx1 - tx0 + 1;
        job.th = ty1 - ty0 + 1;
    }
    /* the slots in order, a later one over an earlier one (no blending) */
    int any = 0;
    for (int s = 0; s < VIC_NUM_SLOTS; s++)
        if (rd64(cs, 0x90 + s * 0xB0) & 1) any |= slot_prepare(r, cs, s, sw, sh, fmt, &job.slots[s]);
    if (!any && !job.tw) {
        LOG_LIMIT(4, "video: VIC job with no slot it could compose and no target rectangle\n");
        return;
    }
    if (g_renderer_up && g_renderer->vic_compose(&job)) return; /* the compute shader composed the picture */
    background_cpu(&job);
    for (int s = 0; s < VIC_NUM_SLOTS; s++)
        if (job.slots[s].on) compose_cpu(&job.slots[s], fmt, &job.oy, &job.oc);
}
