/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* SceMp4: an MP4 file's H.264 and AAC units, as the SDK's SceAvPlayer reads them (one statically linked into a game:
   the calls and structures below are what that library does with them). The file is read whole into memory when it
   is opened, through the game's file callbacks (SceAvPlayerFileReplacement's open, size, readOffset and close, each
   called as guest code) or from its path, and FFmpeg's MP4 demuxer reads it from there. A video unit is H.264 in
   Annex B (h264_mp4toannexb), as the console's decoder takes it; an audio unit a raw AAC frame. Timestamps are 90 kHz
   ticks, the movie's duration in 0.1 ms units, as the library reads them.

   The NIDs are Vita3K's (nids.inc, GPL-2.0-or-later); its SceMp4 is stubs, so what each call does is the library's
   use of it: GetStreamInfo's (0xd5b26179) name is ours */
#include "vita/hle/hle.h"
#include "common/utilities/endian.h"
#include "common/utilities/common.h"
#include <inttypes.h>
#include <libavcodec/bsf.h>
#include <libavformat/avformat.h>
#include <libavutil/mathematics.h>
#include <stdlib.h>
#include <string.h>

#define FILES     4u
#define STREAMS   8u
#define CHUNK     0x100000u   /* read a callback at a time */
#define MOST      0x20000000u /* the largest movie read */
#define IO_BUFFER 0x8000u
#define UNITS     1024u       /* the last audio units' places, for the AAC decoder (vita_mp4_unit_size): the
                                 demuxer runs ahead of it by hundreds of units */
#define PAGE      0x1000u

typedef struct {
    bool used;
    uint32_t object, open, close, read, size; /* the game's callbacks, 0 when the path is read instead */
    uint32_t scratch;                         /* the guest buffer a read goes to */
    unsigned char* data;
    size_t bytes, loaded, at; /* the file's size, what is read so far, the demuxer's position */
    uint64_t reported, box;   /* the size the size callback gave; the top-level box being looked at */
    bool seen_moov, seen_mdat;
    AVFormatContext* fmt;
    AVIOContext* io;
    int streams[STREAMS]; /* each of ours, FFmpeg's index */
    bool video[STREAMS];
    AVBSFContext* annexb[STREAMS];
    unsigned stream_count;
    AVPacket* unit; /* the last unit GetNextUnit gave, whose bytes GetNextUnitData copies */
    bool has_unit;
} Mp4;
struct VitaMp4 {
    Mp4 files[FILES];
    struct {
        uint32_t address, size;
    } units[UNITS];
    unsigned next;
};

static uint32_t arg(VitaProcess* p, VitaThread* t, unsigned n) { return vita_arg(p, t, n); }
static struct VitaMp4* state(VitaProcess* p) {
    if (!p->mp4) p->mp4 = calloc(1, sizeof *p->mp4);
    return p->mp4;
}
static Mp4* file_of(VitaProcess* p, uint32_t handle) {
    return p->mp4 && handle >= 1 && handle <= FILES && p->mp4->files[handle - 1].used ? &p->mp4->files[handle - 1]
                                                                                       : NULL;
}
static uint32_t handle_of(VitaProcess* p, const Mp4* m) { return (uint32_t)(m - p->mp4->files) + 1; }
static void release(VitaProcess* p, Mp4* m) {
    for (unsigned i = 0; i < STREAMS; ++i) av_bsf_free(&m->annexb[i]);
    av_packet_free(&m->unit);
    if (m->fmt) avformat_close_input(&m->fmt);
    if (m->io) {
        av_freep(&m->io->buffer);
        avio_context_free(&m->io);
    }
    if (m->scratch) vita_free(p, m->scratch);
    free(m->data);
    memset(m, 0, sizeof *m);
}
void vita_mp4_free(VitaProcess* p) {
    if (!p->mp4) return;
    for (unsigned i = 0; i < FILES; ++i)
        if (p->mp4->files[i].used) release(p, &p->mp4->files[i]);
    free(p->mp4);
    p->mp4 = NULL;
}
uint32_t vita_mp4_unit_size(VitaProcess* p, uint32_t address) {
    struct VitaMp4* s = p->mp4;
    for (unsigned i = 0; s && i < UNITS; ++i) {
        unsigned k = (s->next + UNITS - 1 - i) % UNITS; /* the newest first */
        if (s->units[k].address == address && s->units[k].size) return s->units[k].size;
    }
    return 0;
}

