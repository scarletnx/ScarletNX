/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_NX_NN_AUDIO_H
#define SNX_NX_NN_AUDIO_H
#include "common/cpu/aarch64/cpu.h"

/* The native nn::audio (audio.c, audio.hook): the audio outs. Their state is g_hle.audio.outs_pool (services_int.h),
   under g_host.audio.lk */
#define SNX_AUDIO_OUT_RATE 48000u /* the DSP's, the only rate an audio out plays at */

/* the audio clock's turn (audio.c's audio_clock): every started out played up to now, guest time */
void nnaudio_out_tick(u64 now);
/* the output device's volume changed (g_hle.audio.device_volume): every out's host output follows. The caller holds
   g_host.audio.lk */
void nnaudio_out_device_volume(void);
/* a save state's load: the outs' host outputs closed, and opened again for the outs the loaded state holds */
void nnaudio_out_state_drop(void);
void nnaudio_out_state_rebuild(void);
#endif
