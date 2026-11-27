/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* libvpx's VP9 test vectors as NVDEC jobs. NVDEC takes a frame's headers already parsed, as a picture setup, and the
   probabilities the frame decodes with, as a table: the context it starts from, adapted by the frames before it,
   with its compressed header's updates applied. Only a decoder knows that context, so FFmpeg's VP9 decoder, as
   released (no SNXV context), decodes the vector one frame at a time and its state after each frame is read into
   the job, field by field as nvtegra (the homebrew FFmpeg backend for the console, nvtegra_vp9.c) and Amberlith's
   tools/vp9-jobs.py fill them. Each surface is the lowest one no reference slot holds and the frame before did not
   take, as Amberlith hands them out. This reads FFmpeg's private VP9Context, so it builds against the source and
   configuration FFmpeg was built from (cmake/ffmpeg.cmake) */
#include "nvdec_vp9_jobs.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libavcodec/avcodec.h"
#include "libavcodec/vp9data.h"
#include "libavcodec/vp9dec.h"
#include "libavutil/md5.h"

#define SNX_VP9J_MAX_DIM 4096u

/* a frame: where it is in the vector's data, and the container's block it came in */
typedef struct {
    uint32_t off, len, block;
} SnxVp9jSpan;

/* the frames of a vector, in decode order */
typedef struct {
    SnxVp9jSpan* frames;
    uint32_t count, cap;
} SnxVp9jFrames;

static int snx_vp9j_fail(char* err, size_t cap, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, cap, fmt, ap);
    va_end(ap);
    return -1;
}

static uint8_t* snx_vp9j_read(const char* path, size_t* n) {
    FILE* f = fopen(path, "rb");
    uint8_t* d = NULL;
    long size;
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) == 0 && (size = ftell(f)) >= 0 && fseek(f, 0, SEEK_SET) == 0 &&
        (d = malloc((size_t)size + 1)) != NULL) {
        if (fread(d, 1, (size_t)size, f) != (size_t)size) {
            free(d);
            d = NULL;
        } else {
            d[size] = 0;
            *n = (size_t)size;
        }
    }
    fclose(f);
    return d;
}

static int snx_vp9j_add(SnxVp9jFrames* fr, uint32_t off, uint32_t len, uint32_t block) {
    if (fr->count == fr->cap) {
        uint32_t cap = fr->cap ? fr->cap * 2 : 256;
        SnxVp9jSpan* frames = realloc(fr->frames, cap * sizeof *frames);
        if (!frames) return -1;
        fr->frames = frames;
        fr->cap = cap;
    }
    fr->frames[fr->count].off = off;
    fr->frames[fr->count].len = len;
    fr->frames[fr->count++].block = block;
    return 0;
}

/* A block's frames: a superframe's index (VP9 Annex B) at its end lists their sizes; a block without one is one
   frame */
static int snx_vp9j_split(const uint8_t* d, uint32_t off, uint32_t len, uint32_t block, SnxVp9jFrames* fr) {
    uint8_t marker;
    uint32_t width, count, index, i, j, at, sum = 0;
    if (!len) return -1;
    marker = d[off + len - 1];
    if ((marker & 0xE0) != 0xC0) return snx_vp9j_add(fr, off, len, block);
    width = 1 + ((marker >> 3) & 3);
    count = 1 + (marker & 7);
    index = 2 + width * count;
    if (len < index || d[off + len - index] != marker) return snx_vp9j_add(fr, off, len, block);
    at = off;
    for (i = 0; i < count; i++) {
        uint32_t size = 0;
        for (j = 0; j < width; j++) size |= (uint32_t)d[off + len - index + 1 + i * width + j] << (8 * j);
        if (!size || size > len - index - sum) return -1;
        if (snx_vp9j_add(fr, at, size, block)) return -1;
        at += size;
        sum += size;
    }
    return 0;
}

/* ---- the containers: IVF, and just enough of WebM (Matroska) to take the VP9 track's blocks out */
typedef struct {
    const uint8_t* d;
    SnxVp9jFrames* fr;
    uint64_t track, vp9_track; /* the track entry being read, the VP9 one (0 none) */
    int vp9_tracks;
    uint32_t blocks;
    int bad;
} SnxVp9jWebm;

