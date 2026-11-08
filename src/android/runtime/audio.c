/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* MediaPlayer's and SoundPool's audio (audio.h). A source is the bytes of a file, which FFmpeg demuxes from memory and
   decodes, and libswresample makes 48 kHz stereo of 16-bit samples. A sound (SoundPool's) is decoded whole when it
   loads. A stream (MediaPlayer's) is decoded as it plays. Each voice plays one of them with its volumes, loops, rate,
   pause and position, and SDL's audio stream asks the mixer for the sum of the voices as the device plays it */
#include "android/runtime/audio.h"
#include "android/runtime/jvm.h"
#include <SDL3/SDL.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/mem.h>
#include <libswresample/swresample.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RATE        48000
#define MAX_VOICES  64
#define IO_BUFFER   (32 * 1024)

/* ---- the decoder: one source's audio stream, from memory ---- */

typedef struct {
    uint8_t* data; /* the source's bytes, the decoder's own */
    int64_t size, at;
    AVFormatContext* format;
    AVIOContext* io;
    AVCodecContext* codec;
    SwrContext* swr;
    AVPacket* packet;
    AVFrame* frame;
    int stream;
    int16_t* pcm; /* decoded frames not played yet */
    size_t pcm_frames, pcm_at, pcm_capacity;
    bool eof;
} Decoder;

static int io_read(void* opaque, uint8_t* buffer, int size) {
    Decoder* d = opaque;
    int64_t n = d->size - d->at < size ? d->size - d->at : size;
    if (n <= 0) return AVERROR_EOF;
    memcpy(buffer, d->data + d->at, (size_t)n);
    d->at += n;
    return (int)n;
}
static int64_t io_seek(void* opaque, int64_t offset, int whence) {
    Decoder* d = opaque;
    int64_t to;
    switch (whence & ~AVSEEK_FORCE) {
    case AVSEEK_SIZE: return d->size;
    case SEEK_SET: to = offset; break;
    case SEEK_CUR: to = d->at + offset; break;
    case SEEK_END: to = d->size + offset; break;
    default: return -1;
    }
    if (to < 0 || to > d->size) return -1;
    d->at = to;
    return to;
}

static void decoder_free(Decoder* d) {
    if (!d) return;
    swr_free(&d->swr);
    avcodec_free_context(&d->codec);
    avformat_close_input(&d->format);
    if (d->io) av_freep(&d->io->buffer);
    avio_context_free(&d->io);
    av_packet_free(&d->packet);
    av_frame_free(&d->frame);
    free(d->pcm);
    free(d->data);
    free(d);
}

/* a decoder of the source's best audio stream, which takes data (malloc's); NULL when FFmpeg reads none */
static Decoder* decoder_open(uint8_t* data, int64_t size) {
    Decoder* d = calloc(1, sizeof *d);
    if (!d) {
        free(data);
        return NULL;
    }
    d->data = data;
    d->size = size;
    uint8_t* buffer = av_malloc(IO_BUFFER);
    d->io = buffer ? avio_alloc_context(buffer, IO_BUFFER, 0, d, io_read, NULL, io_seek) : NULL;
    if (!d->io) av_free(buffer);
    d->format = d->io ? avformat_alloc_context() : NULL;
    if (d->format) {
        d->format->pb = d->io;
        d->format->flags |= AVFMT_FLAG_CUSTOM_IO;
        if (avformat_open_input(&d->format, NULL, NULL, NULL) < 0) d->format = NULL; /* freed by the call */
    }
    const AVCodec* codec = NULL;
    d->stream = d->format && avformat_find_stream_info(d->format, NULL) >= 0
                    ? av_find_best_stream(d->format, AVMEDIA_TYPE_AUDIO, -1, -1, &codec, 0)
                    : -1;
    if (d->stream >= 0 && codec) d->codec = avcodec_alloc_context3(codec);
    if (!d->codec || avcodec_parameters_to_context(d->codec, d->format->streams[d->stream]->codecpar) < 0 ||
        avcodec_open2(d->codec, codec, NULL) < 0) {
        decoder_free(d);
        return NULL;
    }
    AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
    if (swr_alloc_set_opts2(&d->swr, &stereo, AV_SAMPLE_FMT_S16, RATE, &d->codec->ch_layout, d->codec->sample_fmt,
                            d->codec->sample_rate, 0, NULL) < 0 ||
        swr_init(d->swr) < 0 || !(d->packet = av_packet_alloc()) || !(d->frame = av_frame_alloc())) {
        decoder_free(d);
        return NULL;
    }
    return d;
}

/* the source's length in milliseconds, -1 when it does not say */
static int64_t decoder_duration(const Decoder* d) {
    if (d->format->duration != AV_NOPTS_VALUE && d->format->duration > 0) return d->format->duration / 1000;
    const AVStream* s = d->format->streams[d->stream];
    if (s->duration != AV_NOPTS_VALUE && s->duration > 0)
        return s->duration * 1000 * s->time_base.num / s->time_base.den;
    return -1;
}

/* the frame's samples (or the resampler's last ones, for NULL) converted onto the end of the decoded frames */
static bool convert(Decoder* d, const AVFrame* frame) {
    int in = frame ? frame->nb_samples : 0;
    int out = swr_get_out_samples(d->swr, in);
    if (out <= 0) return true;
    size_t kept = d->pcm_frames - d->pcm_at;
    if (d->pcm_at) memmove(d->pcm, d->pcm + 2 * d->pcm_at, kept * 4);
    d->pcm_frames = kept;
    d->pcm_at = 0;
    if (kept + (size_t)out > d->pcm_capacity) {
        size_t n = kept + (size_t)out + RATE / 4;
        int16_t* pcm = realloc(d->pcm, n * 4);
        if (!pcm) return false;
        d->pcm = pcm;
        d->pcm_capacity = n;
    }
    uint8_t* to = (uint8_t*)(d->pcm + 2 * kept);
    int n = swr_convert(d->swr, &to, out, frame ? (const uint8_t* const*)frame->extended_data : NULL, in);
    if (n < 0) return false;
    d->pcm_frames += (size_t)n;
    return true;
}

/* more frames decoded: false at the source's end (or an error), with nothing more to come */
static bool decode_more(Decoder* d) {
    while (!d->eof) {
        int r = avcodec_receive_frame(d->codec, d->frame);
        if (r == 0) {
            bool ok = convert(d, d->frame);
            av_frame_unref(d->frame);
            if (!ok) d->eof = true;
            return ok;
        }
        if (r == AVERROR_EOF) {
            convert(d, NULL);
            d->eof = true;
            return d->pcm_frames > d->pcm_at;
        }
        if (r != AVERROR(EAGAIN)) {
            d->eof = true;
            return false;
        }
        r = av_read_frame(d->format, d->packet);
        if (r < 0) {
            avcodec_send_packet(d->codec, NULL); /* the decoder's last frames, then AVERROR_EOF */
            continue;
        }
        if (d->packet->stream_index == d->stream) avcodec_send_packet(d->codec, d->packet);
        av_packet_unref(d->packet);
    }
    return false;
}

/* the source from msec on: a seek to the key frame before it, and the decoded frames up to it dropped */
static void decoder_seek(Decoder* d, int64_t msec) {
    const AVStream* s = d->format->streams[d->stream];
    int64_t ts = av_rescale_q(msec, (AVRational){1, 1000}, s->time_base);
    if (av_seek_frame(d->format, d->stream, ts, AVSEEK_FLAG_BACKWARD) < 0)
        av_seek_frame(d->format, -1, 0, AVSEEK_FLAG_BACKWARD | AVSEEK_FLAG_BYTE);
    avcodec_flush_buffers(d->codec);
    swr_init(d->swr);
    d->pcm_frames = d->pcm_at = 0;
    d->eof = false;
}

/* ---- sounds, voices and the mixer ---- */

typedef struct {
    int16_t* pcm;
    size_t frames;
    int refs; /* the pool's and each voice's, under the mixer's lock */
} Sound;

typedef struct {
    int id; /* 0 for a free slot */
    Sound* sound;
    Decoder* stream;
    double at;             /* a sound's next frame, fractional at a rate other than 1 */
    int64_t played;        /* a stream's frames played since its start or seek */
    float left, right, rate;
    int loops;             /* the plays after this one: -1 for ever */
    bool paused, keep;     /* keep: a stream's voice stays at its end, for its player to start it again */
    bool completed;        /* it reached its end since its player last asked */
} Voice;

static struct {
    SDL_Mutex* lock;
    SDL_AudioStream* stream;
    bool started, failed, headless;
    Voice voices[MAX_VOICES];
    int next_id;
    int32_t* mix;
    int16_t* out;
    size_t capacity;
} a;

void android_audio_set_headless(bool headless) { a.headless = headless; }

static void sound_release(Sound* s) {
    if (s && --s->refs == 0) {
        free(s->pcm);
        free(s);
    }
}
static void voice_free(Voice* v) {
    sound_release(v->sound);
    decoder_free(v->stream);
    memset(v, 0, sizeof *v);
}
/* a new voice's ID: never 0, never one given before until the count wraps */
static int next_id(void) {
    if (a.next_id == INT32_MAX) a.next_id = 0;
    return ++a.next_id;
}
static Voice* voice_of(int id) {
    for (int i = 0; id > 0 && i < MAX_VOICES; ++i)
        if (a.voices[i].id == id) return &a.voices[i];
    return NULL;
}

/* a voice's frames added into mix, and the voice at its end paused (a player's) or gone */
static void mix_voice(Voice* v, int32_t* mix, size_t frames) {
    if (!frames) return;
    int32_t l = (int32_t)(v->left * 32768.0f), r = (int32_t)(v->right * 32768.0f);
    for (size_t i = 0; i < frames; ++i) {
        const int16_t* f = NULL;
        if (v->sound) {
            if ((size_t)v->at >= v->sound->frames) {
                if (v->loops == 0 || !v->sound->frames) break;
                if (v->loops > 0) --v->loops;
                v->at -= (double)v->sound->frames;
            }
            f = v->sound->pcm + 2 * (size_t)v->at;
            v->at += v->rate;
        } else {
            Decoder* d = v->stream;
            if (d->pcm_at >= d->pcm_frames && !decode_more(d)) {
                if (v->loops == 0) break;
                if (v->loops > 0) --v->loops;
                decoder_seek(d, 0);
                v->played = 0;
                if (!decode_more(d)) break;
            }
            f = d->pcm + 2 * d->pcm_at++;
            ++v->played;
        }
        mix[2 * i] += (f[0] * l) >> 15;
        mix[2 * i + 1] += (f[1] * r) >> 15;
        if (i + 1 == frames) return;
    }
    /* the end */
    v->completed = true;
    if (v->keep) {
        v->paused = true;
    } else {
        voice_free(v);
    }
}

static void SDLCALL feed(void* userdata, SDL_AudioStream* stream, int additional, int total) {
    (void)userdata, (void)total;
    if (additional <= 0) return;
    size_t frames = (size_t)additional / 4;
    SDL_LockMutex(a.lock);
    if (frames > a.capacity) {
        int32_t* mix = realloc(a.mix, frames * 8);
        if (mix) a.mix = mix;
        int16_t* out = realloc(a.out, frames * 4);
        if (out) a.out = out;
        if (mix && out) a.capacity = frames;
    }
    if (frames > a.capacity) frames = a.capacity;
    memset(a.mix, 0, frames * 8);
    for (int i = 0; i < MAX_VOICES; ++i)
        if (a.voices[i].id && !a.voices[i].paused) mix_voice(&a.voices[i], a.mix, frames);
    for (size_t i = 0; i < 2 * frames; ++i)
        a.out[i] = (int16_t)(a.mix[i] > 32767 ? 32767 : a.mix[i] < -32768 ? -32768 : a.mix[i]);
    SDL_UnlockMutex(a.lock);
    if (frames) SDL_PutAudioStreamData(stream, a.out, (int)(frames * 4));
}

/* SDL's audio device, opened the first time: the system's, else the dummy driver, which plays nothing at real time */
static bool open_device(const char* driver) {
    if (driver) SDL_SetHint(SDL_HINT_AUDIO_DRIVER, driver);
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) return false;
    SDL_AudioSpec spec = {SDL_AUDIO_S16, 2, RATE};
    a.stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, feed, NULL);
    if (a.stream && SDL_ResumeAudioStreamDevice(a.stream)) return true;
    if (a.stream) SDL_DestroyAudioStream(a.stream);
    a.stream = NULL;
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    return false;
}
static bool start(void) {
    if (a.started || a.failed) return a.started;
    a.lock = SDL_CreateMutex();
    if (a.lock && ((!a.headless && open_device(NULL)) || open_device("dummy"))) {
        a.started = true;
        return true;
    }
    fprintf(stderr, "scarletnx-android: no audio device: %s\n", SDL_GetError());
    a.failed = true;
    return false;
}

