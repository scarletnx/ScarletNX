/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* AV1 on the Apple media engine: VideoToolbox (VideoToolbox.framework) decodes the stream in the media engine of the
   Apple GPU, and each picture comes back as a CVPixelBuffer of two planes (8 bit 4:2:0, NV12) whose rows this hands to
   the renderer as FFmpeg's AV_PIX_FMT_NV12 frame, which is what the CPU's decoder gives it too.

   Nothing here decodes on the CPU: the session is made with kVTVideoDecoderSpecification_RequireHardwareAcceleratedVideoDecoder,
   so a stream the media engine can't take fails the session, and native_video_decoder.c then decodes it with the CPU's
   decoder (dav1d) and says so in the log.

   The stream's sequence header comes in band, because the service hands AV1 packets without a configuration (only
   H.264 gets one). It is read out of the first packet that carries it: that OBU is the configOBUs of the av1C record
   (the ISOBMFF binding of AV1) that CoreMedia's format description holds, and the fields beside it, the profile, the
   level and the colour config, come from the same parse (AV1 spec, 5.5.1). The parse stops where av1C needs nothing
   more, and a parse that runs out of bytes refuses the stream rather than guess.

   The packet before the sequence header, if the stream has one, is kept and decoded after it, so a stream whose
   configuration is late still starts at its first picture. */
#include "nx/hle/services/native_video.h"
#include "nx/runtime/platform/log.h"
#include "common/platform/thread.h"
#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
#include <VideoToolbox/VideoToolbox.h>
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/buffer.h>
#include <stdlib.h>
#include <string.h>

#define VT_QUEUE 6   /* pictures the media engine may hold before the pump takes one (an alt-ref frame delays one) */
#define VT_EARLY 4   /* packets kept before the sequence header arrives */
#define VT_EARLY_BYTES (1u << 20) /* and their bytes all together */

/* a bit reader over an OBU's payload, most significant bit first */
typedef struct {
    const u8* p;
    u32 n;
    u32 bit;
    int bad;
} VtBits;

static u32 vt_f(VtBits* b, u32 n) {
    u32 v = 0;
    for (u32 i = 0; i < n; i++) {
        u32 at = b->bit++;
        if (at >> 3 >= b->n) {
            b->bad = 1;
            return 0;
        }
        v = (v << 1) | ((u32)(b->p[at >> 3] >> (7 - (at & 7))) & 1u);
    }
    return v;
}
/* uvlc: the leading zero bits, then that many bits, as AV1's num_ticks_per_picture_minus_1 reads them */
static u32 vt_uvlc(VtBits* b) {
    u32 lead = 0;
    while (!b->bad && lead < 32 && vt_f(b, 1) == 0) lead++;
    if (b->bad || lead >= 32) {
        b->bad = 1;
        return 0;
    }
    return ((1u << lead) - 1u) + vt_f(b, lead);
}

/* the sequence header's fields that av1C carries, and its coded size */
typedef struct {
    u32 profile, level, tier;
    u32 high_bitdepth, twelve_bit, monochrome, sub_x, sub_y, chroma_pos;
    u32 full; /* the stream's colour range: 1 is full, as the picture it decodes to must keep it */
    u32 width, height;
} VtSeq;