/* an EBML number at *pos, below end: an ID keeps its length marker, a size drops it (unknown: all ones). 0, or -1
   when it does not fit */
static int snx_vp9j_vint(const uint8_t* d, size_t end, size_t* pos, int id, uint64_t* value, int* unknown) {
    uint32_t length = 1, i;
    uint8_t first;
    if (*pos >= end) return -1;
    first = d[*pos];
    while (length <= 8 && !(first & (0x100u >> length))) length++;
    if (length > 8 || end - *pos < length) return -1;
    *value = id ? first : first & ((0x100u >> length) - 1);
    for (i = 1; i < length; i++) *value = *value << 8 | d[*pos + i];
    if (unknown) *unknown = !id && *value == (((uint64_t)1 << (7 * length)) - 1);
    *pos += length;
    return 0;
}

static void snx_vp9j_walk(SnxVp9jWebm* w, size_t pos, size_t end, int depth) {
    while (pos < end && !w->bad) {
        uint64_t id, size;
        int unknown;
        size_t body, stop;
        if (snx_vp9j_vint(w->d, end, &pos, 1, &id, NULL) || snx_vp9j_vint(w->d, end, &pos, 0, &size, &unknown) ||
            (!unknown && size > end - pos)) {
            w->bad = 1;
            return;
        }
        body = pos;
        stop = unknown ? end : pos + (size_t)size;
        if (id == 0xAE) { /* TrackEntry */
            w->track = 0;
            if (depth < 8) snx_vp9j_walk(w, body, stop, depth + 1);
        } else if (id == 0x18538067 || id == 0x1F43B675 || id == 0xA0 || id == 0x1654AE6B) {
            /* Segment, Cluster, BlockGroup, Tracks */
            if (depth < 8) snx_vp9j_walk(w, body, stop, depth + 1);
        } else if (id == 0xD7 && size <= 8) { /* TrackNumber */
            w->track = 0;
            while (body < stop) w->track = w->track << 8 | w->d[body++];
        } else if (id == 0x86 && stop - body == 5 && !memcmp(w->d + body, "V_VP9", 5)) { /* CodecID */
            w->vp9_track = w->track;
            w->vp9_tracks++;
        } else if (id == 0xA3 || id == 0xA1) { /* SimpleBlock, Block */
            uint64_t number;
            size_t at = body;
            if (snx_vp9j_vint(w->d, stop, &at, 0, &number, NULL) || stop - at < 3 || (w->d[at + 2] & 0x06)) {
                w->bad = 1; /* laced blocks are not expected in a video track */
                return;
            }
            /* the tracks come before the clusters in every vector here */
            if (w->vp9_tracks == 1 && number == w->vp9_track &&
                snx_vp9j_split(w->d, (uint32_t)(at + 3), (uint32_t)(stop - at - 3), w->blocks++, w->fr))
                w->bad = 1;
        }
        pos = stop;
    }
}

static int snx_vp9j_frames(const uint8_t* d, size_t n, SnxVp9jFrames* fr, char* err, size_t cap) {
    SnxVp9jWebm w;
    if (n >= 32 && !memcmp(d, "DKIF", 4)) {
        size_t pos = (size_t)d[6] | (size_t)d[7] << 8;
        uint32_t block = 0;
        while (pos + 12 <= n) {
            uint32_t len =
                (uint32_t)d[pos] | (uint32_t)d[pos + 1] << 8 | (uint32_t)d[pos + 2] << 16 | (uint32_t)d[pos + 3] << 24;
            if (len > n - pos - 12 || snx_vp9j_split(d, (uint32_t)(pos + 12), len, block++, fr))
                return snx_vp9j_fail(err, cap, "a broken IVF frame at %lu", (unsigned long)pos);
            pos += 12 + len;
        }
        return fr->count ? 0 : snx_vp9j_fail(err, cap, "an IVF file with no frames");
    }
    memset(&w, 0, sizeof w);
    w.d = d;
    w.fr = fr;
    snx_vp9j_walk(&w, 0, n, 0);
    if (w.bad) return snx_vp9j_fail(err, cap, "neither IVF nor a WebM file this reads");
    if (w.vp9_tracks != 1) return snx_vp9j_fail(err, cap, "%d VP9 tracks, not one", w.vp9_tracks);
    return fr->count ? 0 : snx_vp9j_fail(err, cap, "a VP9 track with no frames");
}