/* ---- snx.android.media.Audio's natives ---- */

/* a Java byte[]'s copy (malloc's) */
static uint8_t* bytes_of(JNIEnv* env, jbyteArray data, int64_t* size) {
    jsize n = data ? (*env)->GetArrayLength(env, data) : 0;
    uint8_t* copy = n > 0 ? malloc((size_t)n) : NULL;
    if (copy) (*env)->GetByteArrayRegion(env, data, 0, n, (jbyte*)copy);
    *size = n;
    return copy;
}

/* nativeLoad(data): a sound, decoded whole, or 0 when FFmpeg reads no audio in it */
static jlong JNICALL native_load(JNIEnv* env, jclass unused, jbyteArray data) {
    (void)unused;
    int64_t size;
    uint8_t* bytes = bytes_of(env, data, &size);
    Decoder* d = bytes ? decoder_open(bytes, size) : NULL;
    if (!d) return 0;
    while (decode_more(d)) {}
    Sound* s = calloc(1, sizeof *s);
    if (s) {
        s->pcm = d->pcm;
        s->frames = d->pcm_frames;
        s->refs = 1;
        d->pcm = NULL;
    }
    decoder_free(d);
    return (jlong)(intptr_t)s;
}
/* nativeUnload(sound): the pool's hold on it given up; voices that play it keep it to their end */
static void JNICALL native_unload(JNIEnv* env, jclass unused, jlong sound) {
    (void)env, (void)unused;
    if (!a.lock) {
        sound_release((Sound*)(intptr_t)sound);
        return;
    }
    SDL_LockMutex(a.lock);
    sound_release((Sound*)(intptr_t)sound);
    SDL_UnlockMutex(a.lock);
}

