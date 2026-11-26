/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Noise reduction for a microphone's mono signed 16-bit PCM, in blocks of 10 ms, by one of two methods. The neural one
   is RNNoise, a small recurrent network trained on speech that gives each band of a frame its gain: the copy built in
   (cmake/dependencies.cmake). It works at 48 kHz only. The spectral one is the Speex DSP library's preprocessor
   (libspeexdsp, BSD-3-Clause), which follows the quietest level of each band, so it lowers a steady noise and little
   else. The host has it or not, and it is never shipped. Every console's microphone runs it (common/utilities/mic.h) */
#ifndef SNX_UTILITIES_DENOISE_H
#define SNX_UTILITIES_DENOISE_H
#include "common/platform/mic.h"
#include <stdint.h>

/* the spectral method's library's file name on this host */
#if defined(_WIN32)
#define SNX_DENOISE_LIBRARY "libspeexdsp-1.dll"
#elif defined(__APPLE__)
#define SNX_DENOISE_LIBRARY "libspeexdsp.1.dylib"
#else
#define SNX_DENOISE_LIBRARY "libspeexdsp.so.1"
#endif
/* the most the spectral method lowers what it takes for noise, in dB. The library's default is -15. How strong the
   console's own reduction is isn't known */
#define SNX_DENOISE_SUPPRESS_DB (-30)

/* the methods, as the mic_noise_reduction setting numbers them */
enum { SNX_DENOISE_OFF, SNX_DENOISE_NEURAL, SNX_DENOISE_SPECTRAL };
/* the one rate the neural method takes, in blocks of a hundredth of it */
#define SNX_DENOISE_NEURAL_RATE 48000

typedef struct SnxDenoise SnxDenoise;
/* a reducer for blocks of block samples at rate Hz by method. The neural one falls back to the spectral one at another
   rate than 48 kHz or another block than 480. NULL for SNX_DENOISE_OFF, or when neither method is there. Safe from any
   thread. The first reducer that needs the spectral library says through say whether it loaded */
SnxDenoise* snx_denoise_new(uint32_t rate, uint32_t block, int method, SnxMicSay say);
/* the method it runs: SNX_DENOISE_NEURAL or SNX_DENOISE_SPECTRAL */
int snx_denoise_method(const SnxDenoise* d);
/* the samples its output lags its input by: a block, as both methods overlap their frames */
uint32_t snx_denoise_delay(const SnxDenoise* d);
/* one block, in place: its noise as the reducer has learnt it from the blocks before, lowered. One thread at a time
   for every reducer together, as the network's tables are shared */
void snx_denoise_run(SnxDenoise* d, int16_t* block);
void snx_denoise_free(SnxDenoise* d);
#endif