/* ---- the file in memory, for the demuxer ---- */
static int io_read(void* opaque, uint8_t* buffer, int size) {
    Mp4* m = opaque;
    size_t left = m->bytes - m->at, n = (size_t)size < left ? (size_t)size : left;
    if (!n) return AVERROR_EOF;
    memcpy(buffer, m->data + m->at, n);
    m->at += n;
    return (int)n;
}
static int64_t io_seek(void* opaque, int64_t offset, int whence) {
    Mp4* m = opaque;
    if (whence & AVSEEK_SIZE) return (int64_t)m->bytes;
    whence &= ~AVSEEK_FORCE;
    int64_t base = whence == SEEK_CUR ? (int64_t)m->at : whence == SEEK_END ? (int64_t)m->bytes : 0, to = base + offset;
    if (to < 0 || to > (int64_t)m->bytes) return -1;
    m->at = (size_t)to;
    return to;
}
/* the demuxer on the file, its H.264 and AAC streams ours: false when it isn't one */
static bool demux(Mp4* m) {
    unsigned char* buffer = av_malloc(IO_BUFFER);
    if (!buffer || !(m->io = avio_alloc_context(buffer, IO_BUFFER, 0, m, io_read, NULL, io_seek))) {
        av_free(buffer);
        return false;
    }
    if (!(m->fmt = avformat_alloc_context())) return false;
    m->fmt->pb = m->io;
    m->fmt->flags |= AVFMT_FLAG_CUSTOM_IO;
    if (avformat_open_input(&m->fmt, NULL, av_find_input_format("mov"), NULL) < 0) {
        m->fmt = NULL; /* freed on failure */
        return false;
    }
    if (avformat_find_stream_info(m->fmt, NULL) < 0) return false;
    for (unsigned i = 0; i < m->fmt->nb_streams; ++i) {
        AVStream* st = m->fmt->streams[i];
        enum AVCodecID id = st->codecpar->codec_id;
        if ((id != AV_CODEC_ID_H264 && id != AV_CODEC_ID_AAC) || m->stream_count == STREAMS) {
            st->discard = AVDISCARD_ALL;
            continue;
        }
        unsigned k = m->stream_count++;
        m->streams[k] = (int)i, m->video[k] = id == AV_CODEC_ID_H264;
        if (!m->video[k]) continue;
        const AVBitStreamFilter* f = av_bsf_get_by_name("h264_mp4toannexb");
        if (!f || av_bsf_alloc(f, &m->annexb[k]) < 0 ||
            avcodec_parameters_copy(m->annexb[k]->par_in, st->codecpar) < 0)
            return false;
        m->annexb[k]->time_base_in = st->time_base;
        if (av_bsf_init(m->annexb[k]) < 0) return false;
    }
    return m->stream_count && (m->unit = av_packet_alloc()) != NULL;
}

/* ---- sceMp4OpenFile(path, SceAvPlayerFileReplacement*, allocator, work, workSize, priority, log level): the
   handle, 0 on failure (the library takes any other value as one) ---- */