/* nativePlay(sound, left, right, loops, rate): a voice of the sound, which ends by itself; 0 when none plays */
static jint JNICALL native_play(JNIEnv* env, jclass unused, jlong sound, jfloat left, jfloat right, jint loops,
                                jfloat rate) {
    (void)env, (void)unused;
    Sound* s = (Sound*)(intptr_t)sound;
    if (!s || !start()) return 0;
    SDL_LockMutex(a.lock);
    int id = 0;
    for (int i = 0; i < MAX_VOICES && !id; ++i)
        if (!a.voices[i].id) {
            Voice* v = &a.voices[i];
            id = v->id = next_id();
            v->sound = s;
            ++s->refs;
            v->left = left;
            v->right = right;
            v->loops = loops;
            v->rate = rate > 0 ? rate : 1;
        }
    SDL_UnlockMutex(a.lock);
    return id;
}

/* nativeOpen(data): a paused voice of a stream decoded as it plays, which stays at its end; 0 when FFmpeg reads no
   audio in the source. Its duration in milliseconds into duration[0] (-1 when the source does not say) */
static jint JNICALL native_open(JNIEnv* env, jclass unused, jbyteArray data, jlongArray duration) {
    (void)unused;
    int64_t size;
    uint8_t* bytes = bytes_of(env, data, &size);
    Decoder* d = bytes ? decoder_open(bytes, size) : NULL;
    if (!d) return 0;
    jlong ms = (jlong)decoder_duration(d);
    if (duration) (*env)->SetLongArrayRegion(env, duration, 0, 1, &ms);
    if (!start()) {
        decoder_free(d);
        return 0;
    }
    SDL_LockMutex(a.lock);
    int id = 0;
    for (int i = 0; i < MAX_VOICES && !id; ++i)
        if (!a.voices[i].id) {
            Voice* v = &a.voices[i];
            id = v->id = next_id();
            v->stream = d;
            v->left = v->right = v->rate = 1;
            v->paused = v->keep = true;
        }
    SDL_UnlockMutex(a.lock);
    if (!id) decoder_free(d);
    return id;
}

