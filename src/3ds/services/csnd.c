/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* csnd:SND, the CSND sound channels, ported from Azahar's core/hle/service/csnd/csnd_snd.cpp and csnd_snd.h at
   azahar-emu/azahar 6e42de8: each session (up to four) gets its shared memory and mutex at Initialize, and the command
   lists the game writes there drive the CSND hardware (audio/csnd.c): channels configured, started, paused and
   stopped, with Azahar's volume fades (40 steps, four a 1.666 ms tick) before a pause or a stop, and the state the game
   reads written back. While a session is initialized or a channel plays, the hardware renders a frame each DSP frame
   time, the commands landing at the sample they come at, and the frame is mixed into the DSP's output (audio/output.c)
   as Azahar's DspInterface mixes it */
#include "3ds/services/csnd.h"
#include "3ds/audio/output.h"
#include "3ds/runtime/internal.h"
#include "common/utilities/endian.h"
#include <string.h>

#define CSND_INVALID_STATE       0xc8a0b7ffu /* InvalidResultValue, CSND, InvalidState, Status */
#define CSND_ALREADY_INITIALIZED 0xe0a0183bu /* 59, OS, InvalidState, Usage */
#define CSND_INVALID_ADDRESS     0xe0e3fff6u /* InvalidPointer, Common, InvalidArgument, Usage */
#define CLIENT_CHANNELS          0xffffff00u /* the channels a client may use: 8 to 31 */
#define LIST_END                 0xffffu
#define COMMAND_SIZE             0x20u
#define CHANNEL_STATE_SIZE       0xcu
#define CAPTURE_STATE_SIZE       8u
#define FADE_STEPS               40
#define FADE_STEPS_PER_TICK      4u
#define FADE_TICK_NS             1666000u
#define FADE_STEP_NS             200000u
#define MAX_VOLUME               0x8000
enum {
    SET_PLAY_STATE_RAW = 0x000,
    SET_PLAY_STATE = 0x001,
    SET_ENCODING = 0x002,
    SET_LOOP_BLOCK = 0x003,
    SET_LOOP_MODE = 0x004,
    SET_HOLD = 0x005,
    SET_LINEAR_INTERPOLATION = 0x006,
    SET_TIMER = 0x008,
    SET_VOLUME = 0x009,
    SET_BLOCK = 0x00a,
    SET_ADPCM_STATE = 0x00b,
    SET_LOOP_ADPCM_STATE = 0x00c,
    SET_LOOP_ADPCM_RELOAD = 0x00d,
    CONFIGURE_CHANNEL = 0x00e,
    SET_CAPTURE_ENABLE = 0x100,
    SET_CAPTURE_ONE_SHOT = 0x101,
    SET_CAPTURE_FORMAT = 0x102,
    SET_CAPTURE_BIT2 = 0x103,
    SET_CAPTURE_TIMER = 0x104,
    SET_CAPTURE_BUFFER = 0x105,
    CONFIGURE_CAPTURE = 0x106,
    APPLY_MASKS = 0x200,
    UPDATE_STATE = 0x300
};