static uint32_t opened(VitaProcess* p, Mp4* m) {
    if (demux(m)) {
        printf("vita: an MP4 file of %zu bytes, %u streams\n", m->bytes, m->stream_count);
        return handle_of(p, m);
    }
    printf("vita: an MP4 file of %zu bytes the demuxer can't read\n", m->bytes);
    release(p, m);
    return 0;
}
static uint32_t read_chunk(VitaProcess* p, VitaThread* t, Mp4* m);
static uint32_t on_close(VitaProcess* p, VitaThread* t, void* context) {
    (void)t;
    Mp4* m = context;
    vita_free(p, m->scratch);
    m->scratch = 0;
    return opened(p, m);
}
static uint32_t on_read(VitaProcess* p, VitaThread* t, void* context) {
    Mp4* m = context;
    int32_t got = (int32_t)t->cpu.r[0];
    size_t want = m->bytes - m->loaded < CHUNK ? m->bytes - m->loaded : CHUNK;
    if (got <= 0 || (size_t)got > want) {
        printf("vita: the game's MP4 file read %" PRId32 " bytes at %zu of %zu\n", got, m->loaded, m->bytes);
        release(p, m);
        return 0;
    }
    memcpy(m->data + m->loaded, vita_ptr(p, m->scratch), (size_t)got);
    m->loaded += (size_t)got;
    if (m->loaded < m->bytes) return read_chunk(p, t, m);
    const uint32_t args[1] = {m->object};
    vita_hle_guest_call(p, t, m->close, args, 1, on_close, m);
    return 0;
}
/* readOffset(object, buffer, u64 position, length) */
static uint32_t read_chunk(VitaProcess* p, VitaThread* t, Mp4* m) {
    size_t want = m->bytes - m->loaded < CHUNK ? m->bytes - m->loaded : CHUNK;
    const uint32_t args[5] = {m->object, m->scratch, (uint32_t)m->loaded, (uint32_t)((uint64_t)m->loaded >> 32),
                              (uint32_t)want};
    vita_hle_guest_call(p, t, m->read, args, 5, on_read, m);
    return 0;
}
/* The file read whole, once its extent is known */
static uint32_t read_file(VitaProcess* p, VitaThread* t, Mp4* m, uint64_t bytes) {
    if (!bytes || bytes > MOST || !(m->data = malloc((size_t)bytes))) {
        printf("vita: no room to read an MP4 file of %" PRIu64 " bytes\n", bytes);
        release(p, m);
        return 0;
    }
    m->bytes = (size_t)bytes;
    return read_chunk(p, t, m);
}
/* The file's extent: its top-level boxes walked from the start, a header at a time. The size callback may give more
   than the movie (a game's file inside an archive answers the archive's size: the console's demuxer reads only what it
   needs), so the walk stops at the first box past moov and mdat that isn't one of MP4's */
static bool known_box(const unsigned char* type) {
    static const char* const types[] = {"ftyp", "moov", "mdat", "free", "skip", "wide", "uuid", "pdin",
                                        "moof", "mfra", "meta", "sidx", "styp", "uuid"};
    for (size_t i = 0; i < sizeof types / sizeof *types; ++i)
        if (!memcmp(type, types[i], 4)) return true;
    return false;
}
static uint32_t read_box(VitaProcess* p, VitaThread* t, Mp4* m);
static uint32_t on_box(VitaProcess* p, VitaThread* t, void* context) {
    Mp4* m = context;
    int32_t got = (int32_t)t->cpu.r[0];
    const unsigned char* h = vita_ptr(p, m->scratch);
    uint64_t size = snx_ld_be32(h), end;
    if (size == 1 && got >= 16)
        size = snx_ld_be64(h + 8);
    else if (size == 0)
        size = m->reported - m->box; /* to the end */
    bool box = got >= 8 && size >= 8 && known_box(h + 4) && (m->box || !memcmp(h + 4, "ftyp", 4));
    if (!box || ((m->seen_moov && m->seen_mdat) && !known_box(h + 4)) || m->box + size > m->reported)
        end = m->box; /* the movie ends before this */
    else {
        m->seen_moov |= !memcmp(h + 4, "moov", 4), m->seen_mdat |= !memcmp(h + 4, "mdat", 4);
        m->box += size;
        if (m->box < m->reported) return read_box(p, t, m);
        end = m->box;
    }
    if (!m->seen_moov) {
        printf("vita: an MP4 file without its moov box\n");
        release(p, m);
        return 0;
    }
    return read_file(p, t, m, end);
}
static uint32_t read_box(VitaProcess* p, VitaThread* t, Mp4* m) {
    uint64_t left = m->reported - m->box;
    const uint32_t args[5] = {m->object, m->scratch, (uint32_t)m->box, (uint32_t)(m->box >> 32),
                              left < 16 ? (uint32_t)left : 16};
    vita_hle_guest_call(p, t, m->read, args, 5, on_box, m);
    return 0;
}
static uint32_t on_size(VitaProcess* p, VitaThread* t, void* context) {
    Mp4* m = context;
    m->reported = t->cpu.r[0] | (uint64_t)t->cpu.r[1] << 32;
    if (!m->reported || !(m->scratch = vita_alloc(p, &p->user, CHUNK, PAGE))) {
        printf("vita: no room to read an MP4 file of %" PRIu64 " bytes\n", m->reported);
        release(p, m);
        return 0;
    }
    return read_box(p, t, m);
}
static uint32_t on_open(VitaProcess* p, VitaThread* t, void* context) {
    Mp4* m = context;
    if ((int32_t)t->cpu.r[0] < 0) {
        release(p, m);
        return 0;
    }
    const uint32_t args[1] = {m->object};
    vita_hle_guest_call(p, t, m->size, args, 1, on_size, m);
    return 0;
}
static uint32_t open_file(VitaProcess* p, VitaThread* t) {
    uint32_t path = arg(p, t, 0), replacement = arg(p, t, 1);
    struct VitaMp4* s = state(p);
    Mp4* m = NULL;
    for (unsigned i = 0; s && i < FILES && !m; ++i)
        if (!s->files[i].used) m = &s->files[i];
    if (!m) return 0;
    memset(m, 0, sizeof *m);
    m->used = true;
    if (replacement && vita_read32(p, replacement + 4) && vita_read32(p, replacement + 0xc) &&
        vita_read32(p, replacement + 0x10)) {
        m->object = vita_read32(p, replacement), m->open = vita_read32(p, replacement + 4);
        m->close = vita_read32(p, replacement + 8), m->read = vita_read32(p, replacement + 0xc);
        m->size = vita_read32(p, replacement + 0x10);
        if (!m->close) m->close = VITA_RETURN;
        const uint32_t args[2] = {m->object, path};
        vita_hle_guest_call(p, t, m->open, args, 2, on_open, m);
        return 0;
    }
    /* no callbacks: the path, as the library's own sceIo ones read it */
    char* host = vita_io_host_path(p, path);
    m->data = host ? snx_read_file(host, &m->bytes) : NULL;
    free(host);
    if (!m->data) {
        release(p, m);
        return 0;
    }
    m->loaded = m->bytes;
    return opened(p, m);
}

