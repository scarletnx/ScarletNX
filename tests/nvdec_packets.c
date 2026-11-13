/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Compile the production implementation with either the isolated FFmpeg double (default CTest target),
   or real FFmpeg headers/libraries and -DSNX_TEST_REAL_FFMPEG for synthetic I_PCM decode coverage and a VP8 clip. */
#include "nx/gpu/video/nvdec.c"
#include "support/nvdec_ref.h"
#ifdef SNX_TEST_REAL_FFMPEG
#include <libavutil/md5.h>
#endif

static unsigned checks;
#define CHECK(x)                                                                                                       \
    do {                                                                                                               \
        ++checks;                                                                                                      \
        if (!(x)) {                                                                                                    \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x);                                                    \
            exit(1);                                                                                                   \
        }                                                                                                              \
    } while (0)

/* guest memory: the small surfaces from 0, block_linear_test's big ones after (iova_ptr), one arena g_mem names, as
   nvdec.c hands the renderer guest addresses */
#define MEMORY 0x6000u
#define BIGMEM (4u << 20)
static u8 arena[MEMORY + BIGMEM];
u8* g_mem = arena;
#define memory arena
static u8* const big = arena + MEMORY;
static u32 regs[0x1000 / 4];
static u8* const setup = memory + 0x1000;
static u8* const stream = memory + 0x2000;
static char last_log[512]; /* the last line nvdec.c logged */
void logf_(int level, const char* fmt, ...) {
    va_list ap;
    (void)level;
    va_start(ap, fmt);
    vsnprintf(last_log, sizeof last_log, fmt, ap);
    va_end(ap);
}
#ifndef SNX_TEST_REAL_FFMPEG
static void logged(const char* want) {
    if (!strstr(last_log, want)) fprintf(stderr, "logged \"%s\", not \"%s\"\n", last_log, want);
    CHECK(strstr(last_log, want) != NULL);
}
#endif
/* the renderer lays the picture out on the GPU; here the reference does it in guest memory (nvdec_ref.c) */
static int write_ref(const NvdecPic* p) {
    nvdec_write_ref(p);
    return 1;
}
static const Renderer stand_in = {.name = "stand-in", .nvdec_write = write_ref};
const Renderer* g_renderer = &stand_in;
int g_renderer_up = 1;
/* block_linear_test's surfaces, from iova BIG on */
#define BIG 0x1000000u
u8* iova_ptr(u64 addr, u64 size) {
    if (addr >= BIG) return addr - BIG <= BIGMEM && size <= BIGMEM - (addr - BIG) ? big + (addr - BIG) : NULL;
    return addr && addr <= MEMORY && size <= MEMORY - addr ? memory + addr : NULL;
}
/* Surface addressing is deliberately linear in this test, so the production NV12 interleave/write path runs, except
   in block_linear_test: the block linear layout, GOBs of 64 bytes by 8 rows in blocks of 1 << bh GOBs, the GOB's 16
   byte groups in their order. Each walk of it is counted */
