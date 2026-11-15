/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The microphone's noise reduction (audio_denoise.h) through the Speex DSP library's preprocessor, found by its C
   functions in the library the host has. Its noise estimate follows the quietest level of each band over the last
   seconds, so a steady noise (a fan, a hum, traffic) is learnt and lowered while a voice, which comes and goes, passes.
   Only the noise suppression is switched on. The library's gain control, voice detection and dereverberation are off
   in a new state, and are left so (asked to set its voice detection, even off, the library prints a warning) */
#include "nx/runtime/platform/audio_denoise.h"
#include "common/platform/dl.h"
#include "nx/runtime/platform/log.h"
#include <stdlib.h>

/* speex_preprocess_ctl's requests, as the library's header numbers them */
enum {
    SPEEX_SET_DENOISE = 0,
    SPEEX_SET_NOISE_SUPPRESS = 18,
};

typedef void* (*SpeexInit)(int frame_size, int rate);
typedef int (*SpeexCtl)(void* state, int request, void* value);
typedef int (*SpeexRun)(void* state, int16_t* samples);
typedef void (*SpeexDestroy)(void* state);

/* the library, loaded once: state 0 not tried, 1 being loaded, 2 loaded, 3 not there */
static struct {
    int state;
    SpeexInit init;
    SpeexCtl ctl;
    SpeexRun run;
    SpeexDestroy destroy;
} g_speex;

struct SnxDenoise {
    void* state;
};

static void speex_load(void) {
    void* lib = plat_dl_open(SNX_DENOISE_LIBRARY);
    if (!lib) {
        WARN("audio: %s did not load (%s): the microphone is heard without noise reduction\n", SNX_DENOISE_LIBRARY,
             plat_dl_error());
        return;
    }
    g_speex.init = (SpeexInit)plat_dl_sym(lib, "speex_preprocess_state_init");
    g_speex.ctl = (SpeexCtl)plat_dl_sym(lib, "speex_preprocess_ctl");
    g_speex.run = (SpeexRun)plat_dl_sym(lib, "speex_preprocess_run");
    g_speex.destroy = (SpeexDestroy)plat_dl_sym(lib, "speex_preprocess_state_destroy");
    if (!g_speex.init || !g_speex.ctl || !g_speex.run || !g_speex.destroy) {
        WARN("audio: %s has no preprocessor: the microphone is heard without noise reduction\n", SNX_DENOISE_LIBRARY);
        g_speex.init = NULL;
        plat_dl_close(lib);
        return;
    }
    LOG("audio: the microphone's noise reduction is %s's preprocessor\n", SNX_DENOISE_LIBRARY);
}

/* 1 when the library is loaded: the first caller loads it, any other waits for that */
static int speex_ready(void) {
    int state = __atomic_load_n(&g_speex.state, __ATOMIC_ACQUIRE), expected = 0;
    if (state == 0 &&
        __atomic_compare_exchange_n(&g_speex.state, &expected, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        speex_load();
        __atomic_store_n(&g_speex.state, g_speex.init ? 2 : 3, __ATOMIC_RELEASE);
    }
    while ((state = __atomic_load_n(&g_speex.state, __ATOMIC_ACQUIRE)) < 2) {}
    return state == 2;
}

SnxDenoise* snx_denoise_new(uint32_t rate, uint32_t block, int suppress_db) {
    if (!rate || !block || block > rate || !speex_ready()) return NULL;
    SnxDenoise* d = calloc(1, sizeof *d);
    if (!d) return NULL;
    d->state = g_speex.init((int)block, (int)rate);
    if (!d->state) {
        free(d);
        return NULL;
    }
    int on = 1;
    g_speex.ctl(d->state, SPEEX_SET_DENOISE, &on);
    g_speex.ctl(d->state, SPEEX_SET_NOISE_SUPPRESS, &suppress_db);
    return d;
}

void snx_denoise_run(SnxDenoise* d, int16_t* block) {
    if (d) g_speex.run(d->state, block);
}

void snx_denoise_free(SnxDenoise* d) {
    if (!d) return;
    g_speex.destroy(d->state);
    free(d);
}