static void reply(SnxCtrIpc* c, unsigned normal, unsigned translated, uint32_t result) {
    memset(c->out + 1, 0, (normal + translated) * sizeof *c->out);
    c->out[0] = (c->in[0] & 0xffff0000u) | normal << 6 | translated;
    c->out[1] = result;
}
/* the session's client: its slot, claimed at its first command; slots of sessions gone are free again */
static SnxCtrCsndClient* client_of(SnxCtrIpc* c) {
    SnxCtrProcess* p = c->process;
    SnxCtrCsnd* cs = &p->csnd;
    SnxCtrCsndClient* free_slot = NULL;
    for (unsigned i = 0; i < SNX_CTR_CSND_CLIENTS; ++i) {
        SnxCtrCsndClient* k = &cs->clients[i];
        if (k->session && (p->objects[k->session].type != CTR_OBJ_SESSION ||
                           p->objects[k->session].data.session.service != SNX_CTR_SERVICE_CSND)) {
            if (k->mutex) { /* Azahar's ClientDisconnected */
                --cs->initialized_clients;
                snx_ctr_object_release(p, k->mutex);
                snx_ctr_object_release(p, k->shared);
            }
            for (unsigned u = 0; u < SNX_CTR_CSND_CAPTURES; ++u)
                if (k->capture_mask >> u & 1) cs->capture_in_use[u] = 0;
            *k = (SnxCtrCsndClient){0};
        }
        if (k->session == c->object) return k;
        if (!k->session && !free_slot) free_slot = k;
    }
    if (free_slot) free_slot->session = c->object;
    return free_slot;
}
static uint32_t shared_physical(const SnxCtrProcess* p, const SnxCtrCsndClient* k) {
    return p->objects[k->shared].data.shared.physical;
}
static uint32_t shared_size(const SnxCtrProcess* p, const SnxCtrCsndClient* k) {
    return k->shared ? p->objects[k->shared].data.shared.size : 0;
}
/* Azahar's WriteShared: nothing when it would pass the block's end */
static void write_shared(SnxCtrProcess* p, const SnxCtrCsndClient* k, uint32_t offset, const void* bytes, uint32_t n) {
    uint32_t size = shared_size(p, k);
    if (!k->shared || offset > size || size - offset < n) return;
    (void)snx_ctr_phys_write(p->memory, shared_physical(p, k) + offset, bytes, n);
}
static void write32(SnxCtrProcess* p, const SnxCtrCsndClient* k, uint32_t offset, uint32_t v) {
    unsigned char b[4];
    snx_st_le32(b, v);
    write_shared(p, k, offset, b, 4);
}

/* ---- the frame the hardware renders, and the fades ---- */
/* Azahar's UpdateHardware: the frame rendered up to now, so a command lands at the sample it comes at */
static void update_hardware(SnxCtrProcess* p) {
    SnxCtrCsnd* cs = &p->csnd;
    if (!cs->next_frame_ns) return;
    uint64_t now = snx_ctr_now_ns(), elapsed = now > cs->frame_start_ns ? now - cs->frame_start_ns : 0;
    uint64_t scaled = elapsed * SNX_CTR_DSP_SAMPLES / SNX_CTR_DSP_FRAME_NS;
    if (scaled > SNX_CTR_DSP_SAMPLES) scaled = SNX_CTR_DSP_SAMPLES;
    if (scaled > cs->frame_samples) {
        snx_ctr_csnd_render(&cs->hw, p->memory, cs->frame + cs->frame_samples, (unsigned)scaled - cs->frame_samples);
        cs->frame_samples = (uint32_t)scaled;
    }
}
static void start_ticking(SnxCtrProcess* p) {
    SnxCtrCsnd* cs = &p->csnd;
    if (cs->next_frame_ns) return;
    cs->frame_samples = 0;
    cs->frame_start_ns = snx_ctr_now_ns();
    cs->next_frame_ns = cs->frame_start_ns + SNX_CTR_DSP_FRAME_NS;
}
/* Azahar's AudioTickCallback: the rest of the frame rendered and mixed into the output, the next one scheduled while
   a session is initialized or a channel plays */
