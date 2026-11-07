/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Where the DSP's frames go: the default playback device through SDL3, and a WAV recording of the first two minutes,
   like the presenter's frame snapshots, so an unattended run can be listened to afterwards. */
#ifndef SNX_CTR_AUDIO_OUTPUT_H
#define SNX_CTR_AUDIO_OUTPUT_H
#include "3ds/audio/dsp.h"
#include <stdbool.h>

/* Opens the device and the recording (either may be missing, NULL wav_path records nothing). False when neither */
bool snx_ctr_audio_init(const char* wav_path);
/* Before init, or with no output, the frame is dropped. The CSND samples waiting are mixed into it */
void snx_ctr_audio_frame(const int16_t frame[SNX_CTR_DSP_SAMPLES][2]);
/* CSND's frames (services/csnd.c) mixed into the DSP's, as Azahar's DspInterface mixes them (mix.c): a CSND frame
   waits for the DSP's next frames (four frames at most, the oldest dropped), or after two CSND frames with no DSP frame
   between it plays alone */
void snx_ctr_audio_csnd_frame(const int16_t frame[SNX_CTR_DSP_SAMPLES][2]);
/* the DSP's frame with the CSND samples waiting mixed in, one for one, into out */
void snx_ctr_audio_mix_csnd(const int16_t frame[SNX_CTR_DSP_SAMPLES][2], int16_t out[SNX_CTR_DSP_SAMPLES][2]);
void snx_ctr_audio_mix_reset(void);
void snx_ctr_audio_free(void);
#endif