static void vt_seq(VtBits* b, VtSeq* s) {
    *s = (VtSeq){0};
    s->profile = vt_f(b, 3);
    (void)vt_f(b, 1); /* still_picture */
    u32 reduced = vt_f(b, 1);
    if (reduced) {
        s->level = vt_f(b, 5); /* and no tier bit: a reduced header's seq_tier is 0 (AV1 5.5.1) */
    } else {
        u32 timing = vt_f(b, 1), model = 0, delay_len = 0;
        if (timing) {
            (void)vt_f(b, 32); /* num_units_in_display_tick */
            (void)vt_f(b, 32); /* time_scale */
            if (vt_f(b, 1)) (void)vt_uvlc(b); /* equal_picture_interval's num_ticks_per_picture_minus_1 */
            model = vt_f(b, 1);
            if (model) {
                delay_len = vt_f(b, 5) + 1;
                (void)vt_f(b, 32); /* num_units_in_decoding_tick */
                (void)vt_f(b, 5);  /* buffer_removal_time_length_minus_1 */
                (void)vt_f(b, 5);  /* frame_presentation_time_length_minus_1 */
            }
        }
        u32 initial_delay = vt_f(b, 1);
        u32 ops = vt_f(b, 5) + 1;
        for (u32 i = 0; i < ops && !b->bad; i++) {
            (void)vt_f(b, 12); /* operating_point_idc */
            u32 level = vt_f(b, 5), tier = level > 7 ? vt_f(b, 1) : 0;
            if (model && vt_f(b, 1)) { /* decoder_model_present_for_this_op */
                (void)vt_f(b, delay_len);
                (void)vt_f(b, delay_len);
                (void)vt_f(b, 1); /* low_delay_mode_flag */
            }
            if (initial_delay && vt_f(b, 1)) (void)vt_f(b, 4); /* initial_display_delay_minus_1 */
            if (i == 0) {
                s->level = level;
                s->tier = tier;
            }
        }
    }
    u32 wbits = vt_f(b, 4) + 1, hbits = vt_f(b, 4) + 1;
    s->width = vt_f(b, wbits) + 1;
    s->height = vt_f(b, hbits) + 1;
    if (!reduced && vt_f(b, 1)) { /* frame_id_numbers_present_flag */
        (void)vt_f(b, 4);
        (void)vt_f(b, 3);
    }
    (void)vt_f(b, 1); /* use_128x128_superblock */
    (void)vt_f(b, 1); /* enable_filter_intra */
    (void)vt_f(b, 1); /* enable_intra_edge_filter */
    if (!reduced) {
        (void)vt_f(b, 1); /* enable_interintra_compound */
        (void)vt_f(b, 1); /* enable_masked_compound */
        (void)vt_f(b, 1); /* enable_warped_motion */
        (void)vt_f(b, 1); /* enable_dual_filter */
        u32 order_hint = vt_f(b, 1);
        if (order_hint) {
            (void)vt_f(b, 1); /* enable_jnt_comp */
            (void)vt_f(b, 1); /* enable_ref_frame_mvs */
        }
        u32 force_sct = vt_f(b, 1) ? 2 : vt_f(b, 1); /* seq_choose_screen_content_tools, then seq_force_... */
        if (force_sct > 0) {
            if (!vt_f(b, 1)) (void)vt_f(b, 1); /* seq_choose_integer_mv, then seq_force_integer_mv */
        }
        if (order_hint) (void)vt_f(b, 3); /* order_hint_bits_minus_1 */
    }
    (void)vt_f(b, 1); /* enable_superres */
    (void)vt_f(b, 1); /* enable_cdef */
    (void)vt_f(b, 1); /* enable_restoration */
    /* color_config */
    s->high_bitdepth = vt_f(b, 1);
    s->twelve_bit = s->profile == 2 && s->high_bitdepth ? vt_f(b, 1) : 0;
    s->monochrome = s->profile == 1 ? 0 : vt_f(b, 1);
    u32 described = vt_f(b, 1), primaries = 2, transfer = 2, matrix = 2;
    if (described) {
        primaries = vt_f(b, 8);
        transfer = vt_f(b, 8);
        matrix = vt_f(b, 8);
    }
    if (s->monochrome) {
        s->full = vt_f(b, 1);
        s->sub_x = s->sub_y = 1;
    } else if (described && primaries == 1 && transfer == 13 && matrix == 0) { /* BT.709, sRGB, identity */
        s->full = 1; /* the identity matrix means full range, and no bit says it */
        s->sub_x = s->sub_y = 0;
    } else {
        s->full = vt_f(b, 1);
        if (s->profile == 0)
            s->sub_x = s->sub_y = 1;
        else if (s->profile == 1)
            s->sub_x = s->sub_y = 0;
        else if (s->twelve_bit) {
            s->sub_x = vt_f(b, 1);
            s->sub_y = s->sub_x ? vt_f(b, 1) : 0;
        } else {
            s->sub_x = 1;
            s->sub_y = 0;
        }
        if (s->sub_x && s->sub_y) s->chroma_pos = vt_f(b, 2);
    }
    /* the walk stops here: separate_uv_delta_q and film_grain_params_present change nothing of what the record
       holds or of the picture's layout */
}

/* The sequence header OBU of a packet (its bytes with the header and the size field, and the fields beside it): 1 when
   the packet has one. A packet whose OBUs have no size field stops the walk, as nothing delimits them */
