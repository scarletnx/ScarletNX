/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* audren.c's host side with the DSP stubbed: the output device's volume (audren_volume, from audio.c) on every
   renderer's host stream, which opens at its first frame, and on one that opens after it; RequestUpdate's results;
   the log's line about a playing renderer's host queue */
#include "nx/hle/services/services_int.h"
#include "nx/hle/services/audren_dsp.h"
#include <stdarg.h>

#include "nx/hle/services/audren.c"
#include "nx/hle/hle.c"

u8* g_mem;
static unsigned test_checks;

static void test_fail(const char* expression, int line) {
    fprintf(stderr, "audren_host.c:%d: check failed: %s\n", line, expression);
    exit(1);
}
#define CHECK(expr)                                                                                                    \
    do {                                                                                                               \
        test_checks++;                                                                                                 \
        if (!(expr)) test_fail(#expr, __LINE__);                                                                       \
    } while (0)

void pm_lock(PMutex* m) { (void)m; }
void pm_unlock(PMutex* m) { (void)m; }
void pm_lock_announce(PMutex* m, const char* name) { (void)m, (void)name; }
/* the log's last line, and how many it took */
static char test_line[256];
static unsigned test_lines;
void logf_(int level, const char* format, ...) {
    va_list a;
    (void)level;
    va_start(a, format);
    vsnprintf(test_line, sizeof test_line, format, a);
    va_end(a);
    test_lines++;
}
struct PAudio {
    float volume;
    u32 queued;      /* the bytes it says it still holds */
    unsigned frames; /* the frames it was given */
};
PAudio* plat_audio_open(u32 rate, u32 channels) {
    PAudio* a = calloc(1, sizeof *a);
    (void)rate, (void)channels;
    CHECK(a != NULL);
    a->volume = 1.0f;
    return a;
}
void plat_audio_close(PAudio* a) { free(a); }
int plat_audio_queue(PAudio* a, const void* samples, u32 bytes) {
    (void)samples, (void)bytes;
    a->frames++;
    return 1;
}
void plat_audio_start(PAudio* a, int started) { (void)a, (void)started; }
u32 plat_audio_queued(PAudio* a) { return a->queued; }
void plat_audio_volume(PAudio* a, float volume) {
    if (a) a->volume = volume;
}
/* the DSP: a state with a rate and nothing else */
ARState* audren_state_new(const u8* params) {
    ARState* s = calloc(1, sizeof *s);
    (void)params;
    CHECK(s != NULL);
    s->rate = 48000;
    s->frame = 240;
    return s;
}
void audren_state_free(ARState* s) { free(s); }
static int test_update_result; /* what the DSP's update returns */
int audren_state_update(ARState* s, const u8* in, u64 in_size, u8* out, u64 out_size) {
    (void)s, (void)in, (void)in_size, (void)out, (void)out_size;
    return test_update_result;
}
int audren_revision(u32 rev) { return (int)(rev >> 24) - '0'; }
int audren_frame(ARState* s, i16* stereo) {
    (void)s, (void)stereo;
    return 1;
}

static PAudio* host_of(u32 id) { return audren_at(id)->out; }
/* started, and its first frame played: the host stream opens */
static void play(u32 id) {
    audren_at(id)->state = SNX_AUDREN_STARTED;
    audren_tick();
}

void pm_init(PMutex* m) { (void)m; }

/* an update (audren_update): InvalidUpdateInfo, 2153-0041, without an update's header, and the DSP's failures as
   the renderer's results, InvalidUpdateInfo and InsufficientBuffer (2153-0004), any other OperationFailed (2153-0002);
   InvalidHandle for a name that is no open renderer's */
static void test_update_results(u32 id) {
    u8 memory[0x100] = {0};
    g_mem = memory;
    CHECK(audren_update(id, 0, 0, 0x80, 0x80) == RC(153, 41) && audren_update(id, 0x40, 0x40, 0, 0) == RC(153, 41));
    CHECK(audren_update(id, 0x40, 0x3f, 0x80, 0x80) == RC(153, 41));
    test_update_result = -41;
    CHECK(audren_update(id, 0x40, 0x40, 0x80, 0x80) == RC(153, 41));
    test_update_result = -4;
    CHECK(audren_update(id, 0x40, 0x40, 0x80, 0x80) == RC(153, 4));
    test_update_result = -1;
    CHECK(audren_update(id, 0x40, 0x40, 0x80, 0x80) == RC(153, 2));
    test_update_result = 0;
    CHECK(audren_update(id, 0x40, 0x40, 0x80, 0x80) == 0);
    CHECK(audren_update(0, 0x40, 0x40, 0x80, 0x80) == RC(153, 1536));
    CHECK(audren_update(SNX_AUDREN_MAX + 1, 0x40, 0x40, 0x80, 0x80) == RC(153, 1536));
    g_mem = NULL;
}

/* frames of the clock that find id's host output holding ms of sound (48 kHz stereo s16: 192 bytes a millisecond) */
static void frames_at(u32 id, unsigned frames, u32 ms) {
    host_of(id)->queued = ms * 192;
    while (frames--) audren_tick();
}
/* The line about a playing renderer's host queue, once in 10 s of frames (2000 of 5 ms): the least, the most and
   the average the clock found, the frames that found it empty and those dropped above 100 ms, which the host is
   not given; then the next 10 s counted from zero. The frame that opens the output, before it plays, is not one */
static void test_queue_line(void) {
    static const u8 params[0x34];
    u32 id = audren_open(params, 0), again = 0;
    char line[256];
    CHECK(id != 0);
    play(id);
    unsigned lines = test_lines;
    CHECK(host_of(id)->frames == 1);
    frames_at(id, 3, 0);
    frames_at(id, 5, 120);
    CHECK(host_of(id)->frames == 4);
    frames_at(id, 1991, 36);
    CHECK(test_lines == lines);
    frames_at(id, 1, 36);
    CHECK(test_lines == lines + 1);
    snprintf(line, sizeof line,
             "audren %u: the host held 0 to 120 ms of sound over the last 10 s, 36 ms on average; 3 frames "
             "found it empty, and 5 found more than 100 ms and were dropped\n",
             id);
    CHECK(!strcmp(test_line, line));
    frames_at(id, 1000, 12);
    frames_at(id, 999, 49);
    CHECK(test_lines == lines + 1);
    frames_at(id, 1, 100); /* at the bound, not above it: played */
    CHECK(test_lines == lines + 2 && host_of(id)->frames == 1 + 1995 + 2000);
    snprintf(line, sizeof line,
             "audren %u: the host held 12 to 100 ms of sound over the last 10 s, 31 ms on average; 0 frames "
             "found it empty, and 0 found more than 100 ms and were dropped\n",
             id);
    CHECK(!strcmp(test_line, line));
    /* a renderer opened in a closed one's slot starts its own count */
    frames_at(id, 1999, 20);
    audren_close(id);
    for (int i = 0; i <= SNX_AUDREN_MAX && again != id; i++) {
        audren_close(again);
        again = audren_open(params, 0);
    }
    CHECK(again == id);
    play(id);
    frames_at(id, 1999, 7);
    CHECK(test_lines == lines + 2);
    frames_at(id, 1, 9);
    CHECK(test_lines == lines + 3);
    snprintf(line, sizeof line,
             "audren %u: the host held 7 to 9 ms of sound over the last 10 s, 7 ms on average; 0 frames "
             "found it empty, and 0 found more than 100 ms and were dropped\n",
             id);
    CHECK(!strcmp(test_line, line));
    audren_close(id);
}

int main(void) {
    hle_init();
    static const u8 params[0x34];
    static const u8 device[0x34] = {[0] = 0x80, [1] = 0xbb, [4] = 240, [8] = 6, [0x22] = 1};
    u32 first = audren_open(params, 0), second = audren_open(params, 0), later = audren_open(device, 1);
    CHECK(first && second && later && first != second && second != later && first != later);
    /* as OpenAudioRenderer leaves them: stopped, the limit at 100%, the voice drop parameter 1, and the parameters
       they read back */
    CHECK(audren_at(first)->state == SNX_AUDREN_STOPPED && audren_at(first)->limit == 100);
    CHECK(audren_at(first)->voice_drop == 1.0f && !audren_at(first)->manual);
    CHECK(audren_at(later)->sample_rate == 48000 && audren_at(later)->sample_count == 240);
    CHECK(audren_at(later)->mix_buffer_count == 6 && audren_at(later)->manual == 1);
    CHECK(audren_at(later)->rendering_device == 1 && audren_at(0) == NULL && audren_at(SNX_AUDREN_MAX + 1) == NULL);
    play(first);
    play(second);
    CHECK(host_of(first)->volume == 1.0f && host_of(second)->volume == 1.0f && !host_of(later));
    audren_volume(0.5f);
    CHECK(host_of(first)->volume == 0.5f && host_of(second)->volume == 0.5f);
    play(later); /* opened after the volume was set */
    CHECK(host_of(later)->volume == 0.5f);
    audren_volume(1.0f);
    CHECK(host_of(first)->volume == 1.0f && host_of(second)->volume == 1.0f && host_of(later)->volume == 1.0f);
    test_update_results(first);
    audren_close(first);
    audren_close(second);
    audren_close(later);
    audren_close(later); /* a name already closed: nothing */
    for (int i = 0; i < SNX_AUDREN_MAX; i++) CHECK(!g_hle.audren.rens[i].used); /* every renderer's slot free */
    /* every slot taken: the next open makes none, and frees the state it made */
    u32 all[SNX_AUDREN_MAX];
    for (int i = 0; i < SNX_AUDREN_MAX; i++) CHECK((all[i] = audren_open(params, 0)) != 0);
    CHECK(audren_open(params, 0) == 0);
    for (int i = 0; i < SNX_AUDREN_MAX; i++) audren_close(all[i]);
    test_queue_line();
    printf("audren_host: %u checks passed\n", test_checks);
    return 0;
}