/* ---- the reference decoder's state, as a job */
static void snx_vp9j_md5(const AVFrame* f, uint8_t out[16]) {
    struct AVMD5* m = av_md5_alloc();
    int p, y;
    av_md5_init(m);
    for (p = 0; p < 3; p++) {
        int w = p ? (f->width + 1) >> 1 : f->width, h = p ? (f->height + 1) >> 1 : f->height;
        for (y = 0; y < h; y++) av_md5_update(m, f->data[p] + (ptrdiff_t)y * f->linesize[p], (size_t)w);
    }
    av_md5_final(m, out);
    av_free(m);
}

static void snx_vp9j_put16(uint8_t* s, uint32_t at, int v) {
    s[at] = (uint8_t)((unsigned)v & 0xFF);
    s[at + 1] = (uint8_t)(((unsigned)v >> 8) & 0xFF);
}

/* nvdec_vp9_pic_s, as nvtegra_vp9_prepare_frame_setup fills it from FFmpeg's state. The pitches come once the
   vector's largest frame is known. ref_w and ref_h: the size of the picture each reference slot named holds, 0 for
   an empty one. 0x74 holds the chroma AC delta and 0x75 the DC one, as NVIDIA's header names the two bytes (nvtegra
   writes them the other way round; Amberlith's vp9_two_frames runs both) */
static void snx_vp9j_setup(uint8_t* s, const VP9Context* x, int prev_shown, uint32_t body_len, const uint32_t* ref_w,
                           const uint32_t* ref_h) {
    const VP9BitstreamHeader* h = &x->s.h;
    int i;
    memset(s, 0, SNX_VP9J_SETUP);
    s[0x30] = (uint8_t)body_len;
    s[0x31] = (uint8_t)(body_len >> 8);
    s[0x32] = (uint8_t)(body_len >> 16);
    s[0x33] = (uint8_t)(body_len >> 24);
    for (i = 0; i < 3; i++) {
        snx_vp9j_put16(s, 0x48 + 8 * (uint32_t)i, (int)ref_w[i]);
        snx_vp9j_put16(s, 0x4A + 8 * (uint32_t)i, (int)ref_h[i]);
    }
    snx_vp9j_put16(s, 0x60, x->w);
    snx_vp9j_put16(s, 0x62, x->h);
    s[0x68] = (uint8_t)(h->keyframe | x->last_keyframe << 1 | h->errorres << 3 | prev_shown << 4 | h->intraonly << 5);
    for (i = 0; i < 3; i++) s[0x6D + i] = h->signbias[i];
    s[0x70] = h->filter.level;
    s[0x71] = (uint8_t)h->filter.sharpness;
    s[0x72] = h->yac_qi;
    s[0x73] = (uint8_t)h->ydc_qdelta;
    s[0x74] = (uint8_t)h->uvac_qdelta;
    s[0x75] = (uint8_t)h->uvdc_qdelta;
    s[0x76] = h->lossless;
    s[0x77] = (uint8_t)h->txfmmode;
    s[0x78] = h->keyframe ? 0 : h->highprecisionmvs;
    s[0x79] = (uint8_t)h->filtermode;
    s[0x7A] = (uint8_t)h->comppredmode;
    s[0x7B] = (uint8_t)(h->allowcompinter ? h->fixcompref + 1 : 0);
    s[0x7C] = (uint8_t)(h->allowcompinter ? h->varcompref[0] + 1 : 0);
    s[0x7D] = (uint8_t)(h->allowcompinter ? h->varcompref[1] + 1 : 0);
    s[0x7E] = (uint8_t)h->tiling.log2_tile_cols;
    s[0x7F] = (uint8_t)h->tiling.log2_tile_rows;
    s[0x80] = h->segmentation.enabled;
    s[0x81] = h->segmentation.update_map;
    s[0x82] = h->segmentation.temporal;
    s[0x83] = h->segmentation.absolute_vals;
    for (i = 0; i < 8; i++) {
        s[0x84 + 4 * i] = h->segmentation.feat[i].q_enabled;
        s[0x85 + 4 * i] = h->segmentation.feat[i].lf_enabled;
        s[0x86 + 4 * i] = h->segmentation.feat[i].ref_enabled;
        s[0x87 + 4 * i] = h->segmentation.feat[i].skip_enabled;
        snx_vp9j_put16(s, 0xA4 + 8 * (uint32_t)i, h->segmentation.feat[i].q_val);
        snx_vp9j_put16(s, 0xA6 + 8 * (uint32_t)i, h->segmentation.feat[i].lf_val);
        snx_vp9j_put16(s, 0xA8 + 8 * (uint32_t)i, h->segmentation.feat[i].ref_val);
    }
    s[0xE4] = h->lf_delta.enabled;
    for (i = 0; i < 4; i++) s[0xE5 + i] = (uint8_t)h->lf_delta.ref[i];
    for (i = 0; i < 2; i++) s[0xE9 + i] = (uint8_t)h->lf_delta.mode[i];
}

