/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The native audio outs (src/nx/nn/audio.c), their handlers called as the hooks call them, on a stand-in CPU over a
   guest buffer, with a clock the test moves and a host output that keeps what it was given: what OpenAudioOut writes
   into the game's AudioOut, the buffers' play and release on the clock, Stop and Flush, the buffer event, the ring's
   bound, an out with no host device, and audio.hook's table */
#include "nx/hle/services/services_int.h"
#include <stdarg.h>

#include "nx/nn/audio.c"
#include "nx/hle/hle.c"
#include "hle/hook/hooks.hook.inc" /* audio.hook's table and adapters */
#include "kobj_ids.h"              /* the kernel objects the stubs make, as ids */

u8* g_mem;
u64 g_as_size = AS_MAX; /* the reservation's size, mem.c's, which this test does not link */
static u64 test_now;
static unsigned test_checks, test_events, test_attached, test_handles;
static int test_no_device, test_no_attach;
struct PAudio {
    u8 pcm[32768];
    u32 bytes;
    unsigned queued, cleared;
    int started;
    float volume;
    u32 channels;
};

static void test_fail(const char* expression, int line) {
    fprintf(stderr, "nn_audio.c:%d: check failed: %s\n", line, expression);
    exit(1);
}
#define CHECK(expr)                                                                                                    \
    do {                                                                                                               \
        test_checks++;                                                                                                 \
        if (!(expr)) test_fail(#expr, __LINE__);                                                                       \
    } while (0)

/* single-threaded: the timing and the ownership are what is checked, with no audio device */
void pm_init(PMutex* m) { (void)m; }
void pm_lock(PMutex* m) { (void)m; }
void pm_unlock(PMutex* m) { (void)m; }
u64 guest_ns(void) { return test_now; } /* the outs play in guest time: no pause here */
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
/* audio.c's, which this test does not link */
int snx_audio_guest_ok(u64 addr, u64 size, u32 perm) {
    u64 end = addr + size;
    MemBlock b;
    if (end < addr || end > AS_SIZE) return 0;
    while (addr < end) {
        mem_query(addr, &b);
        if ((b.perm & perm) != perm || b.addr + b.size <= addr) return 0;
        addr = b.addr + b.size;
    }
    return 1;
}
static char test_last_log[256]; /* the last warning */
void logf_(int level, const char* format, ...) {
    va_list ap;
    if (level) return;
    va_start(ap, format);
    vsnprintf(test_last_log, sizeof test_last_log, format, ap);
    va_end(ap);
}
void kobj_ref(KObj* o) { o->refs++; }
void kobj_unref(KObj* o) {
    CHECK(o->refs > 0);
    if (--o->refs) return;
    o->type = 0; /* gone: kev no longer finds it */
    test_events--;
}
void kevent_signal(KEvent* e) { e->signaled = 1; }
u32 ev_handle(u32 ev) {
    CHECK(kev(ev) != NULL);
    kobj_ref(&kev(ev)->o); /* the handle's reference */
    test_handles++;
    return 7;
}
/* hook/codec.c's: the event the SDK's AttachReadableHandleToSystemEvent would attach, with the handle's reference;
   the guest address it was attached to is kept */
static u64 test_attached_to[4];
static KEvent test_event_pool[16];
static u32 test_event_next;
KEvent* hook_attach_event(CPU* c, u64 event, const char* name) {
    KEvent* e;
    (void)c, (void)name;
    if (test_no_attach) return NULL;
    CHECK(test_event_next < 16);
    e = &test_event_pool[test_event_next++];
    e->o.type = KO_EVENT;
    e->o.refs = 1; /* the handle's */
    test_events++;
    test_attached_to[test_attached++ % 4] = event;
    return e;
}
int hook_attach_handle(CPU* c, u64 event, u32 handle) {
    (void)c;
    CHECK(handle == 7);
    test_attached_to[test_attached++ % 4] = event;
    return 1;
}

/* audren.c's: the device's volume on the renderers */
static float test_renderer_gain = 1.0f;
void audren_volume(float gain) { test_renderer_gain = gain; }
/* the SDK's DestroySystemEvent, which the device's release runs in the game: here the call is kept */
#define DESTROY 0x9000u
static int test_no_destroy;
static u64 test_destroyed;
u64 kernel_find_symbol_exact(const char* name) {
    return !test_no_destroy && !strcmp(name, "_ZN2nn2os18DestroySystemEventEPNS0_15SystemEventTypeE") ? DESTROY : 0;
}
void hook_guest_call(CPU* c, u64 fn, u64 a0, u64 a1, u64 a2, u64 a3) {
    (void)c, (void)a1, (void)a2, (void)a3;
    CHECK(fn == DESTROY);
    test_destroyed = a0;
}

static CPU cpu;
static const HookLayout_nn_audio_AudioOut L = {0x110, {0x0}, {0x4}, {0x8}, {0xc}, {0x10}};
#define T_OUT   0x1000u /* the game's AudioOut */
#define T_OUT2  0x1200u
#define PARAMS  0x1400u /* an AudioOutParameter */
#define EVENT   0x1500u /* the game's SystemEvent */
#define EVENT2  0x1540u
#define NAME    0x1600u
#define BUFFERS 0x3000u /* the game's AudioOutBuffers, 0x28 bytes each */
#define SAMPLES 0x400u

static AudOut* out_at(u64 guest) {
    for (u32 i = 0; i < AUDOUT_MAX; i++)
        if (g_hle.audio.outs_pool[i].guest == guest) return &g_hle.audio.outs_pool[i];
    return NULL;
}
static void parameter(u32 rate, u16 channels) {
    snx_st_le32(GP(PARAMS), rate);
    snx_st_le16(GP(PARAMS + 4), channels);
    snx_st_le16(GP(PARAMS + 6), 0);
}
/* the default out, stereo: what the game's AudioOut reads afterwards */
static AudOut* open_output(u64 out) {
    parameter(48000, 2);
    memset(GP(out), 0xCC, 0x120);
    CHECK(nnaudio_open_default_out(&cpu, &L, out, PARAMS) == 0);
    CHECK(snx_ld_le32(GP(out)) == 48000 && snx_ld_le32(GP(out + 4)) == 2 && snx_ld_le32(GP(out + 8)) == 2);
    CHECK(snx_ld_le32(GP(out + 12)) == 1); /* stopped */
    CHECK(out_at(out) != NULL);
    return out_at(out);
}
/* the AudioOutBuffer n of the game's, filled as SetAudioOutBufferInfo fills it: its address */
static u64 buffer(u32 n, u64 sample_addr, u64 capacity, u64 bytes, u64 offset) {
    u64 at = BUFFERS + 0x28 * n;
    CHECK(n < 100);
    snx_st_le64(GP(at), 0);
    snx_st_le64(GP(at + 8), sample_addr);
    snx_st_le64(GP(at + 16), capacity);
    snx_st_le64(GP(at + 24), bytes);
    snx_st_le64(GP(at + 32), offset);
    return at;
}
static int append(u64 out, u32 n, u64 sample_addr, u64 capacity, u64 bytes, u64 offset) {
    return nnaudio_append_out_buffer(&cpu, out, buffer(n, sample_addr, capacity, bytes, offset));
}
#define B(n) ((u64)(BUFFERS + 0x28 * (n)))
/* every released buffer taken back, oldest first, into taken: how many */
static u64 taken[80];
static u32 released(u64 out) {
    u32 n = 0;
    u64 b;
    memset(taken, 0, sizeof taken);
    while ((b = nnaudio_get_released_out_buffer(&cpu, out)) != 0) {
        CHECK(n < 80);
        taken[n++] = b;
    }
    return n;
}

/* GetAudioOutState, ContainsAudioOutBuffer, GetAudioOutBufferCount and RegisterBufferEvent, which the SDK's
   audio-out loop polls between appends */
static void test_queries(void) {
    AudOut* q = open_output(T_OUT);
    unsigned attached = test_attached;
    CHECK(nnaudio_get_out_state(&cpu, &L, T_OUT) == 1); /* stopped until Start */
    CHECK(nnaudio_start_out(&cpu, T_OUT) == 0);
    CHECK(nnaudio_get_out_state(&cpu, &L, T_OUT) == 0);
    nnaudio_stop_out(&cpu, T_OUT);
    CHECK(nnaudio_get_out_state(&cpu, &L, T_OUT) == 1);
    CHECK(nnaudio_get_out_buffer_count(&cpu, T_OUT) == 0);
    CHECK(append(T_OUT, 0x11, SAMPLES, 196, 192, 0) == 1); /* 48 frames: 1 ms at 48 kHz */
    CHECK(append(T_OUT, 0x22, SAMPLES, 196, 192, 0) == 1);
    CHECK(nnaudio_get_out_buffer_count(&cpu, T_OUT) == 2);
    CHECK(nnaudio_contains_out_buffer(&cpu, T_OUT, B(0x22)) == 1);
    CHECK(nnaudio_contains_out_buffer(&cpu, T_OUT, B(0x33)) == 0);
    /* played: not counted any more, still contained until GetReleasedAudioOutBuffer takes it back */
    CHECK(nnaudio_start_out(&cpu, T_OUT) == 0);
    test_now += 1500000;
    CHECK(nnaudio_get_out_buffer_count(&cpu, T_OUT) == 1);
    CHECK(nnaudio_contains_out_buffer(&cpu, T_OUT, B(0x11)) == 1);
    CHECK(released(T_OUT) == 1 && taken[0] == B(0x11));
    CHECK(nnaudio_contains_out_buffer(&cpu, T_OUT, B(0x11)) == 0);
    /* Stop hands the one still queued back: no longer counted, contained until taken */
    nnaudio_stop_out(&cpu, T_OUT);
    CHECK(nnaudio_get_out_state(&cpu, &L, T_OUT) == 1 && nnaudio_get_out_buffer_count(&cpu, T_OUT) == 0);
    CHECK(nnaudio_contains_out_buffer(&cpu, T_OUT, B(0x22)) == 1);
    /* RegisterBufferEvent: a new event attached to the game's SystemEvent, the out's own reference beside the
       handle's; a second SystemEvent gets a handle to the same event */
    CHECK(q->ev == 0);
    nnaudio_register_buffer_event(&cpu, T_OUT, EVENT);
    CHECK(q->ev && test_attached == attached + 1 && test_attached_to[attached % 4] == EVENT);
    CHECK(kev(q->ev)->o.refs == 2);
    u32 ev = q->ev;
    unsigned handles = test_handles;
    nnaudio_register_buffer_event(&cpu, T_OUT, EVENT2);
    CHECK(q->ev == ev && test_handles == handles + 1 && test_attached_to[(attached + 1) % 4] == EVENT2);
    CHECK(kev(ev)->o.refs == 3);
    CHECK(released(T_OUT) == 1 && taken[0] == B(0x22));
    /* CloseAudioOut marks the game's struct stopped, frees the out and drops its reference on the event */
    snx_st_le32(GP(T_OUT + 12), 0x55);
    nnaudio_close_out(&cpu, &L, T_OUT);
    CHECK(out_at(T_OUT) == NULL && snx_ld_le32(GP(T_OUT + 12)) == 1 && kev(ev)->o.refs == 2);
    /* an AudioOut that isn't open: its state is what the struct holds, as the SDK reads it, and the rest answers as
       an empty, stopped out, each call named in the log */
    snx_st_le32(GP(T_OUT + 12), 0);
    CHECK(nnaudio_get_out_state(&cpu, &L, T_OUT) == 0);
    test_last_log[0] = 0;
    CHECK(nnaudio_start_out(&cpu, T_OUT) == RC(153, 2));
    CHECK(strstr(test_last_log, "StartAudioOut") && strstr(test_last_log, "0x1000"));
    CHECK(append(T_OUT, 1, SAMPLES, 196, 192, 0) == 0 && nnaudio_get_released_out_buffer(&cpu, T_OUT) == 0);
    CHECK(nnaudio_get_out_buffer_count(&cpu, T_OUT) == 0 && nnaudio_get_out_played_sample_count(&cpu, T_OUT) == 0);
    CHECK(nnaudio_flush_out_buffers(&cpu, T_OUT) == 0 && nnaudio_get_out_volume(&cpu, T_OUT) == 1.0f);
}

/* OpenAudioOut: the name asked for, the game's AudioOut filled, the SDK's results for an out opened twice and for
   one too many; ListAudioOuts */
static void test_open(void) {
    parameter(44100, 6);
    memset(GP(NAME), 0, 0x100);
    memset(GP(T_OUT), 0xCC, 0x120);
    CHECK(nnaudio_open_out(&cpu, &L, T_OUT, NAME, PARAMS) == 0); /* an empty name: the default device */
    /* the game gets what it asked for: here 6 channels, 48 kHz s16, stopped */
    CHECK(snx_ld_le32(GP(T_OUT)) == 48000 && snx_ld_le32(GP(T_OUT + 4)) == 6 && snx_ld_le32(GP(T_OUT + 8)) == 2);
    CHECK(snx_ld_le32(GP(T_OUT + 12)) == 1);
    CHECK(out_at(T_OUT)->channels == 6 && out_at(T_OUT)->output->channels == 6);
    CHECK(!strcmp((char*)GP(T_OUT + 0x10), "DeviceOut"));
    for (u32 i = 9; i < 0x100; i++) CHECK(GP(T_OUT + 0x10)[i] == 0);
    for (u32 i = 0x110; i < 0x120; i++) CHECK(GP(T_OUT)[i] == 0xCC); /* nothing past the name */
    /* the same AudioOut again: the SDK's own result, and the out as it was */
    CHECK(nnaudio_open_out(&cpu, &L, T_OUT, NAME, PARAMS) == RC(153, 9) && out_at(T_OUT)->channels == 6);
    nnaudio_close_out(&cpu, &L, T_OUT);
    /* by its name, and a name that is no device's */
    strcpy((char*)GP(NAME), "DeviceOut");
    CHECK(nnaudio_open_out(&cpu, &L, T_OUT, NAME, PARAMS) == 0);
    nnaudio_close_out(&cpu, &L, T_OUT);
    strcpy((char*)GP(NAME), "Speaker");
    test_last_log[0] = 0;
    CHECK(nnaudio_open_out(&cpu, &L, T_OUT, NAME, PARAMS) == RC(153, 1) && out_at(T_OUT) == NULL);
    CHECK(strstr(test_last_log, "Speaker") && strstr(test_last_log, "DeviceOut"));
    /* with the game's SystemEvent: the buffer event attached at once */
    unsigned attached = test_attached;
    CHECK(nnaudio_open_default_out_event(&cpu, &L, T_OUT, EVENT, PARAMS) == 0);
    CHECK(out_at(T_OUT)->ev && test_attached == attached + 1 && test_attached_to[attached % 4] == EVENT);
    nnaudio_close_out(&cpu, &L, T_OUT);
    strcpy((char*)GP(NAME), "DeviceOut");
    CHECK(nnaudio_open_out_event(&cpu, &L, T_OUT, EVENT2, NAME, PARAMS) == 0);
    CHECK(out_at(T_OUT)->ev && test_attached_to[(attached + 1) % 4] == EVENT2);
    nnaudio_close_out(&cpu, &L, T_OUT);
    /* an SDK with no AttachReadableHandleToSystemEvent: the out opens with no event, and the log says what follows */
    test_no_attach = 1;
    test_last_log[0] = 0;
    CHECK(nnaudio_open_default_out_event(&cpu, &L, T_OUT, EVENT, PARAMS) == 0 && out_at(T_OUT)->ev == 0);
    CHECK(strstr(test_last_log, "AttachReadableHandleToSystemEvent") && strstr(test_last_log, "0x1000"));
    test_no_attach = 0;
    nnaudio_close_out(&cpu, &L, T_OUT);

    /* every out the runtime holds, then one more: the SDK's result for its own limit, with the limit in the log */
    for (u32 i = 0; i < AUDOUT_MAX; i++) CHECK(nnaudio_open_default_out(&cpu, &L, 0x4000 + 0x10 * i, PARAMS) == 0);
    test_last_log[0] = 0;
    CHECK(nnaudio_open_default_out(&cpu, &L, T_OUT, PARAMS) == RC(153, 5) && strstr(test_last_log, "32"));
    for (u32 i = 0; i < AUDOUT_MAX; i++) nnaudio_close_out(&cpu, &L, 0x4000 + 0x10 * i);

    /* ListAudioOuts: the one device, into an array of names; none into an array of none */
    memset(GP(NAME), 0xCC, 0x120);
    CHECK(nnaudio_list_outs(&cpu, NAME, 4) == 1);
    CHECK(!strcmp((char*)GP(NAME), "DeviceOut") && GP(NAME)[0xFF] == 0 && GP(NAME)[0x100] == 0xCC);
    CHECK(nnaudio_list_outs(&cpu, NAME, 0) == 0 && nnaudio_list_outs(&cpu, 0, 4) == 0);
}

/* the channels an out opens with: 1, 2, 4 or 6 as asked, two for anything else; and a buffer whose samples overrun
   its capacity, which isn't played on the host */
static void test_layouts(void) {
    static const u16 asked[5] = {1, 4, 3, 0, 6}, given[5] = {1, 4, 2, 2, 6};
    for (u32 i = 0; i < 5; i++) {
        parameter(48000, asked[i]);
        CHECK(nnaudio_open_default_out(&cpu, &L, T_OUT, PARAMS) == 0);
        CHECK(snx_ld_le32(GP(T_OUT + 4)) == given[i] && out_at(T_OUT)->channels == given[i]);
        CHECK(out_at(T_OUT)->output->channels == given[i]);
        nnaudio_close_out(&cpu, &L, T_OUT);
    }
    AudOut* q = open_output(T_OUT);
    PAudio* host = q->output;
    CHECK(append(T_OUT, 0x31, 0x2000, 96, 96, 0) == 1);
    CHECK(q->nqueued == 1 && q->frames[(q->first + q->nreleased) % AUDOUT_BUFFERS] == 24 && host->bytes == 96);
    CHECK(append(T_OUT, 0x32, 0x2000, 96, 0, 0) == 1); /* holds nothing */
    CHECK(q->nqueued == 2 && host->bytes == 96);
    unsigned before = host->queued;
    CHECK(append(T_OUT, 0x33, 0x2000, 64, 96, 0) == 1); /* 96 bytes in a 64 byte buffer: timed, not played */
    CHECK(q->nqueued == 3 && host->queued == before);
    CHECK(append(T_OUT, 0x34, 0x2000, 96, 64, 64) == 1); /* past the capacity from its offset */
    CHECK(q->nqueued == 4 && host->queued == before);
    /* an AudioOutBuffer the game can't have: not appended, and the log names its address */
    test_last_log[0] = 0;
    CHECK(nnaudio_append_out_buffer(&cpu, T_OUT, 0x7FF0) == 0 && q->nqueued == 4 && strstr(test_last_log, "0x7ff0"));
    CHECK(nnaudio_append_out_buffer(&cpu, T_OUT, 0) == 0 && q->nqueued == 4);
    nnaudio_close_out(&cpu, &L, T_OUT);
}

/* a data offset past the buffer's capacity (a game leaves the field unset, 0xCD in every byte) plays the buffer from
   its start, as Eden and yuzu play every buffer: they never read the field. The message names both offsets */
static void test_offset_past_capacity(void) {
    AudOut* q = open_output(T_OUT);
    PAudio* host = q->output;
    u8 expected[192];
    for (u32 i = 0; i < 196; i++) GP(SAMPLES)[i] = (u8)(i * 5 + 1);
    memcpy(expected, GP(SAMPLES), sizeof expected);
    test_last_log[0] = 0;
    CHECK(append(T_OUT, 0x51, SAMPLES, 196, 192, 0xCDCDCDCDCDCDCDCDull) == 1);
    CHECK(q->nqueued == 1 && host->queued == 1 && host->bytes == 192 && !memcmp(host->pcm, expected, 192));
    CHECK(strstr(test_last_log, "0xcdcdcdcdcdcdcdcd") && strstr(test_last_log, "capacity 196") &&
          strstr(test_last_log, "from offset 0"));
    CHECK(append(T_OUT, 0x52, SAMPLES, 196, 192, 197) == 1); /* one past the capacity */
    CHECK(host->queued == 2 && host->bytes == 384 && !memcmp(host->pcm + 192, expected, 192));
    nnaudio_close_out(&cpu, &L, T_OUT);
}

/* a buffer ending in part of a frame (a whole .wav file, its header and all): its whole frames played, the rest left
   out, as the DSP counts frames */
static void test_partial_frame(void) {
    AudOut* q = open_output(T_OUT);
    CHECK(append(T_OUT, 0x41, 0x2000, 0x1000, 4 * 100 + 2, 0) == 1);
    CHECK(q->frames[(q->first + q->nreleased) % AUDOUT_BUFFERS] == 100 && q->output->bytes == 4 * 100);
    CHECK(append(T_OUT, 0x42, 0x2000, 0x1000, 3, 0) == 1); /* under a frame: timed as nothing, nothing played */
    CHECK(q->nqueued == 2 && q->output->queued == 1);
    nnaudio_close_out(&cpu, &L, T_OUT);
    parameter(48000, 6);
    CHECK(nnaudio_open_default_out(&cpu, &L, T_OUT, PARAMS) == 0);
    q = out_at(T_OUT);
    CHECK(q->channels == 6 && append(T_OUT, 0x43, 0x2000, 0x1000, 12 * 10 + 10, 0) == 1);
    CHECK(q->frames[(q->first + q->nreleased) % AUDOUT_BUFFERS] == 10 && q->output->bytes == 12 * 10);
    nnaudio_close_out(&cpu, &L, T_OUT);
}

/* two outs at once, each found by its own AudioOut; the output device's volume on both; the clock's turn; and a
   save state's load, which opens the host outputs again */
static void test_two_outs(void) {
    AudOut* a = open_output(T_OUT);
    AudOut* b = open_output(T_OUT2);
    CHECK(a != b);
    nnaudio_register_buffer_event(&cpu, T_OUT, EVENT);
    nnaudio_register_buffer_event(&cpu, T_OUT2, EVENT2);
    CHECK(a->ev && b->ev && a->ev != b->ev);
    CHECK(append(T_OUT, 1, SAMPLES, 196, 192, 0) == 1 && append(T_OUT2, 2, SAMPLES, 196, 96, 0) == 1);
    CHECK(nnaudio_start_out(&cpu, T_OUT) == 0 && nnaudio_start_out(&cpu, T_OUT2) == 0);
    kev(a->ev)->signaled = kev(b->ev)->signaled = 0;
    test_now += 500000; /* 24 frames: the second out's buffer, half of the first's */
    nnaudio_out_tick(test_now);
    CHECK(!kev(a->ev)->signaled && kev(b->ev)->signaled);
    CHECK(nnaudio_get_released_out_buffer(&cpu, T_OUT) == 0 && nnaudio_get_released_out_buffer(&cpu, T_OUT2) == B(2));
    nnaudio_set_out_volume(&cpu, T_OUT, 0.5f);
    g_hle.audio.device_volume = 0.5f;
    nnaudio_out_device_volume();
    CHECK(a->output->volume == 0.25f && b->output->volume == 0.5f);
    /* the out's own volume, under the device's; and an out opened later starts at the device's */
    nnaudio_set_out_volume(&cpu, T_OUT, 1.0f);
    CHECK(a->output->volume == 0.5f);
    nnaudio_set_out_volume(&cpu, T_OUT, 0.5f);
    CHECK(a->output->volume == 0.25f);
    AudOut* later = open_output(0x1800);
    CHECK(later->output->volume == 0.5f);
    nnaudio_close_out(&cpu, &L, 0x1800);
    nnaudio_out_state_drop();
    CHECK(a->output == NULL && b->output == NULL);
    nnaudio_out_state_rebuild();
    CHECK(a->output && b->output && a->output->started && b->output->started && a->output->bytes == 0);
    CHECK(a->output->volume == 0.25f && b->output->volume == 0.5f);
    g_hle.audio.device_volume = 1.0f;
    nnaudio_close_out(&cpu, &L, T_OUT);
    nnaudio_close_out(&cpu, &L, T_OUT2);
}

/* the audio device: the one output's name and channels, its volume on the outs and the renderers, the auto-tune
   flag, and the events a game acquires */
static void test_device(void) {
    CPU* c = &cpu;
    /* ListAudioDeviceName and ListAudioOutputDeviceName: one AudioDeviceName of 0x100 bytes; none into no room */
    memset(GP(NAME), 0xCC, 0x220);
    CHECK(nnaudio_list_device_names(c, NAME, 2) == 1);
    CHECK(!strcmp((char*)GP(NAME), "AudioTvOutput") && GP(NAME)[0xFF] == 0 && GP(NAME + 0x100)[0] == 0xCC);
    memset(GP(NAME), 0xCC, 0x100);
    CHECK(nnaudio_list_device_names(c, NAME, 0) == 0 && GP(NAME)[0] == 0xCC);
    CHECK(nnaudio_list_device_names(c, 0, 2) == 0);
    /* GetActiveAudioDeviceName: the name, zero padded to the AudioDeviceName, nothing past it */
    memset(GP(NAME), 0xCC, 0x110);
    nnaudio_get_active_device_name(c, NAME);
    CHECK(!strcmp((char*)GP(NAME), "AudioTvOutput"));
    for (u32 i = 13; i < 0x100; i++) CHECK(GP(NAME)[i] == 0);
    CHECK(GP(NAME)[0x100] == 0xCC);
    CHECK(nnaudio_get_active_device_channel_count(c) == 2);
    /* the device keeps the volume set on it and plays the outs and the renderers at it; another name has 1 and keeps
       nothing */
    AudOut* q = open_output(T_OUT);
    memset(GP(NAME), 0, 0x200);
    strcpy((char*)GP(NAME), "AudioTvOutput");
    strcpy((char*)GP(NAME + 0x100), "AudioBuiltInSpeakerOutput");
    nnaudio_set_out_volume(c, T_OUT, 0.5f);
    nnaudio_set_device_output_volume(c, NAME, 0.5f);
    CHECK(g_hle.audio.device_volume == 0.5f && q->output->volume == 0.25f && test_renderer_gain == 0.5f);
    nnaudio_set_device_output_volume(c, NAME + 0x100, 0.25f);
    CHECK(g_hle.audio.device_volume == 0.5f && q->output->volume == 0.25f && test_renderer_gain == 0.5f);
    CHECK(nnaudio_get_device_output_volume(c, NAME) == 0.5f &&
          nnaudio_get_device_output_volume(c, NAME + 0x100) == 1.0f);
    CHECK(nnaudio_get_device_output_volume(c, 0) == 1.0f);
    nnaudio_set_device_output_volume(c, NAME, 1.0f);
    CHECK(g_hle.audio.device_volume == 1.0f && q->output->volume == 0.5f && test_renderer_gain == 1.0f);
    nnaudio_close_out(c, &L, T_OUT);
    /* the auto-tune mode reads back as set */
    CHECK(nnaudio_is_device_auto_tune(c) == 0);
    nnaudio_set_device_auto_tune(c, 1);
    CHECK(nnaudio_is_device_auto_tune(c) == 1);
    nnaudio_set_device_auto_tune(c, 0);
    CHECK(nnaudio_is_device_auto_tune(c) == 0);
    /* the output, input and switch events: a new event each, attached to the game's SystemEvent, which holds its only
       reference, signaled from the start. The Ounce's notifications are not signaled */
    void (*const acquire[4])(CPU*, u64) = {nnaudio_acquire_device_output_event, nnaudio_acquire_device_input_event,
                                           nnaudio_acquire_device_switch_event, nnaudio_acquire_device_notification};
    for (u32 i = 0; i < 4; i++) {
        unsigned attached = test_attached;
        u32 next = test_event_next;
        acquire[i](c, EVENT + 0x40 * i);
        CHECK(test_attached == attached + 1 && test_attached_to[attached % 4] == EVENT + 0x40 * i);
        CHECK(test_event_next == next + 1 && test_event_pool[next].o.refs == 1);
        CHECK(test_event_pool[next].signaled == (i < 3));
    }
    /* an SDK with no AttachReadableHandleToSystemEvent: the log names the SystemEvent */
    test_no_attach = 1;
    test_last_log[0] = 0;
    nnaudio_acquire_device_output_event(c, EVENT);
    CHECK(strstr(test_last_log, "AttachReadableHandleToSystemEvent") && strstr(test_last_log, "0x1500"));
    test_no_attach = 0;
    /* the release destroys the game's SystemEvent with the SDK's own function; without one, the log says so */
    test_destroyed = 0;
    nnaudio_release_device_notification(c, EVENT2);
    CHECK(test_destroyed == EVENT2);
    test_no_destroy = 1;
    test_destroyed = 0;
    test_last_log[0] = 0;
    nnaudio_release_device_notification(c, EVENT2);
    CHECK(test_destroyed == 0 && strstr(test_last_log, "DestroySystemEvent") && strstr(test_last_log, "0x1540"));
    test_no_destroy = 0;
}

/* audio.hook: every function is hooked for any SDK, by the handler that takes its arguments, and the adapters hand
   them over as the game's call left them */
static void test_table(void) {
    static const struct {
        const char* symbol;
        const char* handler;
    } hooks[] = {
        {"_ZN2nn5audio12OpenAudioOutEPNS0_8AudioOutEPKcRKNS0_17AudioOutParameterE", "nnaudio_open_out"},
        {"_ZN2nn5audio12OpenAudioOutEPNS0_8AudioOutEPNS_2os11SystemEventEPKcRKNS0_17AudioOutParameterE",
         "nnaudio_open_out_event"},
        {"_ZN2nn5audio19OpenDefaultAudioOutEPNS0_8AudioOutERKNS0_17AudioOutParameterE", "nnaudio_open_default_out"},
        {"_ZN2nn5audio19OpenDefaultAudioOutEPNS0_8AudioOutEPNS_2os11SystemEventERKNS0_17AudioOutParameterE",
         "nnaudio_open_default_out_event"},
        {"_ZN2nn5audio19RegisterBufferEventEPNS0_8AudioOutEPNS_2os11SystemEventE", "nnaudio_register_buffer_event"},
        {"_ZN2nn5audio13CloseAudioOutEPNS0_8AudioOutE", "nnaudio_close_out"},
        {"_ZN2nn5audio16GetAudioOutStateEPKNS0_8AudioOutE", "nnaudio_get_out_state"},
        {"_ZN2nn5audio13StartAudioOutEPNS0_8AudioOutE", "nnaudio_start_out"},
        {"_ZN2nn5audio12StopAudioOutEPNS0_8AudioOutE", "nnaudio_stop_out"},
        {"_ZN2nn5audio20AppendAudioOutBufferEPNS0_8AudioOutEPNS0_14AudioOutBufferE", "nnaudio_append_out_buffer"},
        {"_ZN2nn5audio25GetReleasedAudioOutBufferEPNS0_8AudioOutE", "nnaudio_get_released_out_buffer"},
        {"_ZN2nn5audio22ContainsAudioOutBufferEPKNS0_8AudioOutEPKNS0_14AudioOutBufferE", "nnaudio_contains_out_buffer"},
        {"_ZN2nn5audio22GetAudioOutBufferCountEPKNS0_8AudioOutE", "nnaudio_get_out_buffer_count"},
        {"_ZN2nn5audio28GetAudioOutPlayedSampleCountEPKNS0_8AudioOutE", "nnaudio_get_out_played_sample_count"},
        {"_ZN2nn5audio20FlushAudioOutBuffersEPNS0_8AudioOutE", "nnaudio_flush_out_buffers"},
        {"_ZN2nn5audio17SetAudioOutVolumeEPNS0_8AudioOutEf", "nnaudio_set_out_volume"},
        {"_ZN2nn5audio17GetAudioOutVolumeEPKNS0_8AudioOutE", "nnaudio_get_out_volume"},
        {"_ZN2nn5audio13ListAudioOutsEPNS0_12AudioOutInfoEi", "nnaudio_list_outs"},
        {"_ZN2nn5audio19ListAudioDeviceNameEPNS0_15AudioDeviceNameEi", "nnaudio_list_device_names"},
        {"_ZN2nn5audio25ListAudioOutputDeviceNameEPNS0_15AudioDeviceNameEi", "nnaudio_list_device_names"},
        {"_ZN2nn5audio24GetActiveAudioDeviceNameEPNS0_15AudioDeviceNameE", "nnaudio_get_active_device_name"},
        {"_ZN2nn5audio30GetActiveAudioOutputDeviceNameEPNS0_15AudioDeviceNameE", "nnaudio_get_active_device_name"},
        {"_ZN2nn5audio41GetActiveAudioDeviceChannelCountForOutputEv", "nnaudio_get_active_device_channel_count"},
        {"_ZN2nn5audio21GetActiveChannelCountEv", "nnaudio_get_active_device_channel_count"},
        {"_ZN2nn5audio26SetAudioDeviceOutputVolumeEPKNS0_15AudioDeviceNameEf", "nnaudio_set_device_output_volume"},
        {"_ZN2nn5audio26GetAudioDeviceOutputVolumeEPKNS0_15AudioDeviceNameE", "nnaudio_get_device_output_volume"},
        {"_ZN2nn5audio39AcquireAudioDeviceNotificationForOutputEPNS_2os11SystemEventE",
         "nnaudio_acquire_device_output_event"},
        {"_ZN2nn5audio38AcquireAudioDeviceNotificationForInputEPNS_2os11SystemEventE",
         "nnaudio_acquire_device_input_event"},
        {"_ZN2nn5audio36AcquireAudioDeviceSwitchNotificationEPNS_2os11SystemEventE",
         "nnaudio_acquire_device_switch_event"},
        {"_ZN2nn5audio40SetDeviceOutputVolumeAutoTuneModeEnabledEb", "nnaudio_set_device_auto_tune"},
        {"_ZN2nn5audio39IsDeviceOutputVolumeAutoTuneModeEnabledEv", "nnaudio_is_device_auto_tune"},
        {"_ZN2nn5audio36AcquireAudioOutputDeviceNotificationEPNS_2os11SystemEventE",
         "nnaudio_acquire_device_notification"},
        {"_ZN2nn5audio35AcquireAudioInputDeviceNotificationEPNS_2os11SystemEventE",
         "nnaudio_acquire_device_notification"},
        {"_ZN2nn5audio36ReleaseAudioOutputDeviceNotificationEPNS_2os11SystemEventE",
         "nnaudio_release_device_notification"},
        {"_ZN2nn5audio35ReleaseAudioInputDeviceNotificationEPNS_2os11SystemEventE",
         "nnaudio_release_device_notification"},
    };
    int k[sizeof hooks / sizeof hooks[0]], first;
    CHECK(hook_table.count == sizeof hooks / sizeof hooks[0]);
    for (unsigned i = 0; i < sizeof hooks / sizeof hooks[0]; i++) {
        u32 versions[HOOK_KEYS] = {0}; /* a module whose SDK version is unknown: only an unversioned hook holds */
        k[i] = hook_select(&hook_table, hooks[i].symbol, versions, &first);
        CHECK(k[i] >= 0 && hook_key[k[i]] == HOOK_ANY && !strcmp(hook_handler[k[i]], hooks[i].handler));
    }
    /* through the adapters, as a game's calls: open with an event and a name, append, start, play, take back, close */
    CPU* c = &cpu;
    parameter(48000, 2);
    strcpy((char*)GP(NAME), "DeviceOut");
    memset(GP(T_OUT), 0, 0x120);
#define CALL(i, a0, a1, a2, a3)                                                                                        \
    do {                                                                                                               \
        c->x[0] = (a0), c->x[1] = (a1), c->x[2] = (a2), c->x[3] = (a3), c->x[30] = 0x1234, c->pc = 0;                  \
        hook_fn[k[i]](c, 0);                                                                                           \
        CHECK(c->pc == 0x1234);                                                                                        \
    } while (0)
    CALL(1, T_OUT, EVENT, NAME, PARAMS);
    CHECK((u32)c->x[0] == 0 && out_at(T_OUT) && out_at(T_OUT)->ev && snx_ld_le32(GP(T_OUT)) == 48000);
    CALL(9, T_OUT, buffer(5, SAMPLES, 196, 192, 0), 0, 0);
    CHECK((c->x[0] & 0xFF) == 1);
    CALL(7, T_OUT, 0, 0, 0);
    CHECK((u32)c->x[0] == 0);
    CALL(6, T_OUT, 0, 0, 0);
    CHECK((u32)c->x[0] == 0); /* started */
    test_now += 1000000;
    CALL(13, T_OUT, 0, 0, 0);
    CHECK(c->x[0] == 48);
    CALL(10, T_OUT, 0, 0, 0);
    CHECK(c->x[0] == B(5));
    { /* the volume goes and comes in s0 */
        f32 half = 0.5f;
        u32 bits;
        memcpy(&bits, &half, 4);
        memset(&c->v[0], 0, sizeof c->v[0]);
        SNX_VSET_S(c->v[0], 0, bits);
        CALL(15, T_OUT, 0, 0, 0);
        CHECK(out_at(T_OUT)->volume == 0.5f);
        memset(&c->v[0], 0, sizeof c->v[0]);
        CALL(16, T_OUT, 0, 0, 0);
        CHECK(hook_f32(c, 0) == 0.5f);
    }
    CALL(5, T_OUT, 0, 0, 0);
    CHECK(out_at(T_OUT) == NULL);
    /* the device's, a name and a volume in s0, and a count back */
    memset(GP(NAME), 0, 0x100);
    strcpy((char*)GP(NAME), "AudioTvOutput");
    {
        f32 half = 0.5f;
        u32 bits;
        memcpy(&bits, &half, 4);
        memset(&c->v[0], 0, sizeof c->v[0]);
        SNX_VSET_S(c->v[0], 0, bits);
        CALL(24, NAME, 0, 0, 0);
        CHECK(g_hle.audio.device_volume == 0.5f);
        memset(&c->v[0], 0, sizeof c->v[0]);
        CALL(25, NAME, 0, 0, 0);
        CHECK(hook_f32(c, 0) == 0.5f);
        g_hle.audio.device_volume = 1.0f;
    }
    CALL(22, 0, 0, 0, 0);
    CHECK((u32)c->x[0] == 2);
    CALL(23, 0, 0, 0, 0); /* GetActiveChannelCount, its name up to SDK 10 */
    CHECK((u32)c->x[0] == 2);
    memset(GP(NAME), 0xCC, 0x100);
    CALL(19, NAME, 3, 0, 0);
    CHECK((u32)c->x[0] == 1 && !strcmp((char*)GP(NAME), "AudioTvOutput"));
#undef CALL
}

int main(void) {
    hle_init();
    g_mem = calloc(1, 0x10000);
    CHECK(g_mem != NULL);
    cpu.mem = g_mem;
    g_hle.audio.device_volume = 1.0f;
    AudOut* q = open_output(T_OUT);
    PAudio* host = q->output;
    CHECK(host && !host->started);
    for (u32 i = 0; i < 196; i++) GP(SAMPLES)[i] = (u8)(i * 7);
    u8 expected[192];
    memcpy(expected, GP(SAMPLES + 4), sizeof expected);
    CHECK(append(T_OUT, 0, SAMPLES, 196, 192, 4) == 1);
    CHECK(host->queued == 1 && host->bytes == 192 && !memcmp(host->pcm, expected, 192));
    memset(GP(SAMPLES), 0, 196); /* the host owns its copy, even when the guest later reuses the memory */
    CHECK(!memcmp(host->pcm, expected, 192));

    test_now = 1000000000;
    CHECK(released(T_OUT) == 0);
    CHECK(nnaudio_start_out(&cpu, T_OUT) == 0);
    CHECK(host->started);
    test_now += 500000;
    CHECK(nnaudio_get_out_played_sample_count(&cpu, T_OUT) == 24);
    /* Stop hands back what wasn't played, as Flush does: the host's queue goes and the buffer event says so. What
       was played stays counted */
    nnaudio_register_buffer_event(&cpu, T_OUT, EVENT);
    kev(q->ev)->signaled = 0;
    nnaudio_stop_out(&cpu, T_OUT);
    CHECK(!host->started && host->cleared == 1 && host->bytes == 0 && kev(q->ev)->signaled);
    i64 played = nnaudio_get_out_played_sample_count(&cpu, T_OUT);
    CHECK(played == 24); /* what was played stays counted: the count never goes back */
    test_now += 1000000000;
    CHECK(nnaudio_get_out_played_sample_count(&cpu, T_OUT) == played && released(T_OUT) == 1 && taken[0] == B(0));
    /* a buffer appended while stopped plays from the next Start */
    CHECK(append(T_OUT, 1, SAMPLES, 196, 192, 0) == 1);
    CHECK(nnaudio_start_out(&cpu, T_OUT) == 0);
    test_now += 500000;
    CHECK(released(T_OUT) == 0 && nnaudio_get_out_played_sample_count(&cpu, T_OUT) == played + 24);
    test_now += 500000;
    CHECK(released(T_OUT) == 1 && taken[0] == B(1));
    CHECK(nnaudio_get_out_played_sample_count(&cpu, T_OUT) == played + 48);

    nnaudio_set_out_volume(&cpu, T_OUT, 0.25f);
    CHECK(host->volume == 0.25f && nnaudio_get_out_volume(&cpu, T_OUT) == 0.25f);
    CHECK(append(T_OUT, 2, SAMPLES, 196, 192, 0) == 1);
    CHECK(nnaudio_flush_out_buffers(&cpu, T_OUT) == 1);
    CHECK(host->cleared == 2 && host->bytes == 0);
    CHECK(released(T_OUT) == 1 && taken[0] == B(2));
    /* flushing does not count the discarded samples as played */
    CHECK(nnaudio_get_out_played_sample_count(&cpu, T_OUT) == played + 48);

    nnaudio_stop_out(&cpu, T_OUT);
    unsigned before = host->queued;
    CHECK(append(T_OUT, 3, 0x7FF0, 192, 192, 0) == 1); /* spans an unreadable allocation */
    CHECK(host->queued == before);
    /* a stopped out keeps what was appended: Flush says it flushed nothing, and a Start of a started out is
       refused */
    CHECK(nnaudio_flush_out_buffers(&cpu, T_OUT) == 0);
    CHECK(released(T_OUT) == 0 && nnaudio_get_out_buffer_count(&cpu, T_OUT) == 1);
    CHECK(nnaudio_start_out(&cpu, T_OUT) == 0);
    CHECK(nnaudio_start_out(&cpu, T_OUT) == RC(153, 2));
    CHECK(nnaudio_flush_out_buffers(&cpu, T_OUT) == 1);
    CHECK(released(T_OUT) == 1);
    for (u32 i = 0; i < 64; i++) CHECK(append(T_OUT, 4 + i, SAMPLES, 196, 192, 0) == 1);
    CHECK(append(T_OUT, 70, SAMPLES, 196, 192, 0) ==
          0);                           /* the ring is full: false, as the SDK answers BufferRingFull */
    CHECK(host->queued == before + 64); /* a full ring refuses the host copy too */
    CHECK(nnaudio_flush_out_buffers(&cpu, T_OUT) == 1);
    CHECK(released(T_OUT) == 64 && taken[0] == B(4) && taken[63] == B(67));
    nnaudio_stop_out(&cpu, T_OUT);
    nnaudio_close_out(&cpu, &L, T_OUT);
    for (u32 i = 0; i < AUDOUT_MAX; i++) CHECK(g_hle.audio.outs_pool[i].guest == 0);

    test_no_device = 1;
    q = open_output(T_OUT);
    CHECK(q->output == NULL);
    CHECK(append(T_OUT, 0, SAMPLES, 196, 192, 0) == 1);
    CHECK(nnaudio_start_out(&cpu, T_OUT) == 0);
    test_now += 1000000;
    CHECK(released(T_OUT) == 1 && taken[0] == B(0) && nnaudio_get_out_played_sample_count(&cpu, T_OUT) == 48);
    nnaudio_close_out(&cpu, &L, T_OUT);
    test_no_device = 0;
    test_queries();
    test_open();
    test_layouts();
    test_offset_past_capacity();
    test_partial_frame();
    test_two_outs();
    test_device();
    test_table();
    for (u32 i = 0; i < AUDOUT_MAX; i++) CHECK(g_hle.audio.outs_pool[i].guest == 0);
    /* every out is closed, and no event went with one: each is still the game's, through its SystemEvent's handle */
    CHECK(test_event_next > 0 && test_events == test_event_next);
    for (u32 i = 0; i < test_event_next; i++) CHECK(test_event_pool[i].o.refs >= 1);
    printf("nn_audio: %u checks\n", test_checks);
    free(g_mem);
    return 0;
}
