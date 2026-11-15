/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The microphone's noise reduction (audio_denoise.h).
   The neural method is RNNoise. It takes frames of 480 samples at 48 kHz as floats on the 16-bit scale, and gives each
   frame back a frame later, as it overlaps one frame's analysis with the next (so does the spectral method). A system librnnoise is found by its C
   functions, else the copy linked in runs (its symbols hidden, so the system's library never binds to them).
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

#define RNNOISE_RATE  48000
#define RNNOISE_FRAME 480

/* speex_preprocess_ctl's requests, as the library's header numbers them */
enum {
    SPEEX_SET_DENOISE = 0,
    SPEEX_SET_NOISE_SUPPRESS = 18,
};

typedef void* (*SpeexInit)(int frame_size, int rate);
typedef int (*SpeexCtl)(void* state, int request, void* value);
typedef int (*SpeexRun)(void* state, int16_t* samples);
typedef void (*SpeexDestroy)(void* state);
typedef void* (*RnnCreate)(void* model);
typedef float (*RnnProcess)(void* state, float* out, const float* in);
typedef void (*RnnDestroy)(void* state);

/* a library, loaded once: state 0 not tried, 1 being loaded, 2 loaded, 3 not there */
static struct {
    int state;
    SpeexInit init;
    SpeexCtl ctl;
    SpeexRun run;
    SpeexDestroy destroy;
} g_speex;
static struct {
    int state;
    RnnCreate create;
    RnnProcess process;
    RnnDestroy destroy;
} g_rnn;

struct SnxDenoise {
    int method;
    uint32_t block;
    void* state;
    RnnProcess process; /* the neural method's, the system's or the one linked in */
    RnnDestroy destroy;
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
/* the system's librnnoise, when the host has one with the functions every version has: 1 when it loaded */
static int rnn_load(void) {
    void* lib = plat_dl_open(SNX_RNNOISE_LIBRARY);
    if (!lib) return 0;
    g_rnn.create = (RnnCreate)plat_dl_sym(lib, "rnnoise_create");
    g_rnn.process = (RnnProcess)plat_dl_sym(lib, "rnnoise_process_frame");
    g_rnn.destroy = (RnnDestroy)plat_dl_sym(lib, "rnnoise_destroy");
    int (*frame_size)(void) = (int (*)(void))plat_dl_sym(lib, "rnnoise_get_frame_size");
    if (!g_rnn.create || !g_rnn.process || !g_rnn.destroy || !frame_size || frame_size() != RNNOISE_FRAME) {
        plat_dl_close(lib);
        return 0;
    }
    LOG("audio: the neural noise reduction is the system's %s\n", SNX_RNNOISE_LIBRARY);
    return 1;
}

/* 1 when a library is loaded: the first caller loads it, any other waits for that */
static int ready(int* state, int (*load)(void)) {
    int now = __atomic_load_n(state, __ATOMIC_ACQUIRE), expected = 0;
    if (now == 0 && __atomic_compare_exchange_n(state, &expected, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        __atomic_store_n(state, load() ? 2 : 3, __ATOMIC_RELEASE);
    while ((now = __atomic_load_n(state, __ATOMIC_ACQUIRE)) < 2) {}
    return now == 2;
}

/* the copy linked in, by the types the system's library is called through */
static void* linked_create(void* model) {
    (void)model;
    return rnnoise_create(NULL);
}
static float linked_process(void* state, float* out, const float* in) { return rnnoise_process_frame(state, out, in); }
static void linked_destroy(void* state) { rnnoise_destroy(state); }

static int neural_new(SnxDenoise* d) {
    int system = ready(&g_rnn.state, rnn_load);
    RnnCreate create = system ? g_rnn.create : linked_create;
    d->process = system ? g_rnn.process : linked_process;
    d->destroy = system ? g_rnn.destroy : linked_destroy;
    d->state = create(NULL);
    return d->state != NULL;
}

SnxDenoise* snx_denoise_new(uint32_t rate, uint32_t block, int method) {
    if (method == SNX_DENOISE_OFF || !rate || !block || block > rate) return NULL;
    SnxDenoise* d = calloc(1, sizeof *d);
    if (!d) return NULL;
    d->block = block;
    if (method == SNX_DENOISE_NEURAL && rate == RNNOISE_RATE && block == RNNOISE_FRAME && neural_new(d)) {
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
    d->process(d->state, d->out, d->in);
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
        d->destroy(d->state);
    free(d);
}
