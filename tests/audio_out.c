/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Exercise the production audout IPC commands with a deterministic clock and a copying PCM sink. */
#include "nx/hle/services/services_int.h"
#include <stdarg.h>

#include "nx/hle/services/audio.c"
#include "nx/hle/hle.c"

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
void logf_(int level, const char* format, ...) {
    (void)level;
    (void)format;
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
/* audren.c's side, recorded: what RequestUpdate handed it and what it answers */
static struct {
    SvcObj* renderer;
    IpcBuf in, out;
    unsigned updates, opens;
    int result, refuse;
    u32 sample_rate;
} test_audren;
int audren_update(SvcObj* o, IpcBuf in, IpcBuf out) {
    CHECK(o == test_audren.renderer);
    test_audren.in = in;
    test_audren.out = out;
    test_audren.updates++;
    return test_audren.result;
}
int audren_open(SvcObj* o, const u8* params) {
    nn_audio_detail_AudioRendererParameterInternal p;
    memcpy(&p, params, sizeof p);
    test_audren.renderer = o;
    test_audren.sample_rate = p.sample_rate;
    test_audren.opens++;
    return !test_audren.refuse;
}
void audren_tick(void) {}
static float test_renderer_gain = 1.0f;
void audren_volume(float gain) { test_renderer_gain = gain; }
KEvent* kevent_new(const char* name) {
    KEvent* e = calloc(1, sizeof *e);
    CHECK(e != NULL);
    e->o.name = name;
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
KEvent* owned_event(const char* name) {
    KEvent* e = kevent_new(name);
    kobj_ref(&e->o);
    return e;
}
u32 ev_handle(KEvent* e) {
    CHECK(e != NULL);
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
static const void* le64(u64 v) {
    u8* p = raw_slot();
    snx_st_le64(p, v);
    return p;
}
static const void* lef(float v) {
    u32 bits;
    memcpy(&bits, &v, 4);
    return le32s(&bits, 1);
}
/* an AudioOutParameter as the message holds it */
static void put_out_parameter(u8* raw, const nn_audio_AudioOutParameter* p) {
    snx_st_le32(raw, p->sample_rate);
    snx_st_le16(raw + 4, p->channel_count);
    snx_st_le16(raw + 6, p->reserved);
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
static float reply_f(IpcCtx* c, u32 off) {
    u32 bits = reply32(c, off);
    float v;
    memcpy(&v, &bits, 4);
    return v;
}
static u64 query64(SvcObj* o, u32 cmd) {
    IpcCtx c = request(cmd, NULL, 0);
    CHECK(o->dispatch(o, &c) == 0 && c.out_size == 8);
    return snx_ld_le64(c.out);
}
static void command(SvcObj* o, u32 cmd) {
    IpcCtx c = request(cmd, NULL, 0);
    CHECK(o->dispatch(o, &c) == 0);
}
/* an AutoSelect buffer is sent in both of its lists, the unused one empty: ins in A and X, outs in B and C. This
   fills the lists up to ins and outs with empty descriptors, for the test to set the ones it uses */
static void auto_descriptors(IpcCtx* c, int ins, int outs) {
    c->na = c->nx = ins;
    c->nb = c->nc = outs;
}
static SvcObj* open_output(SvcObj* manager) {
    u32 args[4] = {48000, 2};
    IpcCtx c = request(3, le32s(args, 4), sizeof args);
    auto_descriptors(&c, 1, 1); /* the name in and the name out, both empty */
    c.has_pid = 1;              /* the aruid */
    c.ncopy = 1;                /* the process */
    c.copy_h[0] = 0xFFFF8001u;
    CHECK(manager->dispatch(manager, &c) == 0 && c.nout_objs == 1);
    CHECK(reply32(&c, 0) == 48000 && reply32(&c, 4) == 2 && reply32(&c, 8) == 2 && reply32(&c, 12) == 1);
    return c.out_objs[0];
}
static int append(SvcObj* o, u32 cmd, u64 tag, u64 sample_addr, u64 capacity, u64 bytes, u64 offset) {
    u64 descriptor[5] = {0, sample_addr, capacity, bytes, offset};
    memcpy(GP(0x100), descriptor, sizeof descriptor);
    snx_le64_array(GP(0x100), 5);
    IpcCtx c = request(cmd, le64(tag), sizeof tag);
    c.na = 1;
    c.nx = cmd == 7; /* AppendAudioOutBufferAuto: an empty X beside it */
    c.a[0].addr = 0x100;
    c.a[0].size = sizeof descriptor;
    return o->dispatch(o, &c);
}
static u32 released(SvcObj* o, u32 cmd) {
    IpcCtx c = request(cmd, NULL, 0);
    c.nb = 1;
    c.nc = cmd == 8; /* GetReleasedAudioOutBuffersAuto: an empty C beside it */
    c.b[0].addr = 0x200;
    c.b[0].size = 64 * 8;
    memset(GP(0x200), 0xFF, 64 * 8);
    CHECK(o->dispatch(o, &c) == 0);
    return reply32(&c, 0);
}

static u32 query32(SvcObj* o, u32 cmd) {
    IpcCtx c = request(cmd, NULL, 0);
    CHECK(o->dispatch(o, &c) == 0 && c.out_size == 4);
    return reply32(&c, 0);
}
static IpcBuf buffer(u64 addr, u64 size) {
    IpcBuf b = {addr, size, 0};
    return b;
}
/* the guest bytes at addr, then a fill byte that must still be there afterwards */
static void sentinel(u64 addr, u64 size) { memset(GP(addr), 0xCC, size); }

/* GetAudioOutState, ContainsAudioOutBuffer, GetAudioOutBufferCount and RegisterBufferEvent, which the SDK's
   audio-out loop polls between appends */
static void test_audout_queries(SvcObj* manager) {
    SvcObj* o = open_output(manager);
    CHECK(query32(o, 0) == 1); /* stopped until Start */
    command(o, 1);
    CHECK(query32(o, 0) == 0);
    command(o, 2);
    CHECK(query32(o, 0) == 1);
    CHECK(query32(o, 9) == 0);
    CHECK(append(o, 3, 0x11, 0x400, 196, 192, 0) == 0); /* 48 frames: 1 ms at 48 kHz */
    CHECK(append(o, 3, 0x22, 0x400, 196, 192, 0) == 0);
    CHECK(query32(o, 9) == 2);
    u64 tag = 0x22;
    IpcCtx c = request(6, le64(tag), sizeof tag);
    CHECK(o->dispatch(o, &c) == 0 && c.out_size == 4 && reply32(&c, 0) == 1);
    tag = 0x33;
    c = request(6, le64(tag), sizeof tag);
    CHECK(o->dispatch(o, &c) == 0 && c.out_size == 4 && reply32(&c, 0) == 0);
    /* played: not counted any more, still contained until GetReleasedAudioOutBuffers takes it back */
    command(o, 1);
    test_now += 1500000;
    CHECK(query32(o, 9) == 1);
    tag = 0x11;
    c = request(6, le64(tag), sizeof tag);
    CHECK(o->dispatch(o, &c) == 0 && reply32(&c, 0) == 1);
    /* GetReleasedAudioOutBuffers without a buffer takes nothing: the tag waits for the next call */
    c = request(5, NULL, 0);
    c.nb = 1;
    c.b[0] = buffer(0, 64 * 8);
    CHECK(o->dispatch(o, &c) == 0 && c.out_size == 4 && reply32(&c, 0) == 0);
    CHECK(released(o, 5) == 1 && gr64(0x200) == 0x11);
    c = request(6, le64(tag), sizeof tag);
    CHECK(o->dispatch(o, &c) == 0 && reply32(&c, 0) == 0);
    /* Stop hands the one still queued back: no longer counted, contained until taken */
    command(o, 2);
    CHECK(query32(o, 0) == 1 && query32(o, 9) == 0);
    tag = 0x22;
    c = request(6, le64(tag), sizeof tag);
    CHECK(o->dispatch(o, &c) == 0 && reply32(&c, 0) == 1);
    /* RegisterBufferEvent: the IAudioOut's event, as a copy handle and no raw data */
    c = request(4, NULL, 0);
    CHECK(o->dispatch(o, &c) == 0 && c.nout_copy == 1 && c.out_copy[0] == 7 && c.out_size == 0 && !c.nout_move);
    CHECK(released(o, 5) == 1 && gr64(0x200) == 0x22);
    svcobj_unref(o);
}

/* OpenAudioOut (1): the parameters at 0, the aruid at 8, the name in A and the name the device has written to B */
static void test_open_audio_out(SvcObj* manager) {
    u8 raw[16] = {0};
    nn_audio_AudioOutParameter params = {44100, 6, 0};
    u64 aruid = 0x1234;
    put_out_parameter(raw, &params);
    snx_st_le64(raw + 8, aruid);
    memset(GP(0x1000), 0, 0x100);
    sentinel(0x1100, 0x120);
    IpcCtx c = request(1, raw, sizeof raw);
    c.na = 1;
    c.a[0] = buffer(0x1000, 0x100); /* an empty name: the default device */
    c.nb = 1;
    c.b[0] = buffer(0x1100, 0x100);
    c.has_pid = 1; /* the aruid */
    c.ncopy = 1;
    c.copy_h[0] = 0xFFFF8001u;
    CHECK(manager->dispatch(manager, &c) == 0 && c.nout_objs == 1 && c.out_size == 16);
    /* the game gets what it asked for: here 6 channels, 48 kHz s16, stopped */
    CHECK(reply32(&c, 0) == 48000 && reply32(&c, 4) == 6 && reply32(&c, 8) == 2 && reply32(&c, 12) == 1);
    CHECK(((AudOut*)c.out_objs[0]->data)->channels == 6);
    CHECK(((AudOut*)c.out_objs[0]->data)->output->channels == 6);
    CHECK(!strcmp((char*)GP(0x1100), "DeviceOut"));
    for (u32 i = 9; i < 0x100; i++) CHECK(GP(0x1100)[i] == 0);
    for (u32 i = 0x100; i < 0x120; i++) CHECK(GP(0x1100)[i] == 0xCC);
    svcobj_unref(c.out_objs[0]);

    /* ListAudioOuts (0): the one device, into an array in B */
    sentinel(0x1100, 0x120);
    c = request(0, NULL, 0);
    c.nb = 1;
    c.b[0] = buffer(0x1100, 0x100);
    CHECK(manager->dispatch(manager, &c) == 0 && c.out_size == 4 && reply32(&c, 0) == 1);
    CHECK(!strcmp((char*)GP(0x1100), "DeviceOut") && GP(0x1100)[0x100] == 0xCC);
}

/* the channels an out opens with: 1, 2, 4 or 6 as asked, two for anything else; an AudioOutBuffer of the SDK's first
   0x20 bytes (no data_offset), and one whose samples overrun its capacity, which isn't played on the host */
static void test_audout_layouts(SvcObj* manager) {
    static const u16 asked[5] = {1, 4, 3, 0, 6}, given[5] = {1, 4, 2, 2, 6};
    for (u32 i = 0; i < 5; i++) {
        nn_audio_AudioOutParameter params = {48000, asked[i], 0};
        u32 rate, channels, format, state;
        SvcObj* o = NULL;
        CHECK(nn_audio_detail_IAudioOutManager_OpenAudioOut(NULL, &params, 0, 1, buffer(0, 0), buffer(0, 0), &rate,
                                                            &channels, &format, &state, &o) == 0);
        CHECK(channels == given[i] && ((AudOut*)o->data)->channels == given[i]);
        CHECK(((AudOut*)o->data)->output->channels == given[i]);
        svcobj_unref(o);
    }
    SvcObj* o = open_output(manager);
    AudOut* q = o->data;
    PAudio* host = q->output;
    u64 old[4] = {0, 0x2000, 96, 96};
    memcpy(GP(0x100), old, sizeof old);
    snx_le64_array(GP(0x100), 4);
    CHECK(nn_audio_detail_IAudioOut_AppendAudioOutBuffer(o, 0x31, buffer(0x100, sizeof old)) == 0);
    CHECK(q->nqueued == 1 && q->frames[(q->first + q->nreleased) % AUDOUT_BUFFERS] == 24 && host->bytes == 96);
    CHECK(nn_audio_detail_IAudioOut_AppendAudioOutBuffer(o, 0x32, buffer(0x100, 0x18)) == 0); /* holds nothing */
    CHECK(q->nqueued == 2 && host->bytes == 96);
    unsigned before = host->queued;
    CHECK(append(o, 3, 0x33, 0x2000, 64, 96, 0) == 0); /* 96 bytes in a 64 byte buffer: timed, not played */
    CHECK(q->nqueued == 3 && host->queued == before);
    CHECK(append(o, 3, 0x34, 0x2000, 96, 64, 64) == 0); /* past the capacity from its offset */
    CHECK(q->nqueued == 4 && host->queued == before);
    svcobj_unref(o);
}

/* a buffer ending in part of a frame (a whole .wav file, its header and all): its whole frames played, the rest left
   out, as the DSP counts frames */
static void test_audout_partial_frame(SvcObj* manager) {
    nn_audio_AudioOutParameter six = {48000, 6, 0};
    u32 rate, channels, format, state;
    SvcObj* o = open_output(manager);
    AudOut* q = o->data;
    CHECK(append(o, 7, 0x41, 0x2000, 0x1000, 4 * 100 + 2, 0) == 0);
    CHECK(q->frames[(q->first + q->nreleased) % AUDOUT_BUFFERS] == 100 && q->output->bytes == 4 * 100);
    CHECK(append(o, 7, 0x42, 0x2000, 0x1000, 3, 0) == 0); /* under a frame: timed as nothing, nothing played */
    CHECK(q->nqueued == 2 && q->output->queued == 1);
    svcobj_unref(o);
    CHECK(nn_audio_detail_IAudioOutManager_OpenAudioOut(NULL, &six, 0, 1, buffer(0, 0), buffer(0, 0), &rate, &channels,
                                                        &format, &state, &o) == 0);
    q = o->data;
    CHECK(channels == 6 && append(o, 3, 0x43, 0x2000, 0x1000, 12 * 10 + 10, 0) == 0);
    CHECK(q->frames[(q->first + q->nreleased) % AUDOUT_BUFFERS] == 10 && q->output->bytes == 12 * 10);
    svcobj_unref(o);
}

/* the Auto commands' AutoSelect buffers, sent through the pointer buffer: X for an input, C for an output. A mapped
   descriptor without an address leaves the pointer one in charge, and one with an address wins */
static void test_auto_pointer_buffers(SvcObj* manager) {
    u8 raw[16] = {0};
    nn_audio_AudioOutParameter params = {48000, 2, 0};
    put_out_parameter(raw, &params);
    sentinel(0x1100, 0x120);
    IpcCtx c = request(3, raw, sizeof raw); /* OpenAudioOutAuto, the name back through C */
    auto_descriptors(&c, 1, 1);
    c.c[0] = buffer(0x1100, 0x100);
    c.has_pid = 1;
    c.ncopy = 1;
    c.copy_h[0] = 0xFFFF8001u;
    CHECK(manager->dispatch(manager, &c) == 0 && c.nout_objs == 1);
    CHECK(!strcmp((char*)GP(0x1100), "DeviceOut") && GP(0x1100)[0x100] == 0xCC);
    SvcObj* o = c.out_objs[0];
    PAudio* host = ((AudOut*)o->data)->output;

    sentinel(0x1200, 0x110);
    c = request(2, NULL, 0); /* ListAudioOutsAuto through C */
    auto_descriptors(&c, 0, 1);
    c.c[0] = buffer(0x1200, 0x100);
    CHECK(manager->dispatch(manager, &c) == 0 && reply32(&c, 0) == 1 && !strcmp((char*)GP(0x1200), "DeviceOut"));
    CHECK(GP(0x1200)[0x100] == 0xCC);

    /* AppendAudioOutBufferAuto with the AudioOutBuffer in X, and an empty A descriptor beside it */
    for (u32 i = 0; i < 96; i++) GP(0x2000)[i] = (u8)(0x80 + i);
    u64 descriptor[5] = {0, 0x2000, 96, 96, 0};
    memcpy(GP(0x1300), descriptor, sizeof descriptor);
    snx_le64_array(GP(0x1300), 5);
    u64 tag = 0x5A5A;
    c = request(7, le64(tag), sizeof tag);
    auto_descriptors(&c, 1, 0);
    c.x[0] = buffer(0x1300, sizeof descriptor);
    unsigned before = host->queued;
    CHECK(o->dispatch(o, &c) == 0);
    CHECK(host->queued == before + 1 && host->bytes == 96 && !memcmp(host->pcm, GP(0x2000), 96));
    /* a mapped descriptor with an address is the buffer, whatever X says */
    u64 other[5] = {0, 0x2000, 96, 48, 0};
    memcpy(GP(0x1340), other, sizeof other);
    snx_le64_array(GP(0x1340), 5);
    tag = 0xA5A5;
    c = request(7, le64(tag), sizeof tag);
    auto_descriptors(&c, 1, 0);
    c.a[0] = buffer(0x1340, sizeof other);
    c.x[0] = buffer(0x1300, sizeof descriptor);
    CHECK(o->dispatch(o, &c) == 0 && host->bytes == 96 + 48);
    command(o, 1);
    test_now += 1000000000;
    command(o, 2);
    /* GetReleasedAudioOutBuffersAuto with the tags array in C */
    memset(GP(0x1400), 0xFF, 4 * 8);
    c = request(8, NULL, 0);
    auto_descriptors(&c, 0, 1);
    c.c[0] = buffer(0x1400, 4 * 8);
    CHECK(o->dispatch(o, &c) == 0 && c.out_size == 4 && reply32(&c, 0) == 2);
    CHECK(gr64(0x1400) == 0x5A5A && gr64(0x1408) == 0xA5A5 && gr64(0x1410) == 0 && gr64(0x1418) == 0);
    svcobj_unref(o);
}

/* IAudioRenderer from OpenAudioRenderer (0): the parameters at 0, the work buffer's size at 0x38, the aruid at 0x40 */
static void test_renderer(void) {
    SvcObj* manager = new_audren();
    u8 raw[0x48] = {0};
    nn_audio_detail_AudioRendererParameterInternal params;
    memset(&params, 0, sizeof params);
    params.sample_rate = 32000;
    params.sample_count = 160;
    params.mix_buffer_count = 6;
    params.voice_count = 24;
    params.sink_count = 1;
    params.revision = 0x38564552; /* REV8: "REV8" in memory */
    memcpy(raw, &params, sizeof params);
    snx_le32_array(raw, 8);        /* the counts, then the flag bytes at 0x20 */
    snx_le32_array(raw + 0x24, 4); /* the splitters, the external context and the revision */
    u64 work = 0x20000;
    snx_st_le64(raw + 0x38, work);
    IpcCtx c = request(0, raw, sizeof raw);
    c.has_pid = 1;
    c.ncopy = 2;
    c.copy_h[0] = 0x100;
    c.copy_h[1] = 0xFFFF8001u;
    /* parameters audren.c can't make a renderer with fail the open, OperationFailed, with no object */
    unsigned objects = test_objects;
    test_audren.refuse = 1;
    int refused = manager->dispatch(manager, &c);
    CHECK(c.nout_objs == 1 && !c.out_objs[0]); /* the generated code hands ipc.c the object it didn't get */
    CHECK(ipc_result(&c, refused) == RC(153, 2) && test_objects == objects);
    test_audren.refuse = 0;
    c = request(0, raw, sizeof raw);
    c.has_pid = 1;
    c.ncopy = 2;
    c.copy_h[0] = 0x100;
    c.copy_h[1] = 0xFFFF8001u;
    CHECK(manager->dispatch(manager, &c) == 0 && c.nout_objs == 1 && c.out_size == 0);
    SvcObj* r = c.out_objs[0];
    CHECK(test_audren.opens == 2 && test_audren.renderer == r && test_audren.sample_rate == 32000);
    CHECK(query32(r, 0) == 32000 && query32(r, 1) == 160 && query32(r, 2) == 6);
    CHECK(query32(r, 3) == 1); /* stopped until Start */
    command(r, 5);
    CHECK(query32(r, 3) == 0);
    command(r, 6);
    CHECK(query32(r, 3) == 1);
    CHECK(query32(r, 9) == 100); /* the rendering time limit starts at 100% */
    u32 limit = 75;
    c = request(8, le32s(&limit, 1), sizeof limit);
    CHECK(r->dispatch(r, &c) == 0 && query32(r, 9) == 75);
    /* the voice drop parameter starts at 1 and reads back as set */
    float drop;
    u32 bits = query32(r, 13);
    memcpy(&drop, &bits, sizeof drop);
    CHECK(drop == 1.0f);
    drop = 0.5f;
    c = request(12, lef(drop), sizeof drop);
    CHECK(r->dispatch(r, &c) == 0);
    bits = query32(r, 13);
    memcpy(&drop, &bits, sizeof drop);
    CHECK(drop == 0.5f);
    /* RequestUpdate (4): the input in A, the output in B0 and the performance buffer in B1, which audren.c doesn't
       take. Its result is the command's */
    c = request(4, NULL, 0);
    c.na = 1;
    c.a[0] = buffer(0x3000, 0x400);
    c.nb = 2;
    c.b[0] = buffer(0x4000, 0x200);
    c.b[1] = buffer(0x5000, 0x100);
    CHECK(r->dispatch(r, &c) == 0 && c.out_size == 0 && test_audren.updates == 1);
    CHECK(test_audren.in.addr == 0x3000 && test_audren.in.size == 0x400);
    CHECK(test_audren.out.addr == 0x4000 && test_audren.out.size == 0x200);
    test_audren.result = RC(153, 41);
    CHECK(r->dispatch(r, &c) == RC(153, 41) && test_audren.updates == 2);
    test_audren.result = 0;
    /* RequestUpdateAuto (10): the same buffers through X0, C0 and C1 */
    c = request(10, NULL, 0);
    auto_descriptors(&c, 1, 2);
    c.x[0] = buffer(0x3100, 0x300);
    c.c[0] = buffer(0x4100, 0x180);
    c.c[1] = buffer(0x5100, 0x80);
    CHECK(r->dispatch(r, &c) == 0 && test_audren.updates == 3);
    CHECK(test_audren.in.addr == 0x3100 && test_audren.in.size == 0x300);
    CHECK(test_audren.out.addr == 0x4100 && test_audren.out.size == 0x180);
    /* QuerySystemEvent (7): the renderer's event, which the audio clock signals every 5 ms */
    audio_init();
    c = request(7, NULL, 0);
    CHECK(r->dispatch(r, &c) == 0 && c.nout_copy == 1 && c.out_copy[0] == 7);
    kobj_unref(&g_hle.audio.ev->o);
    g_hle.audio.ev = NULL;
    svcobj_unref(r);
    svcobj_unref(manager);
}

/* IAudioDevice from GetAudioDeviceService (2): the one output device's name */
static void test_device(void) {
    SvcObj* manager = new_audren();
    u64 aruid = 0;
    IpcCtx c = request(2, le64(aruid), sizeof aruid);
    CHECK(manager->dispatch(manager, &c) == 0 && c.nout_objs == 1);
    SvcObj* d = c.out_objs[0];
    /* its system, input and output events: three, each signaled from the start */
    AudDevice* dev = d->data;
    CHECK(dev->ev[0] != dev->ev[1] && dev->ev[1] != dev->ev[2] && dev->ev[0] != dev->ev[2]);
    CHECK(dev->ev[0]->signaled && dev->ev[1]->signaled && dev->ev[2]->signaled);
    /* ListAudioDeviceName (0): AudioDeviceName entries of 0x100 bytes in B */
    sentinel(0x1000, 0x220);
    c = request(0, NULL, 0);
    c.nb = 1;
    c.b[0] = buffer(0x1000, 0x200);
    CHECK(d->dispatch(d, &c) == 0 && c.out_size == 4 && reply32(&c, 0) == 1);
    CHECK(!strcmp((char*)GP(0x1000), "AudioTvOutput") && GP(0x1000)[0xFF] == 0 && GP(0x1100)[0] == 0xCC);
    /* an array too small for one entry lists none and is left alone */
    sentinel(0x1000, 0x100);
    c.b[0] = buffer(0x1000, 0x80);
    CHECK(d->dispatch(d, &c) == 0 && reply32(&c, 0) == 0 && GP(0x1000)[0] == 0xCC);
    /* ListAudioDeviceNameAuto (6) through C */
    sentinel(0x1000, 0x110);
    c = request(6, NULL, 0);
    auto_descriptors(&c, 0, 1);
    c.c[0] = buffer(0x1000, 0x100);
    CHECK(d->dispatch(d, &c) == 0 && reply32(&c, 0) == 1 && !strcmp((char*)GP(0x1000), "AudioTvOutput"));
    CHECK(GP(0x1000)[0x100] == 0xCC);
    /* GetActiveAudioDeviceName (3): the name, zero padded to the buffer */
    sentinel(0x1000, 0x110);
    c = request(3, NULL, 0);
    c.nb = 1;
    c.b[0] = buffer(0x1000, 0x100);
    CHECK(d->dispatch(d, &c) == 0 && c.out_size == 0 && !strcmp((char*)GP(0x1000), "AudioTvOutput"));
    for (u32 i = 13; i < 0x100; i++) CHECK(GP(0x1000)[i] == 0);
    CHECK(GP(0x1000)[0x100] == 0xCC);
    /* a buffer shorter than the name: cut to fit, still zero-terminated, nothing written past it */
    sentinel(0x1000, 0x20);
    c.b[0] = buffer(0x1000, 5);
    CHECK(d->dispatch(d, &c) == 0 && !memcmp(GP(0x1000), "Audi", 5));
    for (u32 i = 5; i < 0x20; i++) CHECK(GP(0x1000)[i] == 0xCC);
    sentinel(0x1000, 0x20);
    c.b[0] = buffer(0x1000, 1);
    CHECK(d->dispatch(d, &c) == 0 && GP(0x1000)[0] == 0 && GP(0x1001)[0] == 0xCC);
    /* GetActiveAudioDeviceNameAuto (10) through C, as short */
    sentinel(0x1000, 0x20);
    c = request(10, NULL, 0);
    auto_descriptors(&c, 0, 1);
    c.c[0] = buffer(0x1000, 8);
    CHECK(d->dispatch(d, &c) == 0 && !memcmp(GP(0x1000), "AudioTv", 8) && GP(0x1008)[0] == 0xCC);
    CHECK(query32(d, 5) == 2); /* GetActiveChannelCount */
    c = request(4, NULL, 0);   /* QueryAudioDeviceSystemEvent */
    CHECK(d->dispatch(d, &c) == 0 && c.nout_copy == 1 && c.out_copy[0] == 7);
    /* SetAudioDeviceOutputVolume (1) and GetAudioDeviceOutputVolume (2), the name in A: the device keeps the volume set
       on it, and another name has 1 and keeps nothing; the Auto ones (7, 8) through X */
    float half = 0.5f, quarter = 0.25f, got = 0.0f;
    memset(GP(0x1000), 0, 0x200);
    strcpy((char*)GP(0x1000), "AudioTvOutput");
    strcpy((char*)GP(0x1100), "AudioBuiltInSpeakerOutput");
    c = request(1, lef(half), sizeof half);
    c.na = 1;
    c.a[0] = buffer(0x1000, 0x100);
    CHECK(d->dispatch(d, &c) == 0 && c.out_size == 0);
    c = request(1, lef(quarter), sizeof quarter);
    c.na = 1;
    c.a[0] = buffer(0x1100, 0x100);
    CHECK(d->dispatch(d, &c) == 0);
    c = request(2, NULL, 0);
    c.na = 1;
    c.a[0] = buffer(0x1000, 0x100);
    CHECK(d->dispatch(d, &c) == 0 && c.out_size == 4 && (got = reply_f(&c, 0)) == 0.5f);
    c.a[0] = buffer(0x1100, 0x100);
    CHECK(d->dispatch(d, &c) == 0 && (got = reply_f(&c, 0)) == 1.0f);
    c = request(7, lef(quarter), sizeof quarter);
    auto_descriptors(&c, 1, 0);
    c.x[0] = buffer(0x1000, 0x100);
    CHECK(d->dispatch(d, &c) == 0);
    c = request(8, NULL, 0);
    auto_descriptors(&c, 1, 0);
    c.x[0] = buffer(0x1000, 0x100);
    CHECK(d->dispatch(d, &c) == 0 && (got = reply_f(&c, 0)) == 0.25f);
    svcobj_unref(d);
    svcobj_unref(manager);
}

/* the output device's volume on what plays: every audio out's host stream at its own volume times the device's, the
   ones opened later too, and the renderers' (audren.c) at the device's */
static void set_device_volume(SvcObj* d, float volume) {
    IpcCtx c = request(1, lef(volume), sizeof volume);
    c.na = 1;
    c.a[0] = buffer(0x1000, 0x100);
    CHECK(d->dispatch(d, &c) == 0);
}
static void test_device_gain(void) {
    SvcObj* renderers = new_audren();
    u64 aruid = 0;
    IpcCtx c = request(2, le64(aruid), sizeof aruid);
    CHECK(renderers->dispatch(renderers, &c) == 0 && c.nout_objs == 1);
    SvcObj* d = c.out_objs[0];
    memset(GP(0x1000), 0, 0x100);
    strcpy((char*)GP(0x1000), "AudioTvOutput");
    set_device_volume(d, 1.0f);
    SvcObj* manager = new_audout();
    SvcObj* first = open_output(manager);
    PAudio* host = ((AudOut*)first->data)->output;
    float half = 0.5f;
    c = request(12, lef(half), sizeof half); /* SetAudioOutVolume */
    CHECK(first->dispatch(first, &c) == 0 && host->volume == 0.5f);
    set_device_volume(d, 0.5f);
    CHECK(host->volume == 0.25f && test_renderer_gain == 0.5f);
    float full = 1.0f;
    c = request(12, lef(full), sizeof full); /* the out's own volume, under the device's */
    CHECK(first->dispatch(first, &c) == 0 && host->volume == 0.5f);
    c = request(12, lef(half), sizeof half);
    CHECK(first->dispatch(first, &c) == 0 && host->volume == 0.25f);
    SvcObj* second = open_output(manager);
    PAudio* later = ((AudOut*)second->data)->output;
    CHECK(later->volume == 0.5f);
    set_device_volume(d, 1.0f);
    CHECK(host->volume == 0.5f && later->volume == 1.0f && test_renderer_gain == 1.0f);
    svcobj_unref(second);
    svcobj_unref(first);
    svcobj_unref(manager);
    svcobj_unref(d);
    svcobj_unref(renderers);
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
    CHECK(manager->dispatch(manager, &c) == 0 && c.out_size == 4 && reply32(&c, 0) == 0x10000);
    /* multistream: 32 KiB, and 32 KiB a stream; parameters too short to read are one stream's */
    u32 multi[4] = {48000, 6, 4, 2}, size = 0;
    memset(GP(0x3000), 0, 0x110);
    memcpy(GP(0x3000), multi, sizeof multi);
    snx_le32_array(GP(0x3000), 4);
    CHECK(nn_codec_detail_IHardwareOpusDecoderManager_GetWorkBufferSizeForMultiStream(NULL, buffer(0x3000, 0x110),
                                                                                     &size) == 0 &&
          size == 0x28000);
    CHECK(nn_codec_detail_IHardwareOpusDecoderManager_GetWorkBufferSizeForMultiStream(NULL, buffer(0x3000, 0x10),
                                                                                     &size) == 0 &&
          size == 0x10000);
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
    svcobj_unref(other);
    test_opus_decode();
#else
    CHECK(open_opus(manager, 48000, 2, &decoder) == RC(111, 385)); /* this build has no Opus decoder */
#endif
    CHECK(test_objects == objects);
    svcobj_unref(manager);
}

int kobj_never_signaled(KObj* o) {
    (void)o;
    return 0;
}

int main(void) {
    hle_init();
    g_mem = calloc(1, 0x10000);
    CHECK(g_mem != NULL);
    SvcObj* manager = new_audout();
    SvcObj* o = open_output(manager);
    AudOut* q = o->data;
    PAudio* host = q->output;
    CHECK(host && !host->started);
    for (u32 i = 0; i < 196; i++) GP(0x400)[i] = (u8)(i * 7);
    u8 expected[192];
    memcpy(expected, GP(0x404), sizeof expected);
    CHECK(append(o, 7, 0xCAFE, 0x400, 196, 192, 4) == 0);
    CHECK(host->queued == 1 && host->bytes == 192 && !memcmp(host->pcm, expected, 192));
    memset(GP(0x400), 0, 196); /* The host owns its copy, even when the guest later reuses the memory. */
    CHECK(!memcmp(host->pcm, expected, 192));

    test_now = 1000000000;
    CHECK(released(o, 8) == 0 && gr64(0x200) == 0);
    command(o, 1);
    CHECK(host->started);
    test_now += 500000;
    CHECK(query64(o, 10) == 24);
    /* Stop hands back what wasn't played, as Flush does: the host's queue goes and the buffer event says so. What
       was played stays counted */
    q->ev->signaled = 0;
    command(o, 2);
    CHECK(!host->started && host->cleared == 1 && host->bytes == 0 && q->ev->signaled);
    u64 played = query64(o, 10);
    CHECK(played == 24); /* what was played stays counted: the count never goes back */
    test_now += 1000000000;
    CHECK(query64(o, 10) == played && released(o, 5) == 1 && gr64(0x200) == 0xCAFE && gr64(0x208) == 0);
    /* a buffer appended while stopped plays from the next Start */
    CHECK(append(o, 7, 0xCAFF, 0x400, 196, 192, 0) == 0);
    command(o, 1);
    test_now += 500000;
    CHECK(released(o, 8) == 0 && query64(o, 10) == played + 24);
    test_now += 500000;
    CHECK(released(o, 8) == 1 && gr64(0x200) == 0xCAFF && gr64(0x208) == 0);
    CHECK(query64(o, 10) == played + 48);

    float volume = 0.25f;
    IpcCtx c = request(12, lef(volume), sizeof volume);
    CHECK(o->dispatch(o, &c) == 0 && host->volume == volume);
    c = request(13, NULL, 0);
    CHECK(o->dispatch(o, &c) == 0 && c.out_size == 4 && reply_f(&c, 0) == volume);
    CHECK(append(o, 3, 0xBEEF, 0x400, 196, 192, 0) == 0);
    command(o, 11);
    CHECK(host->cleared == 2 && host->bytes == 0);
    CHECK(released(o, 5) == 1 && gr64(0x200) == 0xBEEF && gr64(0x208) == 0);
    CHECK(query64(o, 10) == played + 48); /* Flushing does not count the discarded samples as played. */

    command(o, 2);
    unsigned before = host->queued;
    CHECK(append(o, 7, 1, 0x7FF0, 192, 192, 0) == 0); /* spans an unreadable allocation */
    CHECK(host->queued == before);
    /* a stopped out keeps what was appended: Flush says it flushed nothing, and a Start of a started out is
       refused */
    {
        IpcCtx f = request(11, NULL, 0);
        CHECK(o->dispatch(o, &f) == 0 && f.out_size == 4 && reply32(&f, 0) == 0);
        CHECK(released(o, 8) == 0 && query32(o, 9) == 1);
        command(o, 1);
        f = request(1, NULL, 0);
        CHECK(o->dispatch(o, &f) == RC(153, 2));
        f = request(11, NULL, 0);
        CHECK(o->dispatch(o, &f) == 0 && f.out_size == 4 && reply32(&f, 0) == 1);
    }
    CHECK(released(o, 8) == 1);
    for (u32 i = 0; i < 64; i++) CHECK(append(o, 7, 100 + i, 0x400, 196, 192, 0) == 0);
    CHECK(append(o, 7, 999, 0x400, 196, 192, 0) == RC(153, 8));
    CHECK(host->queued == before + 64); /* A full ring must reject the host copy too. */
    command(o, 11);
    CHECK(released(o, 8) == 64 && gr64(0x200) == 100 && gr64(0x3F8) == 163);
    command(o, 2);
    svcobj_unref(o);
    CHECK(g_hle.audio.outs == NULL);

    test_no_device = 1;
    o = open_output(manager);
    CHECK(((AudOut*)o->data)->output == NULL);
    CHECK(append(o, 7, 77, 0x400, 196, 192, 0) == 0);
    command(o, 1);
    test_now += 1000000;
    CHECK(released(o, 8) == 1 && gr64(0x200) == 77 && query64(o, 10) == 48);
    svcobj_unref(o);
    test_no_device = 0;
    test_audout_queries(manager);
    test_open_audio_out(manager);
    test_audout_layouts(manager);
    test_audout_partial_frame(manager);
    test_auto_pointer_buffers(manager);
    svcobj_unref(manager);
    test_renderer();
    test_device();
    test_device_gain();
    test_hwopus();
    CHECK(test_objects == 0 && test_events == 0 && g_hle.audio.outs == NULL);
    free(g_mem);
    printf("audout: %u checks passed\n", test_checks);
    return 0;
}