static int bl_layout;
static unsigned bl_walks;
u64 bl_offset(u32 x, u32 y, u32 z, u32 pitch, u32 height, u32 bh, u32 bd) {
    bl_walks++;
    if (!bl_layout) return (u64)y * pitch + x;
    u32 gob_w = (pitch + 63) / 64, bhn = 1u << bh, bdn = 1u << bd;
    u32 blocks_h = ((height + 7) / 8 + bhn - 1) / bhn, gy = y / 8, xb = x % 64, yb = y % 8;
    u64 block = 512ull * bhn * bdn;
    u64 o = ((u64)(z / bdn) * blocks_h * gob_w + (u64)(gy / bhn) * gob_w + x / 64) * block +
            ((u64)(z % bdn) * bhn + gy % bhn) * 512;
    return o + (xb / 32) * 256 + (yb / 2) * 64 + ((xb % 32) / 16) * 32 + (yb % 2) * 16 + xb % 16;
}
static void put32(u32 off, u32 value) { snx_st_le32(setup + off, value); }
static void fixture(void) {
    memset(memory, 0, MEMORY);
    memset(regs, 0, sizeof regs);
    regs[0x200 / 4] = 3;
    regs[0x404 / 4] = 0x10;
    regs[0x408 / 4] = 0x20;
    regs[0x424 / 4] = 0x30;
    regs[0x430 / 4] = 0x40;
    regs[0x474 / 4] = 0x50;
    put32(0x48, 3);
    memcpy(stream, "\x65\x88\x84", 3);
    put32(0x60, 1); /* frame_mbs_only_flag */
    put32(0x64, 1);
    put32(0x68, 1);
    put32(0x8c, 64);
    put32(0x90, 64);
    put32(0xb0, 2 | (1 << 12) | (2 << 14)); /* direct_8x8, 4:2:0, POC type 2 */
    memset(setup + 0x1c0, 16, 6 * 16 + 2 * 64);
    memset(memory + 0x4000, 0xcc, 0x2000);
}
static void status_is(int failed) {
    CHECK(rd32(memory, 0x3000) == (failed ? 0u : 1u));
    CHECK(rd32(memory, 0x3004) == (failed ? 1u : 0u));
    CHECK(rd32(memory, 0x300c) == (u32)failed);
    if (failed)
        for (unsigned i = 0x4000; i < MEMORY; ++i) CHECK(memory[i] == 0xcc);
}
static void pixels_are(u8 y, u8 u, u8 v) {
    status_is(0);
    for (unsigned row = 0; row < 16; ++row)
        for (unsigned col = 0; col < 16; ++col) CHECK(memory[0x4000 + row * 64 + col] == y);
    for (unsigned row = 0; row < 8; ++row)
        for (unsigned col = 0; col < 8; ++col) {
            CHECK(memory[0x5000 + row * 64 + col * 2] == u);
            CHECK(memory[0x5000 + row * 64 + col * 2 + 1] == v);
        }
}
static void padding_is_zero(const AVPacket* p) {
    CHECK(p->buf != NULL);
    for (int i = 0; i < AV_INPUT_BUFFER_PADDING_SIZE; ++i) CHECK(p->data[p->size + i] == 0);
}
static void packet_tests(void) {
    AVPacket* p = av_packet_alloc();
    CHECK(p != NULL);
    const u8 hdr[] = {0, 0, 0, 1, 0x67, 0x80};
    const u8 annex[] = {0, 0, 1, 0x65, 0x88, 0, 0, 0, 1, 0x61, 0x99};
    const u8 avcc[] = {0, 0, 0, 2, 0x65, 0x88, 0, 0, 0, 2, 0x61, 0x99};
    const u8 converted[] = {0, 0, 0, 1, 0x65, 0x88, 0, 0, 0, 1, 0x61, 0x99};
    CHECK(!make_packet(p, hdr, sizeof hdr, annex, sizeof annex));
    CHECK(p->size == sizeof hdr + sizeof annex);
    CHECK(!memcmp(p->data, hdr, sizeof hdr));
    CHECK(!memcmp(p->data + sizeof hdr, annex, sizeof annex));
    padding_is_zero(p);
    CHECK(!make_packet(p, hdr, 0, avcc, sizeof avcc));
    CHECK(p->size == sizeof converted && !memcmp(p->data, converted, sizeof converted));
    padding_is_zero(p);
    /* A complete AVCC NAL with a 00 00 01 xx length must not be mistaken for an Annex B start code. */
    u8 large[4 + 357];
    memset(large, 0x88, sizeof large);
    large[0] = large[1] = 0;
    large[2] = 1;
    large[3] = 0x65;
    large[4] = 0x65;
    CHECK(!make_packet(p, hdr, 0, large, sizeof large));
    CHECK(p->size == sizeof large && !memcmp(p->data, "\0\0\0\1", 4));
    CHECK(!memcmp(p->data + 4, large + 4, sizeof large - 4));
    padding_is_zero(p);
    /* A much shorter packet must have fresh zero padding, not the previous payload's tail. */
    CHECK(!make_packet(p, hdr, 0, avcc + 4, 2));
    CHECK(p->size == 5 && !memcmp(p->data, "\0\0\1\x65\x88", 5));
    padding_is_zero(p);
    const u8 leading[] = {0, 0, 0, 0, 0, 1, 0x65, 0x80};
    CHECK(!make_packet(p, hdr, 0, leading, sizeof leading));
    CHECK(p->size == sizeof leading && !memcmp(p->data, leading, sizeof leading));
    padding_is_zero(p);
    const u8 bad[][6] = {{0, 0, 0, 8, 0x65, 0x88},
                         {0, 0, 0, 0, 0x65, 0x88},
                         {0, 0, 0, 2, 0xe5, 0x88},
                         {0, 0, 1, 0, 0, 0},
                         {0xff, 0, 0, 0, 0, 0}};
    for (unsigned i = 0; i < sizeof bad / sizeof *bad; ++i)
        CHECK(make_packet(p, hdr, 0, bad[i], sizeof bad[i]) == AVERROR_INVALIDDATA);
    for (unsigned i = 0; i < 4; ++i) CHECK(make_packet(p, hdr, 0, avcc, i) == AVERROR_INVALIDDATA);
    av_packet_free(&p);
}

#ifndef SNX_TEST_REAL_FFMPEG
/* Script both API calls: capture=1 invokes the real get_buf callback, including on errors/older output.
   This double tests submission, NOT H.264 decoding or the real FFmpeg ABI. */