enum { SET_PAUSE, SET_RESUME, SET_STOP, SET_VOLUME, SET_LOOPS, SET_RATE, SET_SEEK };
/* nativeSet(voice, what, x, y): the voice paused, resumed, stopped (and gone), its volumes (x, y), loops (x), rate
   (x) or position in milliseconds (x) */
static void JNICALL native_set(JNIEnv* env, jclass unused, jint voice, jint what, jdouble x, jdouble y) {
    (void)env, (void)unused;
    if (!a.started) return;
    SDL_LockMutex(a.lock);
    Voice* v = voice_of(voice);
    if (v) switch (what) {
        case SET_PAUSE: v->paused = true; break;
        case SET_RESUME: v->paused = false; break;
        case SET_STOP: voice_free(v); break;
        case SET_VOLUME:
            v->left = (float)x;
            v->right = (float)y;
            break;
        case SET_LOOPS: v->loops = (int)x; break;
        case SET_RATE: v->rate = x > 0 ? (float)x : 1; break;
        case SET_SEEK:
            if (v->stream) {
                decoder_seek(v->stream, (int64_t)x);
                v->played = (int64_t)x * RATE / 1000;
            } else if (v->sound) {
                v->at = x * RATE / 1000;
            }
            v->completed = false;
            break;
        default: break;
        }
    SDL_UnlockMutex(a.lock);
}

