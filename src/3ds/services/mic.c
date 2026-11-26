/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* mic:u, the microphone, as Azahar answers (core/hle/service/mic/mic_u.cpp and mic_u.h at azahar-emu/azahar 6e42de8):
   the commands, their replies and the settings a game stores are Azahar's, and no command fails but one the console
   cannot have been sent (an encoding or a rate past the four, a block size past the block's), which stops the run with
   its words. While it samples, the host's microphone (the microphone and mic_noise_reduction settings; off, or no
   device, records silence) fills the part of the block StartSampling names, 15 samples at a time as the console writes
   them (the host waking for them at most about once a millisecond), in the encoding asked (unsigned is offset by half
   the range), and the block's last word holds the offset after the last sample, from the part's start. A looping part
   starts again at its end, one that does not stays full. The buffer full event is
   signalled each time the part fills, as libctru's MICU_GetEventHandle says (Azahar never signals it). The gain,
   power and clamp a game sets are kept and read back, but change no sample, as in Azahar: the host's microphone has
   its own gain */
#include "3ds/services/mic.h"
#include "3ds/runtime/internal.h"
#include "common/utilities/mic.h"
#include "snx_settings.h"
#include <stdio.h>

#define MIC_UNIT 15 /* samples the console writes at a time, whatever the rate */
/* the samples of the most units one update writes: 100 ms at the highest rate. One that is later than 100 ms (a stalled
   slice, a pause) writes the last 100 ms owed */
#define MIC_MOST (32728 / 10 / MIC_UNIT * MIC_UNIT)

/* StartSampling's rate, by its number: (16756991 / 512) / (number + 1) Hz, rounded as Azahar's */
static uint32_t rate_hz(uint8_t number) {
    static const uint32_t hz[4] = {32728, 16364, 10909, 8182};
    return hz[number & 3];
}
static uint64_t unit_ns(const SnxCtrMic* m) { return MIC_UNIT * 1000000000ull / rate_hz(m->sample_rate); }
/* the units of an update: as many as make a millisecond, so the host wakes no more often for them */
static uint64_t units_per_wake(const SnxCtrMic* m) {
    uint64_t unit = unit_ns(m);
    return (1000000u + unit - 1) / unit;
}
/* the recording's lines, on the terminal */
static void mic_say(bool warn, const char* line) {
    (void)warn;
    fprintf(stderr, "3ds %s", line);
}
static void recording_open(SnxCtrMic* m) {
    snx_mic_free(m->recording);
    m->recording =
        snx_mic_open(rate_hz(m->sample_rate), g_settings.microphone, &g_settings.microphone_noise_reduction, mic_say);
}
static void recording_close(SnxCtrMic* m) {
    snx_mic_free(m->recording);
    m->recording = NULL;
}

static void reply(SnxCtrIpc* c, unsigned normal, unsigned translated) {
    c->out[0] = (c->in[0] & 0xffff0000u) | normal << 6 | translated;
    c->out[1] = 0;
}
/* a bool argument, as Azahar's Pop<bool> reads one: its word's low byte */
static uint8_t flag(uint32_t word) { return (word & 0xff) != 0; }
/* the buffer full event, made at the first request for it and kept while the process lives */
static void buffer_full_event(SnxCtrIpc* c) {
    if (!snx_ctr_ipc_expect(c, 0, 0)) return;
    SnxCtrProcess* p = c->process;
    uint32_t handle = 0, result;
    if (!p->mic.event) {
        result = snx_ctr_object_create(p, CTR_OBJ_EVENT, &handle);
        if (!result) {
            p->mic.event = snx_ctr_handle_object(p, handle);
            ++p->objects[p->mic.event].refs; /* the service's own */
        }
    } else
        result = snx_ctr_handle_add(p, p->mic.event, &handle);
    if (result) {
        snx_ctr_ipc_error_reply(c, result);
        return;
    }
    reply(c, 1, 2);
    c->out[2] = 0; /* one copied handle */
    snx_ctr_ipc_output_handle(c, 3, handle);
    if (c->state != SNX_CTR_IPC_RETURN) snx_ctr_ipc_drop_handle(c, handle);
}
/* the samples, encoded, at the part's next place: a part that fills starts again (looping) or stays full, and the event
   is signalled. The block's last word then holds the offset. Nothing without a block, or with a part past it */
