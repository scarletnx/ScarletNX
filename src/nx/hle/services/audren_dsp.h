/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* the audio renderer: the state its updates leave (audren_update) and one frame of the DSP over it (audren_frame).
   The update's layout, the order of a frame and the resampler follow the console's renderer as Eden's audio_core
   reads it; edenprobe's audio family writes the same layout field by field */
#ifndef SNX_AUDREN_DSP_H
#define SNX_AUDREN_DSP_H
#include "nx/memory/mem.h"

enum {
    SNX_AUDREN_WAVES = 4,     /* wave buffers per voice */
    SNX_AUDREN_CHANNELS = 6,  /* channels per voice, sink or effect */
    SNX_AUDREN_MIX_OUTS = 24, /* destination buffers a voice channel or a mix sends to */
    SNX_AUDREN_HISTORY = 8,   /* samples a resampler keeps from the frame before: 4 taps, or 8 at high quality */
    SNX_AUDREN_TEMP = 0x4000, /* decoded samples one voice channel may need in a frame */
};

typedef struct {
    u64 addr, size, ctx, ctx_size;
    i32 start, end, loop_start, loop_end, loop_count;
    u8 loop, ended, sent; /* sent: handed to the DSP, the game's buffer slot is free to reuse */
} ARWave;

/* a biquad's state: the last two inputs and outputs with float processing, two accumulators with integer */
typedef struct {
    double x1, x2, y1, y2;
    i64 s0, s1;
} ARBiquad;

/* a voice channel resource: its send volumes and what the DSP keeps of the channel between frames */
typedef struct {
    float mix[SNX_AUDREN_MIX_OUTS], prev_mix[SNX_AUDREN_MIX_OUTS];
    u8 in_use;
    u8 valid[SNX_AUDREN_WAVES];
    u32 index, consumed, offset;
    u64 played;
    i64 fraction; /* Q15 */
    i32 loop_count;
    u16 adpcm_header;
    i16 yn0, yn1;
    i16 history[SNX_AUDREN_HISTORY];
    ARBiquad biquad[2];
} ARChannel;

typedef struct {
    u8 in_use, fresh, format, quality, channels, flags;
    u8 play, last_play,
        was_playing; /* the renderer's play state: SNX_AUDREN_STARTED, SNX_AUDREN_STOPPED, SNX_AUDREN_PAUSED, SNX_AUDREN_STOPPING */
    u32 rate, wave_count, wave_index, flush, mix_id, splitter_id;
    u32 res[SNX_AUDREN_CHANNELS];
    float pitch, volume, prev_volume;
    u64 data, data_size;
    ARWave wave[SNX_AUDREN_WAVES];
    float bq_b[2][3], bq_a[2][2]; /* its two biquads, the feedback ones added; Q14 over 16384 before revision 15 */
    u8 bq_on[2], bq_was_on[2];
} ARVoice;

typedef struct {
    u8 in_use;
    float volume;
    u32 buffers, dest_splitter;
    i32 dest, offset, distance;
    float vol[SNX_AUDREN_MIX_OUTS][SNX_AUDREN_MIX_OUTS];
} ARMix;

/* a splitter's destination: the mix it sends to and its volumes, the last frame's to ramp from */
typedef struct {
    float vol[SNX_AUDREN_MIX_OUTS], prev[SNX_AUDREN_MIX_OUTS];
    u32 mix_id;
    i32 next; /* the next destination of its splitter, -1 at the end */
    u8 in_use, need_update;
} ARDest;

typedef struct {
    i32 head; /* its first destination, -1 for none */
    u32 count;
} ARSplitter;

typedef struct {
    u8 type, enabled, fresh;
    u32 mix_id, order;
    u8 p[0xA0]; /* the type's own parameters */
    /* the light limiter's detector, gain and look-ahead line */
    float avg[SNX_AUDREN_CHANNELS], gain[SNX_AUDREN_CHANNELS];
    float* look;
    u32 look_len, look_pos[SNX_AUDREN_CHANNELS];
    ARBiquad bq[SNX_AUDREN_CHANNELS]; /* the biquad filter's, a channel apiece */
    void* fx;                         /* the delay's, a reverb's or the compressor's state, made when it initializes */
} AREffect;

typedef struct {
    u8 type, in_use;
    u8 p[0x120];
    u32 pos, last, last2; /* a circular buffer sink's write position, and where it was one and two frames ago */
} ARSink;