/* sceMp4StartFileStreaming(handle, int* streams, u64* duration) */
static uint32_t start_file_streaming(VitaProcess* p, VitaThread* t) {
    Mp4* m = file_of(p, arg(p, t, 0));
    if (!m) return 0x80000000u;
    if (arg(p, t, 1)) vita_write32(p, arg(p, t, 1), m->stream_count);
    int64_t duration = m->fmt->duration == AV_NOPTS_VALUE ? 0 : m->fmt->duration / 100; /* microseconds to 0.1 ms */
    if (arg(p, t, 2)) vita_write64(p, arg(p, t, 2), (uint64_t)duration);
    return 0;
}
/* the largest unit of a stream, from the file's index, with room for the parameter sets Annex B puts before a key
   frame's */
static uint32_t largest_unit(const AVStream* st) {
    int64_t most = 0;
    for (int i = 0, n = avformat_index_get_entries_count(st); i < n; ++i) {
        const AVIndexEntry* e = avformat_index_get_entry((AVStream*)st, i);
        if (e && e->size > most) most = e->size;
    }
    if (!most) most = (int64_t)st->codecpar->width * st->codecpar->height;
    return (uint32_t)most + (uint32_t)st->codecpar->extradata_size + 0x100u;
}
/* sceMp4GetStreamInfo(handle, index, info): 0x140 bytes, zero: the type (0 video, 1 audio), the codec (1 AVC, 2 AAC),
   a video's size, profile, level, references and largest unit, its display aspect ratio times 10^6, an audio's
   rate and channels; the language in both */