typedef struct {
    char call;
    int result, capture;
} Event;
static const Event* events;
static unsigned event_count, event_at, sends;
static int expected_headers, alloc_fail, ref_fail;
static const u8* retry_data;
static int retry_size;
static const u8* want_packet; /* set: the packet the decoder must get, byte for byte */
static int want_size;
static u8 planes[384];
static Event next_event(char call, AVCodecContext* c) {
    CHECK(event_at < event_count);
    Event e = events[event_at++];
    CHECK(e.call == call);
    if (e.capture) {
        AVFrame f = {0};
        CHECK(!c->get_buffer2(c, &f, 0));
        av_frame_unref(&f);
    }
    return e;
}
static void run(NvdecCtx* d, const Event* script, unsigned n, int headers, int failed) {
    events = script;
    event_count = n;
    event_at = sends = 0;
    expected_headers = headers;
    retry_data = NULL;
    memset(memory + 0x4000, 0xcc, 0x2000);
    nvdec_execute(d, regs);
    CHECK(event_at == event_count);
    status_is(failed);
    if (failed) CHECK(!d->cur_new && !d->cur->buf[0] && !d->last_hdr_n);
}
#define RUN(d, script, headers, failed) run(d, script, sizeof(script) / sizeof *(script), headers, failed)
const AVCodec* avcodec_find_decoder(int id) {
    static AVCodec codec;
    CHECK(id == AV_CODEC_ID_H264 || id == AV_CODEC_ID_VP8 || id == AV_CODEC_ID_VP9);
    return &codec;
}
AVCodecContext* avcodec_alloc_context3(const AVCodec* codec) {
    (void)codec;
    return calloc(1, sizeof(AVCodecContext));
}
int avcodec_open2(AVCodecContext* c, const AVCodec* codec, void* options) {
    (void)codec;
    (void)options;
    CHECK(c->thread_type == FF_THREAD_SLICE && (c->thread_count == 1 || c->thread_count == 4));
    return 0;
}
void avcodec_free_context(AVCodecContext** c) {
    free(*c);
    *c = NULL;
}
int avcodec_default_get_buffer2(AVCodecContext* c, AVFrame* f, int flags) {
    (void)c;
    (void)flags;
    memset(planes, 81, 256);
    memset(planes + 256, 90, 64);
    memset(planes + 320, 240, 64);
    f->width = f->height = 16;
    f->format = AV_PIX_FMT_YUV420P;
    f->data[0] = planes;
    f->data[1] = planes + 256;
    f->data[2] = planes + 320;
    f->linesize[0] = 16;
    f->linesize[1] = f->linesize[2] = 8;
    f->buf[0] = planes;
    return 0;
}
int avcodec_send_packet(AVCodecContext* c, const AVPacket* p) {
    CHECK(p != NULL);
    padding_is_zero(p);
    ++sends;
    if (want_packet)
        CHECK(p->size == want_size && !memcmp(p->data, want_packet, (size_t)want_size));
    else
        CHECK((p->size > 4 && p->data[4] == 0x67) == expected_headers);
    if (retry_data) CHECK(p->data == retry_data && p->size == retry_size);
    Event e = next_event('S', c);
    if (e.result == AVERROR(EAGAIN)) {
        retry_data = p->data;
        retry_size = p->size;
    }
    return e.result;
}
int avcodec_receive_frame(AVCodecContext* c, AVFrame* f) {
    (void)f;
    return next_event('R', c).result;
}
AVPacket* av_packet_alloc(void) { return calloc(1, sizeof(AVPacket)); }
void av_packet_unref(AVPacket* p) {
    free(p->buf);
    memset(p, 0, sizeof *p);
}
void av_packet_free(AVPacket** p) {
    if (*p) {
        av_packet_unref(*p);
        free(*p);
        *p = NULL;
    }
}
int av_new_packet(AVPacket* p, int size) {
    if (alloc_fail) return AVERROR(ENOMEM);
    p->data = malloc((size_t)size + AV_INPUT_BUFFER_PADDING_SIZE);
    CHECK(p->data != NULL);
    p->buf = p->data;
    p->size = size;
    memset(p->data, 0xa5, (size_t)size);
    memset(p->data + size, 0, AV_INPUT_BUFFER_PADDING_SIZE);
    return 0;
}
void av_shrink_packet(AVPacket* p, int size) {
    CHECK(size <= p->size);
    p->size = size;
    memset(p->data + size, 0, AV_INPUT_BUFFER_PADDING_SIZE);
}
AVFrame* av_frame_alloc(void) { return calloc(1, sizeof(AVFrame)); }
void av_frame_unref(AVFrame* f) { memset(f, 0, sizeof *f); }
void av_frame_free(AVFrame** f) {
    free(*f);
    *f = NULL;
}
int av_frame_ref(AVFrame* dst, const AVFrame* src) {
    if (ref_fail) return AVERROR(ENOMEM);
    *dst = *src;
    return 0;
}
const char* av_version_info(void) { return "test double"; }
int av_strerror(int err, char* dst, size_t size) {
    snprintf(dst, size, "%d", err);
    return 0;
}
int av_log_get_level(void) { return AV_LOG_WARNING; }
void av_log_set_level(int level) { (void)level; }
void av_log_set_callback(void (*cb)(void*, int, const char*, va_list)) { (void)cb; }
int av_log_format_line2(void* c, int level, const char* fmt, va_list args, char* line, int size, int* prefix) {
    (void)c;
    (void)level;
    (void)prefix;
    return vsnprintf(line, (size_t)size, fmt, args);
}
static void submission_tests(void) {
    const Event good[] = {{'S', 0, 1}, {'R', 0, 0}, {'R', AVERROR(EAGAIN), 0}};
    const Event retry[] = {
        {'S', AVERROR(EAGAIN), 0}, {'R', 0, 1}, {'R', AVERROR(EAGAIN), 0}, {'S', 0, 1}, {'R', AVERROR(EAGAIN), 0}};
    const Event send_bad[] = {{'S', AVERROR_INVALIDDATA, 1}};
    const Event recv_bad[] = {{'S', 0, 1}, {'R', AVERROR_INVALIDDATA, 0}};
    const Event eof[] = {{'S', 0, 1}, {'R', AVERROR_EOF, 0}};
    const Event stalled[] = {{'S', AVERROR(EAGAIN), 0}, {'R', AVERROR(EAGAIN), 0}, {'S', AVERROR(EAGAIN), 0}};
    const Event drain_bad[] = {{'S', AVERROR(EAGAIN), 0}, {'R', AVERROR_INVALIDDATA, 1}};
    const Event old_only[] = {
        {'S', AVERROR(EAGAIN), 0}, {'R', 0, 1}, {'R', AVERROR(EAGAIN), 0}, {'S', 0, 0}, {'R', AVERROR(EAGAIN), 0}};
    const Event no_new[] = {{'S', 0, 0}, {'R', AVERROR(EAGAIN), 0}};
    fixture();
    NvdecCtx* d = nvdec_new();
    CHECK(d != NULL);
    CHECK(dec_open(d, 3));
    AVFrame f = {0};
    ref_fail = 1;
    CHECK(get_buf(d->dec, &f, 0) == AVERROR(ENOMEM));
    CHECK(!d->cur_new);
    av_frame_unref(&f);
    ref_fail = 0;
    RUN(d, good, 1, 0);
    pixels_are(81, 90, 240);
    RUN(d, good, 0, 0); /* unchanged parameter sets are now cached */
    RUN(d, retry, 0, 0);
    CHECK(sends == 2);
    pixels_are(81, 90, 240);
    put32(0x70, 1); /* changed PPS must be resent if submission fails */
    RUN(d, send_bad, 1, 1);
    RUN(d, good, 1, 0);
    RUN(d, recv_bad, 0, 1);
    RUN(d, eof, 1, 1);
    RUN(d, stalled, 1, 1);
    CHECK(sends == 2);
    RUN(d, drain_bad, 1, 1);
    RUN(d, old_only, 1, 1); /* older callback cannot authorize writing this job */
    RUN(d, good, 1, 0);
    put32(0xb0, rd32(setup, 0xb0) | 0xa0); /* second field reuses the first field's buffer */
    RUN(d, no_new, 0, 0);
    RUN(d, send_bad, 0, 1);
    RUN(d, no_new, 1, 1);   /* rejected field cannot publish stale pixels */
    RUN(d, old_only, 1, 1); /* neither can a buffer captured while draining an older job */
    put32(0xb0, rd32(setup, 0xb0) & ~0xa0u);
    RUN(d, good, 1, 0);
    alloc_fail = 1;
    run(d, NULL, 0, 0, 1);
    CHECK(!sends);
    alloc_fail = 0;
    RUN(d, good, 1, 0);
    stream[0] = 0xff;
    run(d, NULL, 0, 0, 1);
    CHECK(!sends);
    stream[0] = 0x65;
    RUN(d, good, 1, 0);
    regs[0x430 / 4] = 0; /* no surface written must not report success */
    RUN(d, good, 0, 1);
    nvdec_free(d);
}
#else
static void pcm_slice(u8 y, u8 u, u8 v) {
    u8 rb[512] = {0};
    H264BitWriter bw = {.buf = rb, .cap = sizeof rb};
    h264_write_ue(&bw, 0);   /* first_mb_in_slice */
    h264_write_ue(&bw, 2);   /* I slice */
    h264_write_ue(&bw, 0);   /* PPS id */
    h264_write_bits(&bw, 0, 4); /* frame_num */
    h264_write_ue(&bw, 0);   /* idr_pic_id; POC type 2 has no extra fields */
    h264_write_bit(&bw, 0);
    h264_write_bit(&bw, 0); /* IDR reference marking */
    h264_write_se(&bw, 0);  /* slice_qp_delta */
    h264_write_ue(&bw, 25); /* I_PCM macroblock */
    while (bw.bit_pos) h264_write_bit(&bw, 0);
    for (unsigned i = 0; i < 384; ++i) h264_write_bits(&bw, i < 256 ? y : i < 320 ? u : v, 8);
    h264_write_end(&bw);
    u32 n = h264_nal_unit(stream, 0x1000, 0x65, rb, bw.bytes);
    CHECK(n != 0);
    put32(0x48, n);
}
static void skip_slice(u32 frame_num) {
    u8 rb[32] = {0};
    H264BitWriter bw = {.buf = rb, .cap = sizeof rb};
    h264_write_ue(&bw, 0);
    h264_write_ue(&bw, 0);
    h264_write_ue(&bw, 0); /* first MB, P slice, PPS */
    h264_write_bits(&bw, frame_num, 4);
    h264_write_bit(&bw, 0); /* num_ref_idx_active_override_flag */
    h264_write_bit(&bw, 0); /* ref_pic_list_modification_flag_l0 */
    h264_write_bit(&bw, 0); /* adaptive_ref_pic_marking_mode_flag */
    h264_write_se(&bw, 0);  /* slice_qp_delta */
    h264_write_ue(&bw, 1);  /* one skipped MB, predicted from the previous reference */
    h264_write_end(&bw);
    u32 n = h264_nal_unit(stream, 0x1000, 0x41, rb, bw.bytes);
    CHECK(n != 0);
    put32(0x48, n);
}
static void submission_tests(void) {
    fixture();
    NvdecCtx* d = nvdec_new();
    CHECK(d != NULL);
    pcm_slice(81, 90, 240);
    nvdec_execute(d, regs);
    pixels_are(81, 90, 240);
    CHECK(d->last_hdr_n != 0);
    for (u32 frame_num = 1; frame_num <= 2; ++frame_num) {
        skip_slice(frame_num);
        memset(memory + 0x4000, 0xcc, 0x2000);
        nvdec_execute(d, regs);
        pixels_are(81, 90, 240);
    }
    /* Reuse the same SPS/PPS but decode distinct pixels; both NAL payloads require emulation prevention. */
    pcm_slice(0, 0, 0);
    u32 n = rd32(setup, 0x48) - 4; /* the same NAL in four-byte-length-prefixed form */
    stream[0] = (u8)(n >> 24);
    stream[1] = (u8)(n >> 16);
    stream[2] = (u8)(n >> 8);
    stream[3] = (u8)n;
    nvdec_execute(d, regs);
    pixels_are(0, 0, 0);
    pcm_slice(145, 54, 34);
    n = rd32(setup, 0x48) - 4; /* bare NAL */
    memmove(stream, stream + 4, n);
    put32(0x48, n);
    nvdec_execute(d, regs);
    pixels_are(145, 54, 34);
    nvdec_free(d);
}
#endif
/* A VP9 job opens a VP9 decoder whose opaque is the channel's context, which starts with what FFmpeg's patched VP9
   decoder reads there (cmake/ffmpeg-vp9-nvdec.patch); one with no probability table fails without writing the
   picture; a codec NVDEC jobs are not decoded for fails and leaves the decoder as it was */