static void write_samples(SnxCtrProcess* p, const uint8_t* bytes, uint32_t total) {
    SnxCtrMic* m = &p->mic;
    uint32_t va = m->shared ? snx_ctr_shared_va(p, m->shared) : 0;
    if (!va || m->shared_size < 4 || m->start > m->shared_size - 4) return;
    /* a part never reaches the block's last word, as Azahar keeps it */
    uint32_t room = m->size < m->shared_size - 4 - m->start ? m->size : m->shared_size - 4 - m->start;
    bool full = false;
    for (uint32_t done = 0; done < total && m->offset < room;) {
        uint32_t n = total - done < room - m->offset ? total - done : room - m->offset;
        (void)snx_ctr_write_behind(p->space, p->memory, va + m->start + m->offset, bytes + done, n);
        m->offset += n;
        done += n;
        if (m->offset == room) {
            full = true;
            if (m->loop) m->offset = 0;
        }
    }
    uint8_t offset[4] = {(uint8_t)m->offset, (uint8_t)(m->offset >> 8), (uint8_t)(m->offset >> 16),
                         (uint8_t)(m->offset >> 24)};
    (void)snx_ctr_write_behind(p->space, p->memory, va + m->shared_size - 4, offset, sizeof offset);
    if (full && m->event) {
        p->objects[m->event].data.event.signaled = 1;
        snx_ctr_wake_waiters(p, m->event);
    }
}
void snx_ctr_mic_update(SnxCtrProcess* p, uint64_t now_ns) {
    SnxCtrMic* m = &p->mic;
    if (!m->next_ns || now_ns < m->next_ns) return;
    uint64_t unit = unit_ns(m), units = 1 + (now_ns - m->next_ns) / unit,
             most = rate_hz(m->sample_rate) / 10 / MIC_UNIT;
    m->next_ns += units * unit;
    if (units > most) units = most;
    int16_t pcm[MIC_MOST];
    uint8_t bytes[MIC_MOST * 2];
    uint32_t n = (uint32_t)units * MIC_UNIT, wide = m->encoding & 1; /* PCM16 and PCM16 signed */
    snx_mic_take(m->recording, pcm, n);
    for (uint32_t i = 0; i < n; ++i) {
        uint16_t v = (uint16_t)pcm[i] ^ (m->encoding & 2 ? 0 : 0x8000u); /* PCM8 and PCM16 are unsigned */
        if (wide)
            bytes[i * 2] = (uint8_t)v, bytes[i * 2 + 1] = (uint8_t)(v >> 8);
        else
            bytes[i] = (uint8_t)(v >> 8);
    }
    write_samples(p, bytes, n << wide);
}
uint64_t snx_ctr_mic_deadline(const SnxCtrProcess* p) {
    const SnxCtrMic* m = &p->mic;
    return m->next_ns ? m->next_ns + (units_per_wake(m) - 1) * unit_ns(m) : UINT64_MAX;
}
void snx_ctr_mic_reopen(SnxCtrProcess* p) {
    recording_close(&p->mic);
    if (p->mic.sampling) recording_open(&p->mic);
}
void snx_ctr_mic_close(SnxCtrProcess* p) { recording_close(&p->mic); }
/* the block a game lends, retained until it is unmapped or another one is mapped: Azahar maps any it is given, and
   answers success for a handle that is not a block. A size past the block's would have the samples written past it,
   into the game's other memory */
