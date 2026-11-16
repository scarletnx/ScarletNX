/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* nvdec.c's VP9 jobs, through nvdec_execute, on libvpx's test vectors (BSD-3-Clause), each image judged by the MD5
   libvpx lists for it: every picture is on its surface when its own job ends (NVDEC writes a picture as it decodes
   it, and a player reads the surface after the job's fence), a hidden one too, which a later frame shows again or
   predicts from, and nothing is left undone when the stream ends. The jobs come from FFmpeg's decoder reading the
   vector (nvdec_vp9_jobs.c); what decodes them is nvdec.c, vp9_rebuild.cpp and FFmpeg with
   .patch/ffmpeg/0001-vp9-nvdec.patch, as in the runtime. The surfaces are linear here: block linear writes are
   nvdec_packets's.

   usage: snx-test-nvdec-vp9-vectors VECTOR MD5-LIST */
#include "nx/gpu/video/nvdec.c"
#include "support/nvdec_ref.h"
#include "nvdec_vp9_jobs.h"
#include <libavutil/md5.h>
#include <stdarg.h>
#include <stdio.h>

/* the guest memory the engine sees, from iova BASE on: the setup, the status, the table, the frame, the surfaces */
#define BASE      ((u64)0x10000000u)
#define AT_SETUP  0x0u
#define AT_STATUS 0x100u
#define AT_PROBS  0x200u
#define AT_BODY   0x1100u
#define POISON    0xCDu

static u8* g_guest;
static u64 g_guest_size;
static unsigned g_warnings;
u8* g_mem; /* g_guest: nvdec.c hands the renderer guest addresses into it */
/* the renderer lays the picture out on the GPU; here the reference does it in guest memory (nvdec_ref.c) */
static int write_ref(const NvdecPic* p) {
    nvdec_write_ref(p);
    return 1;
}
static const Renderer stand_in = {.name = "stand-in", .nvdec_write = write_ref};
const Renderer* g_renderer = &stand_in;
int g_renderer_up = 1;

u8* iova_ptr(u64 addr, u64 size) {
    if (!size || addr < BASE || addr - BASE > g_guest_size || size > g_guest_size - (addr - BASE)) return NULL;
    return g_guest + (addr - BASE);
}
u64 bl_offset(u32 x, u32 y, u32 z, u32 pitch, u32 height, u32 bh, u32 bd) {
    (void)z;
    (void)height;
    (void)bh;
    (void)bd;
    return (u64)y * pitch + x;
}
void logf_(int level, const char* fmt, ...) {
    va_list ap;
    (void)level;
    if (++g_warnings > 20) return;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

static void md5_hex(const u8* m, char* out) {
    int i;
    for (i = 0; i < 16; i++) snprintf(out + 2 * i, 3, "%02x", m[i]);
}

/* the picture of width by height on a surface, NV12 at the vector's stride: its MD5 as libvpx takes it, the Y, U
   and V planes in turn */
static void surface_md5(const SnxVp9Vector* v, u64 luma, u64 chroma, u32 w, u32 h, u8 out[16]) {
    struct AVMD5* m = av_md5_alloc();
    u8 line[MAX_DIM / 2];
    const u8* y = g_guest + (luma - BASE);
    const u8* c = g_guest + (chroma - BASE);
    u32 r, x, p;
    av_md5_init(m);
    for (r = 0; r < h; r++) av_md5_update(m, y + (size_t)r * v->stride, w);
    for (p = 0; p < 2; p++)
        for (r = 0; r < (h + 1) / 2; r++) {
            for (x = 0; x < (w + 1) / 2; x++) line[x] = c[(size_t)r * v->stride + 2 * x + p];
            av_md5_update(m, line, (w + 1) / 2);
        }
    av_md5_final(m, out);
    av_free(m);
}

static int run(const char* path, const char* md5_path) {
    static u32 regs[0x1000 / 4];
    SnxVp9Vector v;
    NvdecCtx* d;
    char err[256], want[33], got[33];
    u64 luma_size, chroma_size, surface_size, surfaces;
    u32 k, i, at_fence = 0, hidden_written = 0, wrong = 0, max_body = 0;
    int failed;
    if (snx_vp9j_load(path, md5_path, &v, err, sizeof err)) {
        fprintf(stderr, "%s: %s\n", path, err);
        return 1;
    }
    for (k = 0; k < v.count; k++)
        if (v.jobs[k].body_len > max_body) max_body = v.jobs[k].body_len;
    luma_size = (u64)v.stride * v.rows;
    chroma_size = luma_size / 2;
    surface_size = luma_size + chroma_size;
    surfaces = AT_BODY + ((u64)max_body + 0xFF) / 0x100 * 0x100;
    g_guest_size = surfaces + surface_size * v.surfaces;
    if (!(g_guest = calloc(1, (size_t)g_guest_size)) || !(d = nvdec_new())) {
        fprintf(stderr, "%s: out of memory\n", path);
        return 1;
    }
    g_mem = g_guest;
    memset(regs, 0, sizeof regs);
    regs[0x200 / 4] = NVDEC_CODEC_VP9;
    regs[0x404 / 4] = (u32)((BASE + AT_SETUP) >> 8);
    regs[0x408 / 4] = (u32)((BASE + AT_BODY) >> 8);
    regs[0x424 / 4] = (u32)((BASE + AT_STATUS) >> 8);
    regs[0x5C0 / 4] = (u32)((BASE + AT_PROBS) >> 8);
    for (k = 0; k < v.count; k++) {
        const SnxVp9Job* j = &v.jobs[k];
        u64 luma = BASE + surfaces + surface_size * j->surface;
        const u8* expect = j->md5;
        u8 md5[16];
        if (!j->existing) {
            u8* st = g_guest + AT_STATUS;
            u32 mbs = ((j->width + 15) / 16) * ((j->height + 15) / 16);
            memcpy(g_guest + AT_SETUP, j->setup, sizeof j->setup);
            memcpy(g_guest + AT_PROBS, j->probs, sizeof j->probs);
            memcpy(g_guest + AT_BODY, v.data + j->body, j->body_len);
            for (i = 0; i < 4; i++) {
                u64 at = BASE + surfaces + surface_size * (i < 3 ? j->refs[i] : j->surface);
                regs[0x430 / 4 + i] = (u32)(at >> 8);
                regs[0x474 / 4 + i] = (u32)((at + luma_size) >> 8);
            }
            /* what is on the surface is this job's or nothing */
            memset(g_guest + (luma - BASE), POISON, (size_t)surface_size);
            memset(st, 0xFF, 16);
            nvdec_execute(d, regs);
            if (rd32(st, 0) != mbs || rd32(st, 4) || rd32(st, 12)) {
                fprintf(stderr, "%s: frame %u's job failed (status %u %u %u, %u macroblocks)\n", path, k, rd32(st, 0),
                        rd32(st, 4), rd32(st, 12), mbs);
                wrong++;
            }
        }
        surface_md5(&v, luma, luma + luma_size, j->width, j->height, md5);
        if (!memcmp(md5, expect, 16)) {
            if (j->image >= 0) at_fence++;
            if (!j->shown) hidden_written++;
            continue;
        }
        if (++wrong <= 8) {
            md5_hex(expect, want);
            md5_hex(md5, got);
            fprintf(stderr, "%s: frame %u (%s%s, %ux%u, surface %u): %s, %s\n", path, k,
                    j->existing ? "shown again"
                    : j->shown  ? "shown"
                                : "hidden",
                    j->image >= 0 ? "" : ", no image of its own", j->width, j->height, j->surface,
                    j->image >= 0 ? "the image libvpx lists is" : "the reference decoder's picture is", want);
            fprintf(stderr, "%*s  the surface holds %s\n", (int)strlen(path), "", got);
        }
    }
    printf("%s: %u of %u images right at their job's fence, %u of %u hidden pictures written; %u frames (%u shown "
           "with no image of their own), %u surfaces of %ux%u\n",
           path, at_fence, v.images, hidden_written, v.hidden, v.count, v.shown_dropped, v.surfaces, v.stride, v.rows);
    failed = wrong || at_fence != v.images || hidden_written != v.hidden;
    nvdec_free(d);
    free(g_guest);
    snx_vp9j_free(&v);
    return failed;
}

int main(int argc, char** argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s VECTOR MD5-LIST\n", argv[0]);
        return 2;
    }
    return run(argv[1], argv[2]);
}