/* nvdec_vp9EntropyProbs_t, as nvtegra_vp9_update_probs fills it: the probabilities the frame decoded with, the
   prediction modes in the hardware's order, and the keyframe tables, which no compressed header changes. Its
   ref_pred_probs come from a line buffer of the decoder there, which no reader takes, so they stay nought */
static void snx_vp9j_probs(uint8_t* t, const VP9Context* x) {
    static const uint8_t mode[10] = {2, 0, 1, 3, 4, 5, 6, 8, 7, 9};
    const ProbContext* p = &x->prob.p;
    const VP9BitstreamHeader* h = &x->s.h;
    int i, j, k, l, m, c, tx;
    memset(t, 0, SNX_VP9J_PROBS);
    for (i = 0; i < 10; i++) {
        for (j = 0; j < 10; j++) {
            memcpy(t + (i * 10 + j) * 8, ff_vp9_default_kf_ymode_probs[mode[i]][mode[j]], 8);
            t[0x320 + i * 10 + j] = ff_vp9_default_kf_ymode_probs[mode[i]][mode[j]][8];
        }
        memcpy(t + 0x3A0 + i * 8, ff_vp9_default_kf_uvmode_probs[mode[i]], 8);
        t[0x3F0 + i] = ff_vp9_default_kf_uvmode_probs[mode[i]][8];
    }
    memcpy(t + 0x387, h->segmentation.prob, 7);
    if (h->segmentation.temporal)
        memcpy(t + 0x38E, h->segmentation.pred_prob, 3);
    else
        memset(t + 0x38E, 0xFF, 3);
    for (i = 0; i < 7; i++) memcpy(t + 0x400 + i * 4, p->mv_mode[i], 3);
    memcpy(t + 0x41C, p->intra, 4);
    for (i = 0; i < 10; i++) {
        memcpy(t + 0x420 + i * 8, p->uv_mode[mode[i]], 8);
        t[0x520 + i] = p->uv_mode[mode[i]][8];
    }
    memcpy(t + 0x470, p->tx8p, 2);
    memcpy(t + 0x472, p->tx16p, 4);
    memcpy(t + 0x476, p->tx32p, 6);
    for (i = 0; i < 4; i++) {
        memcpy(t + 0x480 + i * 8, p->y_mode[i], 8);
        t[0x47C + i] = p->y_mode[i][8];
    }
    for (i = 0; i < 4; i++)
        for (j = 0; j < 4; j++) {
            memcpy(t + 0x4A0 + (4 * (3 - i) + j) * 4, ff_vp9_default_kf_partition_probs[i][j], 3);
            memcpy(t + 0x4E0 + (4 * (3 - i) + j) * 4, p->partition[i][j], 3);
        }
    memcpy(t + 0x52A, p->filter, 8);
    memcpy(t + 0x532, p->comp, 5);
    memcpy(t + 0x537, p->skip, 3);
    memcpy(t + 0x53B, p->mv_joint, 3);
    for (c = 0; c < 2; c++) {
        t[0x53E + c] = p->mv_comp[c].sign;
        t[0x540 + c] = p->mv_comp[c].class0;
        memcpy(t + 0x542 + c * 3, p->mv_comp[c].fp, 3);
        t[0x548 + c] = p->mv_comp[c].class0_hp;
        t[0x54A + c] = p->mv_comp[c].hp;
        memcpy(t + 0x54C + c * 10, p->mv_comp[c].classes, 10);
        memcpy(t + 0x560 + c * 6, p->mv_comp[c].class0_fp, 6);
        memcpy(t + 0x56C + c * 10, p->mv_comp[c].bits, 10);
    }
    memcpy(t + 0x580, p->single_ref, 10);
    memcpy(t + 0x58A, p->comp_ref, 5);
    for (tx = 0; tx < 4; tx++)
        for (i = 0; i < 2; i++)
            for (j = 0; j < 2; j++)
                for (k = 0; k < 6; k++)
                    for (l = 0; l < 6; l++)
                        for (m = 0; m < 3; m++)
                            t[0x5A0 + tx * 576 + (((i * 2 + j) * 6 + k) * 6 + l) * 4 + m] =
                                x->prob.coef[tx][i][j][k][l][m];
}

