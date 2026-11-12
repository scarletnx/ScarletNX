/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* aud:u (audio.c) through its IPC commands, and the hardware Opus decoder: its decoding (audio.c) and the native
   nn::codec functions over it (nn/codec.c), their handlers called as the hooks call them and codec.hook's table by
   SDK version. The native audio outs and audio device have their own test (nn_audio.c), and the renderer its own
   (nn_audio_renderer.c, audren_host.c) */
#include "nx/hle/services/services_int.h"
#include <setjmp.h>
#include <stdarg.h>

#include "nx/hle/services/audio.c"
#include "nx/nn/codec.c"
#include "nx/hle/hle.c"
#include "hle/hook/hooks.hook.inc" /* codec.hook's table and adapters */
#include "kobj_ids.h"              /* the kernel objects the stubs make, as ids */

u8* g_mem;
u64 g_as_size = AS_MAX; /* the reservation's size, mem.c's, which this test does not link */
static u64 test_now;
static unsigned test_checks, test_objects, test_events;
static int test_no_device;
struct PAudio {
    u8 pcm[32768];
    u32 bytes;
    unsigned queued, cleared;
    int started;
    float volume;
    u32 channels;
};

static void test_fail(const char* expression, int line) {
    fprintf(stderr, "audio_out.c:%d: check failed: %s\n", line, expression);
    exit(1);
}
#define CHECK(expr)                                                                                                    \
    do {                                                                                                               \
        test_checks++;                                                                                                 \
        if (!(expr)) test_fail(#expr, __LINE__);                                                                       \
    } while (0)

/* This single-threaded fixture checks timing and ownership, without needing an audio device. */
void pm_init(PMutex* m) { (void)m; }
void pm_lock(PMutex* m) { (void)m; }
void pm_unlock(PMutex* m) { (void)m; }
u64 plat_ns(void) { return test_now; }
/* the system's name for a thread (common.c), which this test does not link */
void snx_thread_name(const char* name) { (void)name; }
u64 guest_ns(void) { return test_now; } /* the outs play in guest time: no pause here */
/* the audio clock's pause, whose thread never runs here */
void pc_init(PCond* c) { (void)c; }
void pc_broadcast(PCond* c) { (void)c; }
int pc_wait(PCond* c, PMutex* m, i64 timeout_ns) {
    (void)c, (void)m, (void)timeout_ns;
    return 0;
}
void plat_sleep_ns(i64 ns) {
    (void)ns;
    test_fail("the test must not start the audio clock thread", __LINE__);
}
PAudio* plat_audio_open(u32 rate, u32 channels) {
    CHECK(rate == 48000 && (channels == 1 || channels == 2 || channels == 4 || channels == 6));
    if (test_no_device) return NULL;
    PAudio* a = calloc(1, sizeof *a);
    CHECK(a != NULL);
    a->volume = 1.0f;
    a->channels = channels;
    return a;
}
void plat_audio_close(PAudio* a) { free(a); }
int plat_audio_queue(PAudio* a, const void* samples, u32 bytes) {
    CHECK(a && bytes && bytes <= sizeof a->pcm - a->bytes);
    CHECK(bytes % (a->channels * 2) == 0); /* SDL refuses a part of a frame, and the whole call with it */
    memcpy(a->pcm + a->bytes, samples, bytes);
    a->bytes += bytes;
    a->queued++;
    return 1;
}
void plat_audio_start(PAudio* a, int started) {
    if (a) a->started = started;
}
void plat_audio_clear(PAudio* a) {
    if (a) {
        a->bytes = 0;
        a->cleared++;
    }
}
void plat_audio_volume(PAudio* a, float volume) {
    if (a) a->volume = volume;
}
void mem_query(u64 addr, MemBlock* b) {
    memset(b, 0, sizeof *b);
    b->addr = addr < 0x8000 ? 0 : 0x8000;
    b->size = addr < 0x8000 ? 0x8000 : AS_SIZE - 0x8000;
    b->perm = addr < 0x8000 ? PERM_RW : PERM_NONE;
}
static char test_last_log[256];
void logf_(int level, const char* format, ...) {
    va_list ap;
    (void)level;
    va_start(ap, format);
    vsnprintf(test_last_log, sizeof test_last_log, format, ap);
    va_end(ap);
}

SvcObj* svcobj_new(const char* name, int (*dispatch)(SvcObj*, IpcCtx*)) {
    SvcObj* obj = calloc(1, sizeof *obj);
    CHECK(obj != NULL);
    obj->name = name;
    obj->dispatch = dispatch;
    obj->refs = 1;
    test_objects++;
    return obj;
}
#include "svcobj_data.h"
void svcobj_unref(SvcObj* obj) {
    if (!obj || --obj->refs) return;
    if (obj->destroy) obj->destroy(obj);
    test_svcobj_data_free(obj);
    free(obj);
    test_objects--;
}
void out_obj(IpcCtx* c, SvcObj* obj) {
    CHECK(c->nout_objs < 8);
    c->out_objs[c->nout_objs++] = obj;
}
void out_copy_h(IpcCtx* c, u32 handle) {
    CHECK(c->nout_copy < 8);
    c->out_copy[c->nout_copy++] = handle;
}
void ipc_out_overflow(IpcCtx* c) {
    (void)c;
    test_fail("IPC output overflow", __LINE__);
}
/* sf's InvalidCmifRequest, for a request that doesn't send what its command declares */
int ipc_bad_request(SvcObj* o, IpcCtx* c) {
    (void)o, (void)c;
    return RC(11, 420);
}
/* sf's InvalidNumInObjects, for a domain request with another number of objects */
int ipc_bad_objects(SvcObj* o, IpcCtx* c) {
    (void)o, (void)c;
    return RC(10, 235);
}
int stub_ok(SvcObj* o, IpcCtx* c) {
    (void)o;
    (void)c;
    test_fail("unexpected stub command", __LINE__);
    return -1;
}
int quiet_ok(SvcObj* o, IpcCtx* c) { return stub_ok(o, c); }
void svc_register(const char* name, SvcCtor ctor) {
    (void)name;
    (void)ctor;
}
void audren_tick(void) {}
void outstream_tick(u64 now) { (void)now; } /* audio_stream.c's: no output streams here */
/* fatal: the game stops, which a check expects, with what it said kept */
static jmp_buf test_stopped;
static int test_expect_stop;
static char test_stop_text[512];
void fatal(const char* fmt, ...) {
    va_list a;
    va_start(a, fmt);
    vsnprintf(test_stop_text, sizeof test_stop_text, fmt, a);
    va_end(a);
    if (!test_expect_stop) {
        fprintf(stderr, "audio_out.c: unexpected fatal: %s\n", test_stop_text);
        exit(1);
    }
    longjmp(test_stopped, 1);
}
static unsigned test_thread_dumps;
void kernel_dump_threads(void) { test_thread_dumps++; }
/* nn/audio.c's, the native audio outs, which have their own test (nn_audio.c): what audio.c asks of them, counted */
static unsigned test_out_drops, test_out_rebuilds;
void nnaudio_out_tick(u64 now) { (void)now; } /* the clock's thread never runs here */
void nnaudio_out_state_drop(void) { test_out_drops++; }
void nnaudio_out_state_rebuild(void) { test_out_rebuilds++; }
void audren_volume(float gain) { (void)gain; } /* the device's volume, which nn/audio.c sets */
KEvent* kevent_new(const char* name) {
    KEvent* e = calloc(1, sizeof *e);
    CHECK(e != NULL);
    e->o.type = KO_EVENT;
    kobj_set_name(&e->o, name);
    test_events++;
    return e;
}
void kobj_ref(KObj* o) { o->refs++; }
void kobj_unref(KObj* o) {
    CHECK(o->refs > 0);
    if (--o->refs) return;
    free(o);
    test_events--;
}
void kevent_signal(KEvent* e) { e->signaled = 1; }
u32 owned_event(const char* name) {
    KEvent* e = kevent_new(name);
    kobj_ref(&e->o);
    return kobj_id(&e->o);
}
u32 ev_handle(u32 ev) {
    CHECK(kev(ev) != NULL);
    return 7;
}
#ifndef SNX_NO_H264
void ffmpeg_log_init(void) {}
#endif

/* a request with no arguments, as aud:u's commands take */
static IpcCtx request(u32 cmd, const void* raw, u32 size) {
    IpcCtx c;
    memset(&c, 0, sizeof c);
    c.cmd = cmd;
    c.raw = raw;
    c.raw_size = size;
    return c;
}
#ifndef SNX_NO_H264
/* a 440 Hz tone libopus (1.5, 6 kbit/s) encoded as 20 ms packets: three SILK narrowband, then hybrid fullband and
   CELT. FFmpeg's resampler holds back samples of the first and gives them back with the hybrid one, where its SILK
   rate changes: every reply still has to be the packet's 20 ms, in a buffer sized for that */
static const u8 opus_tone[] = {
    0x00, 0x00, 0x00, 0x0B, 0,    0,    0,    0,    0x08, 0x83, 0xF9, 0x5E, 0xDD, 0x07, 0x1D, 0x4F, 0x23, 0x57, 0x20,
    0x00, 0x00, 0x00, 0x1C, 0,    0,    0,    0,    0x08, 0xAC, 0x3F, 0x74, 0x00, 0xE7, 0x95, 0x5C, 0x81, 0xA4, 0x83,
    0xEA, 0x83, 0x32, 0x06, 0x21, 0xF8, 0xF5, 0x7E, 0x77, 0xE7, 0x4A, 0x2B, 0x8D, 0x72, 0x2E, 0xCF, 0x60, 0x00, 0x00,
    0x00, 0x10, 0,    0,    0,    0,    0x08, 0xA8, 0x5F, 0xCD, 0xAD, 0xF0, 0xDB, 0x1D, 0x7C, 0xE3, 0x69, 0xA5, 0x64,
    0xFD, 0xD7, 0x60, 0x00, 0x00, 0x00, 0x22, 0,    0,    0,    0,    0x78, 0x85, 0xE6, 0x29, 0xC2, 0x60, 0x33, 0x08,
    0xDA, 0x6D, 0x3C, 0x99, 0xC5, 0xF2, 0x66, 0x1A, 0x42, 0x46, 0x50, 0x16, 0x63, 0x03, 0x84, 0x78, 0xCF, 0xFF, 0x16,
    0x8D, 0x7C, 0xCF, 0xA8, 0xBD, 0xFA, 0x33, 0x00, 0x00, 0x00, 0x10, 0,    0,    0,    0,    0xF8, 0x63, 0xAE, 0xAD,
    0xD6, 0x7A, 0x46, 0x26, 0x4F, 0x24, 0x1D, 0x9A, 0x2C, 0x1F, 0x02, 0xD7,
};
static void test_opus_decode(void) {
    static const u8 toc[][2] = {{0x08, 0}, {0x19, 0}, {0x62, 0}, {0xFB, 0x03}, {0x0B, 0x07}, {0x83, 0}};
    static const u32 toc_48k[] = {960, 5760, 960, 2880, 0, 0}; /* code 3 with 7 x 20 ms is above 120 ms */
    for (u32 i = 0; i < 6; i++) CHECK(opus_packet_48k(toc[i], (toc[i][0] & 3) == 3 ? 2 : 1) == toc_48k[i]);
    CHECK(opus_packet_48k(toc[3], 1) == 0); /* code 3 without its frame count */
    HwOpus h;
    memset(&h, 0, sizeof h);
    CHECK(hwopus_open(&h, 16000, 1, 1, 0, NULL));
    i16 out[320];
    u32 consumed, samples;
    for (int pass = 0; pass < 2; pass++) {
        u32 off = 0;
        for (u32 i = 0; off < sizeof opus_tone; i++) {
            const u8* p = opus_tone + off;
            u32 len = 8 + p[3];
            if (i == 1) /* one sample short: refused before FFmpeg decodes it, so the stream goes on unchanged */
                CHECK(hwopus_decode(&h, p, len, out, sizeof out - 2, 0, &consumed, &samples) == RC(111, 17));
            consumed = samples = 0;
            CHECK(hwopus_decode(&h, p, len, out, sizeof out, pass && !i, &consumed, &samples) == 0);
            CHECK(consumed == len && samples == 320);
            off += len;
        }
        int loud = 0;
        for (u32 i = 0; i < 320; i++) {
            i16 v = (i16)snx_ld_le16(&out[i]); /* the guest's samples */
            loud |= v > 2000 || v < -2000;
        }
        CHECK(loud); /* the tone, still there after the switch to CELT */
    }
    CHECK(hwopus_decode(&h, opus_tone, 8, out, sizeof out, 0, &consumed, &samples) == RC(111, 17)); /* no payload */
    hwopus_free(&h); /* opened by hand, so it holds none of the pool's decoders */
}
#endif
/* the game's decoder objects, 0x40 apart, and what a decode reads and writes */
#define T_DEC(i)   (0x4000u + 0x40u * (u32)(i))
#define T_MAP      0x3000u
#define T_IN       0x5000u
#define T_OUT      0x5400u
#define T_CONSUMED 0x5800u
#define T_SAMPLES  0x5808u
static CPU cpu;
static u32 live_decoders(void) {
    u32 n = 0;
    for (u32 i = 0; i < HWOPUS_POOL; i++) n += g_hle.audio.opus[i].guest != 0;
    return n;
}
static HwOpus* decoder_at(u64 guest) {
    for (u32 i = 0; i < HWOPUS_POOL; i++)
        if (g_hle.audio.opus[i].guest == guest) return &g_hle.audio.opus[i];
    return NULL;
}
#ifndef SNX_NO_H264
/* the handlers' decode against the decoding itself: the same packets through a decoder opened by hand give the same
   samples, bit by bit; the results are the SDK's own */
static void test_hwopus_decode(void) {
    CPU* c = &cpu;
    HwOpus ref;
    i16 want[320];
    u32 consumed, samples, off = 0, first = 8 + opus_tone[3];
    memset(&ref, 0, sizeof ref);
    CHECK(hwopus_open(&ref, 16000, 1, 1, 0, NULL));
    memset(GP(T_DEC(0)), 0xCC, 0x40);
    CHECK(nncodec_hwopus_init_timed(c, T_DEC(0), 16000, 1, 0x6000, 0x1000, 0) == 0);
    while (off < sizeof opus_tone) {
        u32 len = 8 + opus_tone[off + 3];
        memcpy(GP(T_IN), opus_tone + off, len);
        memset(GP(T_OUT), 0xEE, 0x400);
        memset(GP(T_CONSUMED), 0xEE, 16);
        test_now += 250000;
        CHECK(nncodec_hwopus_decode_timed(c, T_DEC(0), T_CONSUMED, T_SAMPLES, T_OUT, 640, T_IN, len) == 0);
        CHECK(hwopus_decode(&ref, opus_tone + off, len, want, sizeof want, 0, &consumed, &samples) == 0);
        /* a size_t of the bytes taken, an int of the samples given, and nothing past the packet's samples */
        CHECK(snx_ld_le64(GP(T_CONSUMED)) == len && snx_ld_le32(GP(T_SAMPLES)) == 320 && GP(T_SAMPLES)[4] == 0xEE);
        CHECK(!memcmp(GP(T_OUT), want, sizeof want) && GP(T_OUT)[640] == 0xEE);
        off += len;
    }
    /* the SDK's own ResetHardwareOpusDecoder sets the struct's flag: the next decode starts the stream again, as a
       decoder just opened does, and clears it */
    hwopus_free(&ref);
    memset(&ref, 0, sizeof ref);
    CHECK(hwopus_open(&ref, 16000, 1, 1, 0, NULL));
    nncodec_hwopus_reset(c, T_DEC(0));
    CHECK(GP(T_DEC(0))[0x28] == 1);
    memcpy(GP(T_IN), opus_tone, first);
    CHECK(nncodec_hwopus_decode_timed(c, T_DEC(0), T_CONSUMED, T_SAMPLES, T_OUT, 640, T_IN, first) == 0);
    CHECK(hwopus_decode(&ref, opus_tone, first, want, sizeof want, 0, &consumed, &samples) == 0);
    CHECK(GP(T_DEC(0))[0x28] == 0 && !memcmp(GP(T_OUT), want, sizeof want));
    hwopus_free(&ref);
    /* the SDK's results: 6 for an input shorter than a header or than the header says, 7 for an output with no room
       (4 in SDK 0.12, where the session refuses it), 4 for a packet that isn't one, and nothing written for any */
    memset(GP(T_CONSUMED), 0xEE, 16);
    CHECK(nncodec_hwopus_decode_timed(c, T_DEC(0), T_CONSUMED, T_SAMPLES, T_OUT, 640, T_IN, 7) == 6);
    CHECK(nncodec_hwopus_decode_timed(c, T_DEC(0), T_CONSUMED, T_SAMPLES, T_OUT, 640, T_IN, first - 1) == 6);
    CHECK(nncodec_hwopus_decode_timed(c, T_DEC(0), T_CONSUMED, T_SAMPLES, T_OUT, 638, T_IN, first) == 7);
    snx_st_be32(GP(T_IN), 0);
    CHECK(nncodec_hwopus_decode_timed(c, T_DEC(0), T_CONSUMED, T_SAMPLES, T_OUT, 640, T_IN, first) == 4);
    CHECK(GP(T_CONSUMED)[0] == 0xEE && GP(T_SAMPLES)[0] == 0xEE);
    /* the time the last decode took, in microseconds, where the SDK's getter reads it */
    CHECK(snx_ld_le64(GP(T_DEC(0) + 0x20)) == 0); /* the clock stood still during it */
    nncodec_hwopus_finalize(c, T_DEC(0));
    /* the class up to SDK 4: the same decode, no flag and no time in its object; SDK 0.12's leaves the room to the
       session */
    memset(GP(T_DEC(0)), 0xCC, 0x40);
    CHECK(nncodec_hwopus_class_init(c, T_DEC(0), 16000, 1, 0x6000, 0x1000) == 0);
    memcpy(GP(T_IN), opus_tone, first);
    CHECK(nncodec_hwopus_class_decode(c, T_DEC(0), T_CONSUMED, T_SAMPLES, T_OUT, 640, T_IN, first) == 0);
    CHECK(snx_ld_le32(GP(T_SAMPLES)) == 320 && GP(T_DEC(0))[0x20] == 0xCC && GP(T_DEC(0))[0x28] == 0xCC);
    CHECK(nncodec_hwopus_class_decode(c, T_DEC(0), T_CONSUMED, T_SAMPLES, T_OUT, 638, T_IN, first) == 7);
    CHECK(nncodec_hwopus_class_decode_first(c, T_DEC(0), T_CONSUMED, T_SAMPLES, T_OUT, 638, T_IN, first) == 4);
    nncodec_hwopus_class_finalize(c, T_DEC(0));
    /* an object that was never opened: the log names it */
    test_last_log[0] = 0;
    CHECK(nncodec_hwopus_decode_timed(c, T_DEC(0), T_CONSUMED, T_SAMPLES, T_OUT, 640, T_IN, first) == 4);
    CHECK(strstr(test_last_log, "0x4000") && live_decoders() == 0);
}
#endif
static void test_hwopus(void) {
    CPU* c = &cpu;
    /* the console's work buffer sizes (Ryujinx's): the decoder state, then the stream and frame buffers */
    CHECK(nncodec_hwopus_work_size(c, 48000, 2) == 0xd474 && nncodec_hwopus_work_size_option(c, 48000, 2, 0) == 0xd474);
    CHECK(nncodec_hwopus_multi_work_size(c, 48000, 6, 4, 2) == 0x25750);
    /* from SDK 16 the session decodes on the DSP: no decoder state. One game's sound init takes these two sizes, and
       fails with the state in them */
    CHECK(nncodec_hwopus_multi_work_size_dsp(c, 48000, 2, 2, 0) == 0x1ac0);
    CHECK(nncodec_hwopus_multi_work_size_option_dsp(c, 48000, 2, 1, 1, 0) == 0x1500);
    CHECK(nncodec_hwopus_work_size_dsp(c, 48000, 2, 1) == 0x3300); /* the option's bit 0: frames of 120 ms */
    CHECK(nncodec_hwopus_multi_work_size_option(c, 48000, 2, 1, 1, 1) == 0x15394);
    /* the class's, with the object first; from SDK 12 the multistream class keeps its option at 0x40 */
    CHECK(nncodec_hwopus_class_work_size(c, T_DEC(0), 24000, 1) == 0x9f24); /* 24 kHz: half the frame */
    CHECK(nncodec_hwopus_class_multi_work_size(c, T_DEC(0), 48000, 6, 4, 2) == 0x25750);
    snx_st_le32(GP(T_DEC(0) + 0x40), 1);
    CHECK(nncodec_hwopus_class_multi_work_size_option(c, T_DEC(0), 48000, 2, 1, 1) == 0x15394);
    /* what the session refused has no size: more streams than channels, three channels in one stream, a rate Opus
       has no decoder for; the log names the numbers */
    test_last_log[0] = 0;
    CHECK(nncodec_hwopus_multi_work_size(c, 48000, 2, 2, 1) == 0 && strstr(test_last_log, "2 channels in 2 streams"));
    CHECK(nncodec_hwopus_work_size(c, 48000, 3) == 0 && nncodec_hwopus_work_size(c, 44100, 2) == 0);
    CHECK(strstr(test_last_log, "44100 Hz"));
#ifndef SNX_NO_H264
    /* the struct from SDK 6: the open fills what the SDK's own getters read, the rate and the channels, zeroes the
       last decode's time and the reset flag, and leaves a word that isn't zero where the session's object goes */
    memset(GP(T_DEC(0)), 0xCC, 0x40);
    CHECK(nncodec_hwopus_init_timed(c, T_DEC(0), 48000, 2, 0x6000, 0x1000, 0) == 0 && live_decoders() == 1);
    CHECK(snx_ld_le64(GP(T_DEC(0))) != 0 && snx_ld_le32(GP(T_DEC(0) + 0x18)) == 48000);
    CHECK(snx_ld_le32(GP(T_DEC(0) + 0x1c)) == 2 && snx_ld_le64(GP(T_DEC(0) + 0x20)) == 0);
    CHECK(GP(T_DEC(0))[0x28] == 0 && GP(T_DEC(0))[0x29] == 0xCC && GP(T_DEC(0))[8] == 0xCC);
    CHECK(decoder_at(T_DEC(0))->channels == 2 && decoder_at(T_DEC(0))->rate == 48000);
    /* opened again over itself: one decoder still */
    CHECK(nncodec_hwopus_init_timed(c, T_DEC(0), 24000, 1, 0x6000, 0x1000, 0) == 0 && live_decoders() == 1);
    CHECK(decoder_at(T_DEC(0))->step == 2 && snx_ld_le32(GP(T_DEC(0) + 0x18)) == 24000);
    /* the close frees it and clears the struct's first four words, as the SDK does */
    nncodec_hwopus_finalize(c, T_DEC(0));
    CHECK(live_decoders() == 0 && snx_ld_le64(GP(T_DEC(0))) == 0 && snx_ld_le64(GP(T_DEC(0) + 0x18)) == 0);
    CHECK(GP(T_DEC(0))[0x29] == 0xCC);
    nncodec_hwopus_finalize(c, T_DEC(0)); /* closed twice: nothing to let go */
    /* SDK 5's struct has no time and no flag: the bytes there stay the game's */
    memset(GP(T_DEC(0)), 0xCC, 0x40);
    CHECK(nncodec_hwopus_init(c, T_DEC(0), 48000, 1, 0x6000, 0x1000, 0) == 0);
    CHECK(snx_ld_le32(GP(T_DEC(0) + 0x1c)) == 1 && GP(T_DEC(0))[0x20] == 0xCC && GP(T_DEC(0))[0x28] == 0xCC);
    /* the class from SDK 5 is that struct behind its vtable: its Finalize closes the struct's decoder */
    nncodec_hwopus_type_class_finalize(c, T_DEC(0) - 8);
    CHECK(live_decoders() == 0 && snx_ld_le64(GP(T_DEC(0))) == 0);
    /* the class up to SDK 4: the flag that it is initialized, the rate and the channels, and its vtable untouched */
    memset(GP(T_DEC(0)), 0xCC, 0x40);
    CHECK(nncodec_hwopus_class_init(c, T_DEC(0), 24000, 1, 0x6000, 0x1000) == 0);
    CHECK(GP(T_DEC(0))[0] == 0xCC && GP(T_DEC(0))[8] == 1 && GP(T_DEC(0))[9] == 0xCC);
    CHECK(snx_ld_le32(GP(T_DEC(0) + 0xc)) == 24000 && snx_ld_le32(GP(T_DEC(0) + 0x10)) == 1);
    CHECK(nncodec_hwopus_class_set_context(c, T_DEC(0), 0x6000, 0x10) == 0);
    nncodec_hwopus_class_finalize(c, T_DEC(0));
    CHECK(live_decoders() == 0 && GP(T_DEC(0))[8] == 0 && snx_ld_le32(GP(T_DEC(0) + 0xc)) == 24000);
    /* multistream: six channels in four streams, two of them stereo, placed by the mapping; the stream counts where
       each shape keeps them */
    static const u8 mapping[6] = {0, 4, 1, 2, 3, 5};
    memcpy(GP(T_MAP), mapping, sizeof mapping);
    memset(GP(T_DEC(0)), 0xCC, 0x40);
    CHECK(nncodec_hwopus_multi_init_timed(c, T_DEC(0), 48000, 6, 4, 2, T_MAP, 0x6000, 0x1000) == 0);
    CHECK(snx_ld_le32(GP(T_DEC(0) + 0x1c)) == 6 && snx_ld_le32(GP(T_DEC(0) + 0x20)) == 4);
    CHECK(snx_ld_le32(GP(T_DEC(0) + 0x24)) == 2 && snx_ld_le64(GP(T_DEC(0) + 0x28)) == 0 && GP(T_DEC(0))[0x30] == 0);
    CHECK(decoder_at(T_DEC(0))->streams == 4 && decoder_at(T_DEC(0))->mapped &&
          !memcmp(decoder_at(T_DEC(0))->mapping, mapping, 6));
    nncodec_hwopus_multi_finalize(c, T_DEC(0));
    CHECK(live_decoders() == 0 && snx_ld_le64(GP(T_DEC(0) + 0x20)) == 0 && GP(T_DEC(0))[0x30] == 0);
    memset(GP(T_DEC(0)), 0xCC, 0x40);
    CHECK(nncodec_hwopus_class_multi_init(c, T_DEC(0), 48000, 6, 4, 2, T_MAP, 0x6000, 0x1000) == 0);
    CHECK(GP(T_DEC(0))[8] == 1 && snx_ld_le32(GP(T_DEC(0) + 0x14)) == 4 && snx_ld_le32(GP(T_DEC(0) + 0x18)) == 2);
    nncodec_hwopus_class_multi_finalize(c, T_DEC(0));
    CHECK(nncodec_hwopus_multi_init(c, T_DEC(0), 48000, 6, 4, 2, T_MAP, 0x6000, 0x1000) == 0);
    nncodec_hwopus_type_class_multi_finalize(c, T_DEC(0) - 8);
    CHECK(live_decoders() == 0);
    /* no decoder: three channels in one stream, a rate Opus doesn't decode at, a mapping outside the game's memory.
       The SDK's result for none left, the object as it was, and the log's numbers */
    memset(GP(T_DEC(0)), 0xCC, 0x40);
    test_last_log[0] = 0;
    CHECK(nncodec_hwopus_init_timed(c, T_DEC(0), 48000, 3, 0x6000, 0x1000, 0) == 8);
    CHECK(strstr(test_last_log, "3 channels at 48000 Hz") && GP(T_DEC(0))[0] == 0xCC && GP(T_DEC(0))[0x18] == 0xCC);
    CHECK(nncodec_hwopus_init_timed(c, T_DEC(0), 44100, 2, 0x6000, 0x1000, 0) == 8);
    CHECK(nncodec_hwopus_multi_init_timed(c, T_DEC(0), 48000, 6, 4, 2, 0x20000, 0x6000, 0x1000) == 8);
    CHECK(strstr(test_last_log, "0x20000") && live_decoders() == 0);
    /* 24 decoders, as the firmware has; the 25th gets the SDK's result for none left until one closes */
    for (u32 i = 0; i < 24; i++) CHECK(nncodec_hwopus_init_timed(c, T_DEC(i), 48000, 2, 0x6000, 0x1000, 0) == 0);
    test_last_log[0] = 0;
    CHECK(nncodec_hwopus_init_timed(c, T_DEC(24), 48000, 2, 0x6000, 0x1000, 0) == 8 && live_decoders() == 24);
    CHECK(strstr(test_last_log, "all 24 open"));
    nncodec_hwopus_finalize(c, T_DEC(5));
    CHECK(nncodec_hwopus_init_timed(c, T_DEC(24), 48000, 1, 0x6000, 0x1000, 0) == 0);
    CHECK(nncodec_hwopus_init_timed(c, T_DEC(5), 48000, 2, 0x6000, 0x1000, 0) == 8);
    /* the decoders a save state holds count once it is loaded: 24 saved with their host parts nil, as a save writes
       them, then loaded over the running ones as state.c does (the host's parts let go of, then made again) */
    static HleAudio saved;
    saved = g_hle.audio;
    for (u32 i = 0; i < HWOPUS_POOL; i++) {
        saved.opus[i].dec = NULL;
        saved.opus[i].pkt = NULL;
        saved.opus[i].frame = NULL;
        saved.opus[i].queue = NULL;
        saved.opus[i].taps = saved.opus[i].hist = NULL;
    }
    audio_state_drop();
    g_hle.audio = saved;
    audio_state_rebuild();
    CHECK(decoder_at(T_DEC(7))->dec != NULL && decoder_at(T_DEC(24))->channels == 1); /* made again */
    CHECK(nncodec_hwopus_init_timed(c, T_DEC(5), 48000, 2, 0x6000, 0x1000, 0) == 8);
    for (u32 i = 0; i < 25; i++) nncodec_hwopus_finalize(c, T_DEC(i));
    CHECK(live_decoders() == 0);
    test_opus_decode();
    test_hwopus_decode();
#else
    CHECK(nncodec_hwopus_init_timed(c, T_DEC(0), 48000, 2, 0x6000, 0x1000, 0) ==
          8); /* this build has no Opus decoder */
    CHECK(live_decoders() == 0);
#endif
}

/* codec.hook: each function by the handler its SDK's version takes, and none for an SDK that was not read */
static void test_hwopus_table(void) {
#define N     "_ZN2nn5codec"
#define K     "_ZNK2nn5codec"
#define TYPE  "PNS0_23HardwareOpusDecoderTypeE"
#define MULTI "PNS0_34HardwareOpusMultiStreamDecoderTypeE"
    static const struct {
        u32 sdk;
        const char* symbol;
        const char* handler; /* NULL: no hook for that version */
    } hooks[] = {
        {0x000c0e, N "19HardwareOpusDecoder10InitializeEiiPvm", "nncodec_hwopus_class_init"},
        {0x000c0e, N "19HardwareOpusDecoder17DecodeInterleavedEPmPiPsmPKvm", "nncodec_hwopus_class_decode_first"},
        {0x010301, N "19HardwareOpusDecoder17DecodeInterleavedEPmPiPsmPKvm", "nncodec_hwopus_class_decode"},
        {0x020300, N "19HardwareOpusDecoder10InitializeEiiPvm", NULL},
        {0x030500, N "19HardwareOpusDecoder10SetContextEPKvm", "nncodec_hwopus_class_set_context"},
        {0x030500, N "30HardwareOpusMultiStreamDecoder10InitializeEiiiiPKhPvm", "nncodec_hwopus_class_multi_init"},
        {0x040500, N "30HardwareOpusMultiStreamDecoder17DecodeInterleavedEPmPiPsmPKvm",
         "nncodec_hwopus_class_multi_decode"},
        {0x040500, N "19HardwareOpusDecoder8FinalizeEv", "nncodec_hwopus_class_finalize"},
        {0x040500, K "19HardwareOpusDecoder17GetWorkBufferSizeEii", "nncodec_hwopus_class_work_size"},
        {0x0504b4, N "19HardwareOpusDecoder8FinalizeEv", "nncodec_hwopus_type_class_finalize"},
        {0x0504b4, N "19HardwareOpusDecoder10InitializeEiiPvm", NULL}, /* the class calls the struct's from SDK 5 */
        {0x0504b4, K "19HardwareOpusDecoder17GetWorkBufferSizeEii", "nncodec_hwopus_class_work_size"},
        {0x060500, K "19HardwareOpusDecoder17GetWorkBufferSizeEii", NULL},
        {0x0504b4, N "29InitializeHardwareOpusDecoderE" TYPE "iiPvmi", "nncodec_hwopus_init"},
        {0x0504b4, N "33DecodeOpusInterleavedWithHardwareE" TYPE "PmPiPsmPKvm", "nncodec_hwopus_decode"},
        {0x060500, N "29InitializeHardwareOpusDecoderE" TYPE "iiPvmi", "nncodec_hwopus_init_timed"},
        {0x140511, N "33DecodeOpusInterleavedWithHardwareE" TYPE "PmPiPsmPKvm", "nncodec_hwopus_decode_timed"},
        {0x150000, N "29InitializeHardwareOpusDecoderE" TYPE "iiPvmi", NULL},
        {0x0504b4, N "40InitializeHardwareOpusMultiStreamDecoderE" MULTI "iiiiPKhPvmi", "nncodec_hwopus_multi_init"},
        {0x0d0302, N "40InitializeHardwareOpusMultiStreamDecoderE" MULTI "iiiiPKhPvmi",
         "nncodec_hwopus_multi_init_timed"},
        {0x0d0302, N "44DecodeOpusMultiStreamInterleavedWithHardwareE" MULTI "PmPiPsmPKvm",
         "nncodec_hwopus_multi_decode_timed"},
        {0x0d0302, N "38FinalizeHardwareOpusMultiStreamDecoderE" MULTI, "nncodec_hwopus_multi_finalize"},
        {0x0e0300, N "30HardwareOpusMultiStreamDecoder8FinalizeEv", "nncodec_hwopus_type_class_multi_finalize"},
        {0x0f0301, N "30HardwareOpusMultiStreamDecoder8FinalizeEv", NULL},
        {0x0b0400, N "36GetHardwareOpusDecoderWorkBufferSizeEii", "nncodec_hwopus_work_size"},
        {0x0c0307, N "36GetHardwareOpusDecoderWorkBufferSizeEii", NULL}, /* the SDK's own, over the one below */
        {0x0c0307, N "36GetHardwareOpusDecoderWorkBufferSizeEiii", "nncodec_hwopus_work_size_option"},
        {0x0f0301, N "36GetHardwareOpusDecoderWorkBufferSizeEiii", "nncodec_hwopus_work_size_option"},
        {0x100200, N "36GetHardwareOpusDecoderWorkBufferSizeEiii", "nncodec_hwopus_work_size_dsp"},
        {0x0b0400, N "47GetHardwareOpusMultiStreamDecoderWorkBufferSizeEiiii", "nncodec_hwopus_multi_work_size"},
        {0x0f0301, N "47GetHardwareOpusMultiStreamDecoderWorkBufferSizeEiiii", "nncodec_hwopus_multi_work_size"},
        {0x100200, N "47GetHardwareOpusMultiStreamDecoderWorkBufferSizeEiiii", "nncodec_hwopus_multi_work_size_dsp"},
        {0x0f0301, N "47GetHardwareOpusMultiStreamDecoderWorkBufferSizeEiiiii",
         "nncodec_hwopus_multi_work_size_option"},
        {0x140511, N "47GetHardwareOpusMultiStreamDecoderWorkBufferSizeEiiiii",
         "nncodec_hwopus_multi_work_size_option_dsp"},
        {0x0b0400, K "30HardwareOpusMultiStreamDecoder17GetWorkBufferSizeEiiii",
         "nncodec_hwopus_class_multi_work_size"},
        {0x0e0300, K "30HardwareOpusMultiStreamDecoder17GetWorkBufferSizeEiiii",
         "nncodec_hwopus_class_multi_work_size_option"},
    };
    CPU* c = &cpu;
    int first, k;
    for (unsigned i = 0; i < sizeof hooks / sizeof hooks[0]; i++) {
        u32 versions[HOOK_KEYS] = {0};
        versions[HOOK_SDK] = hooks[i].sdk;
        k = hook_select(&hook_table, hooks[i].symbol, versions, &first);
        if (!hooks[i].handler)
            CHECK(k == -2);
        else
            CHECK(k >= 0 && !strcmp(hook_handler[k], hooks[i].handler));
    }
    { /* a module whose SDK is unknown takes none: every function here is one SDK's */
        u32 versions[HOOK_KEYS] = {0};
        CHECK(hook_select(&hook_table, N "29InitializeHardwareOpusDecoderE" TYPE "iiPvmi", versions, &first) < 0);
    }
    { /* through the adapters, as a game's calls: the work buffer's size from two ints, and a refused open's result */
        u32 versions[HOOK_KEYS] = {0};
        versions[HOOK_SDK] = 0x140511;
        k = hook_select(&hook_table, N "36GetHardwareOpusDecoderWorkBufferSizeEiii", versions, &first);
        c->x[0] = 48000, c->x[1] = 2, c->x[2] = 1, c->x[30] = 0x1234, c->pc = 0;
        hook_fn[k](c, 0);
        CHECK(c->x[0] == 0x3300 && c->pc == 0x1234);
        k = hook_select(&hook_table, N "29InitializeHardwareOpusDecoderE" TYPE "iiPvmi", versions, &first);
        c->x[0] = T_DEC(0), c->x[1] = 44100, c->x[2] = 2, c->x[3] = 0x6000, c->x[4] = 0x1000, c->x[5] = 0;
        c->x[30] = 0x1234, c->pc = 0;
        hook_fn[k](c, 0);
        CHECK((u32)c->x[0] == 8 && c->pc == 0x1234 && live_decoders() == 0);
    }
#undef N
#undef K
#undef TYPE
#undef MULTI
}

/* aud:u, the Ounce's manager of managers: the audio in manager it hands out, and the audio out and renderer
   managers, which nothing serves (the outs and the renderer are native): asking for one stops the game after the
   threads' calls, naming nn::audio */
static void test_native_manager(void) {
    static const char* const what[3] = {NULL, "the audio out manager", "the audio renderer manager"};
    SvcObj* aud = new_aud();
    IpcCtx c = request(0, NULL, 0);
    CHECK(aud->dispatch(aud, &c) == 0 && c.nout_objs == 1);
    svcobj_unref(c.out_objs[0]);
    for (u32 cmd = 1; cmd <= 2; cmd++) {
        unsigned dumps = test_thread_dumps;
        c = request(cmd, NULL, 0);
        test_expect_stop = 1;
        int stopped = setjmp(test_stopped);
        if (!stopped) aud->dispatch(aud, &c);
        test_expect_stop = 0;
        CHECK(stopped == 1 && test_thread_dumps == dumps + 1);
        CHECK(strstr(test_stop_text, "aud:u") && strstr(test_stop_text, what[cmd]) &&
              strstr(test_stop_text, "nn::audio (nn/audio.c)"));
    }
    svcobj_unref(aud);
}

int main(void) {
    hle_init();
    g_mem = calloc(1, 0x10000);
    CHECK(g_mem != NULL);
    test_native_manager();
    /* a save state's load closes and reopens the outs' host outputs through nn/audio.c, once each */
    audio_state_drop();
    audio_state_rebuild();
    CHECK(test_out_drops == 1 && test_out_rebuilds == 1);
    cpu.mem = g_mem;
    test_hwopus();
    test_hwopus_table();
    CHECK(test_objects == 0 && test_events == 0);
    free(g_mem);
    printf("audio: %u checks passed\n", test_checks);
    return 0;
}
