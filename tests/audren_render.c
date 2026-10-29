/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The audio renderer's DSP offline: updates written field by field as edenprobe's audio family writes them
   (revision 4, 48 kHz, 240 samples a frame), frames rendered, and what the aux, the circular buffer sinks and the
   voices' counters show. Levels are one bin of a Fourier transform over a frame of whole periods */
#include "common/utilities/bits.h"
#include "nx/hle/services/audren_dsp.c"
#include "nx/hle/hle.c"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>

u8* g_mem;
static char logged[4096]; /* what was logged since the test emptied it */
void logf_(int lvl, const char* fmt, ...) {
    size_t used = strlen(logged);
    va_list ap;
    (void)lvl;
    va_start(ap, fmt);
    vsnprintf(logged + used, sizeof logged - used, fmt, ap);
    va_end(ap);
}

static unsigned checks, failures;
#define CHECK(expr, ...)                                                                                               \
    do {                                                                                                               \
        checks++;                                                                                                      \
        if (!(expr)) {                                                                                                 \
            failures++;                                                                                                \
            fprintf(stderr, "audren_render.c:%d: %s: ", __LINE__, #expr);                                              \
            fprintf(stderr, __VA_ARGS__);                                                                              \
            fputc('\n', stderr);                                                                                       \
        }                                                                                                              \
    } while (0)

enum {
    SNX_ART_RATE = 48000,
    SNX_ART_FRAME = 240,
    SNX_ART_VOICES = 4,
    SNX_ART_EFFECTS = 2,
    SNX_ART_SINKS = 3,
    SNX_ART_MIXES = 2,
    SNX_ART_POOLS = SNX_ART_EFFECTS + 4 * SNX_ART_VOICES,
    SNX_ART_FINAL = 0,
    SNX_ART_SUBMIX = 1,
    SNX_ART_RING_FRAMES = 40,
    SNX_ART_AUX_COUNT = SNX_ART_RING_FRAMES * 2 * SNX_ART_FRAME,
    /* guest addresses: offsets into the arena g_mem points at */
    SNX_ART_G_ARENA = 8 << 20,
    SNX_ART_G_PCM = 0x10000,    /* one second of a tone per voice, SNX_ART_VOICES of them */
    SNX_ART_G_ADPCM = 0x200000, /* an ADPCM buffer and its coefficients */
    SNX_ART_G_AUX_SEND = 0x300000,
    SNX_ART_G_AUX_RETURN = 0x340000,
    SNX_ART_G_RING = 0x380000, /* the circular buffer sinks' rings, 0x20000 apiece */
    SNX_ART_G_RING_BYTES = SNX_ART_RING_FRAMES * 2 * SNX_ART_FRAME * 2,
    /* the splitter section: room for a header, two infos of three destinations and six destinations */
    SNX_ART_SPLITTERS = 2,
    SNX_ART_DESTS = 6,
    SNX_ART_SPLIT_BYTES = 0x20 + SNX_ART_SPLITTERS * (0x1C + 3 * 4) + SNX_ART_DESTS * 0x70
};

/* the update, laid out as edenprobe's source/audio.c lays it out */
static u8 in[0x40 + 0x10 + 0x20 * SNX_ART_POOLS + 0x70 * SNX_ART_VOICES + 0x188 * SNX_ART_VOICES +
             0xC0 * SNX_ART_EFFECTS + SNX_ART_SPLIT_BYTES + 0x20 + 0x930 * SNX_ART_MIXES + 0x140 * SNX_ART_SINKS +
             0x10];
static u8 out[0x1000];
static u8 *u_pool, *u_res, *u_voice, *u_eff, *u_split, *u_mix, *u_sink;
static u32 split_counts[2]; /* the splitters and destinations reset() opens the renderer with */
static u32 art_rev = 4;     /* the revision layout() writes */
static ARState* st;

static void w8(u8* p, u32 off, u8 v) { p[off] = v; }
/* the update and guest memory hold little-endian words, as the game writes them */
static void w16(u8* p, u32 off, u16 v) { snx_st_le16(p + off, v); }
static void w32(u8* p, u32 off, u32 v) { snx_st_le32(p + off, v); }
static void w64(u8* p, u32 off, u64 v) { snx_st_le64(p + off, v); }
static void wf(u8* p, u32 off, float v) {
    u32 bits;
    memcpy(&bits, &v, 4);
    w32(p, off, bits);
}
/* an update's header of host numbers into its first 0x40 bytes */
static void put_header(u8* p, const u32* h) {
    for (u32 i = 0; i < 16; i++) w32(p, i * 4, h[i]);
}
static u8* voice(int i) { return u_voice + i * (art_rev >= 15 ? 0x188 : 0x170); }
static u8* res(int i) { return u_res + i * 0x70; }
static u8* eff(int i) { return u_eff + i * 0xC0; }
static u8* mix(int i) { return u_mix + i * 0x930; }
static u8* sink(int i) { return u_sink + i * 0x140; }

static void layout(void) {
    u32 rev;
    memset(in, 0, sizeof in);
    rev = ('R' << 0) | ('E' << 8) | ('V' << 16) | ((u32)('0' + art_rev) << 24);
    u32 voice_stride = art_rev >= 15 ? 0x188 : 0x170;
    u32 mix_header_size = art_rev >= 7 ? 0x20 : 0;
    u32 h[16] = {rev,
                 0x10,
                 0x20 * SNX_ART_POOLS,
                 voice_stride * SNX_ART_VOICES,
                 0x70 * SNX_ART_VOICES,
                 0xC0 * SNX_ART_EFFECTS,
                 mix_header_size + 0x930 * SNX_ART_MIXES,
                 0x140 * SNX_ART_SINKS,
                 0x10,
                 SNX_ART_SPLIT_BYTES};
    h[15] = 0x40 + h[1] + h[2] + h[3] + h[4] + h[5] + h[6] + h[7] + h[8] + h[9];
    put_header(in, h);
    w32(in, 0x40, rev);
    u8* p = in + 0x50;
    u_pool = p;
    p += 0x20 * SNX_ART_POOLS;
    u_res = p;
    p += 0x70 * SNX_ART_VOICES;
    u_voice = p;
    p += voice_stride * SNX_ART_VOICES;
    u_eff = p;
    p += 0xC0 * SNX_ART_EFFECTS;
    u_split = p; /* all zero, no splitter, until a case writes one */
    p += SNX_ART_SPLIT_BYTES;
    if (mix_header_size) {
        w32(p, 4, SNX_ART_MIXES);
        p += mix_header_size;
    }
    u_mix = p;
    p += 0x930 * SNX_ART_MIXES;
    u_sink = p;
    w64(u_pool, 0, 0);
    w64(u_pool, 8, SNX_ART_G_ARENA);
    w32(u_pool, 0x10, 4); /* asked to attach */
    for (int i = 0; i < SNX_ART_VOICES; i++) {
        w32(res(i), 0, (u32)i);
        w32(voice(i), 0, (u32)i);
    }
    for (int m = 0; m < SNX_ART_MIXES; m++) {
        wf(mix(m), 0, 1.0f);
        w32(mix(m), 4, SNX_ART_RATE);
        w32(mix(m), 8, m == SNX_ART_FINAL ? 2u : 4u);
        w8(mix(m), 0xC, 1);
        w32(mix(m), 0x10, (u32)m);
        w32(mix(m), 0x924, m == SNX_ART_FINAL ? 0x7FFFFFFFu : SNX_ART_FINAL);
        w32(mix(m), 0x928, 0xFFFFFFFFu);
    }
    /* the limiter, off until a case turns it on, then the aux, both on the submix */
    w8(eff(0), 0, SNX_AUDREN_EFFECT_LIMITER);
    w8(eff(0), 1, 1);
    w32(eff(0), 4, SNX_ART_SUBMIX);
    w32(eff(0), 0x18, 0);
    u8* l = eff(0) + 0x20;
    l[0] = 0, l[1] = 1, l[6] = 0, l[7] = 1;
    l[0xC] = 2, l[0xE] = 2;
    w32(l, 0x10, SNX_ART_RATE);
    w32(l, 0x38, 48);
    w32(l, 0x3C, 48);
    wf(l, 0x30, 1.0f);
    wf(l, 0x34, 1.0f);
    wf(l, 0x2C, 1.0f);
    wf(l, 0x24, 0.001f);
    wf(l, 0x28, 0.001f);
    w8(l, 0x40, 2);
    w8(eff(1), 0, SNX_AUDREN_EFFECT_AUX);
    w8(eff(1), 1, 1);
    w32(eff(1), 4, SNX_ART_SUBMIX);
    w32(eff(1), 0x18, 1);
    u8* a = eff(1) + 0x20;
    a[0] = 0, a[1] = 1, a[0x18] = 2, a[0x19] = 3;
    w32(a, 0x30, 2);
    w32(a, 0x34, SNX_ART_RATE);
    w32(a, 0x38, SNX_ART_AUX_COUNT);
    w32(a, 0x3C, 2);
    w64(a, 0x40, SNX_ART_G_AUX_SEND);
    w64(a, 0x48, SNX_ART_G_AUX_SEND + 0x80);
    w64(a, 0x50, SNX_ART_G_AUX_RETURN);
    w64(a, 0x58, SNX_ART_G_AUX_RETURN + 0x80);
    w32(a, 0x60, 4);
    w32(a, 0x64, SNX_ART_FRAME);
    w32(a, 0x68, SNX_ART_FRAME);
    /* the device sink on the final mix's two buffers, then two circular buffer sinks off until a case needs one */
    w8(sink(0), 0, SNX_AUDREN_SINK_DEVICE);
    w8(sink(0), 1, 1);
    w32(sink(0), 0x20 + 0x100, 2);
    sink(0)[0x20 + 0x104] = 0, sink(0)[0x20 + 0x105] = 1;
    for (int k = 1; k < SNX_ART_SINKS; k++) {
        u8* c = sink(k) + 0x20;
        w8(sink(k), 0, SNX_AUDREN_SINK_CIRCULAR);
        w64(c, 0, SNX_ART_G_RING + (u64)(k - 1) * 0x20000);
        w32(c, 8, SNX_ART_G_RING_BYTES);
        w32(c, 0xC, 2);
        w32(c, 0x10, SNX_ART_FRAME);
        w8(c, 0x18, 2);
        c[0x1C] = 0, c[0x1D] = 1;
    }
}

static void update(void) {
    int rc = audren_state_update(st, in, sizeof in, out, sizeof out);
    CHECK(rc == 0, "update returned %d", rc);
    for (int i = 0; i < SNX_ART_VOICES; i++) {
        w8(voice(i), 8, 0);                                                  /* is_new once */
        for (int w = 0; w < 4; w++) w8(voice(i), 0x60 + w * 0x38 + 0x1A, 1); /* sent */
    }
    for (int i = 0; i < SNX_ART_EFFECTS; i++) w8(eff(i), 1, 0);
}
/* a frame with no update before it */
static void frame_only(void) {
    i16 stereo[SNX_ART_FRAME * 2];
    audren_frame(st, stereo);
}
static void frames(int n) {
    i16 stereo[SNX_ART_FRAME * 2];
    for (int i = 0; i < n; i++) {
        update();
        audren_frame(st, stereo);
    }
}

/* a tone of whole periods in each frame, looping over one second of PCM16 at 48 kHz */
static void tone(int v, double hz, double amp, u8 quality, int channel, float gain) {
    i16* pcm = (i16*)(void*)(g_mem + SNX_ART_G_PCM + (u64)v * SNX_ART_RATE * 2);
    for (int i = 0; i < SNX_ART_RATE; i++)
        snx_st_le16(pcm + i, (u16)(i16)lrint(amp * 32767.0 * sin(2.0 * M_PI * hz * i / SNX_ART_RATE)));
    u8* vi = voice(v);
    memset(vi + 8, 0, 0x170 - 8);
    w8(vi, 8, 1);
    w8(vi, 9, 1);
    w8(vi, 0xA, 0); /* started */
    w8(vi, 0xB, SNX_AUDREN_PCM16);
    w32(vi, 0xC, SNX_ART_RATE);
    w32(vi, 0x18, 1);
    wf(vi, 0x1C, 1.0f);
    wf(vi, 0x20, 1.0f);
    w32(vi, 0x3C, 1);
    w32(vi, 0x58, SNX_ART_SUBMIX);
    w32(vi, 0x5C, 0xFFFFFFFFu);
    w64(vi, 0x60, SNX_ART_G_PCM + (u64)v * SNX_ART_RATE * 2);
    w64(vi, 0x68, SNX_ART_RATE * 2);
    w32(vi, 0x70, 0);
    w32(vi, 0x74, SNX_ART_RATE);
    w8(vi, 0x78, 1);            /* loop */
    w32(vi, 0x7C, 0xFFFFFFFFu); /* forever */
    w32(vi, 0x140, (u32)v);
    w8(vi, 0x15E, quality);
    for (int w = 1; w < 4; w++)
        w8(vi, 0x60 + w * 0x38 + 0x1A, 1); /* the slots it does not use: sent, as clients mark them */
    memset(res(v) + 4, 0, 0x60);
    wf(res(v), 4 + channel * 4, gain);
    w8(res(v), 0x64, 1);
}
static void silence(int v) { w8(voice(v), 0xA, 1); /* stopped */ }

/* voice v in stereo: hz0 on its left channel (resource v), hz1 on its right (resource v + 1), interleaved */
static void tone_stereo(int v, double hz0, double hz1, double amp) {
    i16* pcm;
    int i, c;
    tone(v, hz0, amp, 0, 0, 1.0f);
    pcm = (i16*)(void*)(g_mem + SNX_ART_G_PCM + (u64)v * SNX_ART_RATE * 2);
    for (i = 0; i < SNX_ART_RATE; i++)
        for (c = 0; c < 2; c++)
            snx_st_le16(pcm + i * 2 + c,
                        (u16)(i16)lrint(amp * 32767.0 * sin(2.0 * M_PI * (c ? hz1 : hz0) * i / SNX_ART_RATE)));
    w32(voice(v), 0x18, 2);
    w64(voice(v), 0x68, SNX_ART_RATE * 4);
    w32(voice(v), 0x144, (u32)v + 1);
    w8(res(v + 1), 0x64, 1);
}

/* a splitter section written in order: the header, then infos, then destinations */
static u32 split_at;
static void split_begin(u32 infos, u32 dests) {
    memset(u_split, 0, SNX_ART_SPLIT_BYTES);
    memcpy(u_split, "SNDH", 4);
    w32(u_split, 4, infos);
    w32(u_split, 8, dests);
    split_at = 0x20;
}
static void split_info(u32 id, u32 n, const u32* dests) {
    u32 k;
    memcpy(u_split + split_at, "SNDI", 4);
    w32(u_split, split_at + 4, id);
    w32(u_split, split_at + 8, SNX_ART_RATE);
    w32(u_split, split_at + 0xC, n);
    for (k = 0; k < n; k++) w32(u_split, split_at + 0x10 + k * 4, dests[k]);
    split_at += 0x1C + n * 4;
}
/* the destination ids the splitter cases list */
static const u32 split_ids_012[] = {0, 1, 2}, split_ids_345[] = {3, 4, 5}, split_ids_0[] = {0};
/* destination id to mix, volume v0 into its buffer 0 and v1 into its buffer 1 */
static u8* split_sent[SNX_ART_DESTS];
static void split_send(u32 id, u32 mix_id, u8 in_use, float v0, float v1) {
    u8* d = u_split + split_at;
    memcpy(d, "SNDD", 4);
    w32(d, 4, id);
    wf(d, 8, v0);
    wf(d, 0xC, v1);
    w32(d, 0x68, mix_id);
    w8(d, 0x6C, in_use);
    split_sent[id] = d;
    split_at += 0x70;
}

/* one Fourier bin over n samples, as a fraction of full scale */
static double level_i32(const i32* x, int n, double hz) {
    double re = 0, im = 0;
    for (int i = 0; i < n; i++) {
        re += x[i] * cos(2.0 * M_PI * hz * i / SNX_ART_RATE);
        im -= x[i] * sin(2.0 * M_PI * hz * i / SNX_ART_RATE);
    }
    return 2.0 * sqrt(re * re + im * im) / n / 32768.0;
}
static double level_i16(const i16* x, int n, double hz) {
    i32 tmp[SNX_ART_FRAME];
    for (int i = 0; i < n; i++) tmp[i] = x[i];
    return level_i32(tmp, n, hz);
}
/* the left (ch 0) or right channel the first circular buffer sink wrote in frame f since the reset, as host numbers
   in one of a few buffers that outlive the next calls */
static const i16* sink_frame_at(int f, int ch) {
    static i16 frames_read[4][SNX_ART_FRAME];
    static unsigned next;
    i16* x = frames_read[next++ % 4];
    const u8* at = g_mem + SNX_ART_G_RING + ((size_t)(f % SNX_ART_RING_FRAMES) * 2 + (size_t)ch) * SNX_ART_FRAME * 2;
    for (int i = 0; i < SNX_ART_FRAME; i++) x[i] = (i16)rd16(at + i * 2);
    return x;
}

/* a 1 kHz voice at 0.3 alone on the final mix's left, recorded by the first circular buffer sink */
static void bq_voice(void) {
    tone(0, 1000, 0.3, 0, 0, 1.0f);
    w32(voice(0), 0x58, SNX_ART_FINAL);
    w8(sink(1), 1, 1);
}
static void bq_set(int f, u8 on, const i16* b, const i16* a) {
    u8* bq = voice(0) + 0x24 + f * 0xC;
    w8(bq, 0, on);
    for (int k = 0; k < 3; k++) w16(bq, 2 + k * 2, (u16)b[k]);
    for (int k = 0; k < 2; k++) w16(bq, 8 + k * 2, (u16)a[k]);
}
/* Eden's ApplyBiquadFilterInt, or ApplyBiquadFilterFloat when dbl, over n samples from a clear state */
static void ref_biquad(const i16* in, i16* out, int n, const i16* b, const i16* a, int dbl) {
    int i;
    i64 s0 = 0, s1 = 0;
    double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
    for (i = 0; i < n; i++) {
        if (dbl) {
            double x = in[i], y = x * (b[0] / 16384.0) + x1 * (b[1] / 16384.0) + x2 * (b[2] / 16384.0) +
                                  y1 * (a[0] / 16384.0) + y2 * (a[1] / 16384.0);
            out[i] = (i16)(i32)y;
            x2 = x1, x1 = x, y2 = y1, y1 = y;
        } else {
            i64 x = in[i], y = (x * b[0] + s0 + (1 << 13)) >> 14;
            out[i] = (i16)y;
            s0 = s1 + b[1] * x + a[0] * y;
            s1 = b[2] * x + a[1] * y;
        }
    }
}

/* a 1 kHz voice on the submix, its buffers 0 and 1 to the final mix, effect 0 on the submix */
static u8* fx_voice(u8 type) {
    u8* e;
    tone(0, 1000, 0.3, 0, 0, 1.0f);
    wf(mix(SNX_ART_SUBMIX), 0x24, 1.0f);
    wf(mix(SNX_ART_SUBMIX), 0x24 + (24 + 1) * 4, 1.0f);
    w8(sink(1), 1, 1);
    memset(eff(1), 0, 0xC0); /* no aux after it */
    e = eff(0);
    memset(e, 0, 0xC0);
    w8(e, 0, type);
    w8(e, 1, 1);
    w32(e, 4, SNX_ART_SUBMIX);
    return e + 0x20;
}
/* frames from..to - 1 of the sink's left (ch 0) or right channel into x, from x[0] */
static void sink_copy(i16* x, int from, int to, int ch) {
    int f;
    for (f = from; f < to; f++) memcpy(x + (f - from) * SNX_ART_FRAME, sink_frame_at(f, ch), SNX_ART_FRAME * 2);
}
/* the submix's buffer from into the final mix's buffer to at gain g */
static void route(int from, int to, float g) { wf(mix(SNX_ART_SUBMIX), 0x24 + (from * 24 + to) * 4, g); }
/* a delay on effect 0: channels, time in ms, 48 kHz, then gains in Q14 */
static void delay_set(u8* p, u16 channels, u32 ms, i32 in_gain, i32 fb, i32 wet, i32 dry, i32 spread, i32 lp) {
    i32 g[6];
    w32(p, 0xC, (u32)channels << 16 | channels);
    w32(p, 0x10, 2 * ms);
    w32(p, 0x14, ms);
    w32(p, 0x18, (u32)SNX_ART_RATE << 14);
    g[0] = in_gain;
    g[1] = fb;
    g[2] = wet;
    g[3] = dry;
    g[4] = spread;
    g[5] = lp;
    for (int k = 0; k < 6; k++) w32(p, 0x1C + k * 4, (u32)g[k]);
}
/* Eden's InitializeDelayEffect, SetDelayEffectParameter and ApplyDelay, one or two channels */
static void ref_delay(int nch, const i16* const* in, i16* const* out, int n, int len, i32 in_gain, i32 fb, i32 wet,
                      i32 dry, i32 spread, i32 lp) {
    i32 m[2][2];
    i32 fbs;
    i32 dfg;
    i32 cross;
    i32 lpf;
    i32 lpg;
    int i;
    int pos;
    static i64 line[2][4096];
    i64 z[2] = {0, 0};
    memset(line, 0, sizeof line);
    fbs = (i32)(((i64)fb * 16056) >> 14);
    dfg = (i32)(((i64)fbs * (16384 - spread)) >> 14);
    cross = (i32)(((i64)spread * fbs) >> 14);
    lpf = (i32)(((i64)lp * 15564) >> 14);
    lpg = 16384 - lpf;
    m[0][0] = nch == 1 ? fbs : dfg;
    m[0][1] = cross;
    m[1][0] = cross;
    m[1][1] = dfg;
    for (i = 0, pos = 0; i < n; i++, pos = (pos + 1) % len) {
        int c;
        i64 x[2], d[2];
        for (c = 0; c < nch; c++) x[c] = (i64)in[c][i] * 64 * 16384, d[c] = line[c][pos];
        for (c = 0; c < nch; c++) {
            int j;
            i64 fed = 0;
            for (j = 0; j < nch; j++) fed += (d[j] * m[j][c]) >> 14;
            z[c] = (((((x[c] * in_gain) >> 14) + fed) * lpg) >> 14) + ((z[c] * lpf) >> 14);
            line[c][pos] = z[c];
        }
        for (c = 0; c < nch; c++) out[c][i] = (i16)((i32)((((x[c] * dry) >> 14) + ((d[c] * wet) >> 14)) >> 14) / 64);
    }
}

/* Eden's FixedPoint<50, 14>, as the reverb's expected values use it */
static i64 tq(float f) { return (i64)(f * 16384.0f); }
static i64 tmul(i64 a, i64 b) { return snx_mul_shr_s64(a, b, 14); }
static i64 tdiv(i64 a, i64 b) { return snx_div_shl_s64(a, 14, b); }
static i32 tint(i64 q) { return (i32)((q + ((q & 0x3FFF) >> 1)) >> 14); }
static i16 tsat(i32 v) { return (i16)(v > 32767 ? 32767 : v < -32768 ? -32768 : v); }

/* a reverb on effect 0 at 48 kHz: channels, then v from 0x14 in Q14 */
static void reverb_set(u8* p, u16 ch, const i32* v) {
    w32(p, 0xC, (u32)ch << 16 | ch);
    w32(p, 0x10, 48 << 14);
    for (int k = 0; k < 11; k++) w32(p, 0x14 + k * 4, (u32)v[k]);
}

/* Eden's reverb tables (renderer/command/effect/reverb.cpp), mode 0 to 4 */
static const float rv_early_t[5][11] = {
    {0.0f, 3.5f, 2.799988f, 3.899963f, 2.699951f, 13.399963f, 7.899963f, 8.399963f, 9.899963f, 12.0f, 12.5f},
    {0.0f, 11.799988f, 5.5f, 11.199951f, 10.399963f, 38.099976f, 22.199951f, 29.599976f, 21.199951f, 24.799988f, 40.0f},
    {0.0f, 41.5f, 20.5f, 41.299988f, 0.0f, 29.5f, 33.799988f, 45.199951f, 46.799988f, 0.0f, 50.0f},
    {33.099976f, 43.299988f, 22.799988f, 37.899963f, 14.899963f, 35.299988f, 17.899963f, 34.199951f, 0.0f, 43.299988f,
     50.0f},
    {0}};
static const float rv_early_g[5][10] = {
    {0.699951f, 0.679993f, 0.699951f, 0.679993f, 0.699951f, 0.679993f, 0.699951f, 0.679993f, 0.679993f, 0.679993f},
    {0.699951f, 0.679993f, 0.699951f, 0.679993f, 0.699951f, 0.679993f, 0.679993f, 0.679993f, 0.679993f, 0.679993f},
    {0.5f, 0.699951f, 0.699951f, 0.679993f, 0.5f, 0.679993f, 0.679993f, 0.699951f, 0.679993f, 0.0f},
    {0.929993f, 0.919983f, 0.869995f, 0.859985f, 0.939941f, 0.809998f, 0.799988f, 0.769958f, 0.759949f, 0.649963f},
    {0}};
static const float rv_fdn_t[5][4] = {{53.953247f, 79.192566f, 116.238770f, 130.615295f},
                                     {53.953247f, 79.192566f, 116.238770f, 170.615295f},
                                     {5.0f, 10.0f, 5.0f, 10.0f},
                                     {47.029968f, 71.0f, 103.0f, 170.0f},
                                     {53.953247f, 79.192566f, 116.238770f, 170.615295f}};
static const float rv_decay_t[5][4] = {{7.0f, 9.0f, 13.0f, 17.0f},
                                       {7.0f, 9.0f, 13.0f, 17.0f},
                                       {1.0f, 1.0f, 1.0f, 1.0f},
                                       {7.0f, 7.0f, 13.0f, 9.0f},
                                       {7.0f, 9.0f, 13.0f, 17.0f}};
static const float rv_fdn_max[4] = {53.9532470703125f, 79.19256591796875f, 116.23876953125f, 170.61529541015625f},
                   rv_decay_max[4] = {7.0f, 9.0f, 13.0f, 17.0f};

/* the early taps right after InitializeReverbEffect: each update moves them a quarter of the way */
static void rv_first_taps(int mode, i32 early_gain, i32 pre, i32* t, i64* g) {
    int k;
    for (k = 0; k < 10; k++) {
        i32 target = tint(tmul(pre + tq(rv_early_t[mode][k]), 48 << 14));
        if (target > 16800) target = 16800;
        t[k] = (target + 1) / 4;
        g[k] = tdiv(tmul(early_gain, tq(rv_early_g[mode][k])), 4 * 16384);
    }
}
/* tap k of input x at sample i: x 1 + t[k] samples back, as the pre-delay line holds it (64 times, at base gain 1) */
static i64 rv_tap(const i32* x, int i, i32 t, i64 g) {
    int at = i - 1 - t;
    return at < 0 ? 0 : tmul((i64)x[at] * 16384 * 64, g);
}

/* Eden's reverb for one channel (renderer/command/effect/reverb.cpp), its lines as arrays */
typedef struct {
    i64* b;
    int end, in, out, count, max;
    i64 decay;
} RvLine;
typedef struct {
    RvLine pre, fdn[4], decay[4];
    i32 t[10], pre_t;
    i64 g[10], hf_gain[4], hf_prev[4], fb[4], cosine;
} RvRef;
static void rv_line(RvLine* l, int len, i64 decay) {
    l->b = calloc((size_t)len + 1, sizeof *l->b);
    l->end = l->count = l->max = len;
    l->in = l->out = 0;
    l->decay = decay;
}
static i64 rv_tick(RvLine* l, i64 v) {
    i64 r = l->b[l->out];
    if (++l->out >= l->end) l->out = 0;
    l->b[l->in] = v;
    if (++l->in >= l->end) l->in = 0;
    return r;
}
static i64 rv_tapout(const RvLine* l, int idx) {
    int at = l->in - (idx + 1);
    return l->b[at < 0 ? at + l->count : at];
}
static float rv_pow10(float v) { return v >= 0.0f ? 1.0f : v <= -5.3f ? 0.0f : powf(10.0f, v); }
/* UpdateReverbEffectParameter, v as reverb_set takes it, all in range (the clamps left out) */
static void rv_update(RvRef* r, const i32* v) {
    int k, i;
    i32 pre_target;
    int em = v[0], lm = v[3];
    i64 rate = 48 << 14, pre = v[2];
    for (k = 0; k < 10; k++) {
        i64 gain;
        i32 target = tint(tmul(pre + tq(rv_early_t[em][k]), rate)), old = r->t[k] - 1;
        if (target > r->pre.max) target = r->pre.max;
        r->t[k] = old + (target - old) / 4 + 1;
        gain = tmul(v[1], tq(rv_early_g[em][k]));
        r->g[k] = r->g[k] + tdiv(gain - r->g[k], 4 * 16384);
    }
    pre_target = tint(tmul(pre + tq(rv_early_t[em][10]), rate));
    if (pre_target > r->pre.max) pre_target = r->pre.max;
    r->pre_t = r->pre_t + (pre_target - r->pre_t) / 4;
    r->cosine = tq(cosf((float)tdiv(1280 * 16384, rate) / 16384.0f * 3.14159265358979f / 180.0f));
    for (i = 0; i < 4; i++) {
        i64 a, b, c, dd, gain;
        float hf;
        RvLine *f = &r->fdn[i], *d = &r->decay[i];
        i32 tf = tint(tmul(tq(rv_fdn_t[lm][i]), rate)), td = tint(tmul(tq(rv_decay_t[lm][i]), rate));
        f->count = f->count + ((tf > f->max ? f->max : tf) - f->count) / 4;
        f->end = f->count - 1;
        d->count = d->count + ((td > d->max ? d->max : td) - d->count) / 4;
        d->end = d->count - 1;
        d->decay = d->decay + tdiv(tmul(9830, 16384 - v[7]) - d->decay, 4 * 16384); /* 0.5999755859375 */
        a = (i64)(f->max + d->max) * 16384 * -3;
        b = tdiv(a, tmul(v[5], rate));
        c = 0;
        dd = 16384;
        hf = (float)v[6] / 16384.0f;
        if (hf <= 0.9949f) {
            float ff, gg, h2;
            float e = rv_pow10((float)tmul(tq((1.0f / hf - 1.0f) * 2 / 100), tdiv(b, 10 * 16384)) / 16384.0f);
            if (e < 0.0001f) e = 0.0001f;
            ff = 1.0f - e;
            gg = 2.0f - (float)r->cosine / 16384.0f * e * 2;
            h2 = gg * gg - ff * ff * 4;
            c = tq((gg - sqrtf(h2 < 0 ? 0 : h2)) / (ff * 2.0f));
            dd = 16384 - c;
        }
        r->hf_prev[i] = r->hf_prev[i] + tdiv(c - r->hf_prev[i], 4 * 16384);
        gain = tmul(tmul(tq(rv_pow10((float)tdiv(b, 1000 * 16384) / 16384.0f)), dd), 11585); /* 0.70709228515625 */
        r->hf_gain[i] = r->hf_gain[i] + tdiv(gain - r->hf_gain[i], 4 * 16384);
        r->fb[i] = 0;
    }
}
/* InitializeReverbEffect, then ApplyReverbEffect<1> over x into y */
static void rv_ref(const i16* x, i16* y, int frames_n, const u8* state, const i32* v) {
    int f;
    static RvRef r;
    for (f = 0; f < frames_n; f++) {
        int i;
        if (state[f] == SNX_AUDREN_PARAM_INITIALIZED) {
            free(r.pre.b);
            for (i = 0; i < 4; i++) free(r.fdn[i].b), free(r.decay[i].b);
            memset(&r, 0, sizeof r);
            for (i = 0; i < 4; i++) {
                rv_line(&r.fdn[i], (int)(tmul(tq(rv_fdn_max[i]), 48 << 14) >> 14), 16384);
                rv_line(&r.decay[i], (int)(tmul(tq(rv_decay_max[i]), 48 << 14) >> 14), 0);
            }
            rv_line(&r.pre, 16800, 16384);
            rv_update(&r, v);
        } else if (state[f] == SNX_AUDREN_PARAM_UPDATING)
            rv_update(&r, v);
        for (i = f * SNX_ART_FRAME; i < (f + 1) * SNX_ART_FRAME; i++) {
            int k;
            i64 pd;
            i64 o = 0, ap[4], mix[4];
            for (k = 0; k < 10; k++) o += tmul(rv_tapout(&r.pre, r.t[k]), r.g[k]);
            r.pre.b[r.pre.in] = tmul((i64)x[i] * 16384 * 64, v[8]);
            if (++r.pre.in >= r.pre.end) r.pre.in = 0;
            for (k = 0; k < 4; k++)
                r.fb[k] = tmul(r.fb[k], r.hf_prev[k]) + tmul(r.fdn[k].b[r.fdn[k].out], r.hf_gain[k]);
            pd = tmul(rv_tapout(&r.pre, r.pre_t), v[4]);
            mix[0] = r.fb[2] + r.fb[1] + pd;
            mix[1] = -r.fb[0] - r.fb[3] + pd;
            mix[2] = r.fb[0] - r.fb[3] + pd;
            mix[3] = r.fb[1] - r.fb[2] + pd;
            for (k = 0; k < 4; k++) {
                i64 mixed = mix[k] - tmul(r.decay[k].b[r.decay[k].out], r.decay[k].decay);
                ap[k] = rv_tick(&r.decay[k], mixed) + tmul(mixed, r.decay[k].decay);
                rv_tick(&r.fdn[k], ap[k]);
            }
            y[i] = tsat(tint(tmul((i64)x[i] * 16384, v[10]) + tdiv(tmul(o + ap[0], v[9]), 64 * 16384)));
        }
    }
}

/* an I3DL2 reverb on effect 0 at 48 kHz: channels, then the floats from 0x18 */
static void i3dl2_set(u8* p, u16 ch, const float* v) {
    w32(p, 0xC, (u32)ch << 16 | ch);
    w32(p, 0x14, SNX_ART_RATE);
    for (int k = 0; k < 12; k++) wf(p, 0x18 + k * 4, v[k]);
}

/* Eden's I3DL2 tables (renderer/command/effect/i3dl2_reverb.cpp) */
static const float i3_min_t[4] = {5.0f, 6.0f, 13.0f, 14.0f},
                   i3_max_t[4] = {45.7042007446f, 82.7817001343f, 149.938293457f, 271.575805664f},
                   i3_d0_t[4] = {17.0f, 13.0f, 9.0f, 7.0f}, i3_d1_t[4] = {19.0f, 11.0f, 10.0f, 6.0f};
static const float i3_tap_t[20] = {0.0171360000968f,
                                   0.0591540001333f,
                                   0.161733001471f,
                                   0.390186011791f,
                                   0.425262004137f,
                                   0.455410987139f,
                                   0.689737021923f,
                                   0.74590998888f,
                                   0.833844006062f,
                                   0.859502017498f,
                                   0.0f,
                                   0.0750240013003f,
                                   0.168788000941f,
                                   0.299901008606f,
                                   0.337442994118f,
                                   0.371903002262f,
                                   0.599011003971f,
                                   0.716741025448f,
                                   0.817858994007f,
                                   0.85166400671f};
static const float i3_tap_g[20] = {0.67096f, 0.61027f, 1.0f,     0.3568f,  0.68361f, 0.65978f, 0.51939f,
                                   0.24712f, 0.45945f, 0.45021f, 0.64196f, 0.54879f, 0.92925f, 0.3827f,
                                   0.72867f, 0.69794f, 0.5464f,  0.24563f, 0.45214f, 0.44042f};

/* the twenty early tap lengths at 48 kHz, at most the early line's 400 ms */
static void i3_taps(const float* v, i32* t) {
    int k;
    float time = (v[9] * 0.9998f + 0.02f) * 1000.0f;
    for (k = 0; k < 20; k++) {
        t[k] = tint(tmul(tq(v[8] * 1000.0f + time * i3_tap_t[k]), tq(48.0f)));
        if (t[k] >= 19200) t[k] = 19200;
    }
}

/* Eden's I3DL2 reverb for one channel at 48 kHz, its lines as arrays */
typedef struct {
    i64* b;
    int max, in, out, delay;
    float wet;
} I3Line;
static void i3l_delay(I3Line* l, int d) {
    if (l->max < d) return;
    l->delay = d;
    l->in = (l->out + d) % (l->max + 1);
}
static void i3l_make(I3Line* l, int len) {
    l->b = calloc((size_t)len + 1, sizeof *l->b);
    l->max = len;
    l->out = 0;
    i3l_delay(l, len);
    l->wet = 0;
}
static i64 i3l_tick(I3Line* l, i64 v) {
    i64 r;
    l->b[l->in] = v;
    if (++l->in >= l->max) l->in = 0;
    r = l->b[l->out];
    if (++l->out >= l->max) l->out = 0;
    return r;
}
static i64 i3l_tap(const I3Line* l, int idx) {
    int at = l->in - (idx + 1);
    return l->b[at < 0 ? at + l->max + 1 : at];
}
typedef struct {
    float lp0, lp1, lp2, eg, lg, dry, coef[4][3], shelf[4];
    I3Line early, fdn[4], d0[4], d1[4];
    int t[20], etl;
} I3Ref;
static float i3_pow10(float x) { return x >= 0.0f ? 1.0f : x <= -5.3f ? 0.0f : powf(10.0f, x); }
static float i3_cos(float d) { return cosf(d * 3.14159265358979f / 180.0f); }
/* UpdateI3dl2ReverbEffectParameter, v as i3dl2_set takes it, all in range */
static void i3_update(I3Ref* r, const float* v) {
    i64 hf;
    float echo;
    int i;
    i64 delay = tq(48.0f);
    r->dry = v[11];
    r->eg = i3_pow10((float)tq(fminf(v[4] + v[5], 5000.0f) / 2000.0f) / 16384.0f);
    r->lg = i3_pow10((float)tq(fminf(v[4] + v[6], 5000.0f) / 2000.0f) / 16384.0f);
    hf = tq(i3_pow10(v[0] / 2000.0f));
    if (hf >= 16384)
        r->lp1 = 0.0f, r->lp2 = 1.0f;
    else {
        i64 a = tq(1.0f - (float)hf / 16384.0f), b = 32768 + tmul(tq(-i3_cos(v[1] * 256.0f / 48000.0f)), hf * 2);
        float bf = (float)b / 16384.0f, af = (float)a / 16384.0f;
        i64 c = tq(sqrtf(powf(bf, 2.0f) + powf(af, 2.0f) * -4.0f));
        r->lp1 = fminf((float)tdiv(b - c, a * 2) / 16384.0f, 0.99723f);
        r->lp2 = 1.0f - r->lp1;
    }
    r->etl = tint(tmul(tq((v[8] + v[9]) * 1000.0f), delay));
    echo = v[7] * 0.6f * 0.01f;
    for (i = 0; i < 4; i++) {
        float a, b, w, c, d, e;
        i3l_delay(&r->fdn[i], tint(tmul(tq(i3_min_t[i] + v[10] / 100.0f * (i3_max_t[i] - i3_min_t[i])), delay)));
        a = (float)(r->fdn[i].delay + r->d0[i].delay + r->d1[i].delay) * -60.0f / (v[2] * 48000.0f);
        b = a / v[3];
        w = v[1] * 0.5f * 128.0f / 48000.0f;
        c = i3_cos(w) / sinf(w * 3.14159265358979f / 180.0f);
        d = i3_pow10((b - a) / 40.0f);
        e = i3_pow10((b + a) / 40.0f) * 0.7071f;
        r->coef[i][0] = (c * d + 1.0f) * e / (c + d);
        r->coef[i][1] = (1.0f - c * d) * e / (c + d);
        r->coef[i][2] = (c - d) / (c + d);
        r->d0[i].wet = echo;
        r->d1[i].wet = echo * -0.9f;
    }
    i3_taps(v, r->t);
}
/* InitializeI3dl2ReverbEffect, then ApplyI3dl2ReverbEffect<1> over x into y, state[f] for frame f */
static void i3_ref(const i16* x, i16* y, int frames_n, const u8* state, const float* v) {
    int f;
    static I3Ref r;
    for (f = 0; f < frames_n; f++) {
        int i;
        if (state[f] == SNX_AUDREN_PARAM_INITIALIZED) {
            free(r.early.b);
            for (i = 0; i < 4; i++) free(r.fdn[i].b), free(r.d0[i].b), free(r.d1[i].b);
            memset(&r, 0, sizeof r);
            for (i = 0; i < 4; i++) {
                i3l_make(&r.fdn[i], (int)(tmul(tq(i3_max_t[i]), tq(48.0f)) >> 14));
                i3l_make(&r.d0[i], (int)(tmul(tq(i3_d0_t[i]), tq(48.0f)) >> 14));
                i3l_make(&r.d1[i], (int)(tmul(tq(i3_d1_t[i]), tq(48.0f)) >> 14));
            }
            i3l_make(&r.early, 19200);
            i3_update(&r, v);
        } else if (state[f] == SNX_AUDREN_PARAM_UPDATING)
            i3_update(&r, v);
        for (i = f * SNX_ART_FRAME; i < (f + 1) * SNX_ART_FRAME; i++) {
            int k;
            float out;
            i64 late, mix[4];
            i64 etl = i3l_tap(&r.early, r.etl), o = 0, fl[4], ap = 0;
            for (k = 0; k < 20; k++) o += tmul(i3l_tap(&r.early, r.t[k]), tq(i3_tap_g[k]));
            r.lp0 = (float)(tmul((i64)x[i] * 16384, tq(r.lp2)) + tq(r.lp0 * r.lp1)) / 16384.0f;
            i3l_tick(&r.early, tq(r.lp0));
            o = tmul(o, tq(r.eg));
            for (k = 0; k < 4; k++) {
                i64 rd = r.fdn[k].b[r.fdn[k].out];
                fl[k] = tmul(rd, tq(r.coef[k][0])) + tq(r.shelf[k]);
                r.shelf[k] = (float)(tmul(fl[k], tq(r.coef[k][2])) + tmul(rd, tq(r.coef[k][1]))) / 16384.0f;
            }
            late = tmul(etl, tq(r.lg));
            mix[0] = fl[1] + fl[2] + late;
            mix[1] = -fl[0] - fl[3] + late;
            mix[2] = fl[0] - fl[3] + late;
            mix[3] = fl[1] - fl[2] + late;
            for (k = 0; k < 4; k++) {
                i64 m = mix[k] - tmul(r.d0[k].b[r.d0[k].out], tq(r.d0[k].wet));
                i64 a = i3l_tick(&r.d0[k], m) + tmul(m, tq(r.d0[k].wet));
                m = a - tmul(r.d1[k].b[r.d1[k].out], tq(r.d1[k].wet));
                a = i3l_tick(&r.d1[k], m) + tmul(m, tq(r.d1[k].wet));
                i3l_tick(&r.fdn[k], a);
                if (k == 0) ap = a;
            }
            out = (float)(o + ap + tq(r.dry * (float)x[i])) / 16384.0f;
            y[i] = tsat((i32)out);
        }
    }
}

/* a compressor on effect 0, mono: v is threshold, ratio, detector rate, attack, release, out gain and makeup on */
static void compressor_set(u8* p, const float* v) {
    w32(p, 0xC, 1u << 16 | 1);
    w32(p, 0x10, SNX_ART_RATE);
    wf(p, 0x14, v[0]);
    wf(p, 0x18, v[1]);
    for (int k = 0; k < 4; k++) wf(p, 0x24 + k * 4, v[2 + k]);
    w8(p, 0x35, v[6] != 0.0f);
}
/* Eden's compressor (renderer/command/effect/compressor.cpp) for one channel */
static float cmp_db(float db) {
    float e = db / 20.0f * 3.3219f;
    return powf(2.0f, (e - truncf(e)) * 0.69315f);
}
static void cmp_ref(const i16* x, i16* y, int frames_n, const u8* state, const float* v) {
    int f;
    float s00 = 0, s04 = 1, s08 = 1, c0c = 0, c10 = 0, c14 = 0, c20 = 0;
    for (f = 0; f < frames_n; f++) {
        float d00;
        int i;
        if (state[f] != SNX_AUDREN_PARAM_UPDATED) {
            float r, mk;
            if (state[f] == SNX_AUDREN_PARAM_INITIALIZED) s00 = 0, s04 = 1, s08 = 1;
            r = 1.0f / v[1];
            mk = v[6] != 0.0f ? v[0] * 0.5f * (r - 1.0f) - 3.0f : 0.0f;
            c20 = cmp_db(v[5] + mk), c0c = (1.0f - r) / 6.0f, c14 = v[0] + 1.5f, c10 = v[0] - 1.5f;
        }
        d00 = s00;
        for (i = f * SNX_ART_FRAME; i < (f + 1) * SNX_ART_FRAME; i++) {
            float b, c, rate, held;
            d00 += v[2] * ((float)((i64)x[i] * x[i]) / 1.0f - s00);
            b = -100.0f;
            c = 0.0f;
            if (d00 >= 1.0e-10) b = log10f(d00) * 10.0f, c = 1.0f;
            if (b >= c10) c = cmp_db(b >= c14 ? (1.0f / v[1] - 1.0f) * (b - v[0]) : (b - c10) * (b - c10) * -c0c);
            rate = v[3];
            held = c;
            if (s04 - c <= 0.08f) {
                rate = v[4];
                if (s04 - c >= -0.08f && fabsf(s08 - c) >= 0.001f) held = s04;
            }
            s04 = held;
            s08 += (c - s08) * rate;
            y[i] = tsat((i32)((float)x[i] * s08 * c20));
        }
        s00 = d00;
    }
}

/* the first sample where a and b differ, -1 if none */
static int first_diff(const i16* a, const i16* b, int n) {
    int i;
    for (i = 0; i < n; i++)
        if (a[i] != b[i]) return i;
    return -1;
}

/* the aux ring's channel ch in its most recent full frame */
static double captured(int ch, double hz) {
    u32 woff = rd32(g_mem + SNX_ART_G_AUX_SEND + 0x40 + 4);
    u32 frame_at = (woff + SNX_ART_AUX_COUNT - 2 * SNX_ART_FRAME) % SNX_ART_AUX_COUNT;
    const u8* ring = g_mem + SNX_ART_G_AUX_SEND + 0x80 + (size_t)(frame_at + ch * SNX_ART_FRAME) * 4;
    i32 x[SNX_ART_FRAME];
    for (int i = 0; i < SNX_ART_FRAME; i++) x[i] = (i32)rd32(ring + i * 4);
    return level_i32(x, SNX_ART_FRAME, hz);
}

static void reset(void) {
    audren_state_free(st);
    u8 params[0x34] = {0};
    u32 p[13] = {SNX_ART_RATE, SNX_ART_FRAME, 8, SNX_ART_MIXES - 1, SNX_ART_VOICES, SNX_ART_SINKS, SNX_ART_EFFECTS};
    p[9] = split_counts[0], p[10] = split_counts[1]; /* splitter infos 0x24, destinations 0x28 */
    memcpy(params, p, sizeof p);
    st = audren_state_new(params);
    CHECK(st != NULL, "no state");
    memset(g_mem, 0, SNX_ART_G_ARENA);
    layout();
}

/* the reply has the counts the renderer opened with, as Eden sizes it, at the revision-9 effect stride too. A section
   with more or fewer entries is refused before anything is written, as Eden and Ryubing refuse it (#222's were
   revision 15's longer voices: update_voice_v2) */
static void update_output_counts(void) {
    enum { voices = 128, effects = 68, sinks = 2, pools = effects + voices * 4 };
    const u32 counts[][5] = {/* pools, voices, voice channel resources, effects, sinks */
                             {pools, voices, voices, effects, sinks},
                             {pools + 8, voices, voices, effects, sinks},
                             {pools, voices + 8, voices, effects, sinks},
                             {pools, voices, voices + 8, effects, sinks},
                             {pools, voices, voices, effects + 8, sinks},
                             {pools, voices, voices, effects, sinks + 8},
                             {pools + 8, voices + 8, voices + 8, effects + 8, sinks + 8},
                             {pools, voices - 1, voices, effects, sinks},
                             {0, 0, 0, 0, 0}};
    const u32 revisions[] = {4, 5, 9};
    size_t r, c;
    for (r = 0; r < sizeof revisions / sizeof *revisions; r++) {
        u32 revn = revisions[r], rev = ('R' << 0) | ('E' << 8) | ('V' << 16) | ((u32)('0' + revn) << 24);
        u32 effect_stride = revn >= 9 ? 0x90 : 0x10;
        u32 reply = 0x40 + pools * 0x10 + voices * 0x10 + effects * effect_stride + sinks * 0x20 + 0x10 + 0xB0 +
                    (revn >= 5 ? 0x10 : 0);
        if (revn == 9) CHECK(reply == 21456, "reported output size is %u", reply);
        for (c = 0; c < sizeof counts / sizeof *counts; c++) {
            u32 params[13] = {48000, 240, 216, 0, voices, sinks, effects};
            ARState* state = audren_state_new((const u8*)params);
            u32 h[16] = {rev,
                         0x10,
                         counts[c][0] * 0x20,
                         counts[c][1] * 0x170,
                         counts[c][2] * 0x70,
                         counts[c][3] * 0xC0,
                         revn >= 7 ? 0x20 : 0, /* REV7's mixes: their header, counting none */
                         counts[c][4] * 0x140};
            u32 size = 0x40 + h[1] + h[2] + h[3] + h[4] + h[5] + h[6] + h[7];
            u8 *input, *output;
            u32 i;
            CHECK(state != NULL, "no state for output counts");
            if (!state) continue;
            state->frames = 42;
            state->sink[1].last2 = 0x12345678;
            h[15] = size;
            /* The input and reply have the same lifetime and share one allocation. */
            input = calloc(1, size + reply + 32);
            CHECK(input != NULL, "no update buffers");
            if (input) {
                int rc;
                output = input + size;
                put_header(input, h);
                for (i = 0; i < counts[c][0]; i++) w32(input, 0x40 + h[1] + i * 0x20 + 0x10, 4);
                memset(output, 0xA5, reply + 32);
                rc = audren_state_update(state, input, size, output, reply);
                if (c) {
                    CHECK(rc == -41, "revision %u, count case %lu: update returned %d", revn, (unsigned long)c, rc);
                    for (i = 0; i < reply + 32; i++) CHECK(output[i] == 0xA5, "refused update wrote byte %u", i);
                    free(input);
                    audren_state_free(state);
                    continue;
                }
                CHECK(rc == 0, "revision %u: update returned %d", revn, rc);
                if (!rc) {
                    u32 at = 0x40 + pools * 0x10 + voices * 0x10;
                    CHECK(rd32(output + 8) == pools * 0x10, "wrong pool output size");
                    CHECK(rd32(output + 12) == voices * 0x10, "wrong voice output size");
                    CHECK(rd32(output + 20) == effects * effect_stride, "wrong effect output size");
                    CHECK(rd32(output + 28) == sinks * 0x20, "wrong sink output size");
                    CHECK(rd32(output + 60) == reply, "wrong total output size");
                    CHECK(rd32(output + 0x40 + (pools - 1) * 0x10) == 5u, "wrong last pool status");
                    CHECK(output[at + (effects - 1) * effect_stride] == 4u, "wrong last effect status");
                    at += effects * effect_stride;
                    CHECK(rd32(output + at + 0x20) == 0x12345678u, "sink output was misplaced");
                    if (revn >= 5) CHECK(rd64(output + reply - 0x10) == 42, "frame count was misplaced");
                    for (i = 0; i < voices * 0x10; i++) /* no voice in use: nothing written there */
                        CHECK(output[0x40 + pools * 0x10 + i] == 0, "voice output byte %u not cleared", i);
                }
                for (i = 0; i < 32; i++) CHECK(output[reply + i] == 0xA5, "output guard byte %u overwritten", i);
                memset(output, 0xA5, reply + 32);
                CHECK(audren_state_update(state, input, size, output, reply - 1) == -4, "short output accepted");
                CHECK(audren_state_update(state, input, size - 1, output, reply) == -41, "short input accepted");
                if (revn >= 7) { /* REV7's mixes without their header, even with none: Eden and Ryubing refuse it */
                    w32(input, 6 * 4, 0);
                    CHECK(audren_state_update(state, input, size - 0x20, output, reply) == -41,
                          "revision %u took mixes without their header", revn);
                }
                for (i = 0; i < reply + 32; i++) CHECK(output[i] == 0xA5, "rejected update wrote output byte %u", i);
            }
            free(input);
            audren_state_free(state);
        }
    }
}

/* #222: from revision 15 a voice's input is 0x188 bytes (Eden's VoiceInParameterV2), its biquads in floats, 0x18
   each, and what follows them 0x18 further on. Read at 0x170 a voice, Metroid Prime 4's 128 voices made 136, a
   reply 128 bytes over its 21,456, and every voice's fields after the first biquad came from the wrong bytes */
static void update_voice_v2(void) {
    enum { voices = 128, effects = 68, sinks = 2, pools = effects + voices * 4, stride = 0x188 };
    /* a 100 Hz low-pass at 48 kHz: its b are under Q14's step, which would leave a 200 Hz tone 21.1 dB down, not 12.3 */
    static const float lpb[3] = {4.24433674e-05f, 8.48867348e-05f, 4.24433674e-05f},
                       lpa[2] = {1.98148847f, -0.98165828f}, bq1b[3] = {0.25f, -0.5f, 0.125f},
                       bq1a[2] = {0.375f, -0.0625f};
    const u32 rev = ('R' << 0) | ('E' << 8) | ('V' << 16) | ((u32)('0' + 15) << 24), reply = 21456;
    u32 params[13] = {48000, 240, 216, 0, voices, sinks, effects};
    u32 h[16] = {rev, 0x10, pools * 0x20, voices * stride, voices * 0x70, effects * 0xC0, 0x20, sinks * 0x140};
    u32 size = 0x40 + h[1] + h[2] + h[3] + h[4] + h[5] + h[6] + h[7], i;
    ARState* state = audren_state_new((const u8*)params);
    u8 *input = calloc(1, size + reply), *voices_in, *vi;
    const ARVoice* v;
    float x[SNX_ART_FRAME];
    i32 last[SNX_ART_FRAME];
    ARBiquad bq;
    int f;
    double w = 2.0 * M_PI * 200.0 / SNX_ART_RATE, nr, ni, dr, di, got, want;
    CHECK(state != NULL && input != NULL, "no state or buffers for revision 15 voices");
    if (!state || !input) {
        free(input);
        audren_state_free(state);
        return;
    }
    h[15] = size;
    put_header(input, h);
    for (i = 0; i < pools; i++) w32(input, 0x40 + h[1] + i * 0x20 + 0x10, 4);
    voices_in = input + 0x40 + h[1] + h[2] + h[4];
    vi = voices_in + stride; /* voice 1: new, in use, every field set */
    w8(vi, 8, 1);
    w8(vi, 9, 1);
    w8(vi, 0xB, 1);
    w32(vi, 0xC, 32000);
    w32(vi, 0x18, 2);
    wf(vi, 0x1C, 1.5f);
    wf(vi, 0x20, 0.75f);
    w8(vi, 0x24, 1); /* biquad 0: enabled 0, b 4, a 0x10 */
    for (i = 0; i < 3; i++) wf(vi, 0x28 + i * 4, lpb[i]);
    for (i = 0; i < 2; i++) wf(vi, 0x34 + i * 4, lpa[i]);
    w8(vi, 0x3C, 1); /* biquad 1, 0x18 on */
    for (i = 0; i < 3; i++) wf(vi, 0x40 + i * 4, bq1b[i]);
    for (i = 0; i < 2; i++) wf(vi, 0x4C + i * 4, bq1a[i]);
    w32(vi, 0x54, 3);
    w32(vi, 0x58, 2);
    w64(vi, 0x60, 0x1234560);
    w64(vi, 0x68, 0x40);
    w32(vi, 0x70, 5);
    w32(vi, 0x74, 7);
    for (i = 0; i < 4; i++) w8(vi, 0x78 + i * 0x38 + 0x1A, 1); /* its wave buffers already sent */
    w32(vi, 0x158, 1);
    w32(vi, 0x15C, 2);
    w8(vi, 0x174, 1);
    w8(vi, 0x176, 1);
    vi = voices_in + (voices - 1) * stride; /* the last voice */
    w8(vi, 8, 1);
    w8(vi, 9, 1);
    w32(vi, 0xC, 22050);
    w32(vi, 0x70, 6);
    for (i = 0; i < 4; i++) w8(vi, 0x78 + i * 0x38 + 0x1A, 1);
    CHECK(audren_state_update(state, input, size, input + size, reply) == 0, "revision 15 update refused");
    CHECK(rd32(input + size + 12) == voices * 0x10 && rd32(input + size + 60) == reply, "revision 15 reply sizes");
    v = &state->voice[1];
    CHECK(v->in_use && v->format == 1 && v->rate == 32000 && v->channels == 2 && v->pitch == 1.5f && v->volume == 0.75f,
          "revision 15 voice: rate %u, %u channels", v->rate, v->channels);
    CHECK(v->bq_on[0] == 1 && v->bq_b[0][0] == lpb[0] && v->bq_b[0][1] == lpb[1] && v->bq_b[0][2] == lpb[2] &&
              v->bq_a[0][0] == lpa[0] && v->bq_a[0][1] == lpa[1],
          "revision 15 float biquad: b %g %g %g, a %g %g", v->bq_b[0][0], v->bq_b[0][1], v->bq_b[0][2], v->bq_a[0][0],
          v->bq_a[0][1]);
    CHECK(v->bq_on[1] == 1 && v->bq_b[1][0] == bq1b[0] && v->bq_b[1][1] == bq1b[1] && v->bq_b[1][2] == bq1b[2] &&
              v->bq_a[1][0] == bq1a[0] && v->bq_a[1][1] == bq1a[1],
          "revision 15 second biquad: on %u, b %g %g %g, a %g %g", v->bq_on[1], v->bq_b[1][0], v->bq_b[1][1],
          v->bq_b[1][2], v->bq_a[1][0], v->bq_a[1][1]);
    /* through the voices' biquad, a half-scale 200 Hz tone comes out at the float filter's level */
    memset(&bq, 0, sizeof bq);
    for (f = 0; f < 20; f++) { /* the transient gone */
        for (i = 0; i < SNX_ART_FRAME; i++) x[i] = (float)lrint(16384.0 * sin(w * i));
        biquad(x, SNX_ART_FRAME, v->bq_b[0], v->bq_a[0], &bq, 1);
    }
    for (i = 0; i < SNX_ART_FRAME; i++) last[i] = (i32)x[i];
    nr = lpb[0] + lpb[1] * cos(w) + lpb[2] * cos(2 * w);
    ni = -lpb[1] * sin(w) - lpb[2] * sin(2 * w);
    dr = 1.0 - lpa[0] * cos(w) - lpa[1] * cos(2 * w);
    di = lpa[0] * sin(w) + lpa[1] * sin(2 * w);
    want = 0.5 * sqrt((nr * nr + ni * ni) / (dr * dr + di * di));
    got = level_i32(last, SNX_ART_FRAME, 200.0);
    CHECK(fabs(20.0 * log10(got / want)) < 0.05, "revision 15 low-pass at 200 Hz: %.2f dB, want %.2f dB",
          20.0 * log10(got / 0.5), 20.0 * log10(want / 0.5));
    CHECK(v->wave_count == 3 && v->wave_index == 2 && v->data == 0x1234560 && v->data_size == 0x40 && v->mix_id == 5 &&
              v->splitter_id == 7,
          "revision 15 voice after its biquads: mix %u, splitter %u", v->mix_id, v->splitter_id);
    CHECK(v->res[0] == 1 && v->res[1] == 2 && v->flags == 1 && v->quality == 1,
          "revision 15 voice tail: resources %u %u, flags %u, quality %u", v->res[0], v->res[1], v->flags, v->quality);
    v = &state->voice[voices - 1];
    CHECK(v->in_use && v->rate == 22050 && v->mix_id == 6, "revision 15 last voice: rate %u, mix %u", v->rate,
          v->mix_id);
    free(input);
    audren_state_free(state);
}

/* from REV7 the mixes are dirty-only: an entry names its mix, and one past the opened mixes is no other mix */
static void update_mix_dirty_only(void) {
    enum { voices = 128, effects = 68, sinks = 2, pools = effects + voices * 4, entries = 2 };
    const u32 rev = ('R' << 0) | ('E' << 8) | ('V' << 16) | ((u32)('0' + 9) << 24), reply = 21456;
    u32 params[13] = {48000, 240, 216, 2, voices, sinks, effects}; /* two submixes: mixes 0 to 2 */
    u32 h[16] = {rev,           0x10,           pools * 0x20,           voices * 0x170,
                 voices * 0x70, effects * 0xC0, 0x20 + entries * 0x930, sinks * 0x140};
    u32 size = 0x40 + h[1] + h[2] + h[3] + h[4] + h[5] + h[6] + h[7], i;
    ARState* state = audren_state_new((const u8*)params);
    u8 *input = calloc(1, size + reply), *mixes;
    CHECK(state != NULL && input != NULL, "no state or buffers for dirty-only mixes");
    if (!state || !input) {
        free(input);
        audren_state_free(state);
        return;
    }
    h[15] = size;
    put_header(input, h);
    for (i = 0; i < pools; i++) w32(input, 0x40 + h[1] + i * 0x20 + 0x10, 4);
    mixes = input + 0x40 + h[1] + h[2] + h[4] + h[3] + h[5];
    w32(mixes, 4, entries);
    mixes += 0x20;
    wf(mixes, 0, 0.25f); /* entry 0: mix 2 */
    w8(mixes, 0xC, 1);
    w32(mixes, 0x10, 2);
    wf(mixes + 0x930, 0, 0.5f); /* entry 1: a mix not opened */
    w8(mixes + 0x930, 0xC, 1);
    w32(mixes + 0x930, 0x10, 99);
    logged[0] = 0;
    CHECK(audren_state_update(state, input, size, input + size, reply) == 0, "dirty-only mix update refused");
    CHECK(state->mix[2].in_use && state->mix[2].volume == 0.25f, "entry 0 did not reach mix 2: in use %u",
          state->mix[2].in_use);
    CHECK(!state->mix[1].in_use && state->mix[1].volume != 0.5f, "an entry for mix 99 was written over mix 1");
    CHECK(state->warned & SNX_AUDREN_WARN_MIX_ID, "the entry for mix 99 was skipped without a word");
    CHECK(strstr(logged, "audren: the update of mix 99 is skipped: the renderer opened 3 mixes\n"), "logged: %s",
          logged);
    free(input);
    audren_state_free(state);
}

/* revision 14 still writes 0x170 voices: the 0x188 ones start at 15, as Eden and Ryubing gate them */
static void update_voice_rev14(void) {
    enum { voices = 128, effects = 68, sinks = 2, pools = effects + voices * 4, stride = 0x170 };
    const u32 rev = ('R' << 0) | ('E' << 8) | ('V' << 16), reply = 21456;
    u32 params[13] = {48000, 240, 216, 0, voices, sinks, effects};
    u32 h[16] = {rev | (u32)('0' + 14) << 24,
                 0x10,
                 pools * 0x20,
                 voices * stride,
                 voices * 0x70,
                 effects * 0xC0,
                 0x20,
                 sinks * 0x140};
    u32 size = 0x40 + h[1] + h[2] + h[3] + h[4] + h[5] + h[6] + h[7], i;
    ARState* state = audren_state_new((const u8*)params);
    u8 *input = calloc(1, size + reply), *vi;
    CHECK(state != NULL && input != NULL, "no state or buffers for revision 14 voices");
    if (!state || !input) {
        free(input);
        audren_state_free(state);
        return;
    }
    h[15] = size;
    put_header(input, h);
    vi = input + 0x40 + h[1] + h[2] + h[4] + stride; /* voice 1 */
    w8(vi, 8, 1);
    w8(vi, 9, 1);
    w32(vi, 0xC, 32000);
    w32(vi, 0x58, 5);
    for (i = 0; i < 4; i++) w8(vi, 0x60 + i * 0x38 + 0x1A, 1);
    CHECK(audren_state_update(state, input, size, input + size, reply) == 0, "revision 14 update refused");
    CHECK(state->voice[1].rate == 32000 && state->voice[1].mix_id == 5, "revision 14 voice: rate %u, mix %u",
          state->voice[1].rate, state->voice[1].mix_id);
    w32(input, 0, rev | (u32)('0' + 15) << 24);
    CHECK(audren_state_update(state, input, size, input + size, reply) == -41, "revision 15 took 0x170 voices");
    free(input);
    audren_state_free(state);
}

void pm_init(PMutex* m) { (void)m; }

int main(void) {
    hle_init();
    u32 nch;
    int set;
    g_mem = calloc(1, SNX_ART_G_ARENA);
    if (!g_mem) return 1;
    update_output_counts();
    update_voice_v2();
    update_voice_rev14();
    update_mix_dirty_only();

    /* the resampler at a ratio of one: an 11 kHz voice keeps its level at default quality as at high */
    for (u8 q = 0; q < 2; q++) {
        reset();
        w8(eff(1), 2, 1); /* aux on */
        tone(0, 11000, 0.5, q, 0, 1.0f);
        frames(100);
        double lv = captured(0, 11000);
        CHECK(fabs(lv - 0.5) < 0.005, "quality %u: 11 kHz at 0.5 came out at %.4f", q, lv);
    }

    /* a stereo circular buffer sink records the final mix, the left channel first and the right second, and its
       write offset moves */
    reset();
    tone(0, 1000, 0.25, 0, 0, 1.0f);
    tone(1, 1600, 0.25, 0, 1, 1.0f);
    wf(mix(SNX_ART_SUBMIX), 0x24, 1.0f);                /* submix 0 -> final 0 */
    wf(mix(SNX_ART_SUBMIX), 0x24 + (24 + 1) * 4, 1.0f); /* submix 1 -> final 1 */
    w8(sink(1), 1, 1);
    frames(50);
    const i16 *ring_l = sink_frame_at(0, 0), *ring_r = sink_frame_at(0, 1);
    double l1k = level_i16(ring_l, SNX_ART_FRAME, 1000), l16 = level_i16(ring_l, SNX_ART_FRAME, 1600);
    double r1k = level_i16(ring_r, SNX_ART_FRAME, 1000), r16 = level_i16(ring_r, SNX_ART_FRAME, 1600);
    CHECK(fabs(l1k - 0.25) < 0.005 && l16 < 0.005, "left channel: 1 kHz %.4f, 1.6 kHz %.4f", l1k, l16);
    CHECK(fabs(r16 - 0.25) < 0.005 && r1k < 0.005, "right channel: 1.6 kHz %.4f, 1 kHz %.4f", r16, r1k);
    u32 woff;
    update();
    woff = rd32(out + 0x40 + SNX_ART_POOLS * 0x10 + SNX_ART_VOICES * 0x10 + SNX_ART_EFFECTS * 0x10 + 0x20);
    CHECK(woff != 0 && woff % (2 * SNX_ART_FRAME * 2) == 0, "write offset %u", woff);

    /* the limiter switched off keeps a tone's level; in mode 1 the tone comes out at threshold over the detector:
       1 kHz at 0.25, threshold 0.1, rates 0.01, gives 0.25 * 0.1 / (0.25 * 2 / pi) */
    for (int mode = -1; mode <= 1; mode++) {
        reset();
        w8(eff(1), 2, 1);
        tone(0, 1000, 0.25, 0, 0, 1.0f);
        u8* l = eff(0) + 0x20;
        w8(eff(0), 2, mode >= 0);
        w8(eff(0), 1, 1);
        w8(l, 0x43, (u8)(mode < 0 ? 0 : mode));
        wf(l, 0x2C, 0.1f);
        wf(l, 0x24, 0.01f);
        wf(l, 0x28, 0.01f);
        frames(200);
        double lv = captured(0, 1000), rule = 0.25 * 0.1 / (0.25 * 2.0 / M_PI);
        if (mode < 0)
            CHECK(fabs(lv - 0.25) < 0.003, "limiter off: %.4f", lv);
        else
            CHECK(fabs(lv - rule) / rule < 0.03, "limiter mode %d: %.4f, the rule gives %.4f", mode, lv, rule);
    }

    /* a voice's counters: 480 samples played as two frames consume their buffer, and the next update says so */
    reset();
    tone(0, 1000, 0.25, 0, 0, 1.0f);
    w32(voice(0), 0x74, 480);
    w8(voice(0), 0x78, 0); /* no loop */
    frames(3);
    update();
    u64 played;
    u32 consumed;
    played = rd64(out + 0x40 + SNX_ART_POOLS * 0x10);
    consumed = rd32(out + 0x40 + SNX_ART_POOLS * 0x10 + 8);
    CHECK(played == 480 && consumed == 1, "played %llu, consumed %u", (unsigned long long)played, consumed);
    /* a stop consumes what was queued and starts the count over */
    tone(1, 1000, 0.25, 0, 0, 1.0f);
    frames(1);
    silence(1);
    frames(2);
    update();
    played = rd64(out + 0x40 + SNX_ART_POOLS * 0x10 + 0x10);
    consumed = rd32(out + 0x40 + SNX_ART_POOLS * 0x10 + 0x10 + 8);
    CHECK(played == 0 && consumed == 1, "after a stop: played %llu, consumed %u", (unsigned long long)played, consumed);

    /* the resampler tables made from their windowed sincs, against the console's: phase 0 of default quality's
       pass-through table and of high quality's, and one phase in the middle */
    reset();
    static const int lut4_1[4] = {-68, 32639, 69, -5}, lut8_2[8] = {-12, 47, -134, 32767, 81, -16, 2, 0},
                     lut4_0_64[4] = {1221, 15064, 15185, 1266};
    for (int i = 0; i < 4; i++) {
        CHECK(fabs(st->lut4[1][i] * 32768.0 - lut4_1[i]) <= 1.0, "lut4[1] tap %d: %.1f", i, st->lut4[1][i] * 32768.0);
        CHECK(fabs(st->lut4[0][64 * 4 + i] * 32768.0 - lut4_0_64[i]) <= 1.0, "lut4[0] phase 64 tap %d", i);
    }
    for (int i = 0; i < 8; i++)
        CHECK(fabs(st->lut8[2][i] * 32768.0 - lut8_2[i]) <= 1.0, "lut8[2] tap %d: %.1f", i, st->lut8[2][i] * 32768.0);

    /* DSP-ADPCM: the high nibble first, each code times 2^scale plus the prediction from the last two samples */
    reset();
    ARVoice v = {0};
    ARChannel c = {0};
    ARWave w = {0};
    i16 coef[16] = {0};
    coef[2] = 2048; /* pair 1: the last sample, times 1.0 */
    for (int k = 0; k < 16; k++) w16(g_mem + SNX_ART_G_ADPCM + 0x100, (u32)k * 2, (u16)coef[k]);
    u8 frame0[8] = {0x02, 0x12, 0x34, 0x56, 0x7F, 0xED, 0xCB, 0xA9}; /* pair 0, scale 2 */
    u8 frame1[8] = {0x10, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11}; /* pair 1, scale 0: +1 each sample */
    memcpy(g_mem + SNX_ART_G_ADPCM, frame0, 8);
    memcpy(g_mem + SNX_ART_G_ADPCM + 8, frame1, 8);
    v.format = SNX_AUDREN_ADPCM;
    v.channels = 1;
    v.data = SNX_ART_G_ADPCM + 0x100;
    v.data_size = 32;
    w.addr = SNX_ART_G_ADPCM;
    w.size = 16;
    w.end = 28;
    i16 got[28];
    u32 n = decode_adpcm(&v, &c, &w, 0, 28, 0, 28, got);
    static const i16 want0[14] = {4, 8, 12, 16, 20, 24, 28, -4, -8, -12, -16, -20, -24, -28};
    CHECK(n == 28, "decoded %u", n);
    CHECK(!memcmp(got, want0, sizeof want0), "first frame %d %d ... %d", got[0], got[1], got[13]);
    CHECK(got[14] == -27 && got[27] == -14, "second frame %d ... %d", got[14], got[27]);

    /* splitters (Eden's GenerateVoiceCommand): a stereo voice with no mix through destinations c, c + 2, ... */
    split_counts[0] = SNX_ART_SPLITTERS, split_counts[1] = SNX_ART_DESTS;
    reset();
    tone_stereo(0, 1000, 1600, 0.4);
    w32(voice(0), 0x58, SNX_AUDREN_UNUSED_MIX);
    w32(voice(0), 0x5C, 0);
    split_begin(1, 3);
    split_info(0, 3, split_ids_012);
    split_send(0, SNX_ART_FINAL, 1, 1.0f, 0.0f);
    split_send(1, SNX_ART_FINAL, 1, 0.0f, 0.5f);
    split_send(2, SNX_ART_FINAL, 0, 0.0f, 1.0f);
    w8(sink(1), 1, 1);
    frames(4);
    {
        double ll = level_i16(sink_frame_at(3, 0), SNX_ART_FRAME, 1000),
               lr = level_i16(sink_frame_at(3, 0), SNX_ART_FRAME, 1600),
               rr = level_i16(sink_frame_at(3, 1), SNX_ART_FRAME, 1600),
               rl = level_i16(sink_frame_at(3, 1), SNX_ART_FRAME, 1000);
        CHECK(fabs(ll - 0.4) < 0.005 && lr < 0.005, "splitter, left: 1 kHz %.4f, 1.6 kHz %.4f", ll, lr);
        CHECK(fabs(rr - 0.2) < 0.005 && rl < 0.005, "splitter, right: 1.6 kHz %.4f, 1 kHz %.4f", rr, rl);
    }

    /* a submix with no destination mix through splitter 1: destination k takes buffer k mod 2 */
    reset();
    tone(0, 1000, 0.4, 0, 0, 1.0f);
    tone(1, 1600, 0.4, 0, 1, 1.0f);
    w32(mix(SNX_ART_SUBMIX), 8, 2);
    wf(mix(SNX_ART_SUBMIX), 0, 0.5f);
    w32(mix(SNX_ART_SUBMIX), 0x924, SNX_AUDREN_UNUSED_MIX);
    w32(mix(SNX_ART_SUBMIX), 0x928, 1);
    split_begin(1, 3);
    split_info(1, 3, split_ids_345);
    split_send(3, SNX_ART_FINAL, 1, 1.0f, 0.0f);
    split_send(4, SNX_ART_FINAL, 1, 0.5f, 1.0f);
    split_send(5, SNX_ART_FINAL, 1, 0.0f, 0.25f);
    w8(sink(1), 1, 1);
    frames(4);
    {
        double ll = level_i16(sink_frame_at(3, 0), SNX_ART_FRAME, 1000),
               lr = level_i16(sink_frame_at(3, 0), SNX_ART_FRAME, 1600),
               rr = level_i16(sink_frame_at(3, 1), SNX_ART_FRAME, 1600),
               rl = level_i16(sink_frame_at(3, 1), SNX_ART_FRAME, 1000);
        CHECK(fabs(ll - 0.2) < 0.004 && fabs(lr - 0.1) < 0.004, "submix splitter, left: %.4f %.4f", ll, lr);
        CHECK(fabs(rr - 0.2) < 0.004 && fabs(rl - 0.05) < 0.004, "submix splitter, right: %.4f %.4f", rr, rl);
    }

    /* a destination's volume from 1 to 0 ramps over the next frame */
    reset();
    tone(0, 1000, 0.5, 0, 0, 1.0f);
    {
        int i;
        for (i = 0; i < SNX_ART_RATE; i++) w16(g_mem + SNX_ART_G_PCM, (u32)i * 2, 16384);
    }
    w32(voice(0), 0x58, SNX_AUDREN_UNUSED_MIX);
    w32(voice(0), 0x5C, 0);
    split_begin(1, 1);
    split_info(0, 1, split_ids_0);
    split_send(0, SNX_ART_FINAL, 1, 1.0f, 0.0f);
    w8(sink(1), 1, 1);
    frames(3);
    wf(split_sent[0], 8, 0.0f);
    frames(2);
    {
        int i;
        const i16 *steady = sink_frame_at(2, 0), *ramp = sink_frame_at(3, 0), *after = sink_frame_at(4, 0);
        double worst = 0;
        int loud = 0;
        for (i = 0; i < SNX_ART_FRAME; i++) {
            double d = fabs(ramp[i] - steady[0] * (1.0 - i / (double)SNX_ART_FRAME));
            if (d > worst) worst = d;
            loud += after[i] != 0;
        }
        CHECK(steady[0] > 16000 && steady[SNX_ART_FRAME - 1] == steady[0], "steady %d %d", steady[0], steady[239]);
        CHECK(worst <= 1.0, "the ramp is off by %.1f", worst);
        CHECK(loud == 0, "%d samples after the ramp", loud);
    }

    /* a submix sending to itself is a cycle: the sort falls back, and the final mix still plays */
    reset();
    tone(0, 1000, 0.4, 0, 0, 1.0f);
    tone(1, 1600, 0.4, 0, 0, 1.0f);
    w32(voice(1), 0x58, SNX_ART_FINAL);
    w32(mix(SNX_ART_SUBMIX), 0x924, SNX_AUDREN_UNUSED_MIX);
    w32(mix(SNX_ART_SUBMIX), 0x928, 0);
    split_begin(1, 1);
    split_info(0, 1, split_ids_0);
    split_send(0, SNX_ART_SUBMIX, 1, 1.0f, 0.0f);
    w8(sink(1), 1, 1);
    frames(4);
    {
        double lr = level_i16(sink_frame_at(3, 0), SNX_ART_FRAME, 1600);
        CHECK(fabs(lr - 0.4) < 0.005 && (st->warned & SNX_AUDREN_WARN_CYCLE), "cycle: 1.6 kHz %.4f", lr);
    }
    split_counts[0] = split_counts[1] = 0;

    /* voice biquads against Eden's recursion, integers at revision 4 and doubles at 7, chained, then reset */
    static const i16 bq0b[3] = {4096, 8192, 4096}, bq0a[2] = {8192, -4096}, bq1b[3] = {8192, 0, 0}, bq1a[2] = {0, 0};
    for (art_rev = 4; art_rev <= 7; art_rev += 3) {
        int f, mode;
        static i16 plain[5 * SNX_ART_FRAME], got[5 * SNX_ART_FRAME], want[5 * SNX_ART_FRAME], mid[5 * SNX_ART_FRAME];
        int dbl = art_rev >= 7, nn = 5 * SNX_ART_FRAME;
        reset();
        bq_voice();
        frames(5);
        for (f = 0; f < 5; f++) memcpy(plain + f * SNX_ART_FRAME, sink_frame_at(f, 0), SNX_ART_FRAME * 2);
        for (mode = 0; mode < 3; mode++) {
            int f, bad, first, i;
            reset();
            bq_voice();
            bq_set(0, 1, bq0b, bq0a);
            if (mode == 1) bq_set(1, 1, bq1b, bq1a);
            for (f = 0; f < 5; f++) {
                if (mode == 2) w8(voice(0), 0x24, f != 2);
                frames(1);
                memcpy(got + f * SNX_ART_FRAME, sink_frame_at(f, 0), SNX_ART_FRAME * 2);
            }
            if (mode == 2) {
                ref_biquad(plain, want, 2 * SNX_ART_FRAME, bq0b, bq0a, dbl);
                memcpy(want + 2 * SNX_ART_FRAME, plain + 2 * SNX_ART_FRAME, SNX_ART_FRAME * 2);
                ref_biquad(plain + 3 * SNX_ART_FRAME, want + 3 * SNX_ART_FRAME, 2 * SNX_ART_FRAME, bq0b, bq0a, dbl);
            } else {
                ref_biquad(plain, mode ? mid : want, nn, bq0b, bq0a, dbl);
                if (mode) ref_biquad(mid, want, nn, bq1b, bq1a, dbl);
            }
            bad = 0;
            first = -1;
            for (i = 0; i < nn; i++)
                if (got[i] != want[i]) bad++, first = first < 0 ? i : first;
            CHECK(bad == 0, "biquad rev %u mode %d: %d samples off, first %d: %d, want %d", art_rev, mode, bad, first,
                  first < 0 ? 0 : got[first], first < 0 ? 0 : want[first]);
        }
    }
    art_rev = 4;

    /* the biquad filter effect against Eden's recursion, at revisions 4 and 9, through updates and off */
    for (art_rev = 4; art_rev <= 9; art_rev += 5) {
        int mode;
        static i16 left[5 * SNX_ART_FRAME], right[5 * SNX_ART_FRAME], want[5 * SNX_ART_FRAME];
        i16 qb[3] = {4096, 8192, 4096}, qa[2] = {8192, -4096};
        if (art_rev >= 9) qb[1] = 6553, qa[1] = -4915;
        for (mode = 0; mode < 3; mode++) {
            u8* p;
            u32 st_at;
            int f, restart, at;
            reset();
            p = fx_voice(SNX_AUDREN_EFFECT_BIQUAD);
            w8(eff(0), 2, mode != 2);
            p[0] = 0, p[6] = 1;
            if (art_rev >= 9) {
                const float fb[3] = {0.25f, 0.4f, 0.25f}, fa[2] = {0.5f, -0.3f};
                for (int k = 0; k < 3; k++) wf(p, 0x10 + k * 4, fb[k]);
                for (int k = 0; k < 2; k++) wf(p, 0x1C + k * 4, fa[k]);
                w8(p, 0x24, 1);
            } else {
                for (int k = 0; k < 3; k++) w16(p, 0xC + k * 2, (u16)qb[k]);
                for (int k = 0; k < 2; k++) w16(p, 0x12 + k * 2, (u16)qa[k]);
                w8(p, 0x16, 1);
            }
            st_at = art_rev >= 9 ? 0x25 : 0x17;
            for (f = 0; f < 5; f++) {
                w8(p, st_at, f == 0 ? 0 : f == 2 && mode == 1 ? 1 : 2);
                if (f == 1 && mode == 0)
                    frame_only();
                else
                    frames(1);
            }
            sink_copy(left, 0, 5, 0);
            sink_copy(right, 0, 5, 1);
            restart = mode == 1 && art_rev < 6 ? 2 * SNX_ART_FRAME : 5 * SNX_ART_FRAME;
            if (mode == 2)
                memcpy(want, left, sizeof want);
            else {
                ref_biquad(left, want, restart, qb, qa, art_rev >= 7);
                ref_biquad(left + restart, want + restart, 5 * SNX_ART_FRAME - restart, qb, qa, art_rev >= 7);
            }
            at = first_diff(right, want, 5 * SNX_ART_FRAME);
            CHECK(at < 0, "biquad effect rev %u mode %d: sample %d is %d, want %d", art_rev, mode, at,
                  at < 0 ? 0 : right[at], at < 0 ? 0 : want[at]);
        }
    }
    art_rev = 4;

    /* the delay: a mono 10 ms line, an invalid count until frame 5, restarts on initialized and is_new */
    {
        u8* p;
        int f, bad, i;
        static i16 x[12 * SNX_ART_FRAME], y[12 * SNX_ART_FRAME];
        reset();
        p = fx_voice(SNX_AUDREN_EFFECT_DELAY);
        w8(eff(0), 2, 1);
        p[0] = 0, p[6] = 1;
        delay_set(p, 1, 10, 16384, 0, 16384, 0, 0, 0);
        for (f = 0; f < 12; f++) {
            w8(p, 0xE, f < 5 ? 3 : 1);
            w8(p, 0x34, f == 2 || f == 5 ? SNX_AUDREN_PARAM_INITIALIZED : SNX_AUDREN_PARAM_UPDATED);
            if (f == 8) w8(eff(0), 1, 1);
            frames(1);
        }
        sink_copy(x, 0, 12, 0);
        sink_copy(y, 0, 12, 1);
        bad = -1;
        for (i = 0; i < 12 * SNX_ART_FRAME && bad < 0; i++) {
            int fresh = i < 480 || (i >= 5 * SNX_ART_FRAME && i < 7 * SNX_ART_FRAME) ||
                        (i >= 8 * SNX_ART_FRAME && i < 10 * SNX_ART_FRAME);
            if (y[i] != (fresh ? 0 : x[i - 480])) bad = i;
        }
        CHECK(bad < 0, "delay of 480: sample %d is %d", bad, bad < 0 ? 0 : y[bad]);
    }

    /* an effect of a type it does not render is named by its type */
    reset();
    fx_voice(99);
    logged[0] = 0;
    frames(1);
    CHECK(strstr(logged, "audren: effect type 99 is not rendered\n"), "logged: %s", logged);

    /* a stereo delay with every parameter, against Eden's recursion over the same input */
    {
        int on, a, b;
        double lx;
        const i16* xs[2];
        i16* ws[2];
        static i16 x0[10 * SNX_ART_FRAME], x1[10 * SNX_ART_FRAME], y0[10 * SNX_ART_FRAME], y1[10 * SNX_ART_FRAME],
            w0[10 * SNX_ART_FRAME], w1[10 * SNX_ART_FRAME];
        for (on = 0; on < 2; on++) {
            u8* p;
            int f;
            reset();
            p = fx_voice(SNX_AUDREN_EFFECT_DELAY);
            tone(1, 1600, 0.3, 0, 1, 1.0f);
            route(0, 0, 0.0f), route(1, 1, 0.0f), route(2, 0, 1.0f), route(3, 1, 1.0f);
            w8(eff(0), 2, (u8)on);
            p[0] = 0, p[1] = 1, p[6] = 2, p[7] = 3;
            delay_set(p, 2, 5, 12288, 8192, 9830, 6553, 4915, 4096);
            for (f = 0; f < 10; f++) {
                frames(1);
                w8(p, 0x34, SNX_AUDREN_PARAM_UPDATED);
            }
            sink_copy(on ? y0 : x0, 0, 10, 0);
            sink_copy(on ? y1 : x1, 0, 10, 1);
        }
        lx = level_i16(x0 + 9 * SNX_ART_FRAME, SNX_ART_FRAME, 1000);
        CHECK(fabs(lx - 0.3) < 0.005, "delay off copies its input: 1 kHz at %.4f", lx);
        xs[0] = x0;
        xs[1] = x1;
        ws[0] = w0;
        ws[1] = w1;
        ref_delay(2, xs, ws, 10 * SNX_ART_FRAME, 240, 12288, 8192, 9830, 6553, 4915, 4096);
        a = first_diff(y0, w0, 10 * SNX_ART_FRAME);
        b = first_diff(y1, w1, 10 * SNX_ART_FRAME);
        CHECK(a < 0 && b < 0, "stereo delay: left off at %d (%d, want %d), right at %d (%d, want %d)", a,
              a < 0 ? 0 : y0[a], a < 0 ? 0 : w0[a], b, b < 0 ? 0 : y1[b], b < 0 ? 0 : w1[b]);
    }

    /* another effect in a slot starts from nothing (Eden's ResetEffect) */
    {
        u8* p;
        int f, at;
        static i16 left[6 * SNX_ART_FRAME], right[6 * SNX_ART_FRAME], want[3 * SNX_ART_FRAME];
        static const i16 qb[3] = {4096, 8192, 4096}, qa[2] = {8192, -4096};
        reset();
        p = fx_voice(SNX_AUDREN_EFFECT_BIQUAD);
        w8(eff(0), 2, 1);
        for (f = 0; f < 6; f++) {
            memset(p, 0, 0xA0);
            p[0] = 0, p[6] = 1;
            if (f == 2) {
                w8(eff(0), 0, SNX_AUDREN_EFFECT_DELAY);
                delay_set(p, 1, 1, 16384, 0, 16384, 0, 0, 0);
            } else {
                w8(eff(0), 0, SNX_AUDREN_EFFECT_BIQUAD);
                for (int k = 0; k < 3; k++) w16(p, 0xC + k * 2, (u16)qb[k]);
                for (int k = 0; k < 2; k++) w16(p, 0x12 + k * 2, (u16)qa[k]);
                w8(p, 0x16, 1);
                w8(p, 0x17, f == 0 ? SNX_AUDREN_PARAM_INITIALIZED : SNX_AUDREN_PARAM_UPDATED);
            }
            frames(1);
        }
        sink_copy(left, 0, 6, 0);
        sink_copy(right, 0, 6, 1);
        ref_biquad(left + 3 * SNX_ART_FRAME, want, 3 * SNX_ART_FRAME, qb, qa, 0);
        at = first_diff(right + 3 * SNX_ART_FRAME, want, 3 * SNX_ART_FRAME);
        CHECK(at < 0, "a biquad filter after a delay in its slot: sample %d is %d, want %d", at,
              at < 0 ? 0 : right[3 * SNX_ART_FRAME + at], at < 0 ? 0 : want[at]);
    }

    /* six channels: buffer 5 is the back right's line from revision 11, the LFE's before it */
    for (art_rev = 4; art_rev <= 11; art_rev += 7) {
        u8* p;
        int c, f;
        double lv, want;
        reset();
        p = fx_voice(SNX_AUDREN_EFFECT_DELAY);
        tone(0, 1000, 0.3, 0, 5, 1.0f);
        w32(mix(SNX_ART_SUBMIX), 8, 6);
        route(0, 0, 0.0f), route(1, 1, 0.0f), route(5, 0, 1.0f);
        w8(eff(0), 2, 1);
        for (c = 0; c < 6; c++) p[c] = p[6 + c] = (u8)c;
        delay_set(p, 6, 5, 16384, 8192, 16384, 0, 0, 0);
        for (f = 0; f < 40; f++) {
            frames(1);
            w8(p, 0x34, SNX_AUDREN_PARAM_UPDATED);
        }
        lv = level_i16(sink_frame_at(39, 0), SNX_ART_FRAME, 1000);
        want = 0.3 / (1.0 - (art_rev >= 11 ? 8028.0 / 16384.0 : 0.5));
        CHECK(fabs(lv - want) < 0.004, "six-channel delay rev %u: %.4f, want %.4f", art_rev, lv, want);
    }
    art_rev = 4;

    /* the reverb's dry path alone: to_int(x * 0.5) rounds up from two thirds */
    {
        u8* p;
        int f, bad, i;
        static i16 x[5 * SNX_ART_FRAME], y[5 * SNX_ART_FRAME];
        static const i32 v[11] = {0, 0, 0, 0, 0, 24576, 8192, 0, 16384, 0, 8192};
        reset();
        p = fx_voice(SNX_AUDREN_EFFECT_REVERB);
        w8(eff(0), 2, 1);
        p[0] = 0, p[6] = 1;
        reverb_set(p, 1, v);
        for (f = 0; f < 5; f++) {
            frames(1);
            w8(p, 0x40, SNX_AUDREN_PARAM_UPDATED);
        }
        sink_copy(x, 0, 5, 0);
        sink_copy(y, 0, 5, 1);
        bad = -1;
        for (i = 0; i < 5 * SNX_ART_FRAME && bad < 0; i++)
            if (y[i] != tint((i64)x[i] * 8192)) bad = i;
        CHECK(bad < 0, "reverb dry: sample %d is %d for %d", bad, bad < 0 ? 0 : y[bad], bad < 0 ? 0 : x[bad]);
    }

    /* a mono reverb with every parameter, a hundred frames, against Eden's run here */
    {
        u8* p;
        int f, at;
        static i16 x[100 * SNX_ART_FRAME], y[100 * SNX_ART_FRAME], want[100 * SNX_ART_FRAME];
        static const i32 v[11] = {1, 13107, 81920, 0, 11469, 24576, 8192, 4915, 14746, 13107, 9830};
        u8 state[100];
        reset();
        p = fx_voice(SNX_AUDREN_EFFECT_REVERB);
        tone(0, 1000, 0.2, 0, 0, 1.0f);
        w8(eff(0), 2, 1);
        p[0] = 0, p[6] = 1;
        reverb_set(p, 1, v);
        for (f = 0; f < 100; f++) {
            state[f] = f == 0 ? 0 : f == 2 || f == 3 ? 1 : 2;
            w8(p, 0x40, state[f]);
            frames(1);
            if (f % SNX_ART_RING_FRAMES == SNX_ART_RING_FRAMES - 1 || f == 99) { /* before the sink's ring laps */
                int from = f - f % SNX_ART_RING_FRAMES;
                sink_copy(x + from * SNX_ART_FRAME, from, f + 1, 0);
                sink_copy(y + from * SNX_ART_FRAME, from, f + 1, 1);
            }
        }
        rv_ref(x, want, 100, state, v);
        at = first_diff(y, want, 100 * SNX_ART_FRAME);
        CHECK(at < 0, "mono reverb: sample %d is %d, want %d", at, at < 0 ? 0 : y[at], at < 0 ? 0 : want[at]);
    }

    /* early reflections only, right after it starts: each tap a quarter of the way to Eden's */
    {
        int run, i, a, b;
        i32 t[10];
        i64 g[10];
        static i16 x0[5 * SNX_ART_FRAME], x1[5 * SNX_ART_FRAME], y0[5 * SNX_ART_FRAME], y1[5 * SNX_ART_FRAME],
            w0[5 * SNX_ART_FRAME], w1[5 * SNX_ART_FRAME];
        static i32 xs[5 * SNX_ART_FRAME];
        static const i32 v[11] = {0, 16384, 0, 0, 0, 24576, 8192, 0, 16384, 16384, 0};
        static const u8 side[10] = {0, 0, 1, 1, 0, 1, 0, 0, 1, 1};
        for (run = 0; run < 2; run++) {
            u8* p;
            int f;
            reset();
            p = fx_voice(SNX_AUDREN_EFFECT_REVERB);
            tone(1, 1600, 0.3, 0, 1, 1.0f);
            if (run) route(0, 0, 0.0f), route(1, 1, 0.0f), route(2, 0, 1.0f), route(3, 1, 1.0f);
            w8(eff(0), 2, 1);
            p[0] = 0, p[1] = 1, p[6] = 2, p[7] = 3;
            reverb_set(p, 2, v);
            for (f = 0; f < 5; f++) {
                frames(1);
                w8(p, 0x40, SNX_AUDREN_PARAM_UPDATED);
            }
            sink_copy(run ? y0 : x0, 0, 5, 0);
            sink_copy(run ? y1 : x1, 0, 5, 1);
        }
        rv_first_taps(0, 16384, 0, t, g);
        g[4] = tmul(g[4], 8192), g[5] = tmul(g[5], 8192);
        for (i = 0; i < 5 * SNX_ART_FRAME; i++) xs[i] = x0[i] + x1[i];
        for (i = 0; i < 5 * SNX_ART_FRAME; i++) {
            int k;
            i64 o[2] = {0, 0};
            for (k = 0; k < 10; k++) o[side[k]] += rv_tap(xs, i, t[k], g[k]);
            w0[i] = tsat(tint(tdiv(o[0], 64 * 16384)));
            w1[i] = tsat(tint(tdiv(o[1], 64 * 16384)));
        }
        a = first_diff(y0, w0, 5 * SNX_ART_FRAME);
        b = first_diff(y1, w1, 5 * SNX_ART_FRAME);
        CHECK(a < 0 && b < 0, "stereo early reflections: left at %d (%d, want %d), right at %d (%d, want %d)", a,
              a < 0 ? 0 : y0[a], a < 0 ? 0 : w0[a], b, b < 0 ? 0 : y1[b], b < 0 ? 0 : w1[b]);
    }

    /* six channels: the LFE takes every tap at 0.2; the old order before revision 11 swaps the back right */
    for (art_rev = 4; art_rev <= 11; art_rev += 7) {
        int run, i, bad;
        i32 t[10];
        i64 g[10];
        static i16 x[5 * SNX_ART_FRAME], b3[5 * SNX_ART_FRAME], b5[5 * SNX_ART_FRAME];
        static i32 xs[5 * SNX_ART_FRAME];
        static const i32 v[11] = {0, 16384, 0, 0, 0, 24576, 8192, 0, 16384, 16384, 0};
        for (run = 0; run < 2; run++) {
            u8* p;
            int c, f;
            reset();
            p = fx_voice(SNX_AUDREN_EFFECT_REVERB);
            w32(mix(SNX_ART_SUBMIX), 8, 6);
            if (run) route(0, 0, 0.0f), route(1, 1, 0.0f), route(3, 0, 1.0f), route(5, 1, 1.0f);
            w8(eff(0), 2, (u8)run);
            for (c = 0; c < 6; c++) p[c] = p[6 + c] = (u8)c;
            reverb_set(p, 6, v);
            for (f = 0; f < 5; f++) {
                frames(1);
                w8(p, 0x40, SNX_AUDREN_PARAM_UPDATED);
            }
            if (run) {
                sink_copy(b3, 0, 5, 0);
                sink_copy(b5, 0, 5, 1);
            } else
                sink_copy(x, 0, 5, 0);
        }
        rv_first_taps(0, 16384, 0, t, g);
        for (i = 0; i < 5 * SNX_ART_FRAME; i++) xs[i] = x[i];
        bad = -1;
        for (i = 0; i < 5 * SNX_ART_FRAME && bad < 0; i++) {
            int k;
            i16 lfe, back;
            i64 all = 0;
            for (k = 0; k < 10; k++) all += rv_tap(xs, i, t[k], g[k]);
            lfe = tsat(tint(tdiv(tmul(all, 3276), 64 * 16384)));
            back = tsat(tint(tdiv(rv_tap(xs, i, t[8], g[8]) + rv_tap(xs, i, t[9], g[9]), 64 * 16384)));
            if (b3[i] != (art_rev >= 11 ? lfe : back) || b5[i] != (art_rev >= 11 ? back : lfe)) bad = i;
        }
        CHECK(bad < 0, "six-channel reverb rev %u: sample %d is %d and %d", art_rev, bad, bad < 0 ? 0 : b3[bad],
              bad < 0 ? 0 : b5[bad]);
    }
    art_rev = 4;

    /* the I3DL2 reverb's dry path alone (gains of -10000 are 0 in Eden's pow_10) */
    {
        u8* p;
        int f, bad, i;
        static i16 x[5 * SNX_ART_FRAME], y[5 * SNX_ART_FRAME];
        static const float v[12] = {0, 5000, 1.5f, 0.5f, -10000, -10000, -10000, 100, 0.005f, 0.01f, 100, 0.5f};
        reset();
        p = fx_voice(SNX_AUDREN_EFFECT_I3DL2);
        w8(eff(0), 2, 1);
        p[0] = 0, p[6] = 1;
        i3dl2_set(p, 1, v);
        for (f = 0; f < 5; f++) {
            frames(1);
            w8(p, 0x48, SNX_AUDREN_PARAM_UPDATED);
        }
        sink_copy(x, 0, 5, 0);
        sink_copy(y, 0, 5, 1);
        bad = -1;
        for (i = 0; i < 5 * SNX_ART_FRAME && bad < 0; i++)
            if (y[i] != (i32)(x[i] * 0.5f)) bad = i;
        CHECK(bad < 0, "I3DL2 dry: sample %d is %d for %d", bad, bad < 0 ? 0 : y[bad], bad < 0 ? 0 : x[bad]);
    }

    /* a mono I3DL2 reverb with every parameter, a hundred frames, against Eden's run here */
    {
        u8* p;
        int f, at;
        static i16 x[100 * SNX_ART_FRAME], y[100 * SNX_ART_FRAME], want[100 * SNX_ART_FRAME];
        static const float v[12] = {-1000, 5000, 1.5f, 0.5f, -1000, -500, -300, 80, 0.01f, 0.02f, 90, 0.6f};
        u8 state[100];
        reset();
        p = fx_voice(SNX_AUDREN_EFFECT_I3DL2);
        tone(0, 1000, 0.2, 0, 0, 1.0f);
        w8(eff(0), 2, 1);
        p[0] = 0, p[6] = 1;
        i3dl2_set(p, 1, v);
        for (f = 0; f < 100; f++) {
            state[f] = f == 0 ? 0 : f == 2 || f == 3 ? 1 : 2;
            w8(p, 0x48, state[f]);
            frames(1);
            if (f % SNX_ART_RING_FRAMES == SNX_ART_RING_FRAMES - 1 || f == 99) {
                int from = f - f % SNX_ART_RING_FRAMES;
                sink_copy(x + from * SNX_ART_FRAME, from, f + 1, 0);
                sink_copy(y + from * SNX_ART_FRAME, from, f + 1, 1);
            }
        }
        i3_ref(x, want, 100, state, v);
        at = first_diff(y, want, 100 * SNX_ART_FRAME);
        CHECK(at < 0, "mono I3DL2: sample %d is %d, want %d", at, at < 0 ? 0 : y[at], at < 0 ? 0 : want[at]);
    }

    /* six channels, early reflections only: centre and LFE are buffers 2 and 3 from revision 11, 4 and 5 before */
    for (art_rev = 4; art_rev <= 11; art_rev += 7) {
        int run, bad, i;
        i32 t[20];
        static i16 x[5 * SNX_ART_FRAME], ctr[5 * SNX_ART_FRAME], lfe[5 * SNX_ART_FRAME];
        static const float v[12] = {0, 5000, 1.5f, 0.5f, 0, 0, -10000, 100, 0.005f, 0.01f, 100, 0};
        int cb = art_rev >= 11 ? 2 : 4, lb = art_rev >= 11 ? 3 : 5;
        for (run = 0; run < 2; run++) {
            u8* p;
            int c, f;
            reset();
            p = fx_voice(SNX_AUDREN_EFFECT_I3DL2);
            w32(mix(SNX_ART_SUBMIX), 8, 6);
            if (run) route(0, 0, 0.0f), route(1, 1, 0.0f), route(cb, 1, 1.0f), route(lb, 0, 1.0f);
            w8(eff(0), 2, (u8)run);
            for (c = 0; c < 6; c++) p[c] = p[6 + c] = (u8)c;
            i3dl2_set(p, 6, v);
            for (f = 0; f < 5; f++) {
                frames(1);
                w8(p, 0x48, SNX_AUDREN_PARAM_UPDATED);
            }
            if (run) {
                sink_copy(lfe, 0, 5, 0);
                sink_copy(ctr, 0, 5, 1);
            } else
                sink_copy(x, 0, 5, 0);
        }
        i3_taps(v, t);
        bad = -1;
        for (i = 0; i < 5 * SNX_ART_FRAME && bad < 0; i++) {
            int k;
            i64 all = 0, first = 0;
            for (k = 0; k < 20; k++) {
                int at = i - 1 - t[k];
                i64 tap = at < 0 ? 0 : tmul((i64)x[at] * 16384, tq(i3_tap_g[k]));
                all += tap;
                if (k == 0) first = tap;
            }
            if (lfe[i] != tsat((i32)((float)all / 16384.0f)) || ctr[i] != tsat((i32)((float)first / 16384.0f))) bad = i;
        }
        CHECK(bad < 0, "six-channel I3DL2 rev %u: sample %d is %d (LFE) and %d (centre)", art_rev, bad,
              bad < 0 ? 0 : lfe[bad], bad < 0 ? 0 : ctr[bad]);
    }
    art_rev = 4;

    /* the capture into a 4800-sample ring: offsets, total and lost count after 25 frames unread */
    for (nch = 1; nch <= 2; nch++) {
        u8* p;
        int bad;
        u32 span, k, c, i, wo, lost, total, lost_frames, rd;
        static i16 x0[25 * SNX_ART_FRAME], x1[25 * SNX_ART_FRAME];
        const u8* dsp = g_mem + SNX_ART_G_AUX_SEND + 0x40;
        const u8* ring = g_mem + SNX_ART_G_AUX_SEND + 0x80;
        reset();
        p = fx_voice(SNX_AUDREN_EFFECT_CAPTURE);
        if (nch == 2) tone(1, 1600, 0.3, 0, 1, 1.0f);
        w8(eff(0), 2, 1);
        p[0] = 0, p[1] = 1, p[0x18] = 1, p[0x19] = 0;
        w32(p, 0x30, nch);
        w32(p, 0x38, 4800);
        w64(p, 0x40, SNX_ART_G_AUX_SEND);
        frames(25);
        sink_copy(x0, 0, 25, 0);
        sink_copy(x1, 0, 25, 1);
        bad = -1;
        span = SNX_ART_FRAME * nch;
        for (k = 25 - 4800 / span; k < 25 && bad < 0; k++)
            for (c = 0; c < nch; c++)
                for (i = 0; i < SNX_ART_FRAME; i++)
                    if ((i32)rd32(ring + (k * span + c * SNX_ART_FRAME + i) % 4800 * 4) !=
                        (c ? x1 : x0)[k * SNX_ART_FRAME + i])
                        bad = (int)(k * span + c * SNX_ART_FRAME + i);
        wo = rd32(dsp + 4), lost = rd32(dsp + 8), total = rd32(dsp + 0xC);
        lost_frames = 25 - 4800 / span;
        CHECK(bad < 0, "capture of %u: ring sample %d", nch, bad);
        CHECK(wo == 25 * span % 4800 && total == 25 * span && lost == lost_frames * span,
              "capture of %u: write %u, total %u, lost %u", nch, wo, total, lost);
        if (nch == 1) {
            double lv = level_i16(x1 + 24 * SNX_ART_FRAME, SNX_ART_FRAME, 1000);
            CHECK(lv < 0.001, "the capture wrote its output: %.4f", lv);
        }
        w8(eff(0), 2, 0);
        frames(1);
        rd = rd32(dsp), wo = rd32(dsp + 4), total = rd32(dsp + 0xC), lost = rd32(dsp + 8);
        CHECK(rd == 0 && wo == 0 && total == 0 && lost == lost_frames * span,
              "capture switched off: read %u, write %u, total %u, lost %u", rd, wo, total, lost);
    }

    /* a game that reads everything before each update loses nothing */
    {
        u8* p;
        int f;
        u32 lost, total, edge, zero;
        reset();
        p = fx_voice(SNX_AUDREN_EFFECT_CAPTURE);
        w8(eff(0), 2, 1);
        w32(p, 0x30, 1);
        w32(p, 0x38, 4800);
        w64(p, 0x40, SNX_ART_G_AUX_SEND);
        for (f = 0; f < 25; f++) {
            memcpy(g_mem + SNX_ART_G_AUX_SEND + 0xC, g_mem + SNX_ART_G_AUX_SEND + 0x40 + 0xC, 4);
            frames(1);
        }
        lost = rd32(g_mem + SNX_ART_G_AUX_SEND + 0x40 + 8), total = rd32(g_mem + SNX_ART_G_AUX_SEND + 0x4C);
        CHECK(lost == 0 && total == 25 * SNX_ART_FRAME, "a capture read in time: lost %u, total %u", lost, total);

        /* near the wrap, Eden's WriteAuxBufferDsp holds both counts one short of the game's */
        edge = 0xFFFFFFF0u;
        zero = 0;
        wr32(g_mem + SNX_ART_G_AUX_SEND + 0x40 + 8, edge), wr32(g_mem + SNX_ART_G_AUX_SEND + 0x4C, edge);
        wr32(g_mem + SNX_ART_G_AUX_SEND + 8, zero), wr32(g_mem + SNX_ART_G_AUX_SEND + 0xC, zero);
        frames(1);
        lost = rd32(g_mem + SNX_ART_G_AUX_SEND + 0x40 + 8), total = rd32(g_mem + SNX_ART_G_AUX_SEND + 0x4C);
        CHECK(lost == 0xFFFFFFFFu && total == 0xFFFFFFFFu, "capture counts at the wrap: lost %08x, total %08x", lost,
              total);
    }

    /* at revision 4 Eden takes no compressor update: it never runs */
    {
        u8* p;
        double lv;
        static const float v[7] = {70.0f, 4.0f, 0.001f, 0.05f, 0.002f, 3.0f, 1.0f};
        reset();
        p = fx_voice(SNX_AUDREN_EFFECT_COMPRESSOR);
        w8(eff(0), 2, 1);
        p[0] = 0, p[6] = 1;
        compressor_set(p, v);
        frames(5);
        lv = level_i16(sink_frame_at(4, 1), SNX_ART_FRAME, 1000);
        CHECK(lv < 0.001, "a revision 4 compressor ran: %.4f", lv);
    }

    /* the compressor at revision 9, above and inside the knee, against Eden's run here; off it copies */
    art_rev = 9;
    for (set = 0; set < 3; set++) {
        u8* p;
        int f, at;
        static const float vs[2][7] = {{70.0f, 4.0f, 0.001f, 0.05f, 0.002f, 3.0f, 1.0f},
                                       {78.0f, 2.0f, 0.001f, 0.05f, 0.002f, -2.0f, 0.0f}};
        static i16 x[30 * SNX_ART_FRAME], y[30 * SNX_ART_FRAME], want[30 * SNX_ART_FRAME];
        u8 state[30];
        reset();
        p = fx_voice(SNX_AUDREN_EFFECT_COMPRESSOR);
        w8(eff(0), 2, set < 2);
        p[0] = 0, p[6] = 1;
        compressor_set(p, vs[set & 1]);
        for (f = 0; f < 30; f++) {
            state[f] = f == 0 ? 0 : f == 10 ? 1 : 2;
            w8(p, 0x34, state[f]);
            if (f == 1)
                frame_only();
            else
                frames(1);
        }
        sink_copy(x, 0, 30, 0);
        sink_copy(y, 0, 30, 1);
        if (set < 2)
            cmp_ref(x, want, 30, state, vs[set]);
        else
            memcpy(want, x, sizeof want);
        at = first_diff(y, want, 30 * SNX_ART_FRAME);
        CHECK(at < 0, "compressor %d: sample %d is %d, want %d", set, at, at < 0 ? 0 : y[at], at < 0 ? 0 : want[at]);
    }
    art_rev = 4;

    printf("%u checks, %u failed\n", checks, failures);
    audren_state_free(st);
    free(g_mem);
    return failures != 0;
}
