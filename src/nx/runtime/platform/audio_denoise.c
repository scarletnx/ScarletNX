/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The microphone's noise reduction (audio_denoise.h).
   The neural method is RNNoise. It takes frames of 480 samples at 48 kHz as floats on the 16-bit scale, and gives each
   frame back a frame later, as it overlaps one frame's analysis with the next (so does the spectral method). It is the
   copy built in, never a system librnnoise: another version's network gives other gains and another delay.
   The spectral method is the Speex DSP library's preprocessor, found by its C functions in the library the host has.
   Its noise estimate follows the quietest level of each band over the last seconds, so a steady noise (a fan, a hum,
   traffic) is learnt and lowered while a voice, which comes and goes, passes. Only its noise suppression is switched
   on. Its gain control, voice detection and dereverberation are off in a new state, and are left so (asked to set its
   voice detection, even off, the library prints a warning) */
#include "nx/runtime/platform/audio_denoise.h"
#include "common/platform/dl.h"
#include "nx/runtime/platform/log.h"
#include <math.h>
#include <rnnoise.h>
#include <stdlib.h>
#include <stdatomic.h>

#define RNNOISE_FRAME (SNX_DENOISE_NEURAL_RATE / 100)

/* speex_preprocess_ctl's requests, as the library's header numbers them */
enum {
    SPEEX_SET_DENOISE = 0,
    SPEEX_SET_NOISE_SUPPRESS = 18,
};

typedef void* (*SpeexInit)(int frame_size, int rate);
typedef int (*SpeexCtl)(void* state, int request, void* value);
typedef int (*SpeexRun)(void* state, int16_t* samples);
typedef void (*SpeexDestroy)(void* state);

/* a library, loaded once: state 0 not tried, 1 being loaded, 2 loaded, 3 not there */
static struct {
    _Atomic int state;
    SpeexInit init;
    SpeexCtl ctl;
    SpeexRun run;
    SpeexDestroy destroy;
} g_speex;

struct SnxDenoise {
    int method;
    uint32_t block;
    void* state; /* the neural method's DenoiseState, or the spectral one's */
    float in[RNNOISE_FRAME], out[RNNOISE_FRAME];
};

/* 1 when the library loaded */
static int speex_load(void) {
    void* lib = plat_dl_open(SNX_DENOISE_LIBRARY);
    if (!lib) {
        LOG("audio: %s did not load (%s): no spectral noise reduction\n", SNX_DENOISE_LIBRARY, plat_dl_error());
        return 0;
    }
    g_speex.init = (SpeexInit)plat_dl_sym(lib, "speex_preprocess_state_init");
    g_speex.ctl = (SpeexCtl)plat_dl_sym(lib, "speex_preprocess_ctl");
    g_speex.run = (SpeexRun)plat_dl_sym(lib, "speex_preprocess_run");
    g_speex.destroy = (SpeexDestroy)plat_dl_sym(lib, "speex_preprocess_state_destroy");
    if (!g_speex.init || !g_speex.ctl || !g_speex.run || !g_speex.destroy) {
        WARN("audio: %s has no preprocessor: no spectral noise reduction\n", SNX_DENOISE_LIBRARY);
        plat_dl_close(lib);
        return 0;
    }
    return 1;
}
/* 1 when a library is loaded: the first caller loads it, any other waits for that */
static int ready(_Atomic int* state, int (*load)(void)) {
    int now = atomic_load_explicit(state, memory_order_acquire), expected = 0;
    if (now == 0 &&
        atomic_compare_exchange_strong_explicit(state, &expected, 1, memory_order_acq_rel, memory_order_acquire))
        atomic_store_explicit(state, load() ? 2 : 3, memory_order_release);
    while ((now = atomic_load_explicit(state, memory_order_acquire)) < 2) {}
    return now == 2;
}

SnxDenoise* snx_denoise_new(uint32_t rate, uint32_t block, int method) {
    if (method == SNX_DENOISE_OFF || !rate || !block || block > rate) return NULL;
    SnxDenoise* d = calloc(1, sizeof *d);
    if (!d) return NULL;
    d->block = block;
    if (method == SNX_DENOISE_NEURAL && rate == SNX_DENOISE_NEURAL_RATE && block == RNNOISE_FRAME &&
        (d->state = rnnoise_create(NULL)) != NULL) {
        d->method = SNX_DENOISE_NEURAL;
        return d;
    }
    if (ready(&g_speex.state, speex_load) && (d->state = g_speex.init((int)block, (int)rate)) != NULL) {
        int on = 1, suppress = SNX_DENOISE_SUPPRESS_DB;
        g_speex.ctl(d->state, SPEEX_SET_DENOISE, &on);
        g_speex.ctl(d->state, SPEEX_SET_NOISE_SUPPRESS, &suppress);
        d->method = SNX_DENOISE_SPECTRAL;
        return d;
    }
    free(d);
    return NULL;
}

int snx_denoise_method(const SnxDenoise* d) { return d ? d->method : SNX_DENOISE_OFF; }

uint32_t snx_denoise_delay(const SnxDenoise* d) { return d ? d->block : 0; }

void snx_denoise_run(SnxDenoise* d, int16_t* block) {
    if (!d) return;
    if (d->method == SNX_DENOISE_SPECTRAL) {
        g_speex.run(d->state, block);
        return;
    }
    for (int i = 0; i < RNNOISE_FRAME; i++) d->in[i] = block[i];
    rnnoise_process_frame(d->state, d->out, d->in);
    for (int i = 0; i < RNNOISE_FRAME; i++) {
        float v = d->out[i];
        block[i] = (int16_t)(v > 32767.0f ? 32767 : v < -32768.0f ? -32768 : lrintf(v));
    }
}

void snx_denoise_free(SnxDenoise* d) {
    if (!d) return;
    if (d->method == SNX_DENOISE_SPECTRAL)
        g_speex.destroy(d->state);
    else
        rnnoise_destroy(d->state);
    free(d);
}
