/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.apache and NOTICE;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
   This fork's changes are under GPL-3.0-or-later, the license of the fork as a whole (LICENSE). */
/* NVDEC: H.264 decoding with FFmpeg's libavcodec. The game passes no SPS or PPS: they are rebuilt from the picture
   setup struct (nvdec_h264_pic_s, its iova in register 0x404). Each NVDEC channel has its own decoder, and each
   picture goes into the output surface its job names, where VIC reads it */
#include "nx/gpu/video/host1x_int.h"
#include "nx/gpu/video/vp9_rebuild.h"
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/log.h>

/* ------------------------------------------------------------ H.264: SPS/PPS rebuilt from nvdec_h264_pic_s */
typedef struct {
    u8* p;
    u32 cap, bytes;
    u32 acc;
    int nb;
} BW;
static void bw_bit(BW* w, u32 b) {
    w->acc = (w->acc << 1) | (b & 1);
    if (++w->nb == 8) {
        if (w->bytes < w->cap) w->p[w->bytes++] = (u8)w->acc;
        w->acc = 0;
        w->nb = 0;
    }
}
static void bw_u(BW* w, u32 v, int n) {
    for (int i = n - 1; i >= 0; i--) bw_bit(w, (v >> i) & 1);
}
static void bw_ue(BW* w, u32 v) {
    u32 x = v + 1;
    int n = 0;
    while ((x >> n) > 1) n++;
    for (int i = 0; i < n; i++) bw_bit(w, 0);
    bw_u(w, x, n + 1);
}
static void bw_se(BW* w, i32 v) { bw_ue(w, v > 0 ? (u32)(2 * v - 1) : (u32)(-2 * v)); }
static void bw_trailing(BW* w) {
    bw_bit(w, 1);
    while (w->nb) bw_bit(w, 0);
}
/* start code + emulation prevention bytes. 0 when it doesn't fit in cap */
static u32 nal_out(u8* dst, u32 cap, u8 hdr, const u8* rbsp, u32 n) {
    u32 o = 0, i = 0;
    if (cap < 5) return 0;
    dst[o++] = 0;
    dst[o++] = 0;
    dst[o++] = 0;
    dst[o++] = 1;
    dst[o++] = hdr;
    int z = 0;
    for (; i < n && o + 2 < cap; i++) {
        if (z >= 2 && rbsp[i] <= 3) {
            dst[o++] = 3;
            z = 0;
        }
        dst[o++] = rbsp[i];
        z = rbsp[i] == 0 ? z + 1 : 0;
    }
    return i == n ? o : 0;
}
static i32 sext(u32 v, int bits) { return (i32)(v << (32 - bits)) >> (32 - bits); }
static const u8 zz4[16] = {0, 1, 4, 8, 5, 2, 3, 6, 9, 12, 13, 10, 7, 11, 14, 15};
static const u8 zz8[64] = {0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,  12, 19, 26, 33, 40, 48,
                           41, 34, 27, 20, 13, 6,  7,  14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23,
                           30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};
