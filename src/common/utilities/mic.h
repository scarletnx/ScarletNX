/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A game's recording of the host's microphone, the Switch's and every console's: the device the microphone setting
   names (common/platform/mic.h; "off", or no device, gives silence), its samples reduced in blocks of 10 ms as
   mic_noise_reduction asks (common/utilities/denoise.h, read again at each block), and the reduced samples that wait
   for the game's time to take them. A device delivers in bursts (of 10 to 20 ms), so the game's clock takes what it
   owes only once SNX_MIC_HOLD_MS wait, which keeps a burst late by a few ms from leaving a gap. A recording that runs
   short gives silence and waits until it holds that much again, and 10 ms more each time it runs short, up to 200 ms.
   One that runs ahead (the game's time slower than the host's, or stopped by a pause) drops its oldest samples past
   the hold + SNX_MIC_SLACK_MS, which keeps the delay from growing. Its lines go through say, each kind of them said
   at most 8 times a run */
#ifndef SNX_UTILITIES_MIC_H
#define SNX_UTILITIES_MIC_H
#include "common/platform/mic.h"
#include <stdint.h>

#define SNX_MIC_HOLD_MS  40
#define SNX_MIC_SLACK_MS 60

typedef struct SnxMic SnxMic;
/* a recording at rate Hz from device (the microphone setting's text), reduced as *noise (mic_noise_reduction) asks:
   NULL only when memory runs out. Opening a device takes a while: outside a lock the game's threads wait on */
SnxMic* snx_mic_open(uint32_t rate, const char* device, const int* noise, SnxMicSay say);
/* the next n samples of the recording, or silence where it has none (no device, still filling up, or short) */
void snx_mic_take(SnxMic* m, int16_t* out, uint32_t n);
/* the rate it records at */
uint32_t snx_mic_rate(const SnxMic* m);
void snx_mic_free(SnxMic* m);
#endif