typedef struct {
    u32 rate, frame, buffers; /* sample rate, samples a frame, mix buffers */
    u32 nvoices, nres, nmix, neffects, nsinks, nsplit, ndest;
    ARVoice* voice;
    ARChannel* chan;
    ARMix* mix;
    AREffect* effect;
    ARSink* sink;
    ARSplitter* split;
    ARDest* dest;
    i32* stack; /* the mix sort's depth-first search, with splitters: nmix * (nmix + 1) entries */
    u8* seen;   /* and each mix's state in it, then the edges of the one being searched */
    float*
        buf;   /* (buffers + SNX_AUDREN_CHANNELS) * frame: the mix buffers, then one per voice channel being rendered */
    i16* temp; /* SNX_AUDREN_TEMP + SNX_AUDREN_HISTORY decoded samples */
    i32* order;                        /* the mixes from the farthest from the final mix to it */
    u64 frames;                        /* frames rendered */
    u32 rev;                           /* the revision of the last update */
    u32 warned;                        /* features met once and not rendered, as SNX_AUDREN_WARN_* bits */
    float lut4[3][512], lut8[3][1024]; /* the resampler's tables, made with the state */
} ARState;

enum { SNX_AUDREN_STARTED = 0, SNX_AUDREN_STOPPED = 1, SNX_AUDREN_PAUSED = 2, SNX_AUDREN_STOPPING = 3 };
enum { SNX_AUDREN_PCM16 = 2, SNX_AUDREN_PCMF32 = 5, SNX_AUDREN_ADPCM = 6 };
enum {
    SNX_AUDREN_EFFECT_MIX = 1,
    SNX_AUDREN_EFFECT_AUX = 2,
    SNX_AUDREN_EFFECT_DELAY = 3,
    SNX_AUDREN_EFFECT_REVERB = 4,
    SNX_AUDREN_EFFECT_I3DL2 = 5,
    SNX_AUDREN_EFFECT_BIQUAD = 6,
    SNX_AUDREN_EFFECT_LIMITER = 7,
    SNX_AUDREN_EFFECT_CAPTURE = 8,
    SNX_AUDREN_EFFECT_COMPRESSOR = 9
};
/* an effect's parameter state: the game sets it, a rendered frame moves it to updated */
enum { SNX_AUDREN_PARAM_INITIALIZED = 0, SNX_AUDREN_PARAM_UPDATING = 1, SNX_AUDREN_PARAM_UPDATED = 2 };
enum { SNX_AUDREN_SINK_DEVICE = 1, SNX_AUDREN_SINK_CIRCULAR = 2 };
#define SNX_AUDREN_FINAL_MIX  0
#define SNX_AUDREN_UNUSED_MIX 0x7FFFFFFF

/* AudioRendererParameter (OpenAudioRenderer's 0x34 bytes, host numbers as the IPC layer gives its raw arguments) as
   the state's sizes. NULL when they are out of range */
ARState* audren_state_new(const u8* params);
void audren_state_free(ARState* s);
/* save states: the state as audren_state_size bytes with no host pointer in them (pack), and a new state from such
   bytes (unpack, NULL when they don't hold one). Effects' lines start again empty */
u64 audren_state_size(const ARState* s);
void audren_state_pack(const ARState* s, u8* out);
ARState* audren_state_unpack(const u8* in, u64 size);
/* RequestUpdate's input into the state, and its output written: 0, or the result code to return */
int audren_state_update(ARState* s, const u8* in, u64 in_size, u8* out, u64 out_size);
/* one frame. The device sink's first two channels go to stereo (frame samples, interleaved); returns 1 when a
   device sink is in use and stereo holds its output, 0 when there is none */
int audren_frame(ARState* s, i16* stereo);
/* ---- HLE state in g_hle (hle.h) */
/* the renderers open at once: the console's audren:u has two sessions (Eden's MaxRendererSessions), with room for a
   game that closes one late */
#define SNX_AUDREN_MAX 8
/* the compressors' states: an effect each, of the 0x100 a renderer has at most (audren_state_new) */
#define SNX_AUDREN_COMPRESSORS (0x100 * SNX_AUDREN_MAX)
/* the compressor's state (audren_dsp.c), used while it is in its pool */
typedef struct {
    u32 used;
    float detector, held, gain, knee, knee_lo, knee_hi, rate, out;
} ARCompressor;
/* the pool of the compressors' states, guarded by the lock every update and frame run under (audren.c's) */
typedef struct {
    ARCompressor compressors[SNX_AUDREN_COMPRESSORS];
    u32 compressor_next;
} HleAudrenDsp;
#endif