static void scaling_list(BW* w, const u8* raster, int n, const u8* zz) {
    int last = 8;
    for (int j = 0; j < n; j++) {
        int v = raster[zz[j]];
        int d = v - last;
        if (d > 127) d -= 256;
        if (d < -128) d += 256;
        bw_se(w, d);
        last = v;
    }
}
static u32 build_headers(const u8* ps, u8* out, u32 cap) {
    u8 rb[1024];
    BW w;
    u32 o = 0;
    u32 log2_poc = rd32(ps, 0x58), dpoc0 = rd32(ps, 0x5C), fmo = rd32(ps, 0x60), wmb = rd32(ps, 0x64),
        hmb = rd32(ps, 0x68);
    u32 ecm = rd32(ps, 0x70), pop = rd32(ps, 0x74), l0 = rd32(ps, 0x78), l1 = rd32(ps, 0x7C), dfc = rd32(ps, 0x80),
        rpc = rd32(ps, 0x84), t8 = rd32(ps, 0x88);
    u32 b0 = rd32(ps, 0xB0), b1 = rd32(ps, 0xB4), bl = rd32(ps, 0x2D0);
    u32 mbaff = b0 & 1, d8 = (b0 >> 1) & 1, wp = (b0 >> 2) & 1, cip = (b0 >> 3) & 1, l2fn = (b0 >> 8) & 15,
        cfi = (b0 >> 12) & 3, poct = (b0 >> 14) & 3;
    i32 qp26 = sext((b0 >> 16) & 63, 6), cqo = sext((b0 >> 22) & 31, 5), cqo2 = sext((b0 >> 27) & 31, 5);
    u32 wbi = b1 & 3, qpz = (bl >> 1) & 1;
    /* SPS, High profile so the PPS may use 8x8 transform / scaling lists */
    memset(&w, 0, sizeof w);
    w.p = rb;
    w.cap = sizeof rb;
    bw_u(&w, 100, 8);
    bw_u(&w, 0, 8);
    bw_u(&w, 51, 8);
    bw_ue(&w, 0);
    bw_ue(&w, cfi ? cfi : 1);
    if (cfi == 3) bw_u(&w, 0, 1); /* separate_colour_plane_flag */
    bw_ue(&w, 0);
    bw_ue(&w, 0);
    bw_u(&w, qpz, 1);
    bw_u(&w, 0, 1);
    bw_ue(&w, l2fn);
    bw_ue(&w, poct);
    if (poct == 0)
        bw_ue(&w, log2_poc);
    else if (poct == 1) {
        bw_u(&w, dpoc0, 1);
        bw_se(&w, 0);
        bw_se(&w, 0);
        bw_ue(&w, 0);
    }
    bw_ue(&w, 16);
    bw_u(&w, 0, 1);
    bw_ue(&w, wmb ? wmb - 1 : 0);
    bw_ue(&w, fmo ? (hmb ? hmb - 1 : 0) : (hmb / 2 ? hmb / 2 - 1 : 0));
    bw_u(&w, fmo, 1);
    if (!fmo) bw_u(&w, mbaff, 1);
    bw_u(&w, d8, 1);
    bw_u(&w, 0, 1);
    bw_u(&w, 0, 1);
    bw_trailing(&w);
    u32 sn = nal_out(out + o, cap - o, 0x67, rb, w.bytes);
    if (!sn) return 0;
    o += sn;
    /* PPS, scaling matrices taken from the pic setup */
    memset(&w, 0, sizeof w);
    w.p = rb;
    w.cap = sizeof rb;
    bw_ue(&w, 0);
    bw_ue(&w, 0);
    bw_u(&w, ecm, 1);
    bw_u(&w, pop, 1);
    bw_ue(&w, 0);
    bw_ue(&w, l0);
    bw_ue(&w, l1);
    bw_u(&w, wp, 1);
    bw_u(&w, wbi, 2);
    bw_se(&w, qp26);
    bw_se(&w, 0);
    bw_se(&w, cqo);
    bw_u(&w, dfc, 1);
    bw_u(&w, cip, 1);
    bw_u(&w, rpc, 1);
    bw_u(&w, t8, 1);
    bw_u(&w, 1, 1);
    for (int i = 0; i < 6; i++) {
        bw_u(&w, 1, 1);
        scaling_list(&w, ps + 0x1C0 + i * 16, 16, zz4);
    }
    if (t8)
        for (int i = 0; i < 2; i++) {
            bw_u(&w, 1, 1);
            scaling_list(&w, ps + 0x220 + i * 64, 64, zz8);
        }
    bw_se(&w, cqo2);
    bw_trailing(&w);
    u32 pn = nal_out(out + o, cap - o, 0x68, rb, w.bytes);
    if (!pn) return 0;
    return o + pn;
}

