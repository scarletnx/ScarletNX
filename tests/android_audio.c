/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* SoundPool's voices in the mixer (src/android/runtime/audio.c), through the natives snx.android.media.Audio
   registers, with a JNI environment of the three functions they call: a looping voice sought past its sound's end,
   before its start and to no number at all keeps its place inside the sound, where the mixer took one play off a
   place many plays past the end and read the samples there. SDL's dummy driver runs the mixer at real time, with no
   sound out */
#include "android/runtime/audio.h"
#include "android/runtime/jvm.h"
#include <SDL3/SDL.h>
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { RATE = 48000, FRAMES = 4800, WAV = 44 + FRAMES * 4, SET_SEEK = 6, NATIVES = 6 };

/* the natives as audio.c registers them, by name */
static struct {
    JNINativeMethod methods[NATIVES];
    int count;
} g_audio;
bool android_jvm_register(JNIEnv* env, const char* class_name, const JNINativeMethod* methods, int count) {
    (void)env;
    assert(!strcmp(class_name, "snx/android/media/Audio") && count == NATIVES);
    memcpy(g_audio.methods, methods, sizeof g_audio.methods);
    g_audio.count = count;
    return true;
}
/* the native of a name into function, a pointer to a function of its type: the table keeps each as a void* */
static void native(const char* name, void* function, size_t size) {
    for (int i = 0; i < g_audio.count; ++i)
        if (!strcmp(g_audio.methods[i].name, name)) {
            assert(size == sizeof g_audio.methods[i].fnPtr);
            memcpy(function, &g_audio.methods[i].fnPtr, size);
            return;
        }
    fprintf(stderr, "android_audio: no native %s\n", name);
    exit(1);
}
#define NATIVE(function, name) native(name, &(function), sizeof(function))

/* a Java array of the test's: its elements and how many */
typedef struct {
    void* elements;
    jsize count;
} Array;
static jsize JNICALL array_length(JNIEnv* env, jarray array) {
    (void)env;
    return ((const Array*)array)->count;
}
static void JNICALL get_bytes(JNIEnv* env, jbyteArray array, jsize start, jsize count, jbyte* out) {
    (void)env;
    memcpy(out, (const jbyte*)((const Array*)array)->elements + start, (size_t)count);
}
static void JNICALL set_booleans(JNIEnv* env, jbooleanArray array, jsize start, jsize count, const jboolean* in) {
    (void)env;
    memcpy((jboolean*)((const Array*)array)->elements + start, in, (size_t)count);
}

static void put32(unsigned char* p, uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = (unsigned char)(v >> (8 * i));
}

int main(void) {
    static const typeof(*(JNIEnv)0) functions = {
        .GetArrayLength = array_length, .GetByteArrayRegion = get_bytes, .SetBooleanArrayRegion = set_booleans};
    JNIEnv env = &functions;
    /* a tenth of a second of 48 kHz stereo, a WAV of 16-bit samples that step by frame */
    unsigned char wav[WAV] = "RIFF\0\0\0\0WAVEfmt \20\0\0\0\1\0\2\0";
    Array source = {wav, WAV};
    jboolean done = 0;
    Array completed = {&done, 1};
    put32(wav + 4, WAV - 8), put32(wav + 24, RATE), put32(wav + 28, RATE * 4);
    wav[32] = 4, wav[34] = 16;
    memcpy(wav + 36, "data", 4), put32(wav + 40, FRAMES * 4);
    for (int i = 0; i < FRAMES * 2; ++i) wav[44 + i * 2] = (unsigned char)i, wav[45 + i * 2] = (unsigned char)(i >> 8);
    android_audio_set_headless(true);
    assert(android_audio_register(&env));
    jlong(JNICALL * load)(JNIEnv*, jclass, jbyteArray);
    jint(JNICALL * play)(JNIEnv*, jclass, jlong, jfloat, jfloat, jint, jfloat);
    void(JNICALL * set)(JNIEnv*, jclass, jint, jint, jdouble, jdouble);
    jlong(JNICALL * position)(JNIEnv*, jclass, jint, jbooleanArray);
    NATIVE(load, "nativeLoad"), NATIVE(play, "nativePlay"), NATIVE(set, "nativeSet"),
        NATIVE(position, "nativePosition");
    jlong sound = load(&env, NULL, (jbyteArray)&source);
    assert(sound);
    jint voice = play(&env, NULL, sound, 1, 1, -1, 1); /* for ever */
    assert(voice > 0);
    const double places[] = {1e12, -5000, 250, NAN}; /* milliseconds: the sound lasts 100 */
    for (size_t i = 0; i < sizeof places / sizeof *places; ++i) {
        set(&env, NULL, voice, SET_SEEK, places[i], 0);
        /* the mixer's next turns: the place stays inside the sound's 100 ms while it plays on from there */
        for (int turn = 0; turn < 20; ++turn) {
            jlong ms = position(&env, NULL, voice, (jbooleanArray)&completed);
            assert(ms >= 0 && ms <= 100);
            SDL_Delay(5);
        }
    }
    return 0;
}