static uint32_t get_stream_info(VitaProcess* p, VitaThread* t) {
    Mp4* m = file_of(p, arg(p, t, 0));
    uint32_t index = arg(p, t, 1), info = arg(p, t, 2);
    if (!m || index >= m->stream_count || !info) return 0x80000000u;
    const AVStream* st = m->fmt->streams[m->streams[index]];
    const AVCodecParameters* c = st->codecpar;
    unsigned char* b = vita_ptr(p, info);
    if (m->video[index]) {
        vita_write32(p, info, 0), vita_write32(p, info + 4, 1);
        vita_write32(p, info + 8, (uint32_t)c->width), vita_write32(p, info + 0xc, (uint32_t)c->height);
        b[0x10] = (unsigned char)c->profile, b[0x11] = 0;
        b[0x14] = (unsigned char)c->level, b[0x15] = 0;
        b[0x18] = 4, b[0x1c] = 4; /* references and frames held: FFmpeg decodes whatever the stream needs */
        vita_write32(p, info + 0x20, largest_unit(st));
        AVRational sar = c->sample_aspect_ratio.num > 0 ? c->sample_aspect_ratio : (AVRational){1, 1};
        double aspect = c->height ? (double)c->width * sar.num / ((double)c->height * sar.den) : 0;
        vita_write64(p, info + 0x30, (uint64_t)(int64_t)(aspect * 1e6 + 0.5));
    } else {
        vita_write32(p, info, 1), vita_write32(p, info + 4, 2);
        vita_write32(p, info + 8, (uint32_t)c->sample_rate);
        b[0x1c] = (unsigned char)c->ch_layout.nb_channels, b[0x1d] = 0;
    }
    const AVDictionaryEntry* language = av_dict_get(st->metadata, "language", NULL, 0);
    if (language) memcpy(b + 0x38, language->value, strnlen(language->value, 3));
    return 0;
}
/* sceMp4GetStreamInfo, its own NID (0x8be0e3d3; 0xd5b26179 above is the call the library's other path makes): the
   arguments are not known. It does what that does, which reads the same handle, index and buffer, and the first calls
   log what they were given and what the pointers point to, to check */
static uint32_t get_stream_info_named(VitaProcess* p, VitaThread* t) {
    static unsigned shown;
    if (shown < 4) {
        ++shown;
        printf("vita: sceMp4GetStreamInfo(0x%08x, 0x%08x, 0x%08x, 0x%08x)\n", arg(p, t, 0), arg(p, t, 1), arg(p, t, 2),
               arg(p, t, 3));
        for (unsigned i = 0; i < 4; ++i) {
            uint32_t a = arg(p, t, i) & ~3u;
            if (a < 0x80000000u || !vita_mapped(p, a, 32)) continue;
            printf("  r%u points to:", i);
            for (unsigned k = 0; k < 8; ++k) printf(" %08x", vita_read32(p, a + k * 4));
            putchar('\n');
        }
    }
    return get_stream_info(p, t);
}
static uint32_t enable_stream(VitaProcess* p, VitaThread* t) {
    Mp4* m = file_of(p, arg(p, t, 0));
    return m && arg(p, t, 1) < m->stream_count ? 0 : 0x80000000u;
}
/* the next unit of our streams into m->unit: 0, 1 at the end, negative on an error */
static int next_unit(Mp4* m, unsigned* ours) {
    for (;;) {
        av_packet_unref(m->unit);
        int r = av_read_frame(m->fmt, m->unit);
        if (r == AVERROR_EOF) return 1;
        if (r < 0) return r;
        unsigned k = 0;
        while (k < m->stream_count && m->streams[k] != m->unit->stream_index) ++k;
        if (k == m->stream_count) continue;
        if (m->video[k]) {
            if (av_bsf_send_packet(m->annexb[k], m->unit) < 0) return -1;
            r = av_bsf_receive_packet(m->annexb[k], m->unit);
            if (r == AVERROR(EAGAIN)) continue;
            if (r < 0) return r;
        }
        *ours = k;
        return 0;
    }
}
static uint64_t ticks(const AVStream* st, int64_t ts) {
    return ts == AV_NOPTS_VALUE ? 0 : (uint64_t)av_rescale_q(ts, st->time_base, (AVRational){1, 90000});
}
/* sceMp4GetNextUnit(handle, unit): 0x38 bytes, its size, pts and dts in 90 kHz ticks, a flag and the stream. 0, or
   above 0 at the end */