/* ------------------------------------------------------------ a decoder per channel */
/* NVDEC writes each picture into the surface its job names as it decodes it, in decode order, and the game shows the
   surfaces in display order itself. FFmpeg hands pictures back in display order instead, some jobs later, and not at
   all for one it judges out of order: so the picture is taken from the decoder's own buffer for it (get_buffer2),
   once the job's packet is decoded, and FFmpeg's output is drained and dropped */
typedef struct {
    u64 luma, chroma;     /* iovas */
    u32 pitch_y, pitch_c; /* the planes' widths in bytes */
    u32 bh;               /* GOBs per block, log2 */
    u32 w, h;             /* the picture in pixels */
} PicOut;
struct NvdecCtx {
    /* first, where FFmpeg's patched VP9 decoder reads them through the context's opaque (cmake/ffmpeg-vp9-nvdec.patch):
       0x534e5856 ("SNXV"), whether the picture before this one was hidden, which NVDEC says only in the next one, and
       the job's probability table (nvdec_vp9EntropyProbs_t), for what its rebuilt compressed header can't carry */
    u32 vp9_hint_magic;
    u32 vp9_previous_invisible;
    u8 vp9_probs[0xEA0];
    u32 app;            /* the codec the decoder was opened for (SET_APPLICATION_ID): 3 H.264, 9 VP9 */
    SnxVp9Builder* vp9; /* VP9's headers rebuilt from each job, with the reference state they carry */
    AVCodecContext* dec;
    AVPacket* pkt;
    AVFrame* frame; /* FFmpeg's output, dropped */
    AVFrame* cur;   /* a reference to the buffer of the picture the decoder started last */
    int cur_new;    /* started during this job */
    /* SPS + PPS from the last successful job: full scaling lists with emulation prevention come to about 800 */
    u8 last_hdr[1024];
    u32 last_hdr_n;
    int fail; /* the decoder could not be opened */
};
static u32 g_vframes; /* pictures written, all channels: only for the log */
NvdecCtx* nvdec_new(void) {
    NvdecCtx* d = calloc(1, sizeof(NvdecCtx));
    if (d) d->vp9_hint_magic = 0x534e5856;
    return d;
}
void nvdec_free(NvdecCtx* d) {
    if (!d) return;
    /* the references and the pictures the decoder still holds go with it: the game closed the stream */
    avcodec_free_context(&d->dec);
    av_packet_free(&d->pkt);
    av_frame_free(&d->frame);
    av_frame_free(&d->cur);
    snx_vp9_free(d->vp9);
    free(d);
}
/* FFmpeg's messages go to the game log at warning level and above, a limited number: by default they go to stderr
   at info level, outside the log, without end */
static void ff_log(void* avcl, int level, const char* fmt, va_list vl) {
    static int n, pfx = 1;
    if (level > av_log_get_level() || __atomic_load_n(&n, __ATOMIC_RELAXED) > 20) return;
    int k = __atomic_fetch_add(&n, 1, __ATOMIC_RELAXED);
    if (k > 20) return;
    if (k == 20) {
        LOG("FFmpeg: more messages not shown\n");
        return;
    }
    char line[512];
    av_log_format_line2(avcl, level, fmt, vl, line, sizeof line, &pfx);
    LOG("FFmpeg: %s%s", line, line[0] && line[strlen(line) - 1] == '\n' ? "" : "\n");
}
void ffmpeg_log_init(void) {
    av_log_set_level(AV_LOG_WARNING);
    av_log_set_callback(ff_log);
}
/* the decoder starts a picture (the last one it starts in a job is the job's own: missing reference frames it makes up
   come before it): keep a reference to its buffer, which the decoder fills during the job */
