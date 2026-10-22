/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* audren.c's host side with the DSP stubbed: the output device's volume (audren_volume, from audio.c) on every
   renderer's host stream, which opens at its first frame, and on one that opens after it */
#include "nx/hle/services/services_int.h"
#include "nx/hle/services/audren_dsp.h"

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
/* audren_update's first-update line */
void logf_(int level, const char* format, ...) {
    (void)level;
    (void)format;
}
struct PAudio {
    float volume;
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
    (void)a, (void)samples, (void)bytes;
    return 1;
}
void plat_audio_start(PAudio* a, int started) { (void)a, (void)started; }
u32 plat_audio_queued(PAudio* a) {
    (void)a;
    return 0;
}
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
int audren_state_update(ARState* s, const u8* in, u64 in_size, u8* out, u64 out_size) {
    (void)s, (void)in, (void)in_size, (void)out, (void)out_size;
    return 0;
}
int audren_frame(ARState* s, i16* stereo) {
    (void)s, (void)stereo;
    return 1;
}

static PAudio* host_of(SvcObj* o) { return audren_of(o)->out; }
/* started, and its first frame played: the host stream opens */
static void play(SvcObj* o) {
    o->u[0] = SNX_AUDREN_STARTED;
    audren_tick();
}

void pm_init(PMutex* m) { (void)m; }

int main(void) {
    hle_init();
    static const u8 params[0x34];
    SvcObj first, second, later;
    memset(&first, 0, sizeof first);
    memset(&second, 0, sizeof second);
    memset(&later, 0, sizeof later);
    CHECK(audren_open(&first, params) && audren_open(&second, params) && audren_open(&later, params));
    first.u[0] = second.u[0] = later.u[0] = SNX_AUDREN_STOPPED; /* as OpenAudioRenderer leaves them */
    play(&first);
    play(&second);
    CHECK(host_of(&first)->volume == 1.0f && host_of(&second)->volume == 1.0f && !host_of(&later));
    audren_volume(0.5f);
    CHECK(host_of(&first)->volume == 0.5f && host_of(&second)->volume == 0.5f);
    play(&later); /* opened after the volume was set */
    CHECK(host_of(&later)->volume == 0.5f);
    audren_volume(1.0f);
    CHECK(host_of(&first)->volume == 1.0f && host_of(&second)->volume == 1.0f && host_of(&later)->volume == 1.0f);
    first.destroy(&first);
    second.destroy(&second);
    later.destroy(&later);
    for (int i = 0; i < SNX_AUDREN_MAX; i++) CHECK(g_hle.audren.rens[i].o == NULL); /* every renderer's slot free */
    printf("audren_host: %u checks passed\n", test_checks);
    return 0;
}
