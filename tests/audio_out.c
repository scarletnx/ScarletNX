/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* hwopus and aud:u (audio.c) through their IPC commands. The native audio outs and audio device have their own test
   (nn_audio.c), and the renderer its own (nn_audio_renderer.c, audren_host.c) */
#include "nx/hle/services/services_int.h"
#include <setjmp.h>
#include <stdarg.h>

#include "nx/hle/services/audio.c"
#include "nx/hle/hle.c"
#include "kobj_ids.h" /* the kernel objects the stubs make, as ids */

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

/* what ipc.c makes of a reply: a failed request returns no data, handles or objects */
static int ipc_result(IpcCtx* c, int rc) {
    if (!rc) return 0;
    for (int i = 0; i < c->nout_objs; i++) svcobj_unref(c->out_objs[i]);
    c->nout_copy = c->nout_move = c->nout_objs = 0;
    c->out_size = 0;
    return rc;
}
/* a request's raw data is the message's, little-endian as the guest writes it: these give a command's arguments in
   that order, from a ring of buffers that outlives the requests a check makes at once */
static u8 test_raw[8][0x48];
static unsigned test_raw_next;
static u8* raw_slot(void) { return test_raw[test_raw_next++ % 8]; }
static const void* le32s(const u32* v, u32 n) {
    u8* p = raw_slot();
    for (u32 i = 0; i < n; i++) snx_st_le32(p + i * 4, v[i]);
    return p;
}
static IpcCtx request(u32 cmd, const void* raw, u32 size) {
    IpcCtx c;
    memset(&c, 0, sizeof c);
    c.cmd = cmd;
    c.raw = raw;
    c.raw_size = size;
    return c;
}
static u32 reply32(IpcCtx* c, u32 off) {
    CHECK(off + 4 <= c->out_size);
    return snx_ld_le32(c->out + off);
}
static IpcBuf buffer(u64 addr, u64 size) {
    IpcBuf b = {addr, size, 0};
    return b;
}

/* hwopus: GetWorkBufferSize (1) and OpenHardwareOpusDecoder (0), the parameters {sample rate, channels} at 0 and the
   work buffer's size at 8. The SDK's decoder takes one or two channels: more is the multistream commands' */
static int open_opus(SvcObj* manager, u32 rate, u32 channels, SvcObj** decoder) {
    u32 raw[3] = {rate, channels, 0x10000};
    IpcCtx c = request(0, le32s(raw, 3), sizeof raw);
    c.ncopy = 1;
    c.copy_h[0] = 0x200;
    int rc = manager->dispatch(manager, &c);
    CHECK(c.nout_objs == 1 && (!rc || !c.out_objs[0])); /* a refused open makes no decoder */
    rc = ipc_result(&c, rc);
    CHECK(c.out_size == 0);
    *decoder = rc ? NULL : c.out_objs[0];
    return rc;
}
#ifndef SNX_NO_H264
/* a 440 Hz tone libopus (1.5, 6 kbit/s) encoded as 20 ms packets: three SILK narrowband, then hybrid fullband and
   CELT. FFmpeg's resampler holds back samples of the first and gives them back with the hybrid one, where its SILK
   rate changes: every reply still has to be the packet's 20 ms, in a buffer sized for that */