static int get_buf(AVCodecContext* c, AVFrame* f, int flags) {
    NvdecCtx* d = c->opaque;
    int r = avcodec_default_get_buffer2(c, f, flags);
    if (r >= 0) {
        av_frame_unref(d->cur);
        r = av_frame_ref(d->cur, f);
        d->cur_new = r >= 0;
    }
    return r;
}
/* the decoder for the job's codec: SET_APPLICATION_ID (register 0x200), which the game's TVMR library writes before
   every job, is 3 for H.264 and 9 for VP9 (VP8 is 5, HEVC 7). A channel that changes codec gets a new decoder */
static int dec_open(NvdecCtx* d, u32 app) {
    if (d->dec && d->app == app) return 1;
    if (d->fail) return 0;
    if (app != 3 && app != 9) {
        LOG_LIMIT(4, "video: NVDEC job for codec %u skipped, only H.264 (3) and VP9 (9) are decoded\n", app);
        return 0;
    }
    if (d->dec) {
        avcodec_free_context(&d->dec);
        av_packet_free(&d->pkt);
        av_frame_free(&d->frame);
        av_frame_free(&d->cur);
        snx_vp9_free(d->vp9);
        d->vp9 = NULL;
        d->last_hdr_n = 0;
    }
    ffmpeg_log_init();
    const AVCodec* codec = avcodec_find_decoder(app == 3 ? AV_CODEC_ID_H264 : AV_CODEC_ID_VP9);
    d->dec = codec ? avcodec_alloc_context3(codec) : NULL;
    d->pkt = av_packet_alloc();
    d->frame = av_frame_alloc();
    d->cur = av_frame_alloc();
    if (app == 9) d->vp9 = snx_vp9_new();
    if (d->dec) {
        /* After send + receive have consumed this job, its captured buffer is complete: frame threading could still
           be decoding it when the surface is written. Slice threads end inside the packet: VP9's tile columns, up to
           four at 1080p, decode side by side. An H.264 picture of the videos games carry is one slice */
        d->dec->thread_type = FF_THREAD_SLICE;
        d->dec->thread_count = app == 9 ? 4 : 1;
        d->dec->opaque = d;
        d->dec->get_buffer2 = get_buf;
    }
    if (!d->dec || !d->pkt || !d->frame || !d->cur || (app == 9 && !d->vp9) || avcodec_open2(d->dec, codec, NULL) < 0) {
        d->fail = 1;
        avcodec_free_context(&d->dec);
        av_packet_free(&d->pkt);
        av_frame_free(&d->frame);
        av_frame_free(&d->cur);
        snx_vp9_free(d->vp9);
        d->vp9 = NULL;
        LOG_LIMIT(1, "video: the FFmpeg %s decoder could not be opened\n", app == 3 ? "H.264" : "VP9");
        return 0;
    }
    d->app = app;
    LOG_LIMIT(1, "video: %s decoder ready (FFmpeg %s, NVDEC jobs are decoded on the CPU)\n", app == 3 ? "H.264" : "VP9",
              av_version_info());
    return 1;
}
static u32 nal_length(const u8* p) { return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; }
static int nal_header(u8 b) { return !(b & 0x80) && (b & 31) && (b & 31) < 24; }
/* NVDEC jobs normally contain Annex B. Also accept complete four-byte-length-prefixed access units and a
   single bare NAL. Validate the whole length-prefixed input before selecting it: a length of 256..511 starts
   with 00 00 01 too. Never turn a truncated length-prefixed packet into a made-up bare NAL. */
