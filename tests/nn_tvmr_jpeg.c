/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The movie library's native JPEG decoder and encoder (src/nx/nn/tvmr_jpeg.c), their handlers called as the hooks
   call them, on a stand-in CPU over guest memory with surfaces in nvmap handles of its own: a picture encoded from
   planar 4:2:0 and from NV12 and decoded back to each YUV layout and to RGBA, the JPEG's segments and tables, the
   SOI that GetBits leaves out, the fences, the statuses of what NVJPG refuses, the 32-bit layouts, movie 7's
   differences, a frame lost to a save state's load, and tvmr_jpeg.hook's table by movie version */
#include "nx/hle/services/services_int.h"
#include <stdarg.h>
#include <stdio.h>

#include "nx/nn/tvmr_jpeg.c"
#include "nx/hle/hle.c"
#include "hle/hook/hooks.hook.inc" /* tvmr_jpeg.hook's table and adapters */

u8* g_mem;
int g_ounce;
u64 g_as_size = AS_MAX;
OwnState g_own;
static unsigned test_checks;

static void test_fail(const char* expression, int line) {
    fprintf(stderr, "nn_tvmr_jpeg.c:%d: check failed: %s\n", line, expression);
    exit(1);
}
#define CHECK(expr)                                                                                                    \
    do {                                                                                                               \
        test_checks++;                                                                                                 \
        if (!(expr)) test_fail(#expr, __LINE__);                                                                       \
    } while (0)

/* guest memory: [GUEST_LO, GUEST_HI) is mapped */
#define GUEST_LO 0x8000u
#define GUEST_HI 0x400000u
static u8 test_mem[GUEST_HI];
#define OBJECTS   0x10000u /* kmem_alloc's */
#define SCRATCH   0x20000u /* surfaces, fences, buffer lists */
#define DEST      0x40000u /* GetBits' destination */
#define STREAM    0x300000u
#define HANDLE_A  1u /* nvmap handles: their memory */
#define HANDLE_B  2u
#define MEM_A     0x100000u
#define MEM_B     0x200000u
#define MEM_SIZE  0x100000u

void pm_init(PMutex* m) { (void)m; }
void pm_lock(PMutex* m) { (void)m; }
void pm_unlock(PMutex* m) { (void)m; }
void mem_query(u64 addr, MemBlock* b) {
    memset(b, 0, sizeof *b);
    b->addr = addr < GUEST_LO ? 0 : addr < GUEST_HI ? GUEST_LO : GUEST_HI;
    b->size = addr < GUEST_LO ? GUEST_LO : addr < GUEST_HI ? GUEST_HI - GUEST_LO : AS_SIZE - GUEST_HI;
    b->perm = addr >= GUEST_LO && addr < GUEST_HI ? PERM_RW : PERM_NONE;
}
int snx_audio_guest_ok(u64 addr, u64 size, u32 perm) {
    u64 end = addr + size;
    MemBlock b;
    if (end < addr || end > AS_SIZE) return 0;
    while (addr < end) {
        mem_query(addr, &b);
        if ((b.perm & perm) != perm || b.addr + b.size <= addr) return 0;
        addr = b.addr + b.size;
    }
    return 1;
}
static char test_last_warning[256];
void logf_(int level, const char* format, ...) {
    va_list ap;
    if (level) return;
    va_start(ap, format);
    vsnprintf(test_last_warning, sizeof test_last_warning, format, ap);
    va_end(ap);
}
u64 kmem_alloc(u64 size, u32 state, u32 perm) {
    (void)state, (void)perm;
    CHECK(size <= SCRATCH - OBJECTS);
    return OBJECTS;
}
int nvmap_get(u32 h, u64* addr, u32* size) {
    *addr = h == HANDLE_A ? MEM_A : h == HANDLE_B ? MEM_B : 0;
    *size = *addr ? MEM_SIZE : 0;
    return *addr != 0;
}
void own_acquire(u64 addr, u64 size) { (void)addr, (void)size; }
static u32 test_waits, test_wait_id, test_wait_value;
int syncpt_wait(u32 id, u32 thresh, i64 timeout_ns) {
    CHECK(timeout_ns < 0);
    test_waits++;
    test_wait_id = id;
    test_wait_value = thresh;
    return 1;
}

static CPU cpu;
static CPU* const c = &cpu;
static u8* G(u64 a) { return test_mem + a; }
static void put_ptr(const Abi* a, u64 at, u64 v) {
    if (a->ptr == 8)
        snx_st_le64(G(at), v);
    else
        snx_st_le32(G(at), (u32)v);
}

/* ---- surfaces */
typedef struct {
    u32 handle, offset, pitch, width, height;
} PlaneDesc;
/* a TVMRVideoSurface at s of type, w x h, with n planes (each a plane object and an NvRmSurface after it) */
static void surface(const Abi* a, u64 s, u32 type, u32 w, u32 h, const PlaneDesc* p, u32 n) {
    memset(G(s), 0, 0x300);
    snx_st_le32(G(s), type);
    snx_st_le32(G(s + 4), w);
    snx_st_le32(G(s + 8), h);
    for (u32 i = 0; i < n; i++) {
        u64 obj = s + 0x40 + i * 0x18, rm = s + 0x100 + i * 0x58;
        put_ptr(a, s + a->planes + (u64)i * a->ptr, obj);
        snx_st_le32(G(obj), p[i].pitch);
        put_ptr(a, obj + a->plane_surf, rm);
        snx_st_le32(G(rm + RM_WIDTH), p[i].width);
        snx_st_le32(G(rm + RM_HEIGHT), p[i].height);
        snx_st_le32(G(rm + RM_LAYOUT), RM_PITCH_LINEAR);
        snx_st_le32(G(rm + RM_PITCH), p[i].pitch);
        snx_st_le32(G(rm + RM_HMEM), p[i].handle);
        snx_st_le32(G(rm + RM_OFFSET), p[i].offset);
    }
}
/* Render's TVMROutputSurface at s: type, and one plane */
static void output_surface(const Abi* a, u64 s, u32 type, const PlaneDesc* p) {
    surface(a, s, type, p->width, p->height, p, 0);
    u64 obj = s + 0x40, rm = s + 0x100;
    put_ptr(a, s + a->out_plane, obj);
    put_ptr(a, obj + a->plane_surf, rm);
    snx_st_le32(G(rm + RM_WIDTH), p->width);
    snx_st_le32(G(rm + RM_HEIGHT), p->height);
    snx_st_le32(G(rm + RM_LAYOUT), RM_PITCH_LINEAR);
    snx_st_le32(G(rm + RM_PITCH), p->pitch);
    snx_st_le32(G(rm + RM_HMEM), p->handle);
    snx_st_le32(G(rm + RM_OFFSET), p->offset);
}

#define W 64u
#define H 48u
/* the picture: a gradient with a block in it */
static u8 pic_y(u32 x, u32 y) { return (u8)(x >= 16 && x < 32 && y >= 16 && y < 32 ? 200 : 16 + 3 * x + y); }
static u8 pic_cb(u32 x, u32 y) { return (u8)(96 + 2 * x); }
static u8 pic_cr(u32 x, u32 y) { return (u8)(160 - 2 * y); }

/* the picture into handle A as planar 4:2:0 (pitch 80, 48 and 48) or NV12 (pitch 80 and 80) */
static void source(const Abi* a, u64 s, int nv12) {
    PlaneDesc p[3] = {{HANDLE_A, 0, 80, W, H}, {HANDLE_A, 0x2000, nv12 ? 80 : 48, W / 2, H / 2},
                      {HANDLE_A, 0x3000, 48, W / 2, H / 2}};
    memset(G(MEM_A), 0, 0x4000);
    for (u32 y = 0; y < H; y++)
        for (u32 x = 0; x < W; x++) G(MEM_A)[y * 80 + x] = pic_y(x, y);
    for (u32 y = 0; y < H / 2; y++)
        for (u32 x = 0; x < W / 2; x++)
            if (nv12) {
                G(MEM_A + 0x2000)[y * 80 + 2 * x] = pic_cb(x, y);
                G(MEM_A + 0x2000)[y * 80 + 2 * x + 1] = pic_cr(x, y);
            } else {
                G(MEM_A + 0x2000)[y * 48 + x] = pic_cb(x, y);
                G(MEM_A + 0x3000)[y * 48 + x] = pic_cr(x, y);
            }
    surface(a, s, nv12 ? 6 : 0, W, H, p, nv12 ? 2 : 3);
}

/* a JPEG's segment of marker m: its payload, NULL for none; up to its first SOS */
static const u8* segment(const u8* j, u32 n, u32 m, u32* len) {
    for (u32 i = 2; i + 4 <= n && j[i] == 0xFF;) {
        u32 seg = 2u + snx_ld_be16(j + i + 2);
        if (j[i + 1] == m) {
            *len = seg - 4;
            return j + i + 4;
        }
        if (j[i + 1] == 0xDA) break;
        i += seg;
    }
    return NULL;
}
static const u8 zigzag[64] = {0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,  12, 19, 26, 33, 40, 48,
                              41, 34, 27, 20, 13, 6,  7,  14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23,
                              30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};

static u32 jpeg_n;
static u8 jpeg[0x10000];
/* encodes the source surface at s with quality q through an encoder of 64-bit movie 4: the JPEG into jpeg */
static void encode_source(u64 s, u8 q) {
    u64 enc = tvmrjpeg_encoder_create(c, 1, 0x10000), out = SCRATCH + 0xf00, bytes = SCRATCH + 0xf10;
    CHECK(enc >= OBJECTS && enc < SCRATCH);
    snx_st_le64(G(out), 0x1234);
    CHECK(tvmrjpeg_encoder_feed_frame64(c, enc, s, 0, out, q) == ST_OK);
    CHECK(snx_ld_le32(G(out)) == NO_FENCE && snx_ld_le32(G(out + 4)) == 0); /* done: nothing to wait for */
    CHECK(tvmrjpeg_encoder_bits_available(c, enc, bytes, 1, ~0u) == ST_OK);
    jpeg_n = snx_ld_le32(G(bytes));
    CHECK(jpeg_n > 600 && jpeg_n < 0x10000);
    snx_st_le32(G(bytes), 0);
    CHECK(tvmrjpeg_encoder_get_bits64(c, enc, bytes, DEST, 0) == ST_OK);
    CHECK(snx_ld_le32(G(bytes)) == jpeg_n);
    memcpy(jpeg, G(DEST), jpeg_n);
    CHECK(tvmrjpeg_encoder_bits_available(c, enc, bytes, 0, 0) == ST_NONE); /* read */
    tvmrjpeg_encoder_destroy(c, enc);
    CHECK(!obj_of(enc, TVMRJPEG_ENCODER));
}
/* the JPEG as one bitstream buffer: its list at SCRATCH + 0xe00 */
static u64 stream(const Abi* a, const u8* j, u32 n) {
    u64 list = SCRATCH + 0xe00;
    memcpy(G(STREAM), j, n);
    put_ptr(a, list, STREAM);
    snx_st_le32(G(list + a->ptr), n);
    return list;
}

static int close_to(int a, int b, int tolerance) { return a - b <= tolerance && b - a <= tolerance; }

static void test_encode(void) {
    u32 len;
    u8 q[2][64];
    source(&g_abi64, SCRATCH, 0);
    encode_source(SCRATCH, 90);
    CHECK(jpeg[0] == 0xFF && jpeg[1] == 0xD8 && jpeg[jpeg_n - 2] == 0xFF && jpeg[jpeg_n - 1] == 0xD9);
    for (u32 m = 0xE0; m <= 0xEF; m++) CHECK(!segment(jpeg, jpeg_n, m, &len)); /* no APPn: NVJPG writes none */
    CHECK(!segment(jpeg, jpeg_n, 0xFE, &len));
    const u8* sof = segment(jpeg, jpeg_n, 0xC0, &len);
    CHECK(sof && sof[0] == 8 && snx_ld_be16(sof + 1) == H && snx_ld_be16(sof + 3) == W && sof[5] == 3);
    CHECK(sof[7] == 0x22 && sof[10] == 0x11 && sof[13] == 0x11); /* 4:2:0 */
    /* the tables of quality 90, but the DC quantizers, which are FFmpeg's 8 */
    quality_tables(q, 90);
    const u8* dqt = segment(jpeg, jpeg_n, 0xDB, &len);
    CHECK(dqt && len >= 130);
    u32 at = 0, seen = 0;
    while (at + 65 <= len) {
        u32 t = dqt[at] & 15;
        CHECK(t < 2 && !(dqt[at] >> 4));
        CHECK(dqt[at + 1] == 8);
        for (u32 k = 1; k < 64; k++) CHECK(dqt[at + 1 + k] == q[t][zigzag[k]]);
        seen |= 1u << t;
        at += 65;
    }
    CHECK(seen == 3);

    /* GetBits without the SOI, as the caller asks for the main picture; a second feed with the one buffer taken */
    u64 enc = tvmrjpeg_encoder_create(c, 1, 0x10000), bytes = SCRATCH + 0xf10;
    CHECK(tvmrjpeg_encoder_feed_frame64(c, enc, SCRATCH, 0, 0, 90) == ST_OK);
    CHECK(tvmrjpeg_encoder_feed_frame64(c, enc, SCRATCH, 0, 0, 90) == ST_FULL);
    CHECK(tvmrjpeg_encoder_get_bits64(c, enc, bytes, 0, 0) == ST_FULL); /* no destination */
    CHECK(tvmrjpeg_encoder_get_bits64(c, enc, bytes, DEST, 1) == ST_OK);
    CHECK(snx_ld_le32(G(bytes)) == jpeg_n - 2 && !memcmp(G(DEST), jpeg + 2, jpeg_n - 2));
    /* GetBitsEx over two buffers, the first too small for it all; then buffers too small, which keep the frame */
    CHECK(tvmrjpeg_encoder_feed_frame64(c, enc, SCRATCH, 0, 0, 90) == ST_OK);
    u64 list = SCRATCH + 0xe80;
    snx_st_le64(G(list), DEST + 0x8000);
    snx_st_le32(G(list + 8), 100);
    snx_st_le64(G(list + 16), DEST + 0x9000);
    snx_st_le32(G(list + 24), 50);
    CHECK(tvmrjpeg_encoder_get_bits_ex64(c, enc, bytes, 2, list, 0) == ST_FULL);
    CHECK(tvmrjpeg_encoder_bits_available(c, enc, bytes, 0, 0) == ST_OK); /* still there */
    snx_st_le32(G(list + 24), 0x2000);
    CHECK(tvmrjpeg_encoder_get_bits_ex64(c, enc, bytes, 2, list, 0) == ST_OK);
    CHECK(!memcmp(G(DEST + 0x8000), jpeg, 100) && !memcmp(G(DEST + 0x9000), jpeg + 100, jpeg_n - 100));
    /* the caller's own tables, and a frame lost to a save state's load (the host's bytes are another frame's) */
    memset(G(SCRATCH + 0xc00), 1, 128);
    CHECK(tvmrjpeg_encoder_feed_frame_quant64(c, enc, SCRATCH, 0, 0, SCRATCH + 0xc00, 0) == ST_BAD);
    CHECK(tvmrjpeg_encoder_feed_frame_quant64(c, enc, SCRATCH, 0, 0, SCRATCH + 0xc00, SCRATCH + 0xc40) == ST_OK);
    g_host.tvmr_jpeg.serial[obj_index(obj_of(enc, TVMRJPEG_ENCODER))][0]++;
    CHECK(tvmrjpeg_encoder_get_bits64(c, enc, bytes, DEST, 0) == ST_FULL);
    CHECK(tvmrjpeg_encoder_bits_available(c, enc, bytes, 0, 0) == ST_NONE);
    /* a type the encoder doesn't take, and movie 7's GetBits with no destination, which drops the frame */
    snx_st_le32(G(SCRATCH), 1);
    CHECK(tvmrjpeg_enc_feed_frame64(c, enc, SCRATCH, 0, 0) == ST_BAD);
    snx_st_le32(G(SCRATCH), 0);
    CHECK(tvmrjpeg_enc_feed_frame64(c, enc, SCRATCH, 0, 0) == ST_OK);
    CHECK(tvmrjpeg_encoder_get_bits64_7(c, enc, bytes, 0, 0) == ST_OK);
    CHECK(tvmrjpeg_encoder_bits_available(c, enc, bytes, 0, 0) == ST_NONE);
    /* the fences before: each waited for, but the one with no syncpoint */
    u64 fences = SCRATCH + 0xd00;
    snx_st_le64(G(fences), SCRATCH + 0xd40);
    snx_st_le64(G(fences + 8), SCRATCH + 0xd48);
    snx_st_le64(G(fences + 16), 0);
    snx_st_le32(G(SCRATCH + 0xd40), 5);
    snx_st_le32(G(SCRATCH + 0xd44), 9);
    snx_st_le32(G(SCRATCH + 0xd48), NO_FENCE);
    test_waits = 0;
    CHECK(tvmrjpeg_enc_feed_frame64(c, enc, SCRATCH, fences, 0) == ST_OK);
    CHECK(test_waits == 1 && test_wait_id == 5 && test_wait_value == 9);
    tvmrjpeg_encoder_destroy(c, enc);
    CHECK(tvmrjpeg_encoder_create(c, 1, 0x24f) == 0); /* no room for the header */

    /* NV12 in: the same picture */
    source(&g_abi64, SCRATCH, 1);
    u32 planar_n = jpeg_n;
    u8 planar[0x10000];
    memcpy(planar, jpeg, jpeg_n);
    encode_source(SCRATCH, 90);
    CHECK(jpeg_n == planar_n && !memcmp(jpeg, planar, jpeg_n));
}

/* Render as each layout's hook calls it: the 64-bit ones with the post fence and the flags on the stack */
static u32 render_any(const Abi* a, int movie7, u64 dec, u64 dst, u64 list, u64 post) {
    u64 sp = SCRATCH + 0xf80;
    c->x[31] = sp;
    snx_st_le64(G(sp), post);
    snx_st_le32(G(sp + 8), 0);
    return a == &g_abi32 ? tvmrjpeg_decoder_render32(c, dec, dst, 0, 0, 0, 1, list, 0, post, 0)
           : movie7      ? tvmrjpeg_decoder_render64_7(c, dec, dst, 0, 0, 0, 1, list, 0)
                         : tvmrjpeg_decoder_render64(c, dec, dst, 0, 0, 0, 1, list, 0);
}

/* decodes jpeg into planar 4:2:0, NV12, YUYV and RGBA surfaces in handle B, against the source */
static void test_decode(const Abi* a, int movie7) {
    u64 info = SCRATCH + 0xa00, dst = SCRATCH + 0x400, list = stream(a, jpeg, jpeg_n), post = SCRATCH + 0xf20;
    u32 (*get_info)(CPU*, u64, u32, u64) = a == &g_abi32 ? tvmrjpeg_get_info32
                                          : movie7         ? tvmrjpeg_get_info64_7
                                                           : tvmrjpeg_get_info64;
    u32 (*yuv)(CPU*, u64, u64, u8, u32, u64, u64, u64, u32) = a == &g_abi32 ? tvmrjpeg_decoder_render_yuv32
                                                             : movie7         ? tvmrjpeg_decoder_render_yuv64_7
                                                                              : tvmrjpeg_decoder_render_yuv64;
    memset(G(info), 0xee, 0x108);
    CHECK(get_info(c, info, 1, list) == ST_OK);
    CHECK(snx_ld_le16(G(info)) == W && snx_ld_le16(G(info + 2)) == H && G(info)[4] == 0);
    CHECK(G(info)[6] == (movie7 ? 0 : 0xee)); /* movie 7's info is 0x108 bytes */
    u64 dec = tvmrjpeg_decoder_create(c, W, H, 0x10000, 0);
    CHECK(dec && snx_ld_le16(G(dec)) == W && snx_ld_le16(G(dec + 2)) == H && snx_ld_le32(G(dec + 4)) == 0x10000);
    CHECK(tvmrjpeg_decoder_create(c, W, H, 0x10000, 1) == 0);

    /* planar 4:2:0 */
    PlaneDesc p[3] = {{HANDLE_B, 0, 72, W, H}, {HANDLE_B, 0x4000, 40, W / 2, H / 2},
                      {HANDLE_B, 0x5000, 40, W / 2, H / 2}};
    memset(G(MEM_B), 0, 0x8000);
    surface(a, dst, 0, W, H, p, 3);
    snx_st_le64(G(post), 0);
    CHECK(yuv(c, dec, dst, 0, 1, list, 0, post, 0) == ST_OK);
    CHECK(snx_ld_le32(G(post)) == NO_FENCE);
    u32 err = 0;
    for (u32 y = 0; y < H; y++)
        for (u32 x = 0; x < W; x++) err += (u32)abs((int)G(MEM_B)[y * 72 + x] - (int)pic_y(x, y));
    CHECK(err / (W * H) <= 2);
    /* the surface's second plane takes Cr, its third Cb */
    CHECK(close_to(G(MEM_B + 0x5000)[3 * 40 + 5], pic_cb(5, 3), 3) &&
          close_to(G(MEM_B + 0x4000)[3 * 40 + 5], pic_cr(5, 3), 3));
    /* shrunk by 2: half the sides */
    memset(G(MEM_B), 0, 0x8000);
    CHECK(yuv(c, dec, dst, 1, 1, list, 0, post, 0) == ST_OK);
    CHECK(close_to(G(MEM_B)[10 * 72 + 10], pic_y(21, 21), 12) && G(MEM_B)[30 * 72 + 40] == 0);
    /* NV12 */
    PlaneDesc nv[2] = {{HANDLE_B, 0, 64, W, H}, {HANDLE_B, 0x4000, 64, W / 2, H / 2}};
    surface(a, dst, 6, W, H, nv, 2);
    CHECK(yuv(c, dec, dst, 0, 1, list, 0, post, 0) == ST_OK);
    CHECK(close_to(G(MEM_B + 0x4000)[3 * 64 + 10], pic_cb(5, 3), 3) &&
          close_to(G(MEM_B + 0x4000)[3 * 64 + 11], pic_cr(5, 3), 3));
    /* YUYV, 4:2:2 from 4:2:0 */
    PlaneDesc yuyv = {HANDLE_B, 0, 2 * W, W, H};
    surface(a, dst, 12, W, H, &yuyv, 1);
    CHECK(yuv(c, dec, dst, 0, 1, list, 0, post, 0) == ST_OK);
    CHECK(close_to(G(MEM_B)[6 * 2 * W + 2 * 20], pic_y(20, 6), 6) &&
          close_to(G(MEM_B)[6 * 2 * W + 2 * 20 + 1], pic_cb(10, 3), 3) &&
          close_to(G(MEM_B)[6 * 2 * W + 2 * 20 + 3], pic_cr(10, 3), 3));
    /* what NVJPG refuses: 4:4:4 from 4:2:0, a block linear surface, flags, types it doesn't write, a surface too
       small, a bitstream larger than the decoder's, a progressive JPEG */
    surface(a, dst, 3, W, H, p, 3);
    CHECK(yuv(c, dec, dst, 0, 1, list, 0, post, 0) == ST_UNSUPPORTED);
    surface(a, dst, 0, W, H, p, 3);
    snx_st_le32(G(ld_ptr(c, a, ld_ptr(c, a, dst + a->planes) + a->plane_surf) + RM_LAYOUT), 3);
    CHECK(yuv(c, dec, dst, 0, 1, list, 0, post, 0) == ST_UNSUPPORTED);
    surface(a, dst, 0, W, H, p, 3);
    CHECK(yuv(c, dec, dst, 0, 1, list, 0, post, 1) == ST_UNSUPPORTED);
    snx_st_le32(G(dst), 9);
    CHECK(yuv(c, dec, dst, 0, 1, list, 0, post, 0) == ST_UNSUPPORTED);
    snx_st_le32(G(dst), 13);
    CHECK(yuv(c, dec, dst, 0, 1, list, 0, post, 0) == ST_BAD);
    PlaneDesc small[3] = {{HANDLE_B, 0, 72, W, H - 16}, p[1], p[2]};
    surface(a, dst, 0, W, H, small, 3);
    CHECK(yuv(c, dec, dst, 0, 1, list, 0, post, 0) == ST_BAD);
    surface(a, dst, 0, W, H, p, 3);
    CHECK(tvmrjpeg_decoder_resize(c, dec, W, H, jpeg_n - 1) == ST_OK && snx_ld_le32(G(dec + 4)) == jpeg_n - 1);
    CHECK(yuv(c, dec, dst, 0, 1, list, 0, post, 0) == ST_FULL);
    CHECK(tvmrjpeg_decoder_resize(c, dec, W, H, 0x10000) == ST_OK);
    u32 len;
    const u8* sof = segment(G(STREAM), jpeg_n, 0xC0, &len);
    G(STREAM)[sof - G(STREAM) - 3] = 0xC2;
    CHECK(get_info(c, info, 1, list) == ST_UNSUPPORTED && snx_ld_le16(G(info)) == 0);
    CHECK(yuv(c, dec, dst, 0, 1, list, 0, post, 0) == ST_UNSUPPORTED);
    G(STREAM)[sof - G(STREAM) - 3] = 0xC0;

    /* RGBA in full range (the decoder's default), with the alpha SetAttributes gives */
    u64 attr = SCRATCH + 0xb00;
    snx_st_le32(G(attr), 1); /* BT.709, but not asked for */
    snx_st_le32(G(attr + 4), 0xff);
    tvmrjpeg_decoder_set_attributes(c, dec, 1, attr);
    PlaneDesc rgba = {HANDLE_B, 0, 4 * W + 32, W, H};
    output_surface(a, dst, a->rgba, &rgba);
    snx_st_le64(G(post), 0);
    CHECK(render_any(a, movie7, dec, dst, list, post) == ST_OK);
    CHECK(snx_ld_le32(G(post)) == NO_FENCE);
    const u8* px = G(MEM_B) + 20 * (4 * W + 32) + 4 * 20; /* in the block: Y 200 */
    int y = 200, cb = pic_cb(10, 10) - 128, cr = pic_cr(10, 10) - 128;
    CHECK(close_to(px[0], y + (int)(1.402 * cr), 6) && close_to(px[1], y - (int)(0.344 * cb + 0.714 * cr), 6) &&
          close_to(px[2], snx_clamp_int(y + (int)(1.772 * cb), 0, 255), 6) && px[3] == 0xff);
    output_surface(a, dst, a->rgba + 1u, &rgba); /* another byte order */
    CHECK(render_any(a, movie7, dec, dst, list, post) == ST_UNSUPPORTED);
    output_surface(a, dst, a->rgba + 4u, &rgba); /* past them: RGBA, as NVJPG's default */
    CHECK(render_any(a, movie7, dec, dst, list, post) == ST_OK);
    tvmrjpeg_decoder_destroy(c, dec);
    CHECK(!obj_of(dec, TVMRJPEG_DECODER));
}

/* the 32-bit encoder: the same JPEG from the same picture in 32-bit surfaces */
static void test_encode32(void) {
    u8 j64[0x10000];
    u32 n64 = jpeg_n;
    memcpy(j64, jpeg, jpeg_n);
    source(&g_abi32, SCRATCH, 0);
    u64 enc = tvmrjpeg_encoder_create(c, 2, 0x10000), bytes = SCRATCH + 0xf10;
    CHECK(tvmrjpeg_encoder_feed_frame32(c, enc, SCRATCH, 0, 0, 90) == ST_OK);
    CHECK(tvmrjpeg_encoder_feed_frame32(c, enc, SCRATCH, 0, 0, 90) == ST_OK); /* two buffers */
    u64 list = SCRATCH + 0xe80;
    snx_st_le32(G(list), DEST);
    snx_st_le32(G(list + 4), 0x10000);
    CHECK(tvmrjpeg_encoder_get_bits_ex32(c, enc, bytes, 1, list, 0) == ST_OK);
    CHECK(snx_ld_le32(G(bytes)) == n64 && !memcmp(G(DEST), j64, n64));
    CHECK(tvmrjpeg_encoder_get_bits32(c, enc, bytes, DEST, 0) == ST_OK && !memcmp(G(DEST), j64, n64));
    tvmrjpeg_encoder_destroy(c, enc);
}

/* the table: each version's handler, none for a movie library none was read in */
static const char* handler(const HookTable* t, const char* const* handlers, const char* symbol, u32 movie) {
    u32 versions[HOOK_KEYS] = {0};
    int first;
    versions[HOOK_MOVIE] = movie;
    int k = hook_select(t, symbol, versions, &first);
    return k >= 0 ? handlers[k] : NULL;
}
static void test_table(void) {
    const char* s = "TVMRJPEGDecoderRender";
    CHECK(!strcmp(handler(&hook_table, hook_handler, s, 0x040401), "tvmrjpeg_decoder_render64"));
    CHECK(!strcmp(handler(&hook_table, hook_handler, s, 0x010500), "tvmrjpeg_decoder_render64"));
    CHECK(!strcmp(handler(&hook_table, hook_handler, s, 0x070302), "tvmrjpeg_decoder_render64_7"));
    CHECK(!handler(&hook_table, hook_handler, s, 0x050000));
    CHECK(!handler(&hook_table, hook_handler, s, 0)); /* a module with no movie label */
    CHECK(!strcmp(handler(&hook32_table, hook32_handler, s, 0x000c10), "tvmrjpeg_decoder_render32"));
    CHECK(!strcmp(handler(&hook32_table, hook32_handler, s, 0x010402), "tvmrjpeg_decoder_render32"));
    CHECK(!handler(&hook32_table, hook32_handler, s, 0x030501));
    CHECK(!handler(&hook_table, hook_handler, "TVMRJPEGEncSupportedLayout", 0x040401));
    /* the label the module carries */
    static const char label[] = "..SDK MW+Vendor+VendorSDK_movie-4_4_1-Release..";
    CHECK(hook_label_version((const u8*)label, sizeof label - 1, "movie") == 0x040401);
    CHECK(hook_label_version((const u8*)label, sizeof label - 1, "nnSdk") == 0);
}

int main(void) {
    cpu.mem = test_mem;
    g_mem = test_mem;
    hle_init();
    test_encode();
    source(&g_abi64, SCRATCH, 0); /* planar again */
    encode_source(SCRATCH, 90);
    test_decode(&g_abi64, 0);
    test_decode(&g_abi64_7, 1);
    test_encode32();
    test_decode(&g_abi32, 0);
    test_table();
    printf("nn_tvmr_jpeg: %u checks passed\n", test_checks);
    return 0;
}