static uint32_t get_next_unit(VitaProcess* p, VitaThread* t) {
    Mp4* m = file_of(p, arg(p, t, 0));
    uint32_t unit = arg(p, t, 1);
    if (!m || !unit) return 0x80000000u;
    unsigned k = 0;
    int r = next_unit(m, &k);
    m->has_unit = r == 0;
    if (r) return r > 0 ? 1u : 0x80000000u;
    const AVStream* st = m->fmt->streams[m->streams[k]];
    int64_t pts = m->unit->pts != AV_NOPTS_VALUE ? m->unit->pts : m->unit->dts;
    memset(vita_ptr(p, unit), 0, 0x38);
    vita_write32(p, unit + 8, (uint32_t)m->unit->size);
    vita_write64(p, unit + 0x10, ticks(st, pts));
    vita_write64(p, unit + 0x18, ticks(st, m->unit->dts));
    vita_write32(p, unit + 0x28, 1);
    ((unsigned char*)vita_ptr(p, unit))[0x2c] = (unsigned char)k;
    ((unsigned char*)vita_ptr(p, unit))[0x30] = (m->unit->flags & AV_PKT_FLAG_KEY) != 0;
    return 0;
}
/* sceMp4GetNextUnitData(handle, destination): the unit's bytes, all of them */
static uint32_t get_next_unit_data(VitaProcess* p, VitaThread* t) {
    Mp4* m = file_of(p, arg(p, t, 0));
    uint32_t to = arg(p, t, 1);
    if (!m || !m->has_unit || !to || !vita_mapped(p, to, (uint32_t)m->unit->size)) return 0x80000000u;
    memcpy(vita_ptr(p, to), m->unit->data, (size_t)m->unit->size);
    /* a raw AAC frame doesn't say how long it is: the decoder looks its size up by where it was put */
    if (m->fmt->streams[m->unit->stream_index]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
        struct VitaMp4* s = p->mp4;
        s->units[s->next % UNITS].address = to, s->units[s->next % UNITS].size = (uint32_t)m->unit->size;
        ++s->next;
    }
    m->has_unit = false;
    return 0;
}
static void seek(Mp4* m, int64_t microseconds) {
    av_seek_frame(m->fmt, -1, microseconds, AVSEEK_FLAG_BACKWARD);
    for (unsigned i = 0; i < STREAMS; ++i)
        if (m->annexb[i]) av_bsf_flush(m->annexb[i]);
    m->has_unit = false;
}
/* sceMp4Reset(handle): from the start */
static uint32_t reset(VitaProcess* p, VitaThread* t) {
    Mp4* m = file_of(p, arg(p, t, 0));
    if (!m) return 0x80000000u;
    seek(m, 0);
    return 0;
}
/* sceMp4JumpPTS(handle, unused, u64 time): the time in milliseconds (the library passes sceAvPlayerJumpToTime's) */
static uint32_t jump_pts(VitaProcess* p, VitaThread* t) {
    Mp4* m = file_of(p, arg(p, t, 0));
    if (!m) return 0x80000000u;
    uint64_t ms = arg(p, t, 2) | (uint64_t)arg(p, t, 3) << 32;
    seek(m, (int64_t)ms * 1000);
    return 0;
}
static uint32_t close_file(VitaProcess* p, VitaThread* t) {
    Mp4* m = file_of(p, arg(p, t, 0));
    if (!m) return 0x80000000u;
    release(p, m);
    return 0;
}
static uint32_t ok(VitaProcess* p, VitaThread* t) {
    (void)p, (void)t;
    return 0;
}

const VitaHle vita_hle_mp4[] = {
    {0x05474af0u, "sceMp4OpenFile", open_file},
    {0x30e49e4du, "sceMp4StartFileStreaming", start_file_streaming},
    {0xd5b26179u, "sceMp4GetStreamInfo", get_stream_info},
    {0x8be0e3d3u, "sceMp4GetStreamInfo", get_stream_info_named},
    {0x609e57adu, "sceMp4EnableStream", enable_stream},
    {0x20a01112u, "sceMp4GetNextUnit", get_next_unit},
    {0xd092a066u, "sceMp4GetNextUnitData", get_next_unit_data},
    {0x40351e1au, "sceMp4Reset", reset},
    {0x96676ba0u, "sceMp4JumpPTS", jump_pts},
    {0x920623c8u, "sceMp4CloseFile", close_file},
    {0x7b4832feu, "sceMp4ReleaseBuffer", ok},
    {0, NULL, NULL},
};