static int make_packet(AVPacket* pkt, const u8* hdr, u32 hn, const u8* bs, u32 slen) {
    u32 p = 0;
    while (slen - p >= 4) {
        u32 n = nal_length(bs + p);
        if (!n || n > slen - p - 4 || !nal_header(bs[p + 4])) break;
        p += 4 + n;
    }
    int lengths = slen && p == slen;
    u32 zeros = 0;
    while (zeros < slen && !bs[zeros]) ++zeros;
    int annexb = zeros >= 2 && zeros + 1 < slen && bs[zeros] == 1 && nal_header(bs[zeros + 1]);
    if (!slen || (!lengths && !annexb && !nal_header(bs[0]))) return AVERROR_INVALIDDATA;
    /* av_new_packet owns/refcounts the storage and reserves AV_INPUT_BUFFER_PADDING_SIZE extra bytes.
       Shrinking after assembly zeros that padding at the actual end, including on shorter subsequent jobs. */
    av_packet_unref(pkt);
    int err = av_new_packet(pkt, (int)(hn + slen + 3));
    if (err < 0) return err;
    u8* out = pkt->data;
    memcpy(out, hdr, hn);
    u32 o = hn;
    if (lengths) {
        for (p = 0; p < slen;) {
            u32 n = nal_length(bs + p);
            memcpy(out + o, "\0\0\0\1", 4);
            memcpy(out + o + 4, bs + p + 4, n);
            o += 4 + n;
            p += 4 + n;
        }
    } else {
        if (!annexb) {
            memcpy(out + o, "\0\0\1", 3);
            o += 3;
        }
        memcpy(out + o, bs, slen);
        o += slen;
    }
    av_shrink_packet(pkt, (int)o);
    return 0;
}
/* EAGAIN is the normal end of a drain, not a decode error. EOF is unexpected: these per-channel decoders are
   never flushed with a NULL packet between jobs, because that would discard reference pictures. */
static int drain_frames(NvdecCtx* d) {
    int err;
    while ((err = avcodec_receive_frame(d->dec, d->frame)) >= 0) av_frame_unref(d->frame);
    return err == AVERROR(EAGAIN) ? 0 : err;
}
static int submit_packet(NvdecCtx* d) {
    d->cur_new = 0;
    int err = avcodec_send_packet(d->dec, d->pkt);
    if (err == AVERROR(EAGAIN)) {
        /* The packet was NOT consumed. Retire the previous output, then retry the same owned packet once.
           Any get_buffer2 callbacks while draining belong to that older job, not this one's surface. */
        err = drain_frames(d);
        if (d->cur_new) av_frame_unref(d->cur);
        d->cur_new = 0;
        if (err < 0) return err;
        err = avcodec_send_packet(d->dec, d->pkt);
    }
    if (err < 0) return err;
    return drain_frames(d);
}
/* bytes of a block linear plane `pitch` bytes wide and h rows high: whole GOBs (64 bytes by 8 rows), in blocks of
   8 << bh rows */
static u64 bl_size(u32 pitch, u32 h, u32 bh) {
    u32 gy = 8u << bh;
    return (u64)((pitch + 63) & ~63u) * ((h + gy - 1) / gy * gy);
}
/* one row of a plane, n bytes from s: bl_offset(x, y) is bl_offset(x, 0) + bl_offset(0, y), so the row's 16-byte
   runs land at base plus the plane's column table, a whole run in one 16-byte move */
static void row_out(u8* p, const u64* col, u64 base, const u8* s, u32 n) {
    u32 k = 0;
    for (; (k + 1) * 16 <= n; k++) memcpy(p + base + col[k], s + k * 16, 16);
    if (k * 16 < n) memcpy(p + base + col[k], s + k * 16, n - k * 16);
}
/* The picture into its surface as NVDEC writes it: a luma plane and one interleaved chroma plane (NV12), from
   FFmpeg's planar 4:2:0. 1 when written */
