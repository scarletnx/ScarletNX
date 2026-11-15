/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Noise reduction for a microphone's mono signed 16-bit PCM, in blocks of a fixed size: the Speex DSP library's
   preprocessor (libspeexdsp, BSD-3-Clause), which the host has or not. It is loaded at the first reducer made, never
   shipped, and without it no reducer is made (said once) */
#ifndef SNX_AUDIO_DENOISE_H
#define SNX_AUDIO_DENOISE_H
#include <stdint.h>

/* the library's file name on this host */
#if defined(_WIN32)
#define SNX_DENOISE_LIBRARY "libspeexdsp-1.dll"
#elif defined(__APPLE__)
#define SNX_DENOISE_LIBRARY "libspeexdsp.1.dylib"
#else
#define SNX_DENOISE_LIBRARY "libspeexdsp.so.1"
#endif
/* the most the reducer lowers what it takes for noise, in dB. The library's default is -15. How strong the console's
   own reduction is isn't known */
#define SNX_DENOISE_SUPPRESS_DB (-30)

typedef struct SnxDenoise SnxDenoise;
/* a reducer for blocks of block samples at rate Hz, suppress_db the most it lowers the noise (a negative number). NULL
   when the library is not there. Safe from any thread */
SnxDenoise* snx_denoise_new(uint32_t rate, uint32_t block, int suppress_db);
/* one block, in place: its noise as the reducer has learnt it from the blocks before, lowered */
void snx_denoise_run(SnxDenoise* d, int16_t* block);
void snx_denoise_free(SnxDenoise* d);
#endif