/* a job failed before its size was known: the status says failed, and the output surface keeps its bytes */
static void failed_untouched(void) {
    CHECK(rd32(memory, 0x3000) == 0 && rd32(memory, 0x300c) == 1);
    for (unsigned i = 0x4000; i < MEMORY; ++i) CHECK(memory[i] == 0xcc);
}
static void vp9_tests(void) {
    fixture();
    NvdecCtx* d = nvdec_new();
    CHECK(d != NULL);
    CHECK(((const u32*)d)[0] == 0x534e5856 && ((const u32*)d)[1] == 0);
    regs[0x200 / 4] = 9;
    regs[0x5C0 / 4] = 0;
    nvdec_execute(d, regs);
    failed_untouched();
    CHECK(d->app == 9 && d->dec && d->dec->opaque == d && d->vp9);
    CHECK(d->dec->thread_type == FF_THREAD_SLICE && d->dec->thread_count == 4);
    regs[0x200 / 4] = 7; /* H.265 */
    memset(memory + 0x3000, 0, 16);
    nvdec_execute(d, regs);
    failed_untouched();
    CHECK(d->app == 9 && d->dec);
    nvdec_free(d);
}

/* VP8 jobs: NVDEC takes the frame without its first bytes (RFC 6386 9.1), which the picture info carries; the job
   puts them back, and the picture goes into surface 3, the current one, at the picture info's frame offset */