static int write_pic(const AVFrame* f, const PicOut* t) {
    u32 w = (u32)f->width, h = (u32)f->height;
    int sy = f->linesize[0], sc = f->linesize[1], sv = f->linesize[2];
    const u8* const* pl = (const u8* const*)f->data;
    if (f->format != AV_PIX_FMT_YUV420P && f->format != AV_PIX_FMT_YUVJ420P) {
        LOG_LIMIT(1, "video: the decoder gave pixel format %d, not 4:2:0, which NVDEC can't write\n", f->format);
        return 0;
    }
    /* the surface bounds what is written: the picture setup's size and the planes' widths */
    if (w > t->w) w = t->w;
    if (h > t->h) h = t->h;
    if (w > t->pitch_y) w = t->pitch_y;
    if ((w + 1) / 2 * 2 > t->pitch_c) w = t->pitch_c & ~1u;
    if (w > 4096) w = 4096;
    u32 ch = (t->h + 1) / 2;
    u8* py = iova_ptr(t->luma, bl_size(t->pitch_y, t->h, t->bh));
    u8* pc = iova_ptr(t->chroma, bl_size(t->pitch_c, ch, t->bh));
    if (!py || !pc) {
        LOG_LIMIT(4, "video: NVDEC output surface (luma iova %llx, chroma %llx) not mapped, picture dropped\n",
                  (unsigned long long)t->luma, (unsigned long long)t->chroma);
        return 0;
    }
    /* 4:2:0 chroma covers pixel pairs: an odd width or height has a last pair, or row, of one pixel */
    u32 cw = (w + 1) / 2 * 2;
    /* a column's offset depends on the block height alone: the two planes' rows share one table */
    u64 col[256]; /* a row's runs: 4096 bytes at most */
    for (u32 k = 0; k * 16 < cw; k++) col[k] = bl_offset(k * 16, 0, 0, t->pitch_y, t->h, t->bh, 0);
    for (u32 y = 0; y < h; y++)
        row_out(py, col, bl_offset(0, y, 0, t->pitch_y, t->h, t->bh, 0), pl[0] + (u64)y * sy, w);
    u8 row[4096];
    for (u32 y = 0; y < (h + 1) / 2; y++) {
        const u8* u = pl[1] + (u64)y * sc;
        const u8* v = pl[2] + (u64)y * sv;
        for (u32 x = 0; x < cw / 2; x++) {
            row[2 * x] = u[x];
            row[2 * x + 1] = v[x];
        }
        row_out(pc, col, bl_offset(0, y, 0, t->pitch_c, ch, t->bh, 0), row, cw);
    }
    g_vframes++;
    return 1;
}
/* nvdec_status_s, its iova in register 0x424. Players check it: an all-zero status means the frame failed */
static void put_status(const u32* r, u32 mbs, int failed) {
    u64 st_iova = (u64)r[0x424 / 4] << 8;
    u8* sp = st_iova ? iova_ptr(st_iova, 16) : NULL;
    if (!sp) return;
    u32 v[4] = {failed ? 0 : mbs, failed ? mbs : 0, 100000, failed ? 1 : 0};
    memcpy(sp, v, 16);
}
static void fail_job(NvdecCtx* d, const u32* r, u32 mbs) {
    /* A failed first field must not let a later second-field job publish a stale picture. Resend the parameter
       sets too: constructing/submitting them is not proof that the decoder accepted them. */
    if (d->cur) av_frame_unref(d->cur);
    d->cur_new = 0;
    d->last_hdr_n = 0;
    if (d->pkt) av_packet_unref(d->pkt);
    put_status(r, mbs, 1);
}
/* a VP9 job: the picture setup (0x100 bytes at register 0x404) and the probability table (register 0x5C0) name what
   the frame body (register 0x408) lacks, its uncompressed and compressed headers, which snx_vp9_compose rebuilds into
   a packet FFmpeg decodes; the picture goes to the surface the setup names */
