/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported from Eden (src/video_core/host1x/codecs/h264.cpp, codecs/vp8.cpp, codecs/decoder.cpp, nvdec.cpp,
   nvdec_common.h, ffmpeg.cpp, codec_types.h):
   SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
   SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
   SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
   SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project */
/* NVDEC: H.264, VP8 and VP9 jobs decoded with FFmpeg's libavcodec, each picture written into the surface its job
   names, on the GPU (the renderer's nvdec_write). host1x.c's channel worker calls nvdec_execute when the stream writes
   Execute, with the channel's method registers (nvdec_regs.h). FFmpeg decodes on the CPU, or H.264 with Vulkan Video
   on the renderer's GPU when the vulkan_video setting asks for it (experimental and untested).

   NVDEC takes a job's slice data alone, with the stream's parameters in its picture info, so H.264's sequence and
   picture parameter sets are rebuilt from it (Eden's H264::ComposeFrame and H264BitWriter); vp9_rebuild.cpp does the
   same for VP9's headers, and vp8_frame_header for the few bytes before a VP8 frame's first partition.

   NVDEC writes a picture into its job's surface as it decodes it, in decode order, and the game shows surfaces in
   display order on its own. FFmpeg hands pictures out in display order, later, or not at all for one it judges out
   of order, so its output is drained and dropped: what reaches the surface is the buffer the decoder allocated for
   the picture it started during this job (get_buf keeps a reference to the last one; references FFmpeg makes up for
   missing pictures start before it).

   FFmpeg is a hard dependency: a build without the decoder once left every movie black without a word */
#include "nx/gpu/video/host1x_int.h"
#include "nx/gpu/video/nvdec_regs.h"
#include "nx/gpu/renderer.h"
#include "nx/gpu/video/vp9_rebuild.h"
#include "common/utilities/bits.h"
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/log.h>

#define CTX_MAGIC  0x534E5856u /* "SNXV": what cmake/ffmpeg-vp9-nvdec.patch looks for at the context's start */
#define HDR_CAP    1024u       /* an SPS and a PPS with full scaling lists and emulation prevention take ~800 */
#define MAX_STREAM (16u << 20) /* the largest bitstream a job may name */
#define MAX_DIM    4096u       /* the largest picture width or height NVDEC decodes */
#define MB_SIZE    16u         /* a macroblock is 16x16 pixels (VP9's superblock sizes are counted in them too) */
#define FF_LOG_MAX 20u
/* the cycle count a status reports: players only check that it is there */
#define STATUS_CYCLES 100000u
/* a GOB, the unit of the block linear layout: 64 bytes wide, 8 rows high, stacked 1 << gob_height high in a block */
#define GOB_WIDTH       64u
#define GOB_HEIGHT_LOG2 3u
#define MAX_GOB_HEIGHT  5u /* blocks of up to 32 GOBs */

/* The patched VP9 decoder reads the first three fields through AVCodecContext.opaque: they stay first, in order */
struct NvdecCtx {
    u32 magic;
    u32 prev_invisible;            /* the previous picture was not shown: NVDEC says so only in the next job's setup */
    u8 probs[VP9_PROB_TABLE_SIZE]; /* the job's probability table, which the rebuilt compressed header can't carry */
    u32 app;                       /* the codec the decoder was opened for (0 none). dec NULL with app set: it failed */
    int cur_new;                   /* cur was started during this job */
    u32 last_hdr_n;                /* the SPS and PPS the decoder last accepted (0: send them with the next job) */
    AVCodecContext* dec;
    SnxVp9Builder* vp9; /* VP9's reference state across jobs */
    AVPacket* pkt;
    AVFrame* cur; /* the last buffer the decoder started */
    AVFrame* out; /* FFmpeg's output, dropped */
    u8 last_hdr[HDR_CAP];
};

/* where a picture goes: NV12 planes, block linear, widths in bytes */
typedef struct {
    u64 luma, chroma;
    u32 pitch_y, pitch_c, bh, w, h; /* bh: the GOB block height log2; w, h: the surface's picture size */
} PicOut;