static void vp8_setup(int key, u32 version, u32 first_part, u32 len, u32 w, u32 h) {
    fixture();
    memset(setup, 0, VP8_PICTURE_INFO_SIZE);
    regs[NVDEC_SET_CODEC_ID] = NVDEC_CODEC_VP8;
    regs[NVDEC_SURFACE_LUMA_OFFSETS] = regs[NVDEC_SURFACE_CHROMA_OFFSETS] = 0; /* surface 0 isn't mapped */
    regs[NVDEC_SURFACE_LUMA_OFFSETS + VP8_SURFACE_CURRENT] = 0x40;
    regs[NVDEC_SURFACE_CHROMA_OFFSETS + VP8_SURFACE_CURRENT] = 0x50;
    snx_st_le16(setup + VP8_FRAME_WIDTH, (u16)w);
    snx_st_le16(setup + VP8_FRAME_HEIGHT, (u16)h);
    setup[VP8_KEY_FRAME] = (u8)key;
    setup[VP8_VERSION] = (u8)version;
    put32(VP8_FIRST_PART_SIZE, first_part);
    put32(VP8_VLD_BUFFER_SIZE, len);
    put32(VP8_FRAME_STRIDE_LUMA, 64);
    put32(VP8_FRAME_STRIDE_CHROMA, 64);
    for (u32 i = 0; i < 16; i++) stream[i] = (u8)(0xA0 + i);
}
#ifndef SNX_TEST_REAL_FFMPEG
static u8 vp8_want[VP8_KEY_HEADER_SIZE + 16];
/* the decoder must get these header bytes, then the job's len bytes of frame */
static void vp8_expect(const char* hdr, u32 hn, u32 len) {
    memcpy(vp8_want, hdr, hn);
    memcpy(vp8_want + hn, stream, len);
    want_packet = vp8_want;
    want_size = (int)(hn + len);
}
/* refused before the decoder sees it, saying why: no packet, the status says failed, the surfaces keep their bytes */
static void vp8_refused(NvdecCtx* d, const char* why) {
    last_log[0] = 0;
    events = NULL;
    event_count = event_at = sends = 0;
    memset(memory + 0x3000, 0, 16);
    memset(memory + 0x4000, 0xcc, 0x2000);
    nvdec_execute(d, regs);
    CHECK(!sends);
    failed_untouched();
    CHECK(!d->cur_new && !d->cur->buf[0]);
    logged(why);
}
static void vp8_tests(void) {
    const Event good[] = {{'S', 0, 1}, {'R', AVERROR(EAGAIN), 0}};
    const Event send_bad[] = {{'S', AVERROR_INVALIDDATA, 1}};
    const Event no_new[] = {{'S', 0, 0}, {'R', AVERROR(EAGAIN), 0}};
    u8 h[VP8_KEY_HEADER_SIZE];
    /* the tag's fields, little endian: key frame 0 in bit 0, version 1-3, show_frame 4, first partition 5-23 */
    CHECK(vp8_frame_header(1, 0, 2, 16, 16, h) == 10 && !memcmp(h, "\x50\x00\x00\x9d\x01\x2a\x10\x00\x10\x00", 10));
    CHECK(vp8_frame_header(1, 3, 0x12345, 1920, 1080, h) == 10 &&
          !memcmp(h, "\xb6\x68\x24\x9d\x01\x2a\x80\x07\x38\x04", 10));
    CHECK(vp8_frame_header(0, 7, 0x7FFFF, 0, 0, h) == 3 && !memcmp(h, "\xff\xff\xff", 3));
    CHECK(vp8_frame_header(0, 0, 0, 0, 0, h) == 3 && !memcmp(h, "\x11\x00\x00", 3));

    NvdecCtx* d = nvdec_new();
    CHECK(d != NULL);
    vp8_setup(1, 0, 2, 5, 16, 16);
    vp8_expect("\x50\x00\x00\x9d\x01\x2a\x10\x00\x10\x00", 10, 5);
    RUN(d, good, 0, 0);
    pixels_are(81, 90, 240);
    CHECK(d->app == NVDEC_CODEC_VP8 && d->dec && !d->vp9 && d->dec->thread_count == 1);
    vp8_setup(0, 3, 4, 4, 16, 16); /* an inter frame: the tag alone; a first partition as long as the frame */
    vp8_expect("\x97\x00\x00", 3, 4);
    RUN(d, good, 0, 0);
    pixels_are(81, 90, 240);
    vp8_setup(0, 0, 2, 5, 16, 16); /* the frame offsets, 8 << 8 bytes, into surfaces 0x800 bytes lower */
    regs[NVDEC_SURFACE_LUMA_OFFSETS + VP8_SURFACE_CURRENT] = 0x38;
    regs[NVDEC_SURFACE_CHROMA_OFFSETS + VP8_SURFACE_CURRENT] = 0x48;
    put32(VP8_LUMA_FRAME_OFFSET, 8);
    put32(VP8_CHROMA_FRAME_OFFSET, 8);
    vp8_expect("\x51\x00\x00", 3, 5);
    RUN(d, good, 0, 0);
    pixels_are(81, 90, 240);
    logged("VP8 pictures go 800 and 800 bytes into their surfaces");
    want_packet = NULL;

    /* jobs NVDEC can't decode never reach the decoder */
    vp8_setup(1, 0, 2, 5, 16, 16);
    regs[NVDEC_PICTURE_INFO_OFFSET] = 0;
    vp8_refused(d, "a VP8 job's picture info at 0 isn't mapped");
    const struct {
        u32 version, first_part, len, w, h;
        const char* why;
    } bad[] = {
        {0, 0, 0, 16, 16, "a 16x16 frame of 0 bytes isn't decoded: NVDEC takes 1 to 4096 pixels a side, 1 to 16777216"},
        {0, 2, MAX_STREAM + 1, 16, 16, "a 16x16 frame of 16777217 bytes isn't decoded"},
        {0, 2, 5, 0, 16, "a 0x16 frame of 5 bytes isn't decoded"},
        {0, 2, 5, 16, 0, "a 16x0 frame of 5 bytes isn't decoded"},
        {0, 2, 5, MAX_DIM + 1, 16, "a 4097x16 frame of 5 bytes isn't decoded"},
        {0, 2, 5, 16, MAX_DIM + 1, "a 16x4097 frame of 5 bytes isn't decoded"},
        {VP8_MAX_VERSION + 1, 2, 5, 16, 16,
         "version 8, first partition 2 of 5 bytes, isn't a frame's: versions 0 to 7"},
        {0, 6, 5, 16, 16, "version 0, first partition 6 of 5 bytes, isn't a frame's"},
        {0, VP8_MAX_FIRST_PART + 1, VP8_MAX_FIRST_PART + 1, 16, 16,
         "first partition 524288 of 524288 bytes, isn't a frame's: versions 0 to 7, a first partition within the frame, "
         "of 524287 bytes at most"}};
    for (unsigned i = 0; i < sizeof bad / sizeof *bad; i++) {
        vp8_setup(1, bad[i].version, bad[i].first_part, bad[i].len, bad[i].w, bad[i].h);
        vp8_refused(d, bad[i].why);
    }
    vp8_setup(1, 0, 2, 5, 16, 16);
    regs[NVDEC_FRAME_BITSTREAM_OFFSET] = MEMORY >> 8; /* the frame past guest memory */
    vp8_refused(d, "a VP8 job's frame of 5 bytes at 6000 isn't mapped");

    /* decoded, so later frames can predict from it, but not written: the surface's layout or its rows, or no
       surface 3 */
    vp8_setup(1, 0, 2, 5, 16, 16);
    setup[VP8_SURFACE_FORMAT] = 1; /* pitch linear */
    RUN(d, good, 0, 1);
    logged("a 16x16 VP8 picture in tile format 1, rows of 64 and 64 bytes, isn't written: block linear (0) only, rows "
           "of 16 and 16 bytes at least");
    vp8_setup(1, 0, 2, 5, 16, 16);
    put32(VP8_FRAME_STRIDE_LUMA, 15);
    RUN(d, good, 0, 1);
    logged("in tile format 0, rows of 15 and 64 bytes, isn't written");
    vp8_setup(1, 0, 2, 5, 16, 16);
    put32(VP8_FRAME_STRIDE_CHROMA, 15);
    RUN(d, good, 0, 1);
    logged("in tile format 0, rows of 64 and 15 bytes, isn't written");
    vp8_setup(1, 0, 2, 5, 16, 16);
    regs[NVDEC_SURFACE_LUMA_OFFSETS + VP8_SURFACE_CURRENT] = 0;
    RUN(d, good, 0, 1);
    logged("NVDEC surface 0/5000 isn't mapped");
    /* the decoder refuses the frame, or starts no picture, while the last job's picture is still held */
    vp8_setup(1, 0, 2, 5, 16, 16);
    RUN(d, send_bad, 0, 1);
    RUN(d, good, 0, 0);
    RUN(d, no_new, 0, 1);
    logged("a VP8 job started no picture");
    /* the channel goes on decoding after each */
    RUN(d, good, 0, 0);
    pixels_are(81, 90, 240);
    nvdec_free(d);
}
#else
/* A 40x24 clip that libvpx 1.15.2's vpxenc made from a gradient with a block that moves (profile 0, a key frame and two
   inter frames), each frame without the bytes NVDEC takes in the picture info, and the MD5 of each picture as I420
   that libvpx's vpxdec gives. Every picture decodes through its job into surface 3, wider and higher than it */
