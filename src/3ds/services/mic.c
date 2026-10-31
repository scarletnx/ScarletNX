/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Copyright 2014-2026 Citra Emulator Project / Azahar Emulator Project
   mic:u, the microphone. Ported from Azahar's core/hle/service/mic/mic_u.cpp (GPL-2.0-or-later) with its "None" input
   (audio_core/null_input.h): the commands, their replies and the settings a game stores are Azahar's, and sampling
   starts and stops but no sample ever reaches the game's shared memory, which the write offset at its end (zero)
   says. The buffer full event is a real event the game can wait on, which Azahar never signals either. Like Azahar,
   no command fails */
#include "3ds/services/mic.h"
#include "3ds/runtime/internal.h"

static void reply(SnxCtrIpc* c, unsigned normal, unsigned translated) {
    c->out[0] = (c->in[0] & 0xffff0000u) | normal << 6 | translated;
    c->out[1] = 0;
}
/* the buffer full event, made at the first request for it and kept for the process */
static void buffer_full_event(SnxCtrIpc* c) {
    if (!snx_ctr_ipc_expect(c, 0, 0)) return;
    SnxCtrProcess* p = c->process;
    uint32_t handle = 0, result;
    if (!p->mic.event) {
        result = snx_ctr_object_create(p, CTR_OBJ_EVENT, &handle);
        if (!result) {
            p->mic.event = snx_ctr_handle_object(p, handle);
            ++p->objects[p->mic.event].refs; /* the service's own, while the process lives */
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
void snx_ctr_mic_dispatch(SnxCtrIpc* c) {
    SnxCtrMic* mic = &c->process->mic;
    switch (c->in[0] >> 16) {
        case 0x01: /* MapSharedMem(size, handle): a game with a handle of another kind gets Azahar's answer, nothing */
            if (!snx_ctr_ipc_expect(c, 1, 2)) return;
            reply(c, 1, 0);
            break;
        case 0x02: /* UnmapSharedMem */
            if (!snx_ctr_ipc_expect(c, 0, 0)) return;
            reply(c, 1, 0);
            break;
        case 0x03: /* StartSampling(encoding, rate, offset, size, loop): a sampling already running is stopped first */
            if (!snx_ctr_ipc_expect(c, 5, 0)) return;
            mic->sample_rate = (uint8_t)c->in[2];
            mic->sampling = 1;
            reply(c, 1, 0);
            break;
        case 0x04: /* AdjustSampling(rate) */
            if (!snx_ctr_ipc_expect(c, 1, 0)) return;
            mic->sample_rate = (uint8_t)c->in[1];
            reply(c, 1, 0);
            break;
        case 0x05: /* StopSampling */
            if (!snx_ctr_ipc_expect(c, 0, 0)) return;
            mic->sampling = 0;
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
            mic->power = c->in[1] & 1;
            reply(c, 1, 0);
            break;
        case 0x0b: /* GetPower */
            if (!snx_ctr_ipc_expect(c, 0, 0)) return;
            reply(c, 2, 0);
            c->out[2] = mic->power;
            break;
        case 0x0c: /* SetIirFilterMic(size, buffer): stubbed in Azahar, which answers the buffer back mapped */
            if (!snx_ctr_ipc_expect(c, 1, 2)) return;
            reply(c, 1, 2);
            c->out[2] = c->in[2];
            c->out[3] = c->in[3];
            break;
        case 0x0d: /* SetClamp(bool) */
            if (!snx_ctr_ipc_expect(c, 1, 0)) return;
            mic->clamp = c->in[1] & 1;
            reply(c, 1, 0);
            break;
        case 0x0e: /* GetClamp */
            if (!snx_ctr_ipc_expect(c, 0, 0)) return;
            reply(c, 2, 0);
            c->out[2] = mic->clamp;
            break;
        case 0x0f: /* SetAllowShellClosed(bool) */
            if (!snx_ctr_ipc_expect(c, 1, 0)) return;
            mic->allow_shell_closed = c->in[1] & 1;
            reply(c, 1, 0);
            break;
        case 0x10: /* SetClientVersion(u32): Azahar logs it */
            if (!snx_ctr_ipc_expect(c, 1, 0)) return;
            reply(c, 1, 0);
            break;
        default: c->state = SNX_CTR_IPC_UNKNOWN; break;
    }
}