static void tick(SnxCtrProcess* p, uint64_t due, uint64_t now) {
    SnxCtrCsnd* cs = &p->csnd;
    if (cs->frame_samples < SNX_CTR_DSP_SAMPLES)
        snx_ctr_csnd_render(&cs->hw, p->memory, cs->frame + cs->frame_samples, SNX_CTR_DSP_SAMPLES - cs->frame_samples);
    snx_ctr_audio_csnd_frame((const int16_t (*)[2])cs->frame);
    cs->frame_samples = 0;
    if (!cs->initialized_clients && !snx_ctr_csnd_busy(&cs->hw)) {
        cs->next_frame_ns = 0;
        return;
    }
    /* one more than a frame behind the host (a slow slice) starts again from now, as hid's samples do */
    cs->frame_start_ns = due + SNX_CTR_DSP_FRAME_NS < now ? now : due;
    cs->next_frame_ns = cs->frame_start_ns + SNX_CTR_DSP_FRAME_NS;
}
static bool running(const SnxCtrCsnd* cs, unsigned channel) {
    uint16_t both = SNX_CTR_CSND_ENABLE | SNX_CTR_CSND_RUN;
    return (cs->hw.channel[channel].cnt & both) == both;
}
static void write_current_volume(SnxCtrCsnd* cs, unsigned channel) {
    snx_ctr_csnd_set_volume(&cs->hw, channel, (uint16_t)cs->volumes[channel].current[0],
                            (uint16_t)cs->volumes[channel].current[1]);
}
static void start_fade(SnxCtrProcess* p) {
    if (p->csnd.next_fade_ns) return;
    p->csnd.fade_step = 0;
    p->csnd.next_fade_ns = snx_ctr_now_ns(); /* at once */
}
static void set_target_volume(SnxCtrProcess* p, unsigned channel, uint16_t left, uint16_t right, bool fade) {
    SnxCtrCsndVolume* v = &p->csnd.volumes[channel];
    const uint16_t target[2] = {left, right};
    for (unsigned side = 0; side < 2; ++side) {
        v->step[side] = ((int32_t)target[side] - v->current[side]) / FADE_STEPS;
        v->target[side] = target[side];
        if (!v->step[side]) v->current[side] = target[side];
    }
    p->csnd.fading |= 1u << channel;
    if (fade) start_fade(p);
}
static void set_requested_volume(SnxCtrProcess* p, unsigned channel, uint16_t left, uint16_t right, bool fade) {
    p->csnd.volumes[channel].requested[0] = left;
    p->csnd.volumes[channel].requested[1] = right;
    set_target_volume(p, channel, left, right, fade);
}
static void stop_channel(SnxCtrCsnd* cs, unsigned channel) {
    uint16_t off = SNX_CTR_CSND_ENABLE | SNX_CTR_CSND_RUN | SNX_CTR_CSND_INTERPOLATE | SNX_CTR_CSND_HOLD;
    snx_ctr_csnd_set_cnt(&cs->hw, channel, cs->hw.channel[channel].cnt & (uint16_t)~off);
}
/* Azahar's FadeCallback: one step of each fading channel's volume; at a tick's last step a channel that reached its
   target stops or pauses if that waited for the fade */
static void fade(SnxCtrProcess* p, uint64_t due, uint64_t now) {
    SnxCtrCsnd* cs = &p->csnd;
    uint32_t work = (cs->fading | cs->stop_pending | cs->pause_pending) & cs->started, step = cs->fade_step;
    if (!work) {
        cs->next_fade_ns = 0;
        return;
    }
    update_hardware(p);
    uint32_t fading_now = cs->fading & cs->started;
    for (unsigned ch = 0; ch < SNX_CTR_CSND_CHANNELS; ++ch) {
        if (!(fading_now >> ch & 1)) continue;
        SnxCtrCsndVolume* v = &cs->volumes[ch];
        for (unsigned side = 0; side < 2; ++side)
            if (v->current[side] != v->target[side]) {
                int32_t next = v->current[side] + v->step[side];
                v->current[side] = next < 0 ? 0 : next > MAX_VOLUME ? MAX_VOLUME : next;
            }
    }
    if (step == FADE_STEPS_PER_TICK - 1)
        for (unsigned ch = 0; ch < SNX_CTR_CSND_CHANNELS; ++ch) {
            if (!(work >> ch & 1)) continue;
            SnxCtrCsndVolume* v = &cs->volumes[ch];
            bool reached = true;
            for (unsigned side = 0; side < 2; ++side) {
                bool done =
                    v->step[side] > 0 ? v->current[side] >= v->target[side] : v->current[side] <= v->target[side];
                if (done)
                    v->current[side] = v->target[side];
                else
                    reached = false;
            }
            if (!reached) continue;
            cs->fading &= ~(1u << ch);
            if (cs->stop_pending >> ch & 1) {
                stop_channel(cs, ch);
                cs->stop_pending &= ~(1u << ch);
                cs->started &= ~(1u << ch);
            }
            if (cs->pause_pending >> ch & 1) {
                snx_ctr_csnd_set_cnt(&cs->hw, ch, cs->hw.channel[ch].cnt & (uint16_t)~SNX_CTR_CSND_RUN);
                cs->pause_pending &= ~(1u << ch);
                cs->started &= ~(1u << ch);
            }
        }
    for (unsigned ch = 0; ch < SNX_CTR_CSND_CHANNELS; ++ch)
        if (fading_now >> ch & 1) write_current_volume(cs, ch);
    if (!((cs->fading | cs->stop_pending | cs->pause_pending) & cs->started)) {
        cs->next_fade_ns = 0;
        return;
    }
    uint64_t wait =
        step + 1 < FADE_STEPS_PER_TICK ? FADE_STEP_NS : FADE_TICK_NS - FADE_STEP_NS * (FADE_STEPS_PER_TICK - 1);
    cs->fade_step = step + 1 < FADE_STEPS_PER_TICK ? step + 1 : 0;
    cs->next_fade_ns = (due + wait > now ? due : now) + wait;
}
void snx_ctr_csnd_update(SnxCtrProcess* p, uint64_t now_ns) {
    SnxCtrCsnd* cs = &p->csnd;
    if (cs->next_fade_ns && now_ns >= cs->next_fade_ns) fade(p, cs->next_fade_ns, now_ns);
    if (cs->next_frame_ns && now_ns >= cs->next_frame_ns) tick(p, cs->next_frame_ns, now_ns);
}
uint64_t snx_ctr_csnd_deadline(const SnxCtrProcess* p) {
    const SnxCtrCsnd* cs = &p->csnd;
    uint64_t deadline = UINT64_MAX;
    if (cs->next_frame_ns) deadline = cs->next_frame_ns;
    if (cs->next_fade_ns && cs->next_fade_ns < deadline) deadline = cs->next_fade_ns;
    return deadline;
}