static const u8 vp8_clip[] = {
    /* frame 0: key, first partition 51 of 200 bytes */
    0x0c, 0x47, 0x08, 0x85, 0x85, 0x88, 0x85, 0x84, 0x88, 0x2c, 0x02, 0x02, 0x75, 0xbb, 0x09, 0x14, 0xc4, 0xc8, 0xf0,
    0x12, 0x80, 0xef, 0x33, 0x11, 0x0f, 0xe3, 0xe3, 0xbe, 0xcc, 0x1b, 0x46, 0xbe, 0xe4, 0x32, 0xd5, 0x8a, 0xd7, 0x06,
    0xdf, 0x92, 0x96, 0x46, 0x64, 0xfa, 0x9f, 0x16, 0x36, 0xbd, 0x6d, 0xca, 0x00, 0xfe, 0xe8, 0xf1, 0x87, 0xdb, 0x04,
    0xbd, 0xc3, 0xac, 0x67, 0x0f, 0x8b, 0x0a, 0x75, 0x28, 0xe9, 0x29, 0x9d, 0xb1, 0xed, 0xe8, 0xde, 0x14, 0x2e, 0xb0,
    0x26, 0x4c, 0x03, 0x17, 0x2b, 0x19, 0xca, 0x71, 0xed, 0xc6, 0x8b, 0xb2, 0x31, 0x30, 0x6d, 0x44, 0xa6, 0x6e, 0xe7,
    0x36, 0x6b, 0x79, 0x8a, 0x41, 0x9a, 0xa5, 0xdf, 0x6b, 0x09, 0xc6, 0xba, 0xd3, 0x70, 0xdc, 0x67, 0xfb, 0xac, 0x9b,
    0x68, 0x60, 0x63, 0xc8, 0xf2, 0x16, 0xf4, 0x8b, 0xee, 0xe8, 0x4e, 0x8e, 0x12, 0xc1, 0xce, 0xec, 0xdf, 0xc2, 0x31,
    0xa2, 0x06, 0x90, 0xe3, 0x0e, 0x4d, 0x33, 0x18, 0x50, 0x98, 0xd7, 0xdf, 0x28, 0x06, 0x8e, 0x92, 0xbc, 0x2d, 0x5c,
    0x2d, 0x7a, 0x94, 0x8d, 0x67, 0x59, 0xa3, 0xfa, 0x93, 0x88, 0x0e, 0x0f, 0xf3, 0x2f, 0xf7, 0x0b, 0x1d, 0xe2, 0x7a,
    0x9a, 0xaf, 0xdf, 0x3a, 0x06, 0xcd, 0x6c, 0x31, 0x4c, 0x0c, 0x3b, 0x83, 0xef, 0x8a, 0xb8, 0x0a, 0xb3, 0xfe, 0xcf,
    0x99, 0xfe, 0xa5, 0x26, 0x03, 0x14, 0x67, 0xda, 0xfa, 0x00,
    /* frame 1: inter, first partition 19 of 108 bytes */
    0x1f, 0x11, 0x60, 0x00, 0x18, 0x02, 0x10, 0x04, 0x5d, 0xd1, 0x5d, 0xe8, 0x82, 0x11, 0x78, 0x5e, 0x70, 0xc3, 0x00,
    0xfe, 0x5b, 0xe4, 0x7a, 0xf2, 0x40, 0xb3, 0x74, 0x14, 0x87, 0xeb, 0xd4, 0xdf, 0x5c, 0x23, 0x8c, 0xad, 0x7d, 0x7b,
    0xa5, 0x34, 0xda, 0x34, 0x1f, 0xb5, 0x09, 0x0a, 0x5a, 0xf2, 0x2c, 0x60, 0x8c, 0x37, 0x01, 0xe4, 0xd1, 0xde, 0x18,
    0x24, 0x17, 0x2b, 0xdc, 0x88, 0x57, 0xf4, 0x66, 0x8d, 0x3b, 0x0f, 0x1a, 0xc1, 0x61, 0x8b, 0xa9, 0xc8, 0x94, 0xd2,
    0x12, 0x03, 0x5f, 0x93, 0x17, 0xe8, 0x76, 0x40, 0x00, 0x00, 0x59, 0x5f, 0xb8, 0x6e, 0xb9, 0x96, 0xc2, 0x5f, 0x4d,
    0xa3, 0x7d, 0x3a, 0xa9, 0xd2, 0xa7, 0x5d, 0x30, 0xac, 0x15, 0x2b, 0xdc, 0x00,
    /* frame 2: inter, first partition 21 of 106 bytes */
    0x1e, 0x11, 0x60, 0x00, 0x18, 0x05, 0x8b, 0xa8, 0x20, 0x07, 0xfc, 0x07, 0x53, 0xfc, 0x26, 0x5a, 0x3a, 0x0f, 0x0e,
    0x84, 0x30, 0xfe, 0xa6, 0x62, 0x0d, 0x4e, 0x1e, 0x02, 0x8c, 0x52, 0xae, 0xcb, 0x2b, 0x93, 0xbd, 0x21, 0x63, 0xdf,
    0x7b, 0x14, 0x3a, 0xda, 0x8b, 0x2e, 0xbe, 0xa1, 0x89, 0xef, 0xae, 0xc4, 0xa0, 0x7c, 0xf6, 0x2a, 0xd9, 0xed, 0xdf,
    0x75, 0xf1, 0x8e, 0x66, 0x87, 0x68, 0x92, 0x90, 0x3a, 0x2a, 0x3e, 0xfc, 0x10, 0xbf, 0xbe, 0x50, 0x9f, 0xaf, 0x5b,
    0x65, 0xd1, 0xf7, 0x39, 0xfc, 0xc2, 0xe6, 0x30, 0x26, 0x90, 0x00, 0x0d, 0x27, 0x25, 0xf3, 0xe5, 0xe1, 0xdd, 0x37,
    0xe2, 0xb5, 0x29, 0xff, 0xb5, 0x0e, 0x40, 0x45, 0x66, 0x20, 0x00};
