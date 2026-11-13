/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The native video service's AV1 decoder (hle/services/native_video_decoder.c) on AOMedia's test vectors, the way the
   device gives: with Vulkan Video where it decodes AV1 (the test turns vulkan_video on), on the CPU with dav1d
   elsewhere, as black pictures in a build without dav1d. Each picture is written into an NV12 video image by the
   service's own write (native_video_image_write), read back, and held to the MD5 the vector's list gives for it.
   A monochrome vector, which Vulkan Video refuses on any device, has to come out whole from the decoder that takes
   over: its first packet is one the Vulkan decoder took. Run alone, by vulkan_video_av1 (SNX_TEST_AV1_VECTORS: a file
   of paths, a line each, every vector of av1_vectors in its order and after it the list of its pictures' MD5s) */
#include "nx/hle/services/native_video.h"
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/md5.h>

#define AV1_IMAGE     (HEAP_BASE + 0x100000) /* the video image a picture is written into */
#define AV1_IMAGE_MAX 0x100000u              /* the most bytes of one */
#define AV1_WIDTH_MAX 512u                   /* the widest picture of a vector here */
#define AV1_LIST_MAX  16u                    /* the most pictures of a vector here */
#define AV1_IVF_HEAD  32u                    /* an IVF file's header, and a frame's: its size, then its timestamp */
#define AV1_IVF_FRAME 12u

static const char* const av1_ways[] = {"none", "the CPU's decoder", "Vulkan Video", "the black stand-in",
                                         "VideoToolbox"};
/* the vectors: three Vulkan Video takes where the driver decodes AV1 Main (two keyframes; pictures predicted from
   others; film grain, which the decoder applies), and a monochrome one, which it never takes */
static const char* const av1_vectors[] = {"av1-1-b8-01-size-66x66.ivf", "av1-1-b8-05-mv.ivf",
                                          "av1-1-b8-23-film_grain-50.ivf", "av1-1-b8-24-monochrome.ivf"};
#define AV1_VECTORS (sizeof av1_vectors / sizeof *av1_vectors)
#define AV1_REFUSED (AV1_VECTORS - 1)

typedef struct {
    u32 first_way, last_way, pictures;
} Av1Run;

static u32 av1_le(const u8* p, u32 bytes) {
    u32 v = 0;
    for (u32 i = 0; i < bytes; i++) v |= (u32)p[i] << (8 * i);
    return v;
}
/* the MD5s of a vector's list, a line each (32 hex digits, then the picture's name): how many, 0 for a broken list */
static u32 av1_list(const char* path, u8 md5[AV1_LIST_MAX][16]) {
    size_t size = 0;
    u8* text = snx_read_file(path, &size);
    u32 n = 0;
    for (size_t at = 0; text && at + 32 <= size && n < AV1_LIST_MAX; n++) {
        for (u32 i = 0; i < 16; i++) {
            unsigned byte = 0;
            if (sscanf((const char*)text + at + 2 * i, "%2x", &byte) != 1) {
                free(text);
                return 0;
            }
            md5[n][i] = (u8)byte;
        }
        while (at < size && text[at] != '\n') at++;
        at++;
    }
    free(text);
    return n;
}
/* the Ounce's NV12 video image a picture of w by h is written into (native_video_pool_create's, kind 1), at
   AV1_IMAGE */
static int av1_pool(u32 w, u32 h, NativeVideoPool* p) {
    const u32 pref[16] = {0, 1, (w - 1) | (h - 1) << 16};
    u64 size = 0;
    *p = (NativeVideoPool){.first = 1, .count = 1, .memory = 1, .cpu = AV1_IMAGE};
    return !nvideo_layout(pref, 1, &p->layout, &size) && size <= AV1_IMAGE_MAX;
}
/* f's MD5 as the lists take a picture: its Y rows, then U's, then V's, a monochrome picture's chroma as 0x80. A 4:2:0
   picture goes through its image first, written as the service writes a codec's output: 0 when it couldn't be
   written, or is another format */
static int av1_picture_md5(const AVFrame* f, u8 out[16]) {
    u32 w = (u32)f->width, h = (u32)f->height, cw = (w + 1) / 2, ch = (h + 1) / 2;
    u8 line[AV1_WIDTH_MAX];
    NativeVideoPool pool;
    struct AVMD5* m;
    if (w > AV1_WIDTH_MAX || !(m = av_md5_alloc())) return 0;
    av_md5_init(m);
    if (f->format == AV_PIX_FMT_GRAY8) {
        for (u32 y = 0; y < h; y++) av_md5_update(m, f->data[0] + (size_t)y * (size_t)f->linesize[0], w);
        memset(line, 0x80, cw);
        for (u32 y = 0; y < 2 * ch; y++) av_md5_update(m, line, cw);
        av_md5_final(m, out);
        av_free(m);
        return 1;
    }
    if (!av1_pool(w, h, &pool)) {
        av_free(m);
        return 0;
    }
    memset(GP(AV1_IMAGE), 0x5a, pool.layout.image_bytes);
    if (!native_video_image_write(&pool, pool.first, f)) {
        av_free(m);
        return 0;
    }
    guest_done(AV1_IMAGE, pool.layout.image_bytes);
    for (u32 y = 0; y < h; y++) av_md5_update(m, GP(AV1_IMAGE + (u64)y * pool.layout.pitch), w);
    for (u32 plane = 0; plane < 2; plane++)
        for (u32 y = 0; y < ch; y++) {
            const u8* row = GP(AV1_IMAGE + pool.layout.chroma_offset + (u64)y * pool.layout.pitch);
            for (u32 x = 0; x < cw; x++) line[x] = row[2 * x + plane];
            av_md5_update(m, line, cw);
        }
    av_md5_final(m, out);
    av_free(m);
    return 1;
}
/* limited range black, the stand-in's picture, all over the picture of w by h in its image */
static int av1_image_black(u32 w, u32 h) {
    NativeVideoPool pool;
    if (!av1_pool(w, h, &pool)) return 0;
    for (u32 y = 0; y < h; y++)
        for (u32 x = 0; x < w; x++)
            if (GP(AV1_IMAGE + (u64)y * pool.layout.pitch)[x] != 16) return 0;
    for (u32 y = 0; y < (h + 1) / 2; y++)
        for (u32 x = 0; x < 2 * ((w + 1) / 2); x++)
            if (GP(AV1_IMAGE + pool.layout.chroma_offset + (u64)y * pool.layout.pitch)[x] != 128) return 0;
    return 1;
}
/* the vector at path (its list at list_path) through a decoder opened with hw (the device for Vulkan Video, taken
   over; NULL for none), fed as the service's pump feeds it: a packet when it asks for one, then the end of the
   stream. Every picture has its packet's timestamp and the list's MD5 (the stand-in's: black), and none is missing */
static int av1_run(const char* path, const char* list_path, void* hw, Av1Run* run) {
    u8 md5[AV1_LIST_MAX][16], got[16];
    size_t size = 0, at = AV1_IVF_HEAD;
    u8* data = snx_read_file(path, &size);
    u32 listed = av1_list(list_path, md5), packets = 0, ended = 0;
    NativeVideoDecoder d;
    AVFrame* frame = av_frame_alloc();
    AVPacket* pkt = av_packet_alloc();
    if (!data || size < AV1_IVF_HEAD || memcmp(data, "DKIF", 4) || !listed) {
        fprintf(stderr, "vulkan_video_av1: %s is no IVF file, or %s no list of its pictures' MD5s\n", path, list_path);
        return 1;
    }
    CHECK(frame && pkt);
    u32 w = av1_le(data + 12, 2), h = av1_le(data + 14, 2);
    CHECK(nvideo_decoder_open(&d, 0, NVIDEO_MIME_AV1, w, h, NULL, 0, hw, "the test's device has no AV1 decode queue"));
    *run = (Av1Run){.first_way = d.way};
    for (;;) {
        int r = nvideo_decoder_receive(&d, frame);
        if (!r) {
            CHECK(frame->pts == (i64)run->pictures);
            CHECK(av1_picture_md5(frame, got));
            if (d.way == NVIDEO_DECODER_BLACK)
                CHECK(av1_image_black(w, h));
            else if (run->pictures >= listed || memcmp(got, md5[run->pictures], 16)) {
                fprintf(stderr, "vulkan_video_av1: %s: picture %u of %u, from %s, is not the list's\n", path,
                        run->pictures + 1, listed, av1_ways[d.way]);
                return 1;
            }
            run->pictures++;
            av_frame_unref(frame);
            continue;
        }
        if (r == AVERROR_EOF) break;
        CHECK(r == AVERROR(EAGAIN) && !ended);
        if (at + AV1_IVF_FRAME <= size) {
            u32 bytes = av1_le(data + at, 4);
            CHECK(bytes && at + AV1_IVF_FRAME + bytes <= size && av_new_packet(pkt, (int)bytes) >= 0);
            memcpy(pkt->data, data + at + AV1_IVF_FRAME, bytes);
            pkt->pts = packets++;
            at += AV1_IVF_FRAME + bytes;
            CHECK(nvideo_decoder_send(&d, pkt) >= 0);
            av_packet_unref(pkt);
        } else {
            CHECK(nvideo_decoder_send(&d, NULL) >= 0);
            ended = 1;
        }
    }
    run->last_way = d.way;
    if (run->pictures != packets || packets != listed) {
        fprintf(stderr, "vulkan_video_av1: %s: %u pictures of %u packets came out, %u listed (%s, then %s)\n", path,
                run->pictures, packets, listed, av1_ways[run->first_way], av1_ways[run->last_way]);
        return 1;
    }
    nvideo_decoder_close(&d);
    av_frame_free(&frame);
    av_packet_free(&pkt);
    free(data);
    return 0;
}
/* the way this host decodes AV1 first: the Apple media engine where it has one and the setting allows it, else Vulkan
   Video where the caller's device has a decode queue for it, else the CPU's decoder */
static u32 av1_first_way(const void* hw) {
#if defined(__APPLE__)
    if (g_settings.apple_video_decode && nvideo_vt_available()) return NVIDEO_DECODER_VT;
#endif
    if (hw) return NVIDEO_DECODER_VULKAN;
    return avcodec_find_decoder_by_name("libdav1d") ? NVIDEO_DECODER_CPU : NVIDEO_DECODER_BLACK;
}
/* whether Vulkan Video is asked first here: the media engine is, where this host has one */
static int av1_vulkan_first(void) {
#if defined(__APPLE__)
    if (g_settings.apple_video_decode && nvideo_vt_available()) return 0;
#endif
    return 1; /* elsewhere the device is asked first, and the loop below skips the one without a queue */
}
static int check_video_av1(const char* paths_file) {
    char *path[AV1_VECTORS] = {0}, *list[AV1_VECTORS] = {0};
    const u32 cpu_way = avcodec_find_decoder_by_name("libdav1d") ? NVIDEO_DECODER_CPU : NVIDEO_DECODER_BLACK;
    /* what the pictures of a run were held to: the media engine's and the CPU's have the list's MD5, the stand-in's
       are black */
    const char* as_black = "black where the stand-in made them", *as = "each with its list's MD5";
    Av1Run run;
    size_t size = 0;
    char* text = (char*)snx_read_file(paths_file, &size);
    u32 found = 0;
    for (size_t at = 0; text && at < size && found < 2 * AV1_VECTORS; found++) {
        size_t n = strcspn(text + at, "\n");
        if (!n || !strstr(text + at, av1_vectors[found / 2])) break;
        *(found & 1 ? &list[found / 2] : &path[found / 2]) = snx_format("%.*s", (int)n, text + at);
        at += n + 1;
    }
    free(text);
    if (found != 2 * AV1_VECTORS) {
        fprintf(stderr,
                "vulkan_video_av1: SNX_TEST_AV1_VECTORS=%s has %u of the %u paths it needs: each vector's, then its "
                "MD5 list's, a line each, the next one %s's\n",
                paths_file, found, (u32)(2 * AV1_VECTORS), av1_vectors[found / 2 < AV1_VECTORS ? found / 2 : 0]);
        return 2;
    }
    CHECK(!check_video_decode_gate());
    /* the device's way from the first packet to the last: nothing these three need is refused */
    for (u32 i = 0; i < AV1_REFUSED; i++) {
        void* hw = g_renderer->nvdec_hw_device(NVDEC_HW_AV1);
        u32 way = av1_first_way(hw);
        CHECK(!hw == !(g_vk.video_codecs & VK_VIDEO_CODEC_OPERATION_DECODE_AV1_BIT_KHR));
        CHECK(!av1_run(path[i], list[i], hw, &run));
        if (run.first_way != way || run.last_way != way) {
            fprintf(stderr, "vulkan_video_av1: %s decoded with %s, then %s: %s expected for both\n", path[i],
                    av1_ways[run.first_way], av1_ways[run.last_way], av1_ways[way]);
            return 1;
        }
        printf("video AV1: %s: %u pictures, %s, from %s\n", av1_vectors[i], run.pictures,
               run.last_way == NVIDEO_DECODER_BLACK ? as_black : as, av1_ways[way]);
    }
    /* the refused one, on the renderer's device where it has one for AV1, and on a Vulkan device FFmpeg makes for
       itself, which every machine this runs on has: the stream is refused before any device is asked. The media
       engine, where this host has one, is asked before either, so there the vector only has its pictures checked */
    for (u32 own = 0; own < 2 && av1_vulkan_first(); own++) {
        AVBufferRef* hw = own ? NULL : g_renderer->nvdec_hw_device(NVDEC_HW_AV1);
        if (own) CHECK(av_hwdevice_ctx_create(&hw, AV_HWDEVICE_TYPE_VULKAN, NULL, NULL, 0) >= 0);
        if (!hw) continue;
        CHECK(!av1_run(path[AV1_REFUSED], list[AV1_REFUSED], hw, &run));
        if (run.first_way != NVIDEO_DECODER_VULKAN || run.last_way != cpu_way) {
            fprintf(stderr, "vulkan_video_av1: %s decoded with %s, then %s: Vulkan Video, then %s expected\n",
                    path[AV1_REFUSED], av1_ways[run.first_way], av1_ways[run.last_way], av1_ways[cpu_way]);
            return 1;
        }
        printf("video AV1: %s: %u pictures, %s, from %s after Vulkan Video refused it (%s device)\n",
               av1_vectors[AV1_REFUSED], run.pictures, run.last_way == NVIDEO_DECODER_BLACK ? as_black : as,
               av1_ways[cpu_way], own ? "FFmpeg's own" : "the renderer's");
    }
    if (!av1_vulkan_first()) {
        CHECK(!av1_run(path[AV1_REFUSED], list[AV1_REFUSED], NULL, &run));
        printf("video AV1: %s: %u pictures, %s, from %s (the media engine was asked before Vulkan Video)\n",
               av1_vectors[AV1_REFUSED], run.pictures, run.last_way == NVIDEO_DECODER_BLACK ? as_black : as,
               av1_ways[run.last_way]);
    }
    for (u32 i = 0; i < AV1_VECTORS; i++) free(path[i]), free(list[i]);
    return 0;
}
