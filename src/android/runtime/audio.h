/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_ANDROID_AUDIO_H
#define SNX_ANDROID_AUDIO_H
#include <jni.h>
#include <stdbool.h>

/* The audio of MediaPlayer and SoundPool (audio.c): FFmpeg decodes the sources, which the framework reads into memory,
   to 48 kHz stereo, and one SDL audio stream mixes the voices that play them */

/* a run without a user (--headless): the mixer runs at real time on SDL's dummy driver, with no sound out */
void android_audio_set_headless(bool headless);
/* snx.android.media.Audio's natives */
bool android_audio_register(JNIEnv* env);
#endif