static const u8 opus_tone[] = {
    0x00, 0x00, 0x00, 0x0B, 0, 0, 0, 0, 0x08, 0x83, 0xF9, 0x5E, 0xDD, 0x07, 0x1D, 0x4F, 0x23, 0x57, 0x20,
    0x00, 0x00, 0x00, 0x1C, 0, 0, 0, 0, 0x08, 0xAC, 0x3F, 0x74, 0x00, 0xE7, 0x95, 0x5C, 0x81, 0xA4, 0x83, 0xEA, 0x83,
    0x32, 0x06, 0x21, 0xF8, 0xF5, 0x7E, 0x77, 0xE7, 0x4A, 0x2B, 0x8D, 0x72, 0x2E, 0xCF, 0x60,
    0x00, 0x00, 0x00, 0x10, 0, 0, 0, 0, 0x08, 0xA8, 0x5F, 0xCD, 0xAD, 0xF0, 0xDB, 0x1D, 0x7C, 0xE3, 0x69, 0xA5, 0x64,
    0xFD, 0xD7, 0x60,
    0x00, 0x00, 0x00, 0x22, 0, 0, 0, 0, 0x78, 0x85, 0xE6, 0x29, 0xC2, 0x60, 0x33, 0x08, 0xDA, 0x6D, 0x3C, 0x99, 0xC5,
    0xF2, 0x66, 0x1A, 0x42, 0x46, 0x50, 0x16, 0x63, 0x03, 0x84, 0x78, 0xCF, 0xFF, 0x16, 0x8D, 0x7C, 0xCF, 0xA8, 0xBD,
    0xFA, 0x33,
    0x00, 0x00, 0x00, 0x10, 0, 0, 0, 0, 0xF8, 0x63, 0xAE, 0xAD, 0xD6, 0x7A, 0x46, 0x26, 0x4F, 0x24, 0x1D, 0x9A, 0x2C,
    0x1F, 0x02, 0xD7,
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
    hwopus_free(&h); /* opened by hand, so it holds none of the service's decoder slots */
}
#endif
static void test_hwopus(void) {
    SvcObj* manager = new_hwopus();
    u32 params[2] = {48000, 2};
    IpcCtx c = request(1, le32s(params, 2), sizeof params);
    CHECK(manager->dispatch(manager, &c) == 0 && c.out_size == 4 && reply32(&c, 0) == 0xd474);
    /* the console's sizes (Ryujinx's): the decoder state, then the stream and frame buffers. Parameters too short to
       read get the error of the opens */
    u32 multi[4] = {48000, 6, 4, 2}, size = 0;
    memset(GP(0x3000), 0, 0x110);
    memcpy(GP(0x3000), multi, sizeof multi);
    snx_le32_array(GP(0x3000), 4);
    CHECK(nn_codec_detail_IHardwareOpusDecoderManager_GetWorkBufferSizeForMultiStream(NULL, buffer(0x3000, 0x110),
                                                                                     &size) == 0 &&
          size == 0x25750);
    /* ExEx (9) decodes on the DSP: no decoder state. One game's sound init takes these two sizes, and fails with the
       state in them */
    u32 exex[2][6] = {{48000, 2, 2, 0, 0, 0}, {48000, 2, 1, 1, 0, 0}};
    for (int i = 0; i < 2; i++) {
        memset(GP(0x3000), 0, 0x118);
        memcpy(GP(0x3000), exex[i], sizeof exex[i]);
        snx_le32_array(GP(0x3000), 6);
        CHECK(nn_codec_detail_IHardwareOpusDecoderManager_GetWorkBufferSizeForMultiStreamExEx(
                  NULL, buffer(0x3000, 0x118), &size) == 0 &&
              size == (i ? 0x1500u : 0x1ac0u));
    }
    nn_codec_detail_HardwareOpusDecoderParameterInternalEx large = {48000, 2, 1, 0}; /* frames of 120 ms */
    CHECK(nn_codec_detail_IHardwareOpusDecoderManager_GetWorkBufferSizeExEx(NULL, &large, &size) == 0 &&
          size == 0x3300);
    CHECK(nn_codec_detail_IHardwareOpusDecoderManager_GetWorkBufferSizeForMultiStream(NULL, buffer(0x3000, 0x10),
                                                                                     &size) == RC(111, 385));
    /* Ex (7) reads the flags at 0x10: frames of 120 ms */
    u32 ex[6] = {48000, 2, 1, 1, 1, 0};
    memset(GP(0x3000), 0, 0x118);
    memcpy(GP(0x3000), ex, sizeof ex);
    snx_le32_array(GP(0x3000), 6);
    CHECK(nn_codec_detail_IHardwareOpusDecoderManager_GetWorkBufferSizeForMultiStreamEx(NULL, buffer(0x3000, 0x118),
                                                                                       &size) == 0 &&
          size == 0x15394);
    /* more than the channels in streams: refused */
    u32 wide[4] = {48000, 2, 2, 1};
    memcpy(GP(0x3000), wide, sizeof wide);
    snx_le32_array(GP(0x3000), 4);
    CHECK(nn_codec_detail_IHardwareOpusDecoderManager_GetWorkBufferSizeForMultiStream(NULL, buffer(0x3000, 0x110),
                                                                                     &size) != 0);
    /* 24 kHz: half the frame */
    nn_codec_detail_HardwareOpusDecoderParameterInternal mono24 = {24000, 1};
    CHECK(nn_codec_detail_IHardwareOpusDecoderManager_GetWorkBufferSize(NULL, &mono24, &size) == 0 && size == 0x9f24);
    unsigned objects = test_objects;
    SvcObj* decoder;
    CHECK(open_opus(manager, 48000, 3, &decoder) == RC(111, 385));
    CHECK(test_objects == objects);
#ifndef SNX_NO_H264
    CHECK(open_opus(manager, 48000, 2, &decoder) == 0);
    CHECK(((HwOpus*)decoder->data)->channels == 2 && ((HwOpus*)decoder->data)->rate == 48000);
    svcobj_unref(decoder);
    CHECK(open_opus(manager, 24000, 1, &decoder) == 0 && ((HwOpus*)decoder->data)->step == 2);
    svcobj_unref(decoder);
    CHECK(open_opus(manager, 44100, 2, &decoder) == RC(111, 385)); /* not a rate Opus decodes at */
    /* 24 live decoders, from any session; the 25th is refused as out of decoders until one closes */
    SvcObj* other = new_hwopus();
    SvcObj* live[24];
    for (u32 i = 0; i < 24; i++) CHECK(open_opus(i & 1 ? other : manager, 48000, 2, &live[i]) == 0);
    unsigned held = test_objects;
    CHECK(open_opus(manager, 48000, 2, &decoder) == RC(111, 385));
    CHECK(open_opus(other, 48000, 1, &decoder) == RC(111, 385));
    CHECK(test_objects == held);
    svcobj_unref(live[5]);
    CHECK(open_opus(manager, 48000, 2, &live[5]) == 0);
    CHECK(open_opus(manager, 48000, 2, &decoder) == RC(111, 385));
    for (u32 i = 0; i < 24; i++) svcobj_unref(live[i]);
    CHECK(open_opus(other, 48000, 2, &decoder) == 0);
    svcobj_unref(decoder);
    /* the decoders a save state holds count once it is loaded: 24 saved with their host parts nil, as a save writes
       them, then loaded over the running ones as state.c does (the host's parts let go of, then made again) */
    static HleAudio saved;
    for (u32 i = 0; i < 24; i++) CHECK(open_opus(i & 1 ? other : manager, 48000, 2, &live[i]) == 0);
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
    CHECK(((HwOpus*)live[7]->data)->dec != NULL); /* made again */
    CHECK(open_opus(manager, 48000, 2, &decoder) == RC(111, 385));
    for (u32 i = 0; i < 24; i++) svcobj_unref(live[i]);
    CHECK(open_opus(manager, 48000, 2, &decoder) == 0);
    svcobj_unref(decoder);
    svcobj_unref(other);
    test_opus_decode();
#else
    CHECK(open_opus(manager, 48000, 2, &decoder) == RC(111, 385)); /* this build has no Opus decoder */
#endif
    CHECK(test_objects == objects);
    svcobj_unref(manager);
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
    test_hwopus();
    CHECK(test_objects == 0 && test_events == 0);
    free(g_mem);
    printf("audio: %u checks passed\n", test_checks);
    return 0;
}