/* ---- the commands a list holds ---- */
static void set_play_state_raw(SnxCtrProcess* p, unsigned channel, bool play) {
    SnxCtrCsnd* cs = &p->csnd;
    if (play)
        snx_ctr_csnd_set_cnt(&cs->hw, channel, cs->hw.channel[channel].cnt | SNX_CTR_CSND_ENABLE);
    else if (running(cs, channel)) {
        cs->stop_pending |= 1u << channel;
        set_target_volume(p, channel, 0, 0, true);
    } else {
        stop_channel(cs, channel);
        cs->volumes[channel].current[0] = cs->volumes[channel].current[1] = 0;
        cs->started &= ~(1u << channel);
    }
}
static void set_play_state(SnxCtrProcess* p, unsigned channel, bool play) {
    SnxCtrCsnd* cs = &p->csnd;
    SnxCtrCsndVolume* v = &cs->volumes[channel];
    if (play) {
        cs->started |= 1u << channel;
        v->current[0] = v->requested[0];
        v->current[1] = v->requested[1];
        write_current_volume(cs, channel);
        snx_ctr_csnd_set_cnt(&cs->hw, channel, cs->hw.channel[channel].cnt | SNX_CTR_CSND_ENABLE | SNX_CTR_CSND_RUN);
    } else if (running(cs, channel)) {
        cs->pause_pending |= 1u << channel;
        set_target_volume(p, channel, 0, 0, true);
    } else {
        snx_ctr_csnd_set_cnt(&cs->hw, channel, cs->hw.channel[channel].cnt & (uint16_t)~SNX_CTR_CSND_RUN);
        v->current[0] = v->current[1] = 0;
        cs->started &= ~(1u << channel);
    }
}
/* the state the game reads: the masks applied, each of its channels' (active, ADPCM predictor and index), and its
   capture units' */