static int snx_vp9j_hex(int c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; }

/* libvpx's MD5 list: one line an image, its hash first */
static int snx_vp9j_md5s(const char* path, uint8_t (**out)[16], uint32_t* count, char* err, size_t cap) {
    size_t n, at = 0;
    uint8_t* txt = snx_vp9j_read(path, &n);
    uint8_t (*md5)[16];
    uint32_t lines = 0, i;
    if (!txt) return snx_vp9j_fail(err, cap, "cannot read %s", path);
    for (i = 0; i < n; i++) lines += txt[i] == '\n';
    if (!(md5 = calloc(lines + 1, 16))) {
        free(txt);
        return snx_vp9j_fail(err, cap, "out of memory");
    }
    *count = 0;
    while (at < n) {
        for (i = 0; i < 32; i++) {
            int v = at + i < n ? snx_vp9j_hex(txt[at + i]) : -1;
            if (v < 0) {
                free(txt);
                free(md5);
                return snx_vp9j_fail(err, cap, "%s: line %u is not an MD5", path, *count + 1);
            }
            md5[*count][i / 2] = (uint8_t)(md5[*count][i / 2] << 4 | v);
        }
        (*count)++;
        while (at < n && txt[at] != '\n') at++;
        at++;
    }
    free(txt);
    *out = md5;
    return *count ? 0 : snx_vp9j_fail(err, cap, "%s lists no image", path);
}

/* one frame into the reference decoder; the MD5 of the image it hands out, when it does */
static int snx_vp9j_decode(AVCodecContext* c, AVPacket* pkt, AVFrame* out, const uint8_t* d, uint32_t len, int* shown,
                           uint8_t md5[16]) {
    int r;
    av_packet_unref(pkt);
    if (av_new_packet(pkt, (int)len) < 0) return AVERROR(ENOMEM);
    memcpy(pkt->data, d, len);
    if ((r = avcodec_send_packet(c, pkt)) < 0) return r;
    *shown = 0;
    while ((r = avcodec_receive_frame(c, out)) == 0) {
        if (out->format != AV_PIX_FMT_YUV420P) {
            av_frame_unref(out);
            return AVERROR_PATCHWELCOME;
        }
        snx_vp9j_md5(out, md5);
        av_frame_unref(out);
        (*shown)++;
    }
    return r == AVERROR(EAGAIN) ? 0 : r;
}