static const struct {
    int key;
    u32 first_part, size;
    const char* md5;
} vp8_frames[] = {{1, 51, 200, "521ee8e0e7ad82845bb103be268befbf"},
                  {0, 19, 108, "81cf3565f96cb3c08ef3681ece98326b"},
                  {0, 21, 106, "405a4ed57874c06b8a2c01d83694bc27"}};
#define VP8_CLIP_W 40u
#define VP8_CLIP_H 24u
static void vp8_tests(void) {
    static u8 i420[VP8_CLIP_W * VP8_CLIP_H * 3 / 2];
    const u32 cw = VP8_CLIP_W / 2, ch = VP8_CLIP_H / 2, mbs = 3 * 2;
    u32 at = 0;
    NvdecCtx* d = nvdec_new();
    CHECK(d != NULL);
    CHECK(sizeof vp8_clip == 200 + 108 + 106);
    for (unsigned i = 0; i < sizeof vp8_frames / sizeof *vp8_frames; i++) {
        u8 sum[16];
        char hex[33];
        vp8_setup(vp8_frames[i].key, 0, vp8_frames[i].first_part, vp8_frames[i].size, VP8_CLIP_W, VP8_CLIP_H);
        memcpy(stream, vp8_clip + at, vp8_frames[i].size);
        at += vp8_frames[i].size;
        memset(memory + 0x4000, 0xcc, 0x2000);
        nvdec_execute(d, regs);
        CHECK(rd32(memory, 0x3000) == mbs && rd32(memory, 0x300c) == 0);
        /* the surfaces back to I420: luma rows, then the interleaved chroma pairs split */
        for (u32 y = 0; y < VP8_CLIP_H; y++) memcpy(i420 + y * VP8_CLIP_W, memory + 0x4000 + y * 64, VP8_CLIP_W);
        for (u32 y = 0; y < ch; y++)
            for (u32 x = 0; x < cw; x++) {
                i420[VP8_CLIP_W * VP8_CLIP_H + y * cw + x] = memory[0x5000 + y * 64 + 2 * x];
                i420[VP8_CLIP_W * VP8_CLIP_H + cw * ch + y * cw + x] = memory[0x5000 + y * 64 + 2 * x + 1];
            }
        /* nothing past the picture: the luma rows' 41st byte, the row under the last, the chroma rows' tail */
        CHECK(memory[0x4000 + VP8_CLIP_W] == 0xcc && memory[0x4000 + VP8_CLIP_H * 64] == 0xcc);
        CHECK(memory[0x5000 + 2 * cw] == 0xcc && memory[0x5000 + ch * 64] == 0xcc);
        av_md5_sum(sum, i420, sizeof i420);
        for (int k = 0; k < 16; k++) snprintf(hex + 2 * k, 3, "%02x", sum[k]);
        if (strcmp(hex, vp8_frames[i].md5))
            fprintf(stderr, "VP8 frame %u: MD5 %s, libvpx's %s\n", i, hex, vp8_frames[i].md5);
        CHECK(!strcmp(hex, vp8_frames[i].md5));
    }
    CHECK(at == sizeof vp8_clip);
    nvdec_free(d);
}
#endif