/* nativePosition(voice): its position in milliseconds; -1 when the voice is gone. completed[0] tells (once) that it
   reached its end */
static jlong JNICALL native_position(JNIEnv* env, jclass unused, jint voice, jbooleanArray completed) {
    (void)unused;
    if (!a.started) return -1;
    SDL_LockMutex(a.lock);
    Voice* v = voice_of(voice);
    jlong ms = v ? (jlong)(v->stream ? v->played : (int64_t)v->at) * 1000 / RATE : -1;
    jboolean done = v && v->completed;
    if (v) v->completed = false;
    SDL_UnlockMutex(a.lock);
    if (completed) (*env)->SetBooleanArrayRegion(env, completed, 0, 1, &done);
    return ms;
}

bool android_audio_register(JNIEnv* env) {
    static const JNINativeMethod methods[] = {
        {(char*)"nativeLoad", (char*)"([B)J", (void*)native_load},
        {(char*)"nativeUnload", (char*)"(J)V", (void*)native_unload},
        {(char*)"nativePlay", (char*)"(JFFIF)I", (void*)native_play},
        {(char*)"nativeOpen", (char*)"([B[J)I", (void*)native_open},
        {(char*)"nativeSet", (char*)"(IIDD)V", (void*)native_set},
        {(char*)"nativePosition", (char*)"(I[Z)J", (void*)native_position},
    };
    return android_jvm_register(env, "snx/android/media/Audio", methods, (int)(sizeof methods / sizeof *methods));
}