static void update_state(SnxCtrProcess* p, const SnxCtrCsndClient* k) {
    SnxCtrCsnd* cs = &p->csnd;
    write32(p, k, k->master_state_offset, cs->hw.channel_mask & k->channel_mask);
    write32(p, k, k->master_state_offset + 4, cs->hw.channel_mask & k->capture_mask);
    uint32_t index = 0;
    for (unsigned ch = 0; ch < SNX_CTR_CSND_CHANNELS; ++ch) {
        if (!(k->channel_mask >> ch & 1)) continue;
        uint32_t adpcm = snx_ctr_csnd_adpcm_state(&cs->hw, ch), offset = k->channel_state_offset + index++ * 0xc;
        unsigned char active = running(cs, ch), predictor[2], adpcm_index = (unsigned char)(adpcm >> 16 & 0x7f);
        snx_st_le16(predictor, (uint16_t)(adpcm & 0xffff));
        write_shared(p, k, offset, &active, 1);
        write_shared(p, k, offset + 4, predictor, 2);
        write_shared(p, k, offset + 6, &adpcm_index, 1);
        write32(p, k, offset + 8, 0);
    }
    for (unsigned unit = 0; unit < SNX_CTR_CSND_CAPTURES; ++unit) {
        if (!(k->capture_mask >> unit & 1)) continue;
        unsigned char zero = 0;
        write_shared(p, k, k->capture_state_offset + unit * CAPTURE_STATE_SIZE, &zero, 1);
        write32(p, k, k->capture_state_offset + unit * CAPTURE_STATE_SIZE + 4, 0);
    }
}
/* a channel's place among the client's channels, whose states follow each other */
static uint32_t channel_index(const SnxCtrCsndClient* k, unsigned channel) {
    uint32_t index = 0;
    for (unsigned i = 0; i < channel; ++i) index += k->channel_mask >> i & 1;
    return index;
}
static void set_cnt_bits(SnxCtrCsnd* cs, unsigned ch, uint16_t mask, uint16_t bits) {
    snx_ctr_csnd_set_cnt(&cs->hw, ch, (uint16_t)((cs->hw.channel[ch].cnt & ~mask) | bits));
}
static void set_capture_bits(SnxCtrCsnd* cs, unsigned unit, uint16_t mask, uint16_t bits) {
    cs->hw.capture[unit].cnt = (uint16_t)((cs->hw.capture[unit].cnt & ~mask) | bits);
}
static void execute(SnxCtrProcess* p, SnxCtrCsndClient* k, const unsigned char* command) {
    SnxCtrCsnd* cs = &p->csnd;
    SnxCtrCsndChannel* hwc;
    uint32_t id = snx_ld_le16(command + 2), params[6];
    for (unsigned i = 0; i < 6; ++i) params[i] = snx_ld_le32(command + 8 + i * 4);
    unsigned ch = params[0] & 0x1f;
    uint32_t value = params[1];
    if (id < 0x100 && !(k->channel_mask >> ch & 1)) return;
    if (id >= 0x100 && id < 0x200 && ch >= SNX_CTR_CSND_CAPTURES) return;
    hwc = &cs->hw.channel[ch];
    bool busy = hwc->cnt & SNX_CTR_CSND_ENABLE;
    switch (id) {
        case SET_PLAY_STATE_RAW: set_play_state_raw(p, ch, value != 0); break;
        case SET_PLAY_STATE: set_play_state(p, ch, value != 0); break;
        case SET_ENCODING:
            if (!busy)
                set_cnt_bits(cs, ch, SNX_CTR_CSND_FORMAT_MASK, (uint16_t)((value & 0xff) << SNX_CTR_CSND_FORMAT_SHIFT));
            break;
        case SET_LOOP_BLOCK:
            hwc->loop = value;
            hwc->size = params[2] & 0x7ffffff;
            break;
        case SET_LOOP_MODE:
            if (!busy)
                set_cnt_bits(cs, ch, SNX_CTR_CSND_LOOP_MASK, (uint16_t)((value & 0xff) << SNX_CTR_CSND_LOOP_SHIFT));
            break;
        case SET_HOLD: set_cnt_bits(cs, ch, SNX_CTR_CSND_HOLD, value ? SNX_CTR_CSND_HOLD : 0); break;
        case SET_LINEAR_INTERPOLATION:
            if (!busy) set_cnt_bits(cs, ch, SNX_CTR_CSND_INTERPOLATE, value ? SNX_CTR_CSND_INTERPOLATE : 0);
            break;
        case SET_TIMER: hwc->timer = (uint16_t)(0x10000u - value); break;
        case SET_VOLUME:
            set_requested_volume(p, ch, (uint16_t)value, (uint16_t)(value >> 16), true);
            snx_ctr_csnd_set_capture_volume(&cs->hw, ch, (uint16_t)params[2], (uint16_t)(params[2] >> 16));
            break;
        case SET_BLOCK:
            hwc->start = value;
            hwc->size = params[2] & 0x7ffffff;
            /* the channel's state word 2 cleared, at its index among the client's channels */
            write32(p, k, k->channel_state_offset + channel_index(k, ch) * CHANNEL_STATE_SIZE + 8, 0);
            break;
        case SET_ADPCM_STATE:
            hwc->adpcm_state = (value & 0xffff) | (params[2] & 0xff) << 16;
            hwc->loop_adpcm_state = 0;
            break;
        case SET_LOOP_ADPCM_STATE:
            hwc->loop_adpcm_state = (value & 0xffff) | (params[2] & 0xff) << 16 | SNX_CTR_CSND_LOOP_RELOAD;
            break;
        case SET_LOOP_ADPCM_RELOAD:
            hwc->loop_adpcm_state =
                (hwc->loop_adpcm_state & ~SNX_CTR_CSND_LOOP_RELOAD) | (value ? SNX_CTR_CSND_LOOP_RELOAD : 0);
            break;
        case CONFIGURE_CHANNEL: { /* params[0]: channel 0-4, interpolation 6, loop 10-11, format 12-13, play 14,
                                     timer 16-31 */
            uint32_t flags = params[0], format = flags >> 12 & 3;
            snx_ctr_csnd_set_cnt(&cs->hw, ch, 0);
            hwc->timer = (uint16_t)(0x10000u - (flags >> 16));
            snx_ctr_csnd_set_volume(&cs->hw, ch, 0, 0);
            snx_ctr_csnd_set_capture_volume(&cs->hw, ch, (uint16_t)params[2], (uint16_t)(params[2] >> 16));
            hwc->loop = params[4];
            hwc->size = params[5] & 0x7ffffff;
            hwc->start = params[3];
            if (format != 2) hwc->adpcm_state = hwc->loop_adpcm_state = 0;
            snx_ctr_csnd_set_cnt(&cs->hw, ch,
                                 (uint16_t)(format << SNX_CTR_CSND_FORMAT_SHIFT |
                                            (flags >> 10 & 3) << SNX_CTR_CSND_LOOP_SHIFT |
                                            (flags >> 6 & 1 ? SNX_CTR_CSND_INTERPOLATE : 0)));
            set_requested_volume(p, ch, (uint16_t)value, (uint16_t)(value >> 16), false);
            set_play_state(p, ch, (flags >> 14 & 1) != 0);
            write32(p, k, k->channel_state_offset + channel_index(k, ch) * CHANNEL_STATE_SIZE + 8, 0);
            break;
        }
        case SET_CAPTURE_ENABLE:
            set_capture_bits(cs, ch, SNX_CTR_CSND_CAPTURE_ENABLE, value ? SNX_CTR_CSND_CAPTURE_ENABLE : 0);
            break;
        case SET_CAPTURE_ONE_SHOT: set_capture_bits(cs, ch, 1, (uint16_t)(value & 0xff)); break;
        case SET_CAPTURE_FORMAT: set_capture_bits(cs, ch, 2, (uint16_t)((value & 0xff) << 1)); break;
        case SET_CAPTURE_BIT2: set_capture_bits(cs, ch, 4, value ? 4 : 0); break;
        case SET_CAPTURE_TIMER: cs->hw.capture[ch].timer = (uint16_t)(0x10000u - value); break;
        case SET_CAPTURE_BUFFER:
            cs->hw.capture[ch].address = value;
            cs->hw.capture[ch].size = params[2];
            write32(p, k, k->capture_state_offset + ch * CAPTURE_STATE_SIZE + 4, 0);
            break;
        case CONFIGURE_CAPTURE:
            cs->hw.capture[ch].timer = (uint16_t)(0x10000u - (value >> 16));
            cs->hw.capture[ch].address = params[2];
            cs->hw.capture[ch].size = params[3];
            cs->hw.capture[ch].cnt = (uint16_t)(value & (SNX_CTR_CSND_CAPTURE_ENABLE | 7));
            write32(p, k, k->capture_state_offset + ch * CAPTURE_STATE_SIZE + 4, 0);
            break;
        case APPLY_MASKS:
            cs->hw.channel_mask = k->channel_mask;
            cs->hw.capture_mask = k->capture_mask;
            break;
        case UPDATE_STATE: update_state(p, k); break;
        default: break; /* PSG, noise and unknown commands: nothing, as in Azahar */
    }
}
/* ExecuteCommands(first offset): the list from there, each command's next offset to the next, to 0xffff, at most as
   many as the block holds; the first one's finished byte set */