/* ---- FFmpeg's log */
/* the lines logged, by any decoding thread, and whether the callback is set, each on a cache line of its own */
static struct {
    u32 logged;
    char pad[60];
    int init;
} g_ff;
static void ff_log(void* avcl, int level, const char* fmt, va_list vl) {
    char line[512];
    int prefix = 1;
    size_t len;
    u32 n;
    if (level > AV_LOG_WARNING || __atomic_load_n(&g_ff.logged, __ATOMIC_RELAXED) > FF_LOG_MAX) return;
    n = __atomic_fetch_add(&g_ff.logged, 1, __ATOMIC_RELAXED);
    if (n > FF_LOG_MAX) return;
    if (n == FF_LOG_MAX) {
        WARN("ffmpeg: more messages not shown\n");
        return;
    }
    line[0] = 0;
    av_log_format_line2(avcl, level, fmt, vl, line, (int)sizeof line, &prefix);
    len = strlen(line);
    while (len && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = 0;
    WARN("ffmpeg: %s\n", line);
}
void ffmpeg_log_init(void) {
    if (__atomic_exchange_n(&g_ff.init, 1, __ATOMIC_ACQ_REL)) return;
    av_log_set_level(AV_LOG_WARNING);
    av_log_set_callback(ff_log);
}
static void ff_warn(const char* what, int err) {
    char msg[AV_ERROR_MAX_STRING_SIZE];
    msg[0] = 0;
    av_strerror(err, msg, sizeof msg);
    WARN_LIMIT(16, "video: NVDEC %s: %s\n", what, msg);
}

/* ---- H.264 bitstream writing (Eden's H264BitWriter), into an RBSP, then NAL units */
/* NAL unit types (H.264 Table 7-1: 24 to 31 are unspecified) and the header byte: forbidden_zero_bit, nal_ref_idc
   in bits 5-6, the type in 0-4 */
enum { H264_NAL_SLICE = 1, H264_NAL_IDR_SLICE = 5, H264_NAL_SPS = 7, H264_NAL_PPS = 8, H264_NAL_LAST_RESERVED = 23 };
#define H264_NAL_REF_IDC_HIGHEST 3u
static u32 h264_nal_header(u32 ref_idc, u32 type) { return ref_idc << 5 | type; }
static u32 h264_nal_type(u8 header) { return header & 0x1Fu; }
static int h264_nal_forbidden_bit(u8 header) { return header >> 7; }
/* the start code before each NAL unit in an Annex B stream (H.264 Annex B), and the byte that keeps two zeros and a
   byte up to 3 in the RBSP from looking like one (emulation_prevention_three_byte, 7.4.1) */
#define H264_START_CODE_SIZE  4u
#define H264_EMULATION_PREVENTION 0x03u

typedef struct {
    u8* buf;
    u32 cap, bytes, bit_pos; /* whole bytes written, bits of the one being filled (from its top) */
    int overflow;            /* a bit didn't fit */
} H264BitWriter;
static void h264_write_bit(H264BitWriter* w, u32 b) {
    if (w->bytes >= w->cap) {
        w->overflow = 1;
        return;
    }
    if (!w->bit_pos) w->buf[w->bytes] = 0;
    if (b) w->buf[w->bytes] |= (u8)(0x80u >> w->bit_pos);
    if (++w->bit_pos == 8) {
        w->bit_pos = 0;
        w->bytes++;
    }
}
/* u(n): the n low bits of v, the highest first */
static void h264_write_bits(H264BitWriter* w, u32 v, u32 n) {
    while (n--) h264_write_bit(w, (v >> n) & 1);
}
/* ue(v), unsigned Exp-Golomb (9.1): len zeros, then v + 1 in len + 1 bits. v + 1 is taken in 64 bits, so v up to
   0xFFFFFFFF codes */
static void h264_write_ue(H264BitWriter* w, u32 v) {
    u64 x = (u64)v + 1;
    u32 len = 0;
    int i;
    while (x >> (len + 1)) len++;
    h264_write_bits(w, 0, len);
    for (i = (int)len; i >= 0; i--) h264_write_bit(w, (u32)(x >> i) & 1);
}
/* se(v), signed Exp-Golomb (9.1.1): k > 0 codes as 2k - 1, k <= 0 as -2k */
static void h264_write_se(H264BitWriter* w, int v) {
    h264_write_ue(w, v > 0 ? 2u * (u32)v - 1 : 2u * (0u - (u32)v));
}
/* rbsp_trailing_bits: a one, then zeros to the byte's end (Eden's End) */
static void h264_write_end(H264BitWriter* w) {
    h264_write_bit(w, 1);
    while (w->bit_pos) h264_write_bit(w, 0);
}
/* a NAL unit: a 4-byte start code, the header byte, the RBSP with emulation prevention. Its length, 0 when it
   doesn't fit cap */
static u32 h264_nal_unit(u8* dst, u32 cap, u32 header, const u8* rbsp, u32 n) {
    u32 o = H264_START_CODE_SIZE + 1, zeros = 0, i;
    if (cap < o) return 0;
    dst[0] = dst[1] = dst[2] = 0;
    dst[3] = 1;
    dst[4] = (u8)header;
    for (i = 0; i < n; i++) {
        if (zeros >= 2 && rbsp[i] <= H264_EMULATION_PREVENTION) {
            if (o >= cap) return 0;
            dst[o++] = H264_EMULATION_PREVENTION;
            zeros = 0;
        }
        if (o >= cap) return 0;
        dst[o++] = rbsp[i];
        zeros = rbsp[i] ? 0 : zeros + 1;
    }
    return o;
}
/* scaling_list() (7.3.2.1.1.1), every entry sent: NVDEC keeps a list in raster order, the stream codes it in
   zig-zag order as deltas from the last entry, wrapped to a signed byte, from 8. The frame zig-zag scans of
   H.264 Tables 8-12 and 8-13 (Eden's zig-zag tables, from libavcodec) */
static void h264_write_scaling_list(H264BitWriter* w, const u8* list, u32 n) {
    static const u8 zig_zag_scan_4x4[16] = {0, 1, 4, 8, 5, 2, 3, 6, 9, 12, 13, 10, 7, 11, 14, 15};
    static const u8 zig_zag_scan_8x8[64] = {0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,
                                            12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6,  7,  14, 21, 28,
                                            35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
                                            58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};
    int last_scale = 8;
    u32 i;
    for (i = 0; i < n; i++) {
        int v = list[n == 16 ? zig_zag_scan_4x4[i] : zig_zag_scan_8x8[i]], delta_scale = v - last_scale;
        if (delta_scale > 127) delta_scale -= 256;
        if (delta_scale < -128) delta_scale += 256;
        h264_write_se(w, delta_scale);
        last_scale = v;
    }
}

/* The SPS's profile and level: High (100), so 8x8 transforms and scaling matrices are allowed, at level 5.1 (51),
   high enough for any picture size and reference count a job can name (Eden writes 31) */
#define H264_PROFILE_HIGH 100u
#define H264_LEVEL_5_1    51u
#define H264_CHROMA_444   3u /* chroma_format_idc of 4:4:4: its SPS has separate_colour_plane_flag */
#define H264_MAX_REF_FRAMES 16u
#define H264_MIN_REF_FRAMES 4u /* Eden's floor */

/* the SPS (7.3.2.1.1) and PPS (7.3.2.2) of the H264DecoderContext at s (Eden's H264::ComposeFrame). Their length in
   out, 0 when the setup can't make them or they don't fit */
static u32 h264_compose_parameter_sets(const u8* s, u8* out, u32 cap) {
    u8 rb[HDR_CAP];
    H264BitWriter w = {rb, sizeof rb, 0, 0, 0};
    u32 lo = rd32(s, H264_FLAGS), hi = rd32(s, H264_FLAGS + 4);
    u32 chroma_format_idc = h264_chroma_format_idc(lo), pic_order_cnt_type = h264_pic_order_cnt_type(lo);
    u32 pic_width_in_mbs = rd32(s, H264_PIC_WIDTH_IN_MBS), frame_height_in_mbs = rd32(s, H264_FRAME_HEIGHT_IN_MBS);
    u32 frame_mbs_only = rd32(s, H264_FRAME_MBS_ONLY_FLAG) != 0;
    u32 transform_8x8 = rd32(s, H264_TRANSFORM_8X8_MODE_FLAG) != 0;
    u32 l0 = rd32(s, H264_NUM_REFIDX_L0_DEFAULT_ACTIVE), l1 = rd32(s, H264_NUM_REFIDX_L1_DEFAULT_ACTIVE);
    u32 max_num_ref_frames, n, k, i;
    if (!pic_width_in_mbs || !frame_height_in_mbs || pic_width_in_mbs > MAX_DIM / MB_SIZE ||
        frame_height_in_mbs > MAX_DIM / MB_SIZE || (!frame_mbs_only && frame_height_in_mbs < 2))
        return 0;
    /* as many references as either list's default holds, from Eden's floor of 4 to the standard's 16 */
    max_num_ref_frames = snx_max_u32(l0, l1) < H264_MAX_REF_FRAMES - 1 ? snx_max_u32(l0, l1) + 1 : H264_MAX_REF_FRAMES;
    if (max_num_ref_frames < H264_MIN_REF_FRAMES) max_num_ref_frames = H264_MIN_REF_FRAMES;
    h264_write_bits(&w, H264_PROFILE_HIGH, 8); /* profile_idc */
    h264_write_bits(&w, 0, 8);                 /* constraint_set flags, reserved_zero_2bits */
    h264_write_bits(&w, H264_LEVEL_5_1, 8);    /* level_idc */
    h264_write_ue(&w, 0);                      /* seq_parameter_set_id */
    h264_write_ue(&w, chroma_format_idc);
    if (chroma_format_idc == H264_CHROMA_444) h264_write_bit(&w, 0); /* separate_colour_plane_flag */
    h264_write_ue(&w, 0);                                            /* bit_depth_luma_minus8 */
    h264_write_ue(&w, 0);                                            /* bit_depth_chroma_minus8 */
    h264_write_bit(&w, h264_qpprime_y_zero_transform_bypass(rd32(s, H264_TRANSFORM_BYPASS)));
    h264_write_bit(&w, 0); /* seq_scaling_matrix_present_flag: the PPS carries them */
    h264_write_ue(&w, h264_log2_max_frame_num_minus4(lo));
    h264_write_ue(&w, pic_order_cnt_type);
    if (pic_order_cnt_type == 0)
        h264_write_ue(&w, rd32(s, H264_LOG2_MAX_PIC_ORDER_CNT_LSB_MINUS4));
    else if (pic_order_cnt_type == 1) {
        h264_write_bit(&w, rd32(s, H264_DELTA_PIC_ORDER_ALWAYS_ZERO_FLAG) != 0);
        h264_write_se(&w, 0); /* offset_for_non_ref_pic */
        h264_write_se(&w, 0); /* offset_for_top_to_bottom_field */
        h264_write_ue(&w, 0); /* num_ref_frames_in_pic_order_cnt_cycle */
    }
    h264_write_ue(&w, max_num_ref_frames);
    h264_write_bit(&w, 0); /* gaps_in_frame_num_value_allowed_flag */
    h264_write_ue(&w, pic_width_in_mbs - 1);
    /* pic_height_in_map_units_minus1: field pictures count map units of two macroblock rows */
    h264_write_ue(&w, (frame_mbs_only ? frame_height_in_mbs : frame_height_in_mbs / 2) - 1);
    h264_write_bit(&w, frame_mbs_only);
    if (!frame_mbs_only) h264_write_bit(&w, h264_mbaff_frame(lo)); /* mb_adaptive_frame_field_flag */
    h264_write_bit(&w, h264_direct_8x8_inference(lo));
    h264_write_bit(&w, 0); /* frame_cropping_flag */
    h264_write_bit(&w, 0); /* vui_parameters_present_flag */
    h264_write_end(&w);
    if (w.overflow ||
        !(n = h264_nal_unit(out, cap, h264_nal_header(H264_NAL_REF_IDC_HIGHEST, H264_NAL_SPS), rb, w.bytes)))
        return 0;

    w = (H264BitWriter){rb, sizeof rb, 0, 0, 0}; /* the PPS's RBSP, in the same buffer */
    h264_write_ue(&w, 0); /* pic_parameter_set_id */
    h264_write_ue(&w, 0); /* seq_parameter_set_id */
    h264_write_bit(&w, rd32(s, H264_ENTROPY_CODING_MODE_FLAG) != 0);
    h264_write_bit(&w, rd32(s, H264_PIC_ORDER_PRESENT_FLAG) != 0); /* bottom_field_pic_order_in_frame_present */
    h264_write_ue(&w, 0);                                          /* num_slice_groups_minus1 */
    h264_write_ue(&w, l0);                                         /* num_ref_idx_l0_default_active_minus1 */
    h264_write_ue(&w, l1);
    h264_write_bit(&w, h264_weighted_pred(lo));
    h264_write_bits(&w, h264_weighted_bipred_idc(hi), 2);
    h264_write_se(&w, snx_sext32(h264_pic_init_qp_minus26_bits(lo), H264_PIC_INIT_QP_MINUS26_BITS));
    h264_write_se(&w, 0); /* pic_init_qs_minus26 */
    h264_write_se(&w, snx_sext32(h264_chroma_qp_index_offset_bits(lo), H264_CHROMA_QP_INDEX_OFFSET_BITS));
    h264_write_bit(&w, rd32(s, H264_DEBLOCKING_FILTER_CONTROL_PRESENT_FLAG) != 0);
    h264_write_bit(&w, h264_constrained_intra_pred(lo));
    h264_write_bit(&w, rd32(s, H264_REDUNDANT_PIC_CNT_PRESENT_FLAG) != 0);
    h264_write_bit(&w, transform_8x8); /* transform_8x8_mode_flag */
    h264_write_bit(&w, 1);             /* pic_scaling_matrix_present_flag */
    for (i = 0; i < H264_SCALING_LISTS_4X4; i++) {
        h264_write_bit(&w, 1); /* pic_scaling_list_present_flag */
        h264_write_scaling_list(&w, s + H264_WEIGHT_SCALE_4X4 + 16 * i, 16);
    }
    if (transform_8x8)
        for (i = 0; i < (chroma_format_idc == H264_CHROMA_444 ? 6u : H264_SCALING_LISTS_8X8); i++) {
            /* NVDEC has two 8x8 lists: 4:4:4's chroma ones fall back to them */
            h264_write_bit(&w, i < H264_SCALING_LISTS_8X8);
            if (i < H264_SCALING_LISTS_8X8) h264_write_scaling_list(&w, s + H264_WEIGHT_SCALE_8X8 + 64 * i, 64);
        }
    h264_write_se(&w, snx_sext32(h264_second_chroma_qp_index_offset_bits(lo), H264_CHROMA_QP_INDEX_OFFSET_BITS));
    h264_write_end(&w);
    if (w.overflow ||
        !(k = h264_nal_unit(out + n, cap - n, h264_nal_header(H264_NAL_REF_IDC_HIGHEST, H264_NAL_PPS), rb, w.bytes)))
        return 0;
    return n + k;
}

/* ---- packets */
/* a header byte a slice data NAL unit could have: forbidden bit clear, a type from 1 to 23 */
static int nal_ok(u8 b) {
    return !h264_nal_forbidden_bit(b) && h264_nal_type(b) >= H264_NAL_SLICE &&
           h264_nal_type(b) <= H264_NAL_LAST_RESERVED;
}
#define AVCC_LENGTH_SIZE 4u /* length-prefixed NAL units: a 4-byte big-endian length before each */
/* complete length-prefixed NAL units, end to end */
static int is_avcc(const u8* s, u32 n) {
    u32 o = 0;
    while (o < n) {
        u32 len;
        if (n - o < AVCC_LENGTH_SIZE + 1) return 0;
        len = snx_ld_be32(s + o);
        if (!len || len > n - o - AVCC_LENGTH_SIZE || !nal_ok(s[o + AVCC_LENGTH_SIZE])) return 0;
        o += AVCC_LENGTH_SIZE + len;
    }
    return n != 0;
}
/* Annex B: at least two zeros, a one, then a NAL header */
static int is_annexb(const u8* s, u32 n) {
    u32 i = 0;
    while (i < n && !s[i]) i++;
    return i >= 2 && i + 1 < n && s[i] == 1 && nal_ok(s[i + 1]);
}
/* the job's packet: hn bytes of headers, then the bitstream as Annex B. It comes as Annex B, as length-prefixed NAL
   units (tried first: a length of 256..511 starts 00 00 01) or as one bare NAL unit. 0 or an AVERROR */
static int make_packet(AVPacket* p, const u8* hdr, u32 hn, const u8* s, u32 n) {
    enum { PACKET_ANNEX_B, PACKET_AVCC, PACKET_BARE_NAL } kind;
    int r;
    u32 size, o, i;
    av_packet_unref(p);
    if (is_avcc(s, n))
        kind = PACKET_AVCC;
    else if (is_annexb(s, n))
        kind = PACKET_ANNEX_B;
    else if (n && nal_ok(s[0]))
        kind = PACKET_BARE_NAL;
    else
        return AVERROR_INVALIDDATA;
    size = hn + n + (kind == PACKET_BARE_NAL ? 3 : 0); /* a 3-byte start code before a bare NAL unit */
    if ((r = av_new_packet(p, (int)size)) < 0) return r;
    memcpy(p->data, hdr, hn);
    o = hn;
    if (kind == PACKET_ANNEX_B)
        memcpy(p->data + o, s, n);
    else if (kind == PACKET_BARE_NAL) {
        p->data[o] = p->data[o + 1] = 0;
        p->data[o + 2] = 1;
        memcpy(p->data + o + 3, s, n);
    } else /* each length becomes a 4-byte start code */
        for (i = 0; i < n;) {
            u32 len = snx_ld_be32(s + i);
            p->data[o] = p->data[o + 1] = p->data[o + 2] = 0;
            p->data[o + 3] = 1;
            memcpy(p->data + o + AVCC_LENGTH_SIZE, s + i + AVCC_LENGTH_SIZE, len);
            o += AVCC_LENGTH_SIZE + len;
            i += AVCC_LENGTH_SIZE + len;
        }
    return 0;
}

/* ---- the picture into its surface */
/* NV12 planes, block linear, laid out on the GPU (the renderer's nvdec_write), which keeps them there for VIC
   (docs/layers/gpu/ownership.md). 1 when written */
static int write_pic(const AVFrame* f, const PicOut* t) {
    NvdecPic p = {0};
    u64 gob_rows;
    u8 *py, *pc;
    if (f->format != AV_PIX_FMT_YUV420P && f->format != AV_PIX_FMT_YUVJ420P && f->format != AV_PIX_FMT_VULKAN) {
        WARN_LIMIT(4, "video: NVDEC picture format %d isn't written (4:2:0 only)\n", f->format);
        return 0;
    }
    if (f->width <= 0 || f->height <= 0 || t->bh > MAX_GOB_HEIGHT) return 0;
    p.w = snx_min_u32(snx_min_u32((u32)f->width, t->w),
                      snx_min_u32(snx_min_u32(t->pitch_y, t->pitch_c / 2 * 2), MAX_DIM));
    p.h = snx_min_u32(snx_min_u32((u32)f->height, t->h), MAX_DIM);
    if (!p.w || !p.h) return 0;
    /* the surfaces mapped for their whole block linear extent: whole GOBs wide, whole blocks high */
    gob_rows = 1u << (GOB_HEIGHT_LOG2 + t->bh); /* a block's rows */
    p.ext_y = ((u64)t->pitch_y + GOB_WIDTH - 1) / GOB_WIDTH * GOB_WIDTH * ((t->h + gob_rows - 1) / gob_rows * gob_rows);
    p.ext_c = ((u64)t->pitch_c + GOB_WIDTH - 1) / GOB_WIDTH * GOB_WIDTH *
              (((t->h + 1) / 2 + gob_rows - 1) / gob_rows * gob_rows);
    py = iova_ptr(t->luma, p.ext_y);
    pc = iova_ptr(t->chroma, p.ext_c);
    if (!py || !pc) {
        WARN_LIMIT(8, "video: NVDEC surface %llx/%llx isn't mapped, picture dropped\n", (unsigned long long)t->luma,
                   (unsigned long long)t->chroma);
        return 0;
    }
    p.luma = (u64)(py - g_mem);
    p.chroma = (u64)(pc - g_mem);
    p.pitch_y = t->pitch_y;
    p.pitch_c = t->pitch_c;
    p.surf_h = t->h;
    p.bh = t->bh;
    if (f->format == AV_PIX_FMT_VULKAN)
        p.hw = f; /* decoded with Vulkan Video, in a GPU image */
    else
        for (int i = 0; i < 3; i++) { /* Y, U, V */
            p.plane[i] = f->data[i];
            p.stride[i] = f->linesize[i];
        }
    if (!g_renderer_up || !g_renderer || !g_renderer->nvdec_write) {
        WARN_LIMIT(4, "video: an NVDEC picture before the renderer started, dropped\n");
        return 0;
    }
    return g_renderer->nvdec_write(&p);
}

/* ---- the decoder */
/* FFmpeg's buffer for a picture it starts: kept as this job's */
static int get_buf(AVCodecContext* c, AVFrame* f, int flags) {
    NvdecCtx* d = c->opaque;
    int r = avcodec_default_get_buffer2(c, f, flags);
    if (r < 0) return r;
    av_frame_unref(d->cur);
    d->cur_new = 0;
    if ((r = av_frame_ref(d->cur, f)) < 0) {
        av_frame_unref(f);
        return r;
    }
    d->cur_new = 1;
    return 0;
}
static void dec_close(NvdecCtx* d) {
    if (d->dec) avcodec_free_context(&d->dec);
    snx_vp9_free(d->vp9);
    d->vp9 = NULL;
    av_frame_unref(d->cur);
    av_packet_unref(d->pkt);
    d->cur_new = 0;
    d->last_hdr_n = 0;
    d->prev_invisible = 0;
}
/* the codecs NVDEC jobs are decoded for: FFmpeg's decoder and the name the log gives it */
static int codec_decoded(u32 app) {
    return app == NVDEC_CODEC_H264 || app == NVDEC_CODEC_VP8 || app == NVDEC_CODEC_VP9;
}
static int codec_id(u32 app) {
    return app == NVDEC_CODEC_VP9 ? AV_CODEC_ID_VP9 : app == NVDEC_CODEC_VP8 ? AV_CODEC_ID_VP8 : AV_CODEC_ID_H264;
}
static const char* codec_name(u32 app) {
    return app == NVDEC_CODEC_VP9 ? "VP9" : app == NVDEC_CODEC_VP8 ? "VP8" : "H.264";
}
/* a fresh decoder for codec app (NVDEC_CODEC_*): 1 when it opened. Slice threads only, since frame threads would
   still be decoding a picture when its surface is written; VP9's tile columns decode side by side on four */
#define VP9_THREADS 4 /* 1.8x on 1080p content of four tile columns */
static int dec_open(NvdecCtx* d, u32 app) {
    const AVCodec* codec;
    int r;
    dec_close(d);
    d->app = app;
    ffmpeg_log_init();
    codec = avcodec_find_decoder(codec_id(app));
    if (!codec) {
        WARN("video: FFmpeg has no %s decoder, so the movies stay black\n", codec_name(app));
        return 0;
    }
    if (app == NVDEC_CODEC_VP9 && !(d->vp9 = snx_vp9_new())) return 0;
    if (!(d->dec = avcodec_alloc_context3(codec))) goto fail;
    d->dec->opaque = d;
    d->dec->get_buffer2 = get_buf;
    d->dec->thread_type = FF_THREAD_SLICE;
    d->dec->thread_count = app == NVDEC_CODEC_VP9 ? VP9_THREADS : 1;
    /* H.264 with Vulkan Video on the renderer's GPU when the vulkan_video setting asks for it (experimental). FFmpeg
       decodes a stream its Vulkan decoder can't take on the CPU, as it does without */
    if (app == NVDEC_CODEC_H264 && g_renderer_up && g_renderer && g_renderer->nvdec_hw_device &&
        (d->dec->hw_device_ctx = g_renderer->nvdec_hw_device(NVDEC_HW_H264)))
        LOG_LIMIT(1, "video: H.264 decodes with Vulkan Video (experimental)\n");
    if ((r = avcodec_open2(d->dec, codec, NULL)) < 0) {
        ff_warn("cannot open its decoder", r);
        avcodec_free_context(&d->dec);
        goto fail;
    }
    return 1;
fail:
    snx_vp9_free(d->vp9);
    d->vp9 = NULL;
    return 0;
}
/* FFmpeg's output, dropped. 0 once it has none left (EAGAIN), -1 on an error (EOF is one: it never flushes) */
static int drain(NvdecCtx* d) {
    for (;;) {
        int r = avcodec_receive_frame(d->dec, d->out);
        if (!r) {
            av_frame_unref(d->out);
            continue;
        }
        if (r == AVERROR(EAGAIN)) return 0;
        ff_warn("decoding failed", r);
        return -1;
    }
}
/* the job's packet into the decoder. A full decoder (EAGAIN) is drained and takes it again, once: a buffer started
   while draining is an older job's, so it is let go */
static int send(NvdecCtx* d) {
    int r = avcodec_send_packet(d->dec, d->pkt);
    if (r == AVERROR(EAGAIN)) {
        d->cur_new = 0;
        if (drain(d)) return -1;
        if (d->cur_new) av_frame_unref(d->cur);
        d->cur_new = 0;
        r = avcodec_send_packet(d->dec, d->pkt);
    }
    av_packet_unref(d->pkt);
    if (r < 0) {
        ff_warn("the decoder refused a picture", r);
        return -1;
    }
    return 0;
}

/* nvdec_status_s: the macroblocks decoded and with errors, the cycle count, the error flag. Players take all zero
   for a failure too */
static void put_status(u8* st, int ok, u32 mbs) {
    if (!st) return;
    snx_st_le32(st + NVDEC_STATUS_MBS_CORRECTLY_DECODED, ok ? mbs : 0);
    snx_st_le32(st + NVDEC_STATUS_MBS_IN_ERROR, ok ? 0 : mbs);
    snx_st_le32(st + NVDEC_STATUS_CYCLE_COUNT, STATUS_CYCLES);
    snx_st_le32(st + NVDEC_STATUS_ERROR_STATUS, !ok);
}
/* a failed job: what it captured goes, so a later second field can't publish stale pixels, and the decoder is sent
   the headers again */
static void job_fail(NvdecCtx* d, u8* st, u32 mbs) {
    av_frame_unref(d->cur);
    av_packet_unref(d->pkt);
    d->cur_new = 0;
    d->last_hdr_n = 0;
    put_status(st, 0, mbs);
}
/* an H.264 job (Eden's H264::ComposeFrame and its surface offsets): the parameter sets rebuilt from the picture info,
   the slice data decoded, the picture written into surface CurrPicIdx */
static void h264_job(NvdecCtx* d, const u32* r, u8* st) {
    u8 hdr[HDR_CAP];
    const u8 *s = iova_ptr(nvdec_offset_address(r[NVDEC_PICTURE_INFO_OFFSET]), H264_CONTEXT_SIZE), *bits = NULL;
    u32 mbs = 0, len = 0, hn, prefix, lo, idx, surface_format;
    PicOut t;
    if (s) {
        mbs = rd32(s, H264_PIC_WIDTH_IN_MBS) * rd32(s, H264_FRAME_HEIGHT_IN_MBS);
        len = rd32(s, H264_STREAM_LEN);
    }
    if (len && len <= MAX_STREAM) bits = iova_ptr(nvdec_offset_address(r[NVDEC_FRAME_BITSTREAM_OFFSET]), len);
    if (!bits) {
        WARN_LIMIT(8, "video: an H.264 job's setup or bitstream isn't mapped (length %u)\n", len);
        job_fail(d, st, mbs);
        return;
    }
    if (!(hn = h264_compose_parameter_sets(s, hdr, sizeof hdr))) {
        WARN_LIMIT(8, "video: an H.264 job's parameter sets can't be built\n");
        job_fail(d, st, mbs);
        return;
    }
    /* the parameter sets go first only when they changed since the decoder last took them */
    prefix = hn == d->last_hdr_n && !memcmp(hdr, d->last_hdr, hn) ? 0 : hn;
    if (make_packet(d->pkt, hdr, prefix, bits, len) < 0) {
        WARN_LIMIT(8, "video: an H.264 job's bitstream (%u bytes) is not H.264\n", len);
        job_fail(d, st, mbs);
        return;
    }
    d->cur_new = 0;
    if (send(d)) {
        job_fail(d, st, mbs);
        return;
    }
    if (prefix) {
        memcpy(d->last_hdr, hdr, hn);
        d->last_hdr_n = hn;
    }
    if (drain(d)) {
        job_fail(d, st, mbs);
        return;
    }
    /* a second field completes the picture its first field started */
    lo = rd32(s, H264_FLAGS);
    if (!d->cur_new && !(h264_field_pic(lo) && h264_second_field(lo) && d->cur->buf[0])) {
        WARN_LIMIT(8, "video: an H.264 job started no picture\n");
        job_fail(d, st, mbs);
        return;
    }
    idx = h264_curr_pic_idx(rd32(s, H264_FLAGS + 4));
    surface_format = rd32(s, H264_SURFACE_FORMAT);
    if (idx >= NVDEC_SURFACES || h264_tile_format(surface_format)) {
        WARN_LIMIT(4, "video: an H.264 picture into surface %u, tile format %u, isn't written\n", idx,
                   h264_tile_format(surface_format));
        job_fail(d, st, mbs);
        return;
    }
    /* the surface's offset plus the picture's frame offset in it, both IOVA >> 8 */
    t.luma = nvdec_offset_address((u64)r[NVDEC_SURFACE_LUMA_OFFSETS + idx] + rd32(s, H264_LUMA_FRAME_OFFSET));
    t.chroma = nvdec_offset_address((u64)r[NVDEC_SURFACE_CHROMA_OFFSETS + idx] + rd32(s, H264_CHROMA_FRAME_OFFSET));
    t.pitch_y = rd32(s, H264_PITCH_LUMA);
    t.pitch_c = rd32(s, H264_PITCH_CHROMA);
    t.bh = h264_gob_height(surface_format) + 1; /* gob_height counts from 2 GOBs */
    t.w = rd32(s, H264_PIC_WIDTH_IN_MBS) * MB_SIZE;
    t.h = rd32(s, H264_FRAME_HEIGHT_IN_MBS) * MB_SIZE;
    if (!write_pic(d->cur, &t)) {
        job_fail(d, st, mbs);
        return;
    }
    put_status(st, 1, mbs);
}
/* a VP9 job: the frame's headers rebuilt from the picture info and the probability table (vp9_rebuild.cpp), the
   frame decoded and, when it starts a picture, written into the surface it names */
static void vp9_job(NvdecCtx* d, const u32* r, u8* st) {
    const u8* s = iova_ptr(nvdec_offset_address(r[NVDEC_PICTURE_INFO_OFFSET]), VP9_PICTURE_INFO_SIZE);
    const u8* probs = iova_ptr(nvdec_offset_address(r[NVDEC_VP9_PROB_TAB_BUFFER_OFFSET]), VP9_PROB_TABLE_SIZE);
    const u8* body = NULL;
    u32 mbs = 0, len = 0;
    SnxVp9Frame f;
    PicOut t;
    if (s) {
        mbs = ((snx_ld_le16(s + VP9_CURRENT_FRAME_WIDTH) + MB_SIZE - 1) / MB_SIZE) *
              ((snx_ld_le16(s + VP9_CURRENT_FRAME_HEIGHT) + MB_SIZE - 1) / MB_SIZE);
        len = rd32(s, VP9_BITSTREAM_SIZE);
    }
    if (len && len <= MAX_STREAM) body = iova_ptr(nvdec_offset_address(r[NVDEC_FRAME_BITSTREAM_OFFSET]), len);
    if (!probs || !body) {
        WARN_LIMIT(8, "video: a VP9 job's setup, probabilities or frame (%u bytes) aren't mapped\n", len);
        job_fail(d, st, mbs);
        return;
    }
    if (!snx_vp9_compose(d->vp9, s, probs, body, len, r, &f) || !f.size) {
        WARN_LIMIT(8, "video: a VP9 job's frame can't be rebuilt\n");
        job_fail(d, st, mbs);
        return;
    }
    /* what the patched decoder reads through opaque, for this picture */
    d->prev_invisible = !f.previous_visible;
    memcpy(d->probs, probs, VP9_PROB_TABLE_SIZE);
    av_packet_unref(d->pkt);
    if (av_new_packet(d->pkt, (int)f.size) < 0) {
        job_fail(d, st, mbs);
        return;
    }
    memcpy(d->pkt->data, f.data, f.size);
    d->cur_new = 0;
    if (send(d) || drain(d)) {
        job_fail(d, st, mbs);
        return;
    }
    /* a picture shown only later still decodes into its surface; one decoding nothing new (show_existing_frame)
       leaves the surfaces as they are */
    if (d->cur_new) {
        t.luma = f.luma;
        t.chroma = f.chroma;
        t.pitch_y = f.pitch_y;
        t.pitch_c = f.pitch_c;
        t.bh = h264_gob_height(f.layout) + 1; /* the same surface format word as H.264's */
        t.w = f.width;
        t.h = f.height;
        if (h264_tile_format(f.layout))
            WARN_LIMIT(4, "video: a VP9 picture in tile format %u isn't written\n", h264_tile_format(f.layout));
        else
            write_pic(d->cur, &t);
    }
    put_status(st, 1, mbs);
}

/* VP8's uncompressed data chunk (RFC 6386 9.1), which NVDEC takes in the picture info instead of the frame: a 3-byte
   little endian tag, 0 in bit 0 for a key frame, the version in bits 1-3, show_frame in bit 4, the first partition's
   size in bits 5-23; then, on a key frame, the start code and 16 bits each of width and of height, whose top 2 bits
   (the upscaling NVDEC's picture info has no field for) stay 0 */
#define VP8_TAG_SIZE        3u
#define VP8_KEY_HEADER_SIZE 10u
#define VP8_MAX_VERSION     7u
#define VP8_MAX_FIRST_PART  0x7FFFFu
#define VP8_MAX_FRAME_STRIDE (UINT32_MAX / VP8_FRAME_STRIDE_UNIT) /* the largest whose bytes fit a u32 */
static const u8 vp8_start_code[3] = {0x9D, 0x01, 0x2A};
/* the chunk of a frame NVDEC decodes (Eden's VP8::ComposeFrame) into out, which holds VP8_KEY_HEADER_SIZE: its
   length. show_frame is always set, since NVDEC writes a hidden picture into its surface too and the game picks what
   it shows */
static u32 vp8_frame_header(int key, u32 version, u32 first_part, u32 w, u32 h, u8* out) {
    u32 tag = (key ? 0u : 1u) | version << 1 | 1u << 4 | first_part << 5;
    out[0] = (u8)tag;
    out[1] = (u8)(tag >> 8);
    out[2] = (u8)(tag >> 16);
    if (!key) return VP8_TAG_SIZE;
    memcpy(out + VP8_TAG_SIZE, vp8_start_code, sizeof vp8_start_code);
    snx_st_le16(out + 6, (u16)w);
    snx_st_le16(out + 8, (u16)h);
    return VP8_KEY_HEADER_SIZE;
}
/* a VP8 job: the frame's chunk put back before its partitions, the frame decoded and written into the current
   picture's surface, which the picture info's frame offsets point into (as H.264's) */
static void vp8_job(NvdecCtx* d, const u32* r, u8* st) {
    u64 info = nvdec_offset_address(r[NVDEC_PICTURE_INFO_OFFSET]);
    u64 frame = nvdec_offset_address(r[NVDEC_FRAME_BITSTREAM_OFFSET]);
    const u8 *s = iova_ptr(info, VP8_PICTURE_INFO_SIZE), *body;
    u32 w, h, mbs, len, version, first_part, surface_format, stride_y, stride_c, min_y, min_c, hn;
    u64 luma_offset, chroma_offset;
    u8 hdr[VP8_KEY_HEADER_SIZE];
    int key, e;
    PicOut t;
    if (!s) {
        WARN_LIMIT(8, "video: a VP8 job's picture info at %llx isn't mapped\n", (unsigned long long)info);
        job_fail(d, st, 0);
        return;
    }
    w = snx_ld_le16(s + VP8_FRAME_WIDTH);
    h = snx_ld_le16(s + VP8_FRAME_HEIGHT);
    mbs = ((w + MB_SIZE - 1) / MB_SIZE) * ((h + MB_SIZE - 1) / MB_SIZE);
    len = rd32(s, VP8_VLD_BUFFER_SIZE);
    key = s[VP8_KEY_FRAME] != 0;
    version = s[VP8_VERSION];
    first_part = rd32(s, VP8_FIRST_PART_SIZE);
    if (!len || len > MAX_STREAM || !w || !h || w > MAX_DIM || h > MAX_DIM) {
        WARN_LIMIT(8,
                   "video: a VP8 job of a %ux%u frame of %u bytes isn't decoded: NVDEC takes 1 to %u pixels a side, "
                   "1 to %u bytes\n",
                   w, h, len, MAX_DIM, MAX_STREAM);
        job_fail(d, st, mbs);
        return;
    }
    if (version > VP8_MAX_VERSION || first_part > len || first_part > VP8_MAX_FIRST_PART) {
        WARN_LIMIT(8,
                   "video: a VP8 job's version %u, first partition %u of %u bytes, isn't a frame's: versions 0 to %u, "
                   "a first partition within the frame, of %u bytes at most\n",
                   version, first_part, len, VP8_MAX_VERSION, VP8_MAX_FIRST_PART);
        job_fail(d, st, mbs);
        return;
    }
    if (!(body = iova_ptr(frame, len))) {
        WARN_LIMIT(8, "video: a VP8 job's frame of %u bytes at %llx isn't mapped\n", len, (unsigned long long)frame);
        job_fail(d, st, mbs);
        return;
    }
    hn = vp8_frame_header(key, version, first_part, w, h, hdr);
    av_packet_unref(d->pkt);
    if ((e = av_new_packet(d->pkt, (int)(hn + len))) < 0) {
        ff_warn("cannot hold a VP8 frame", e);
        job_fail(d, st, mbs);
        return;
    }
    memcpy(d->pkt->data, hdr, hn);
    memcpy(d->pkt->data + hn, body, len);
    d->cur_new = 0;
    if (send(d) || drain(d)) {
        job_fail(d, st, mbs);
        return;
    }
    if (!d->cur_new) {
        WARN_LIMIT(8, "video: a VP8 job started no picture\n");
        job_fail(d, st, mbs);
        return;
    }
    /* checked once the frame is decoded, so the frames after it still have it to predict from */
    surface_format = s[VP8_SURFACE_FORMAT];
    stride_y = rd32(s, VP8_FRAME_STRIDE_LUMA);
    stride_c = rd32(s, VP8_FRAME_STRIDE_CHROMA);
    min_y = (w + VP8_FRAME_STRIDE_UNIT - 1) / VP8_FRAME_STRIDE_UNIT;
    min_c = ((w + 1) / 2 * 2 + VP8_FRAME_STRIDE_UNIT - 1) / VP8_FRAME_STRIDE_UNIT;
    if (h264_tile_format(surface_format) || stride_y < min_y || stride_c < min_c || stride_y > VP8_MAX_FRAME_STRIDE ||
        stride_c > VP8_MAX_FRAME_STRIDE) {
        WARN_LIMIT(4,
                   "video: a %ux%u VP8 picture in tile format %u, frame strides of %u and %u (%u bytes each), isn't "
                   "written: block linear (0) only, strides of %u to %u and of %u to %u\n",
                   w, h, h264_tile_format(surface_format), stride_y, stride_c, VP8_FRAME_STRIDE_UNIT, min_y,
                   VP8_MAX_FRAME_STRIDE, min_c, VP8_MAX_FRAME_STRIDE);
        job_fail(d, st, mbs);
        return;
    }
    luma_offset = rd32(s, VP8_LUMA_FRAME_OFFSET);
    chroma_offset = rd32(s, VP8_CHROMA_FRAME_OFFSET);
    if (luma_offset || chroma_offset)
        LOG_LIMIT(1, "video: VP8 pictures go %llx and %llx bytes into their surfaces, as their frame offsets say\n",
                  (unsigned long long)nvdec_offset_address(luma_offset),
                  (unsigned long long)nvdec_offset_address(chroma_offset));
    t.luma = nvdec_offset_address((u64)r[NVDEC_SURFACE_LUMA_OFFSETS + VP8_SURFACE_CURRENT] + luma_offset);
    t.chroma = nvdec_offset_address((u64)r[NVDEC_SURFACE_CHROMA_OFFSETS + VP8_SURFACE_CURRENT] + chroma_offset);
    t.pitch_y = stride_y * VP8_FRAME_STRIDE_UNIT;
    t.pitch_c = stride_c * VP8_FRAME_STRIDE_UNIT;
    t.bh = h264_gob_height(surface_format) + 1; /* gob_height counts from 2 GOBs */
    t.w = w;
    t.h = h;
    if (!write_pic(d->cur, &t)) {
        job_fail(d, st, mbs);
        return;
    }
    put_status(st, 1, mbs);
}

NvdecCtx* nvdec_new(void) {
    NvdecCtx* d = calloc(1, sizeof *d);
    if (!d) return NULL;
    d->magic = CTX_MAGIC;
    d->pkt = av_packet_alloc();
    d->cur = av_frame_alloc();
    d->out = av_frame_alloc();
    if (!d->pkt || !d->cur || !d->out) {
        nvdec_free(d);
        return NULL;
    }
    return d;
}
void nvdec_free(NvdecCtx* d) {
    if (!d) return;
    if (d->cur && d->pkt) dec_close(d);
    av_packet_free(&d->pkt);
    av_frame_free(&d->cur);
    av_frame_free(&d->out);
    free(d);
}
/* Execute: the job of the codec set_codec_id names, its status written at frame_stats_offset */
void nvdec_execute(NvdecCtx* d, const u32* r) {
    u32 app = r[NVDEC_SET_CODEC_ID];
    u8* st = iova_ptr(nvdec_offset_address(r[NVDEC_FRAME_STATS_OFFSET]), NVDEC_STATUS_SIZE);
    if (!d) return;
    /* H.265 isn't decoded: the job fails and the decoder stays as it is */
    if (!codec_decoded(app)) {
        WARN_LIMIT(4, "video: NVDEC jobs of codec %u aren't decoded\n", app);
        put_status(st, 0, 0);
        return;
    }
    /* a channel that changes codec starts over; a decoder that failed to open stays failed */
    if (d->app != app) dec_open(d, app);
    if (!d->dec) {
        job_fail(d, st, 0);
        return;
    }
    if (app == NVDEC_CODEC_H264)
        h264_job(d, r, st);
    else if (app == NVDEC_CODEC_VP8)
        vp8_job(d, r, st);
    else
        vp9_job(d, r, st);
}
