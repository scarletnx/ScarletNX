/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The host's recording device (SDL), which the system resamples to mono signed 16-bit at the rate asked: the Switch's
   microphone and every console's. snx_mic_device_open opens the first recording device whose name holds the text
   given, in any case (the system's default when none does, or the text is empty); NULL when SDL's audio is not
   started (a headless run) or no device opens. snx_mic_device_read takes up to frames of what it recorded since, the
   count taken; snx_mic_device_available counts what waits. Their lines go through say: warn for a failure */
#ifndef SNX_PLATFORM_MIC_H
#define SNX_PLATFORM_MIC_H
#include <stdbool.h>
#include <stdint.h>

typedef struct SnxMicDevice SnxMicDevice;
/* a line a recording says, its end of line included; warn for a failure, else what it does */
typedef void (*SnxMicSay)(bool warn, const char* line);
SnxMicDevice* snx_mic_device_open(uint32_t rate, const char* device, SnxMicSay say);
void snx_mic_device_close(SnxMicDevice* m);
uint32_t snx_mic_device_available(SnxMicDevice* m);
uint32_t snx_mic_device_read(SnxMicDevice* m, int16_t* samples, uint32_t frames, SnxMicSay say);
#endif