static void execute_commands(SnxCtrIpc* c, SnxCtrCsndClient* k) {
    SnxCtrProcess* p = c->process;
    uint32_t first = c->in[1], size = shared_size(p, k);
    if (!k->shared) {
        reply(c, 1, 0, CSND_INVALID_STATE);
        return;
    }
    update_hardware(p);
    uint64_t most = size / COMMAND_SIZE, executed = 0;
    for (uint32_t offset = first; offset != LIST_END;) {
        unsigned char command[COMMAND_SIZE];
        if ((uint64_t)offset + COMMAND_SIZE > size || executed++ >= most) break;
        if (snx_ctr_phys_read(p->memory, shared_physical(p, k) + offset, command, sizeof command) != SNX_CTR_MEM_OK)
            break;
        execute(p, k, command);
        offset = snx_ld_le16(command);
    }
    if ((uint64_t)first + COMMAND_SIZE <= size) {
        unsigned char one = 1;
        write_shared(p, k, first + 4, &one, 1);
    }
    reply(c, 1, 0, 0);
}
/* Initialize(size, master, channel, capture and direct sound state offsets): the shared memory, zeroed, and a mutex,
   both given back as copied handles */
static void initialize(SnxCtrIpc* c, SnxCtrCsndClient* k) {
    SnxCtrProcess* p = c->process;
    if (k->mutex) {
        reply(c, 1, 0, CSND_ALREADY_INITIALIZED);
        return;
    }
    uint32_t size = c->in[1] ? (c->in[1] + 4095u) & ~4095u : 4096u, physical = snx_ctr_shared_pages(p, size);
    uint32_t mutex = 0, shared = 0, result = physical ? 0 : 0xd86007f3u;
    if (!result) result = snx_ctr_object_create(p, CTR_OBJ_MUTEX, &mutex);
    if (!result) result = snx_ctr_object_create(p, CTR_OBJ_SHARED, &shared);
    if (result) {
        if (mutex) (void)snx_ctr_handle_close(p, mutex);
        snx_ctr_ipc_error_reply(c, result);
        return;
    }
    k->mutex = snx_ctr_handle_object(p, mutex);
    k->shared = snx_ctr_handle_object(p, shared);
    ++p->objects[k->mutex].refs; /* the service's own while the client is initialized */
    ++p->objects[k->shared].refs;
    CtrObject* block = &p->objects[k->shared];
    block->data.shared.physical = physical;
    block->data.shared.size = size;
    block->data.shared.permissions = SNX_CTR_READ | SNX_CTR_WRITE;
    block->data.shared.other_permissions = SNX_CTR_READ | SNX_CTR_WRITE;
    static const unsigned char zeros[4096];
    for (uint32_t at = 0; at < size; at += sizeof zeros)
        (void)snx_ctr_phys_write(p->memory, physical + at, zeros, sizeof zeros);
    k->master_state_offset = c->in[2];
    k->channel_state_offset = c->in[3];
    k->capture_state_offset = c->in[4];
    k->direct_sound_offset = c->in[5];
    ++p->csnd.initialized_clients;
    start_ticking(p);
    reply(c, 1, 3, 0);
    c->out[2] = 1u << 26; /* two copied handles */
    snx_ctr_ipc_output_handle(c, 3, mutex);
    snx_ctr_ipc_output_handle(c, 4, shared);
    if (c->state != SNX_CTR_IPC_RETURN) {
        snx_ctr_ipc_drop_handle(c, mutex);
        snx_ctr_ipc_drop_handle(c, shared);
    }
}
void snx_ctr_csnd_dispatch(SnxCtrIpc* c) {
    SnxCtrProcess* p = c->process;
    SnxCtrCsnd* cs = &p->csnd;
    SnxCtrCsndClient* k = client_of(c);
    uint32_t command = c->in[0] >> 16;
    if (!k) { /* more sessions than four: the session limit keeps this from happening */
        snx_ctr_ipc_error_reply(c, CSND_INVALID_STATE);
        return;
    }
    switch (command) {
        case 0x01:
            if (!snx_ctr_ipc_expect(c, 5, 0)) return;
            initialize(c, k);
            break;
        case 0x02: /* Shutdown */
            if (!snx_ctr_ipc_expect(c, 0, 0)) return;
            if (k->mutex) {
                --cs->initialized_clients;
                snx_ctr_object_release(p, k->mutex);
                snx_ctr_object_release(p, k->shared);
            }
            k->mutex = k->shared = 0;
            reply(c, 1, 0, 0);
            break;
        case 0x03:
            if (!snx_ctr_ipc_expect(c, 1, 0)) return;
            execute_commands(c, k);
            break;
        case 0x05: /* AcquireSoundChannels */
            if (!snx_ctr_ipc_expect(c, 0, 0)) return;
            k->channel_mask = CLIENT_CHANNELS;
            reply(c, 2, 0, 0);
            c->out[2] = k->channel_mask;
            break;
        case 0x06: /* ReleaseSoundChannels */
            if (!snx_ctr_ipc_expect(c, 0, 0)) return;
            k->channel_mask = 0;
            reply(c, 1, 0, 0);
            break;
        case 0x07: /* AcquireCapUnit: the first free unit, its state zeroed */
            if (!snx_ctr_ipc_expect(c, 0, 0)) return;
            {
                unsigned unit = 0;
                while (unit < SNX_CTR_CSND_CAPTURES && cs->capture_in_use[unit]) ++unit;
                if (unit == SNX_CTR_CSND_CAPTURES) {
                    reply(c, 2, 0, CSND_INVALID_STATE);
                    c->out[2] = 0xffffffffu;
                    break;
                }
                cs->capture_in_use[unit] = 1;
                k->capture_mask |= (uint8_t)(1u << unit);
                unsigned char zero = 0;
                write_shared(p, k, k->capture_state_offset + unit * CAPTURE_STATE_SIZE, &zero, 1);
                write32(p, k, k->capture_state_offset + unit * CAPTURE_STATE_SIZE + 4, 0);
                reply(c, 2, 0, 0);
                c->out[2] = unit;
            }
            break;
        case 0x08: /* ReleaseCapUnit(unit) */
            if (!snx_ctr_ipc_expect(c, 1, 0)) return;
            if (c->in[1] >= SNX_CTR_CSND_CAPTURES || !(k->capture_mask >> c->in[1] & 1)) {
                reply(c, 1, 0, CSND_INVALID_STATE);
                break;
            }
            k->capture_mask &= (uint8_t)~(1u << c->in[1]);
            cs->capture_in_use[c->in[1]] = 0;
            reply(c, 1, 0, 0);
            break;
        case 0x09: /* FlushDataCache */
        case 0x0a: /* StoreDataCache */
        case 0x0b: /* InvalidateDataCache(address, size, process): within the linear heap or the FCRAM's mirror */
            if (!snx_ctr_ipc_expect(c, 2, 2)) return;
            {
                uint32_t address = c->in[1], end = address + c->in[2];
                bool heap =
                    0x14000000u <= address && address <= 0x1c000000u && 0x14000000u <= end && end <= 0x1c000000u;
                bool mirror =
                    0x30000000u <= address && address <= 0x40000000u && 0x30000000u <= end && end <= 0x40000000u;
                reply(c, 1, 0, heap || mirror ? 0 : CSND_INVALID_ADDRESS);
            }
            break;
        case 0x0c: /* Reset: every channel stopped at volume 0, no fade or stop left */
            if (!snx_ctr_ipc_expect(c, 0, 0)) return;
            update_hardware(p);
            for (unsigned ch = 0; ch < SNX_CTR_CSND_CHANNELS; ++ch) {
                stop_channel(cs, ch);
                cs->volumes[ch].current[0] = cs->volumes[ch].current[1] = 0;
                write_current_volume(cs, ch);
            }
            cs->started = cs->fading = cs->stop_pending = cs->pause_pending = 0;
            reply(c, 1, 0, 0);
            break;
        default: c->state = SNX_CTR_IPC_UNKNOWN; break; /* PlaySoundDirectly has no handler in Azahar either */
    }
}