static int vt_seq_obu(const u8* p, u32 n, const u8** obu, u32* obu_len, VtSeq* s) {
    for (u32 i = 0; i < n;) {
        u32 start = i;
        u8 h = p[i++];
        if (h & 0x80) return 0; /* the forbidden bit */
        u32 type = (h >> 3) & 0xf;
        if ((h >> 2) & 1) i++; /* an extension header follows, with the temporal and spatial id */
        if (!((h >> 1) & 1)) return 0; /* no size field: nothing delimits the OBUs after it */
        if (i > n) return 0;
        u64 size = 0;
        u32 shift = 0;
        for (;;) {
            if (i >= n || shift > 56) return 0;
            u8 b = p[i++];
            size |= (u64)(b & 0x7f) << shift;
            shift += 7;
            if (!(b & 0x80)) break;
        }
        if (size > n - i) return 0;
        if (type == 1) { /* sequence header */
            VtBits b = {p + i, (u32)size, 0, 0};
            vt_seq(&b, s);
            if (b.bad || !s->width || !s->height) return 0;
            *obu = p + start;
            *obu_len = (u32)size + (i - start);
            return 1;
        }
        i += (u32)size;
    }
    return 0;
}

/* the av1C record: four bytes of the stream's own fields, then the sequence header OBU */
static CFDataRef vt_av1c(const VtSeq* s, const u8* obu, u32 n) {
    u8* rec = malloc(4 + (size_t)n);
    if (!rec) return NULL;
    rec[0] = 0x81; /* marker, version 1 */
    rec[1] = (u8)(((s->profile & 7) << 5) | (s->level & 0x1f));
    rec[2] = (u8)(((s->tier & 1) << 7) | ((s->high_bitdepth & 1) << 6) | ((s->twelve_bit & 1) << 5) |
                  ((s->monochrome & 1) << 4) | ((s->sub_x & 1) << 3) | ((s->sub_y & 1) << 2) | (s->chroma_pos & 3));
    rec[3] = 0; /* reserved, and no initial presentation delay */
    memcpy(rec + 4, obu, n);
    CFDataRef data = CFDataCreate(kCFAllocatorDefault, rec, (CFIndex)(4 + n));
    free(rec);
    return data;
}

typedef struct {
    PMutex lock;
    PCond cond;
    VTDecompressionSessionRef session;
    CMVideoFormatDescriptionRef fmt;
    CVPixelBufferRef pic[VT_QUEUE]; /* the engine's pictures, in the order it made them */
    i64 pts[VT_QUEUE];
    u32 head, count, dropped;
    u32 submitted, delivered; /* packets the engine took, and the pictures it has given for them: it owes the difference */
    AVPacket* early[VT_EARLY]; /* packets before the sequence header */
    u32 nearly, early_bytes;
    u32 index;
    int eos, dead;
    u32 width, height; /* what the service configured, for the log when the stream's coded size differs */
} VtDec;

static void vt_pic_free(void* opaque, u8* data) {
    CVPixelBufferRef pic = opaque;
    (void)data;
    CVPixelBufferUnlockBaseAddress(pic, kCVPixelBufferLock_ReadOnly);
    CFRelease(pic);
}

static void vt_output(void* refcon, void* source, OSStatus status, VTDecodeInfoFlags flags, CVImageBufferRef image,
                      CMTime pts, CMTime duration) {
    VtDec* d = refcon;
    (void)source;
    (void)flags;
    (void)duration;
    if (status != noErr || !image) {
        if (status != noErr) WARN_LIMIT(4, "gsv: video codec %u: the media engine refused a picture (%d)\n", d->index,
                                        (int)status);
        pm_lock(&d->lock);
        d->delivered++; /* taken, and no picture of it: a wait for one is owed nothing */
        pc_signal(&d->cond);
        pm_unlock(&d->lock);
        return;
    }
    i64 at = CMTimeConvertScale(pts, 1000000000, kCMTimeRoundingMethod_Default).value; /* as gsv gives it: ns */
    pm_lock(&d->lock);
    d->delivered++;
    CVPixelBufferRef old = NULL;
    if (d->count == VT_QUEUE) { /* the pump is behind: the oldest goes, and the log says so */
        old = d->pic[d->head];
        d->pic[d->head] = NULL;
        d->head = (d->head + 1) % VT_QUEUE;
        d->count--;
        d->dropped++;
    }
    u32 at_slot = (d->head + d->count) % VT_QUEUE;
    d->pic[at_slot] = (CVPixelBufferRef)CVBufferRetain(image);
    d->pts[at_slot] = at;
    d->count++;
    pc_signal(&d->cond);
    pm_unlock(&d->lock);
    if (old) CFRelease(old);
}

/* the session for a format description, its pictures NV12 in an IOSurface, the media engine required. The range of
   the pixel format is the stream's own: a picture asked for in the other one comes back converted, and the guest's
   image would then not hold what the stream says, as the CPU's decoder writes it */