/* a 15x15 picture into a 16x16 surface: its last chroma pair and row, of one pixel each, are written too */
static void odd_size_test(void) {
    static u8 y[15 * 15], u[8 * 8], v[8 * 8];
    memset(y, 50, sizeof y);
    memset(u, 60, sizeof u);
    memset(v, 70, sizeof v);
    AVFrame f = {0};
    f.width = f.height = 15;
    f.format = AV_PIX_FMT_YUV420P;
    f.data[0] = y;
    f.data[1] = u;
    f.data[2] = v;
    f.linesize[0] = 15;
    f.linesize[1] = f.linesize[2] = 8;
    PicOut t = {0x4000, 0x5000, 64, 64, 1, 16, 16};
    memset(memory + 0x4000, 0xcc, 0x2000);
    CHECK(write_pic(&f, &t));
    CHECK(memory[0x4000 + 14 * 64 + 14] == 50 && memory[0x4000 + 14 * 64 + 15] == 0xcc);
    CHECK(memory[0x5000 + 7 * 64 + 14] == 60 && memory[0x5000 + 7 * 64 + 15] == 70);
    CHECK(memory[0x5000 + 7 * 64 + 16] == 0xcc && memory[0x5000 + 8 * 64] == 0xcc);
}
/* write_pic into the block linear layout, read back byte by byte through it: a 1920x1080 picture in blocks of 16
   GOBs, as videos come, and a 33x17 one, whose rows end inside a 16-byte run and whose chroma has a last pair and
   row of one pixel, in a wider plane; nothing written past the picture. The layout is walked once a column of runs
   and once a row */
static u8 bl_y[1920 * 1080], bl_u[960 * 540], bl_v[960 * 540], bl_want[BIGMEM];
static void block_linear_picture(u32 w, u32 h, u32 pitch, u32 pitch_c, u32 sh, u32 bh) {
    u32 cw = (w + 1) / 2 * 2, ch = (sh + 1) / 2, ys = BIG + 0x10000, cs = BIG + 0x280000;
    for (u32 i = 0; i < w * h; i++) bl_y[i] = (u8)(i * 7 + i / w);
    for (u32 i = 0; i < cw / 2 * ((h + 1) / 2); i++) {
        bl_u[i] = (u8)(i * 3 + 1);
        bl_v[i] = (u8)(i * 5 + 2);
    }
    AVFrame f = {0};
    f.width = (int)w;
    f.height = (int)h;
    f.format = AV_PIX_FMT_YUV420P;
    f.data[0] = bl_y;
    f.data[1] = bl_u;
    f.data[2] = bl_v;
    f.linesize[0] = (int)w;
    f.linesize[1] = f.linesize[2] = (int)(cw / 2);
    PicOut t = {ys, cs, pitch, pitch_c, bh, w, sh};
    memset(big, 0xEE, BIGMEM);
    bl_layout = 1;
    bl_walks = 0;
    CHECK(write_pic(&f, &t));
    CHECK(bl_walks <= (cw + 15) / 16 + h + (h + 1) / 2);
    memset(bl_want, 0xEE, sizeof bl_want);
    for (u32 y = 0; y < h; y++)
        for (u32 x = 0; x < w; x++) bl_want[ys - BIG + bl_offset(x, y, 0, pitch, sh, bh, 0)] = bl_y[y * w + x];
    for (u32 y = 0; y < (h + 1) / 2; y++)
        for (u32 x = 0; x < cw / 2; x++) {
            bl_want[cs - BIG + bl_offset(2 * x, y, 0, pitch_c, ch, bh, 0)] = bl_u[y * (cw / 2) + x];
            bl_want[cs - BIG + bl_offset(2 * x + 1, y, 0, pitch_c, ch, bh, 0)] = bl_v[y * (cw / 2) + x];
        }
    bl_layout = 0;
    CHECK(memcmp(big, bl_want, BIGMEM) == 0);
}
static void block_linear_test(void) {
    block_linear_picture(1920, 1080, 1920, 1920, 1080, 4);
    block_linear_picture(33, 17, 64, 128, 32, 0); /* one GOB a block: rows cross blocks; a wider chroma plane */
}
int main(void) {
    packet_tests();
    odd_size_test();
    block_linear_test();
    submission_tests();
    vp9_tests();
    vp8_tests();
    printf("NVDEC packets: %u checks passed (%s)\n", checks, av_version_info());
    return 0;
}