static int snx_vp9j_jobs(SnxVp9Vector* v, const SnxVp9jFrames* fr, uint8_t (*list)[16], uint32_t images, char* err,
                         size_t cap) {
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_VP9);
    AVCodecContext* c = NULL;
    AVPacket* pkt = av_packet_alloc();
    AVFrame* out = av_frame_alloc();
    int slot_surface[8], prev = -1, prev_shown = 0, r = -1, last_shown = -1, outputs, level = av_log_get_level();
    uint32_t slot_w[8], slot_h[8], max_w = 0, max_h = 0, k, i, image = 0;
    uint8_t out_md5[16];
    for (i = 0; i < 8; i++) {
        slot_surface[i] = -1;
        slot_w[i] = slot_h[i] = 0;
    }
    if (!codec || !pkt || !out || !(c = avcodec_alloc_context3(codec))) {
        snx_vp9j_fail(err, cap, "FFmpeg has no VP9 decoder or no memory");
        goto done;
    }
    c->thread_count = 1;
    /* its warnings are the stream's own (svc_1280x720_3 names references too large to scale, which no block
       predicts from), and the decoder under test says them too; its errors still show */
    av_log_set_level(AV_LOG_ERROR);
    if (avcodec_open2(c, codec, NULL) < 0) {
        snx_vp9j_fail(err, cap, "FFmpeg's VP9 decoder does not open");
        goto done;
    }
    for (k = 0; k < fr->count; k++) {
        SnxVp9Job* j = &v->jobs[k];
        const uint8_t* d = v->data + fr->frames[k].off;
        uint32_t len = fr->frames[k].len;
        j->image = -1;
        if ((d[0] >> 6) != 2 || (d[0] & 0x30)) {
            snx_vp9j_fail(err, cap, "frame %u is not a profile 0 VP9 frame", k);
            goto done;
        }
        if ((r = snx_vp9j_decode(c, pkt, out, d, len, &outputs, out_md5)) < 0) {
            snx_vp9j_fail(err, cap, "frame %u: FFmpeg's VP9 decoder refused it (%d)", k, r);
            goto done;
        }
        r = -1;
        if (d[0] & 0x08) { /* show_existing_frame: the slot's picture, shown again */
            uint32_t slot = d[0] & 7;
            if (slot_surface[slot] < 0 || outputs != 1) {
                snx_vp9j_fail(err, cap, "frame %u shows slot %u again, which %s", k, slot,
                              slot_surface[slot] < 0 ? "holds nothing" : "FFmpeg did not show");
                goto done;
            }
            j->existing = j->shown = 1;
            j->surface = (uint32_t)slot_surface[slot];
            j->width = slot_w[slot];
            j->height = slot_h[slot];
            memcpy(j->md5, out_md5, 16);
        } else {
            const VP9Context* x = c->priv_data;
            const VP9BitstreamHeader* h = &x->s.h;
            const AVFrame* cur = x->s.frames[CUR_FRAME].tf.f;
            uint32_t head = (uint32_t)(h->uncompressed_header_size + h->compressed_header_size), ref_w[3], ref_h[3];
            int busy[SNX_VP9J_SURFACES], surface = 0;
            if (!cur || !cur->buf[0] || cur->format != AV_PIX_FMT_YUV420P || x->w <= 0 || x->h <= 0 ||
                (uint32_t)x->w > SNX_VP9J_MAX_DIM || (uint32_t)x->h > SNX_VP9J_MAX_DIM || head > len ||
                outputs != !h->invisible) {
                snx_vp9j_fail(err, cap, "frame %u: FFmpeg's VP9 decoder left no 4:2:0 picture of it", k);
                goto done;
            }
            j->shown = !h->invisible;
            j->width = (uint32_t)x->w;
            j->height = (uint32_t)x->h;
            j->body = fr->frames[k].off + head;
            j->body_len = len - head;
            snx_vp9j_md5(cur, j->md5);
            if (outputs && memcmp(j->md5, out_md5, 16)) {
                snx_vp9j_fail(err, cap, "frame %u: FFmpeg showed another picture than it decoded", k);
                goto done;
            }
            memset(busy, 0, sizeof busy);
            for (i = 0; i < 8; i++)
                if (slot_surface[i] >= 0) busy[slot_surface[i]] = 1;
            if (prev >= 0) busy[prev] = 1;
            while (surface < (int)SNX_VP9J_SURFACES && busy[surface]) surface++;
            if (surface == (int)SNX_VP9J_SURFACES) {
                snx_vp9j_fail(err, cap, "frame %u needs a surface past the %u", k, SNX_VP9J_SURFACES);
                goto done;
            }
            j->surface = (uint32_t)surface;
            /* a reference no slot holds yet is the frame's own surface, as nvtegra hands it; key and intra-only
               frames name the slots FFmpeg kept from the frame before */
            for (i = 0; i < 3; i++) {
                int s = slot_surface[h->refidx[i]];
                j->refs[i] = (uint32_t)(s >= 0 ? s : surface);
                ref_w[i] = slot_w[h->refidx[i]];
                ref_h[i] = slot_h[h->refidx[i]];
            }
            snx_vp9j_setup(j->setup, x, prev_shown, j->body_len, ref_w, ref_h);
            snx_vp9j_probs(j->probs, x);
            for (i = 0; i < 8; i++)
                if (h->refreshrefmask >> i & 1) {
                    slot_surface[i] = surface;
                    slot_w[i] = j->width;
                    slot_h[i] = j->height;
                }
            if ((uint32_t)surface + 1 > v->surfaces) v->surfaces = (uint32_t)surface + 1;
            if (j->width > max_w) max_w = j->width;
            if (j->height > max_h) max_h = j->height;
            prev = surface;
            prev_shown = j->shown;
            v->hidden += !j->shown;
        }
        if (j->shown) {
            if (last_shown >= 0 && fr->frames[last_shown].block == fr->frames[k].block) v->shown_dropped++;
            last_shown = (int)k;
        }
        /* libvpx hands out one image a block: the last frame shown in it */
        if (last_shown >= 0 && (k + 1 == fr->count || fr->frames[k + 1].block != fr->frames[k].block) &&
            fr->frames[last_shown].block == fr->frames[k].block) {
            SnxVp9Job* shown = &v->jobs[last_shown];
            if (image == images || memcmp(shown->md5, list[image], 16)) {
                snx_vp9j_fail(err, cap, "frame %d: FFmpeg's picture is not image %u of libvpx's list (%u images)",
                              last_shown, image + 1, images);
                goto done;
            }
            shown->image = (int)image++;
        }
    }
    if (image != images) {
        snx_vp9j_fail(err, cap, "FFmpeg shows %u images, libvpx's list has %u", image, images);
        goto done;
    }
    /* every surface as a player allocates them for the stream's largest frame: rows of a multiple of 64 bytes and
       a multiple of 32 rows, NV12 */
    v->stride = ((max_w + 1) / 2 * 2 + 63) / 64 * 64;
    v->rows = ((max_h + 1) / 2 * 2 + 31) / 32 * 32;
    for (k = 0; k < fr->count; k++) {
        SnxVp9Job* j = &v->jobs[k];
        if (j->existing) continue;
        for (i = 0; i < 4; i++)
            if (i == 3 || j->setup[0x48 + 8 * i] || j->setup[0x49 + 8 * i]) {
                snx_vp9j_put16(j->setup, 0x4C + 8 * i, (int)v->stride);
                snx_vp9j_put16(j->setup, 0x4E + 8 * i, (int)v->stride);
            }
    }
    v->images = images;
    r = 0;