static int vt_session(VtDec* d, CMVideoFormatDescriptionRef fmt, int full) {
    CFMutableDictionaryRef spec =
        CFDictionaryCreateMutable(kCFAllocatorDefault, 2, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFMutableDictionaryRef attrs =
        CFDictionaryCreateMutable(kCFAllocatorDefault, 4, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionaryRef surface = CFDictionaryCreate(kCFAllocatorDefault, NULL, NULL, 0, &kCFTypeDictionaryKeyCallBacks,
                                                 &kCFTypeDictionaryValueCallBacks);
    CFNumberRef nv12 =
        CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type,
                       &(SInt32){(SInt32)(full ? kCVPixelFormatType_420YpCbCr8BiPlanarFullRange
                                                : kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange)});
    int ok = spec && attrs && surface && nv12;
    VTDecompressionOutputCallbackRecord cb = {vt_output, d};
    if (ok) {
        CFDictionarySetValue(spec, kVTVideoDecoderSpecification_EnableHardwareAcceleratedVideoDecoder, kCFBooleanTrue);
        CFDictionarySetValue(spec, kVTVideoDecoderSpecification_RequireHardwareAcceleratedVideoDecoder, kCFBooleanTrue);
        CFDictionarySetValue(attrs, kCVPixelBufferPixelFormatTypeKey, nv12);
        CFDictionarySetValue(attrs, kCVPixelBufferIOSurfacePropertiesKey, surface);
        ok = VTDecompressionSessionCreate(kCFAllocatorDefault, fmt, spec, attrs, &cb, &d->session) == noErr;
    }
    if (nv12) CFRelease(nv12);
    if (surface) CFRelease(surface);
    if (attrs) CFRelease(attrs);
    if (spec) CFRelease(spec);
    return ok;
}

/* the format description of a sequence header, and the session that decodes it */
static int vt_start(VtDec* d, const u8* obu, u32 n, const VtSeq* s) {
    CFDataRef av1c = vt_av1c(s, obu, n);
    CFMutableDictionaryRef atoms =
        CFDictionaryCreateMutable(kCFAllocatorDefault, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFMutableDictionaryRef ext =
        CFDictionaryCreateMutable(kCFAllocatorDefault, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    int ok = av1c && atoms && ext;
    if (ok) {
        CFDictionarySetValue(atoms, CFSTR("av1C"), av1c);
        CFDictionarySetValue(ext, kCMFormatDescriptionExtension_SampleDescriptionExtensionAtoms, atoms);
        ok = CMVideoFormatDescriptionCreate(kCFAllocatorDefault, kCMVideoCodecType_AV1, (int32_t)s->width,
                                           (int32_t)s->height, ext, &d->fmt) == noErr;
    }
    if (ext) CFRelease(ext);
    if (atoms) CFRelease(atoms);
    if (av1c) CFRelease(av1c);
    if (!ok) return 0;
    /* the stream's own range, so that the picture keeps the values the stream carries, as the CPU's decoder writes them
       into the guest's image. A monochrome stream asks for full range whatever its header says: the engine converts its
       monochrome picture into 4:2:0 with the full-range one, and a video-range request comes back with every luma
       sample 16 higher than the stream's */
    if (vt_session(d, d->fmt, s->monochrome || s->full)) return 1;
    CFRelease(d->fmt);
    d->fmt = NULL;
    return 0;
}

int nvideo_vt_available(void) { return VTIsHardwareDecodeSupported(kCMVideoCodecType_AV1); }

void* nvideo_vt_open(u32 index, u32 width, u32 height) {
    VtDec* d = calloc(1, sizeof *d);
    if (!d) return NULL;
    d->index = index;
    d->width = width;
    d->height = height;
    pm_init(&d->lock);
    pc_init(&d->cond);
    return d;
}

static void vt_release(VtDec* d) {
    if (d->session) VTDecompressionSessionInvalidate(d->session);
    for (u32 i = 0; i < d->count; i++) CFRelease(d->pic[(d->head + i) % VT_QUEUE]);
    d->count = d->head = 0;
    for (u32 i = 0; i < d->nearly; i++) av_packet_free(&d->early[i]);
    d->nearly = d->early_bytes = 0;
    d->submitted = d->delivered = 0; /* no frame of that session is outstanding: its packets went with it */
    if (d->session) CFRelease(d->session);
    if (d->fmt) CFRelease(d->fmt);
    d->session = NULL;
    d->fmt = NULL;
}

void nvideo_vt_close(void* h) {
    VtDec* d = h;
    if (!d) return;
    if (d->session) {
        VTDecompressionSessionFinishDelayedFrames(d->session);
        VTDecompressionSessionWaitForAsynchronousFrames(d->session);
    }
    vt_release(d); /* no picture of the engine's is on its way: the waits above let them all in */
    pm_destroy(&d->lock);
    free(d);
}

/* one packet's bytes decoded with the session: 1 when the engine took it */
static int vt_decode(VtDec* d, const u8* p, u32 n, i64 pts_ns) {
    CMBlockBufferRef bb = NULL;
    CMSampleBufferRef sb = NULL;
    OSStatus st = CMBlockBufferCreateWithMemoryBlock(kCFAllocatorDefault, NULL, n, kCFAllocatorDefault, NULL, 0, n, 0, &bb);
    if (st == noErr) st = CMBlockBufferReplaceDataBytes(p, bb, 0, n);
    if (st == noErr) {
        CMSampleTimingInfo timing = {kCMTimeInvalid, CMTimeMake(pts_ns, 1000000000), kCMTimeInvalid};
        size_t size = n;
        st = CMSampleBufferCreateReady(kCFAllocatorDefault, bb, d->fmt, 1, 1, &timing, 1, &size, &sb);
    }
    if (st == noErr) {
        VTDecodeInfoFlags flags = 0;
        pm_lock(&d->lock);
        d->submitted++; /* before the call: its callback can run in it */
        pm_unlock(&d->lock);
        st = VTDecompressionSessionDecodeFrame(d->session, sb, 0, NULL, &flags);
        if (st != noErr) { /* the engine calls no callback for a frame it did not take */
            pm_lock(&d->lock);
            d->delivered++;
            pc_signal(&d->cond);
            pm_unlock(&d->lock);
        }
    }
    if (sb) CFRelease(sb);
    if (bb) CFRelease(bb);
    if (st != noErr) WARN_LIMIT(4, "gsv: video codec %u: the media engine refused a packet (%d)\n", d->index, (int)st);
    return st == noErr;
}

/* the packets kept before the sequence header, in their order */
static void vt_replay(VtDec* d) {
    for (u32 i = 0; i < d->nearly; i++) {
        AVPacket* p = d->early[i];
        vt_decode(d, p->data, (u32)p->size, p->pts == AV_NOPTS_VALUE ? 0 : p->pts);
        av_packet_free(&d->early[i]);
    }
    d->nearly = d->early_bytes = 0;
}

int nvideo_vt_send(void* h, const AVPacket* pkt) {
    VtDec* d = h;
    if (d->dead) return AVERROR(ENOSYS);
    if (!pkt) { /* the end of the stream: what the engine still holds comes out */
        if (d->session) {
            VTDecompressionSessionFinishDelayedFrames(d->session);
            VTDecompressionSessionWaitForAsynchronousFrames(d->session);
        }
        pm_lock(&d->lock);
        d->eos = 1;
        pc_signal(&d->cond);
        pm_unlock(&d->lock);
        return 0;
    }
    const u8* obu = NULL;
    u32 obu_len = 0;
    VtSeq seq;
    int has_seq = !d->session && vt_seq_obu(pkt->data, (u32)pkt->size, &obu, &obu_len, &seq);
    if (has_seq && seq.high_bitdepth) {
        /* the guest's image is 8 bit NV12, and so is the copy into it (gpu/vk/nvdec.c): a 10 or 12 bit stream decodes
           on the CPU until that image can hold it (the HDR item of #1849) */
        LOG_LIMIT(1, "gsv: video codec %u: a %u-bit stream: the CPU decodes it (the image is 8 bit)\n", d->index,
                  seq.twelve_bit ? 12u : 10u);
        pm_lock(&d->lock);
        d->dead = 1;
        pm_unlock(&d->lock);
        return AVERROR(ENOSYS);
    }
    if (has_seq) {
        if (!vt_start(d, obu, obu_len, &seq)) {
            WARN_LIMIT(4,
                       "gsv: video codec %u: the media engine's AV1 session won't open for %ux%u profile %u: the CPU "
                       "decodes this stream\n",
                       d->index, seq.width, seq.height, seq.profile);
            pm_lock(&d->lock);
            d->dead = 1;
            pm_unlock(&d->lock);
            return AVERROR(ENOSYS); /* the caller starts its own decoder, from the packets it kept */
        }
        LOG("gsv: video codec %u: AV1 decodes on the media engine (VideoToolbox), %ux%u profile %u level %u\n", d->index,
            seq.width, seq.height, seq.profile, seq.level);
        if (d->width && d->height && (d->width != seq.width || d->height != seq.height))
            LOG("gsv: video codec %u: the stream's coded size is %ux%u, not the configured %ux%u\n", d->index, seq.width,
                seq.height, d->width, d->height);
        vt_replay(d);
        return vt_decode(d, pkt->data, (u32)pkt->size, pkt->pts == AV_NOPTS_VALUE ? 0 : pkt->pts) ? 0 : AVERROR(ENOSYS);
    }
    if (!d->session) { /* before the sequence header: kept, for the stream's first picture */
        if (d->nearly == VT_EARLY || d->early_bytes + (u32)pkt->size > VT_EARLY_BYTES) {
            WARN_LIMIT(4, "gsv: video codec %u: no sequence header in the first packets: the media engine can't start\n",
                       d->index);
            pm_lock(&d->lock);
            d->dead = 1;
            pm_unlock(&d->lock);
            return AVERROR(ENOSYS);
        }
        if (!(d->early[d->nearly] = av_packet_clone(pkt))) return AVERROR(ENOMEM);
        d->early_bytes += (u32)pkt->size;
        d->nearly++;
        return 0;
    }
    return vt_decode(d, pkt->data, (u32)pkt->size, pkt->pts == AV_NOPTS_VALUE ? 0 : pkt->pts) ? 0 : AVERROR(ENOSYS);
}

int nvideo_vt_receive(void* h, AVFrame* frame) {
    VtDec* d = h;
    CVPixelBufferRef pic;
    i64 pts;
    pm_lock(&d->lock);
    while (!d->count && d->submitted != d->delivered && !d->eos && !d->dead)
        pc_wait(&d->cond, &d->lock, 1000000); /* a picture the engine still owes, which is on its way */
    if (!d->count) {
        int eos = d->eos || d->dead;
        u32 dropped = d->dropped;
        d->dropped = 0;
        pm_unlock(&d->lock);
        if (dropped) WARN_LIMIT(4, "gsv: video codec %u: the media engine's pictures came faster than they were taken: %u "
                                   "dropped\n", d->index, dropped);
        return eos ? AVERROR_EOF : AVERROR(EAGAIN);
    }
    pic = d->pic[d->head];
    d->pic[d->head] = NULL;
    pts = d->pts[d->head];
    d->head = (d->head + 1) % VT_QUEUE;
    d->count--;
    pm_unlock(&d->lock);
    if (CVPixelBufferLockBaseAddress(pic, kCVPixelBufferLock_ReadOnly) != kCVReturnSuccess ||
        !CVPixelBufferGetBaseAddressOfPlane(pic, 0) || !CVPixelBufferGetBaseAddressOfPlane(pic, 1)) {
        CVPixelBufferUnlockBaseAddress(pic, kCVPixelBufferLock_ReadOnly);
        CFRelease(pic);
        return AVERROR(EIO);
    }
    frame->format = AV_PIX_FMT_NV12;
    frame->width = (int)CVPixelBufferGetWidth(pic);
    frame->height = (int)CVPixelBufferGetHeight(pic);
    frame->data[0] = CVPixelBufferGetBaseAddressOfPlane(pic, 0);
    frame->data[1] = CVPixelBufferGetBaseAddressOfPlane(pic, 1);
    frame->data[2] = NULL;
    frame->linesize[0] = (int)CVPixelBufferGetBytesPerRowOfPlane(pic, 0);
    frame->linesize[1] = (int)CVPixelBufferGetBytesPerRowOfPlane(pic, 1);
    frame->linesize[2] = 0;
    frame->pts = pts;
    frame->buf[0] = av_buffer_create(frame->data[0], 1, vt_pic_free, pic, 0);
    if (!frame->buf[0]) {
        vt_pic_free(pic, NULL); /* the unlock and the reference the frame would have let go */
        return AVERROR(ENOMEM);
    }
    return 0;
}

void nvideo_vt_flush(void* h) {
    VtDec* d = h;
    if (d->session) {
        VTDecompressionSessionFinishDelayedFrames(d->session);
        VTDecompressionSessionWaitForAsynchronousFrames(d->session);
    }
    pm_lock(&d->lock);
    vt_release(d); /* the next stream's sequence header makes its own session */
    d->eos = d->dead = 0;
    pm_unlock(&d->lock);
}