static void vp9_execute(NvdecCtx* d, const u32* r) {
    u64 ps_iova = (u64)r[0x404 / 4] << 8, probs_iova = (u64)r[0x5C0 / 4] << 8;
    u8* ps = iova_ptr(ps_iova, 0x100);
    u8* probs = iova_ptr(probs_iova, 0xEA0);
    if (!ps || !probs) {
        LOG_LIMIT(4, "video: VP9 picture setup or probability table not mapped (%llx, %llx)\n",
                  (unsigned long long)ps_iova, (unsigned long long)probs_iova);
        fail_job(d, r, 0);
        return;
    }
    u32 len = rd32(ps, 0x30), w = (u32)ps[0x60] | ((u32)ps[0x61] << 8), h = (u32)ps[0x62] | ((u32)ps[0x63] << 8);
    u32 mbs = ((w + 15) / 16) * ((h + 15) / 16);
    u8* body = len && len <= (16u << 20) ? iova_ptr((u64)r[0x408 / 4] << 8, len) : NULL;
    if (!body) {
        LOG_LIMIT(4, "video: VP9 frame body not mapped or invalid (%u bytes)\n", len);
        fail_job(d, r, mbs);
        return;
    }
    SnxVp9Frame packet;
    if (!snx_vp9_compose(d->vp9, ps, probs, body, len, r, &packet)) {
        LOG_LIMIT(4, "video: VP9 frame setup or body invalid, job skipped\n");
        fail_job(d, r, mbs);
        return;
    }
    av_packet_unref(d->pkt);
    int err = av_new_packet(d->pkt, (int)packet.size);
    if (err >= 0) {
        memcpy(d->pkt->data, packet.data, packet.size);
        d->vp9_previous_invisible = !packet.previous_visible;
        memcpy(d->vp9_probs, probs, sizeof d->vp9_probs); /* the packet is decoded within submit_packet */
        err = submit_packet(d);
    }
    av_packet_unref(d->pkt);
    if (err < 0) {
        char reason[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(err, reason, sizeof reason);
        LOG_LIMIT(8, "video: VP9 decoder rejected packet: %s\n", reason);
    }
    PicOut pt = {0};
    pt.luma = packet.luma;
    pt.chroma = packet.chroma;
    pt.pitch_y = packet.pitch_y;
    pt.pitch_c = packet.pitch_c;
    pt.bh = ((packet.layout >> 2) & 7) + 1;
    pt.w = packet.width;
    pt.h = packet.height;
    int wrote = 0;
    if (err >= 0 && d->cur_new && d->cur->buf[0]) {
        if (packet.layout & 3)
            LOG_LIMIT(2, "video: VP9 output tile format %u is not handled\n", packet.layout & 3);
        else if (!pt.luma || !pt.chroma)
            LOG_LIMIT(4, "video: VP9 output surface has no address\n");
        else
            wrote = write_pic(d->cur, &pt);
    }
    /* a picture that writes nothing (one only shown again later) still decoded: only a decode error fails the job */
    if (err < 0)
        fail_job(d, r, mbs);
    else
        put_status(r, mbs, 0);
    {
        static u32 jobs;
        jobs++;
        if (jobs <= 4 || jobs % 120 == 0)
            LOG("video: VP9 job %u: %ux%u, %u body bytes, decode %s, surface %s (luma %llx)\n", jobs, w, h, len,
                err < 0 ? "failed" : "ok", wrote ? "written" : "not written", (unsigned long long)pt.luma);
    }
}
void nvdec_execute(NvdecCtx* d, const u32* r) {
    u32 app = r[0x200 / 4];
    if (!dec_open(d, app)) {
        fail_job(d, r, 0);
        return;
    }
    if (app == 9) {
        vp9_execute(d, r);
        return;
    }
    u64 ps_iova = (u64)r[0x404 / 4] << 8, bs_iova = (u64)r[0x408 / 4] << 8;
    u8* ps = iova_ptr(ps_iova, 0x300);
    if (!ps) {
        LOG_LIMIT(4, "video: NVDEC picture setup at iova %llx not mapped\n", (unsigned long long)ps_iova);
        fail_job(d, r, 0);
        return;
    }
    u32 wmb = rd32(ps, 0x64), hmb = rd32(ps, 0x68), mbs = wmb * hmb;
    u32 slen = rd32(ps, 0x48);
    if (!slen || slen > (16u << 20)) {
        LOG_LIMIT(4, "video: NVDEC job with a %u byte bitstream skipped\n", slen);
        fail_job(d, r, mbs);
        return;
    }
    u8* bs = iova_ptr(bs_iova, slen);
    if (!bs) {
        LOG_LIMIT(4, "video: NVDEC bitstream at iova %llx (%u bytes) not mapped\n", (unsigned long long)bs_iova, slen);
        fail_job(d, r, mbs);
        return;
    }
    u8 hdr[sizeof d->last_hdr];
    u32 hn = build_headers(ps, hdr, sizeof hdr);
    if (!hn) {
        LOG_LIMIT(1, "video: NVDEC SPS and PPS don't fit in %u bytes, job skipped\n", (unsigned)sizeof hdr);
        fail_job(d, r, mbs);
        return;
    }
    u32 prefix = (hn != d->last_hdr_n || memcmp(hdr, d->last_hdr, hn)) ? hn : 0;
    int err = make_packet(d->pkt, hdr, prefix, bs, slen);
    if (err >= 0) err = submit_packet(d);
    av_packet_unref(d->pkt);
    if (err >= 0) {
        memcpy(d->last_hdr, hdr, hn);
        d->last_hdr_n = hn;
    }
    /* Where the picture goes: CurrPicIdx (bits 2..8 of +0xB4) picks one of the 17 surfaces, and the picture setup says
       how NVDEC lays it out. tileFormat (+0x6C bits 0..1) 0, the only one the game's TVMR library writes, is block
       linear with 2 << gob_height (bits 2..4) GOBs per block. pitch_luma and pitch_chroma (+0x8C, +0x90) are the
       planes' widths in bytes, and the frame offsets (+0x9C, +0xA8, in 256 bytes) are added to the surface's.
       A field pair is one picture: the second field's job (field_pic_flag and second_field, bits 5 and 7 of +0xB0)
       writes it again, complete */
    u32 idx = (rd32(ps, 0xB4) >> 2) & 0x7F, lay = rd32(ps, 0x6C), b0 = rd32(ps, 0xB0);
    PicOut pt = {0};
    if (idx < 17) {
        pt.luma = ((u64)r[0x430 / 4 + idx] + rd32(ps, 0x9C)) << 8;
        pt.chroma = ((u64)r[0x474 / 4 + idx] + rd32(ps, 0xA8)) << 8;
    }
    pt.pitch_y = rd32(ps, 0x8C);
    pt.pitch_c = rd32(ps, 0x90);
    pt.bh = ((lay >> 2) & 7) + 1;
    pt.w = wmb * 16;
    pt.h = hmb * 16;
    int wrote = 0;
    if (err >= 0 && (d->cur_new || (b0 & 0xA0) == 0xA0) && d->cur->buf[0]) {
        if (lay & 3)
            LOG_LIMIT(1, "video: NVDEC output tile format %u isn't handled (block linear, 0, is), picture dropped\n",
                      lay & 3);
        else if (idx >= 17 || !pt.luma || !pt.chroma)
            LOG_LIMIT(4, "video: NVDEC job names output surface %u, which has no address, picture dropped\n", idx);
        else
            wrote = write_pic(d->cur, &pt);
    }
    if (err < 0 || !wrote)
        fail_job(d, r, mbs);
    else
        put_status(r, mbs, 0);
    {
        static u32 njobs;
        njobs++;
        if (njobs % 120 == 0) LOG("video: %u NVDEC jobs so far, %u pictures decoded\n", njobs, g_vframes);
    }
    {
        static int said;
        if (said < 3 || (err < 0 && said < 8)) {
            said++;
            char es[AV_ERROR_MAX_STRING_SIZE] = "ok";
            if (err < 0) av_strerror(err, es, sizeof es);
            LOG("video: NVDEC H.264 job: %ux%u MBs, %u bytes, decoder %s, picture %s surface %u (luma iova %llx, "
                "pitch %u, block height %u)\n",
                wmb, hmb, slen, es, wrote ? "written to" : "not written to", idx, (unsigned long long)pt.luma,
                pt.pitch_y, 1u << pt.bh);
        }
    }
}