static void map_shared(SnxCtrIpc* c) {
    if (!snx_ctr_ipc_expect(c, 1, 2)) return;
    SnxCtrProcess* p = c->process;
    uint32_t object = c->handle_count == 1 ? snx_ctr_handle_object(p, c->handle_in[0]) : 0;
    bool block = object && p->objects[object].type == CTR_OBJ_SHARED;
    if (block && c->in[1] > p->objects[object].data.shared.size) {
        c->state = SNX_CTR_IPC_UNSUPPORTED;
        return;
    }
    if (block) {
        if (p->mic.shared) snx_ctr_object_release(p, p->mic.shared);
        p->mic.shared = object;
        ++p->objects[object].refs; /* the service's own */
        p->mic.shared_size = c->in[1];
    }
    reply(c, 1, 0);
}
static void unmap_shared(SnxCtrProcess* p) {
    if (p->mic.shared) snx_ctr_object_release(p, p->mic.shared);
    p->mic.shared = 0;
}
void snx_ctr_mic_dispatch(SnxCtrIpc* c) {
    SnxCtrMic* mic = &c->process->mic;
    switch (c->in[0] >> 16) {
        case 0x01: map_shared(c); break; /* MapSharedMem(size, the block's handle) */
        case 0x02:                       /* UnmapSharedMem */
            if (!snx_ctr_ipc_expect(c, 0, 0)) return;
            unmap_shared(c->process);
            reply(c, 1, 0);
            break;
        case 0x03: /* StartSampling(encoding, rate, offset, size, loop): one already running is stopped first */
            if (!snx_ctr_ipc_expect(c, 5, 0)) return;
            /* no such encoding or rate, which the console's answer to is not known */
            if (c->in[1] > 3 || c->in[2] > 3) {
                c->state = SNX_CTR_IPC_UNSUPPORTED;
                return;
            }
            mic->encoding = (uint8_t)c->in[1];
            mic->sample_rate = (uint8_t)c->in[2];
            mic->start = c->in[3];
            mic->size = c->in[4];
            mic->loop = flag(c->in[5]);
            mic->offset = 0;
            mic->sampling = 1;
            mic->next_ns = snx_ctr_now_ns() + unit_ns(mic);
            recording_open(mic);
            reply(c, 1, 0);
            break;
        case 0x04: /* AdjustSampling(rate): a recording at the new rate */
            if (!snx_ctr_ipc_expect(c, 1, 0)) return;
            if (c->in[1] > 3) {
                c->state = SNX_CTR_IPC_UNSUPPORTED;
                return;
            }
            mic->sample_rate = (uint8_t)c->in[1];
            if (mic->sampling) recording_open(mic);
            reply(c, 1, 0);
            break;
        case 0x05: /* StopSampling */
            if (!snx_ctr_ipc_expect(c, 0, 0)) return;
            mic->sampling = 0;
            mic->next_ns = 0;
            recording_close(mic);
            reply(c, 1, 0);
            break;
        case 0x06: /* IsSampling */
            if (!snx_ctr_ipc_expect(c, 0, 0)) return;
            reply(c, 2, 0);
            c->out[2] = mic->sampling;
            break;
        case 0x07: buffer_full_event(c); break;
        case 0x08: /* SetGain(u8) */
            if (!snx_ctr_ipc_expect(c, 1, 0)) return;
            mic->gain = (uint8_t)c->in[1];
            reply(c, 1, 0);
            break;
        case 0x09: /* GetGain */
            if (!snx_ctr_ipc_expect(c, 0, 0)) return;
            reply(c, 2, 0);
            c->out[2] = mic->gain;
            break;
        case 0x0a: /* SetPower(bool) */
            if (!snx_ctr_ipc_expect(c, 1, 0)) return;
            mic->power = flag(c->in[1]);
            reply(c, 1, 0);
            break;
        case 0x0b: /* GetPower */
            if (!snx_ctr_ipc_expect(c, 0, 0)) return;
            reply(c, 2, 0);
            c->out[2] = mic->power;
            break;
        case 0x0c: /* SetIirFilterMic(size, a mapped buffer): no filter, the buffer given back as Azahar does */
            if (!snx_ctr_ipc_expect(c, 1, 2)) return;
            reply(c, 1, 2);
            c->out[2] = c->in[2];
            c->out[3] = c->in[3];
            break;
        case 0x0d: /* SetClamp(bool) */
            if (!snx_ctr_ipc_expect(c, 1, 0)) return;
            mic->clamp = flag(c->in[1]);
            reply(c, 1, 0);
            break;
        case 0x0e: /* GetClamp */
            if (!snx_ctr_ipc_expect(c, 0, 0)) return;
            reply(c, 2, 0);
            c->out[2] = mic->clamp;
            break;
        case 0x0f: /* SetAllowShellClosed(bool) */
            if (!snx_ctr_ipc_expect(c, 1, 0)) return;
            mic->allow_shell_closed = flag(c->in[1]);
            reply(c, 1, 0);
            break;
        case 0x10: /* SetClientVersion(u32): Azahar only logs it */
            if (!snx_ctr_ipc_expect(c, 1, 0)) return;
            reply(c, 1, 0);
            break;
        default: c->state = SNX_CTR_IPC_UNKNOWN; break;
    }
}