done:
    av_log_set_level(level);
    avcodec_free_context(&c);
    av_packet_free(&pkt);
    av_frame_free(&out);
    return r;
}

int snx_vp9j_load(const char* path, const char* md5_path, SnxVp9Vector* v, char* err, size_t cap) {
    SnxVp9jFrames fr;
    uint8_t (*list)[16] = NULL;
    uint32_t images = 0;
    size_t n = 0;
    int r = -1;
    memset(v, 0, sizeof *v);
    memset(&fr, 0, sizeof fr);
    if (!(v->data = snx_vp9j_read(path, &n))) return snx_vp9j_fail(err, cap, "cannot read %s", path);
    if (n > 0x7FFFFFFF) {
        snx_vp9j_fail(err, cap, "%s is too large", path);
    } else if (!snx_vp9j_frames(v->data, n, &fr, err, cap) && !snx_vp9j_md5s(md5_path, &list, &images, err, cap)) {
        if (!(v->jobs = calloc(fr.count, sizeof *v->jobs)))
            snx_vp9j_fail(err, cap, "out of memory");
        else {
            v->count = fr.count;
            r = snx_vp9j_jobs(v, &fr, list, images, err, cap);
        }
    }
    free(fr.frames);
    free(list);
    if (r) snx_vp9j_free(v);
    return r;
}

void snx_vp9j_free(SnxVp9Vector* v) {
    free(v->jobs);
    free(v->data);
    memset(v, 0, sizeof *v);
}
