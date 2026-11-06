/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   dsp::DSP, the service applications drive the DSP through: pipes, interrupt events, the semaphore and component
   loading. Ported from Azahar core/hle/service/dsp/dsp_dsp.cpp (GPL-2.0-or-later, Copyright 2014-2026 Citra Emulator
   Project / Azahar Emulator Project) at azahar-emu/azahar a4b335e, wire forms checked against libctru's dsp.c. */
#include "3ds/services/dsp.h"
#include "3ds/audio/dsp.h"
#include "3ds/audio/output.h"
#include "3ds/runtime/internal.h"
#include "common/utilities/endian.h"
#include "common/utilities/common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DSP_INTERRUPT_PIPE 2u
#define DSP_MAX_EVENTS     6u
#define DSP_ERR_TOO_MANY   0xc860a7ffu /* InvalidResultValue, DSP, OutOfResource, Status */
#define DSP_ERR_HANDLE     0xd8e007f7u
/* Past this much behind the host clock, the frames missed are dropped instead of being run back to back */
#define DSP_MAX_LATE_NS    100000000u

static void dsp_interrupt(void* context, unsigned pipe) {
    SnxCtrProcess* p = context;
    uint32_t id = p->dsp.interrupt[DSP_INTERRUPT_PIPE + pipe];
    if (!id) return;
    p->objects[id].data.event.signaled = 1;
    snx_ctr_wake_waiters(p, id);
}
uint32_t snx_ctr_dsp_open(SnxCtrProcess* p) {
    if (p->dsp.core) return 0;
    p->dsp.core = snx_ctr_dsp_new(p->memory, dsp_interrupt, p);
    if (!p->dsp.core) return 0xd86007f3u; /* out of memory */
    p->dsp.next_frame_ns = snx_ctr_now_ns() + SNX_CTR_DSP_FRAME_NS;
    return 0;
}
void snx_ctr_dsp_close(SnxCtrProcess* p) {
    snx_ctr_dsp_free(p->dsp.core);
    p->dsp.core = NULL;
}
void snx_ctr_dsp_frame(SnxCtrProcess* p, uint64_t now_ns) {
    if (!p->dsp.core) return;
    p->dsp.next_frame_ns += SNX_CTR_DSP_FRAME_NS;
    if (now_ns > p->dsp.next_frame_ns + DSP_MAX_LATE_NS) p->dsp.next_frame_ns = now_ns + SNX_CTR_DSP_FRAME_NS;
    int16_t frame[SNX_CTR_DSP_SAMPLES][2];
    if (snx_ctr_dsp_tick(p->dsp.core, frame)) snx_ctr_audio_frame(frame);
}

static void reply(SnxCtrIpc* c, unsigned normal, unsigned translated) {
    c->out[0] = (c->in[0] & 0xffff0000u) | normal << 6 | translated;
    c->out[1] = 0;
}
static void stop(SnxCtrIpc* c, const char* reason) {
    fprintf(stderr, "3ds dsp: %s\n", reason);
    c->state = SNX_CTR_IPC_UNSUPPORTED;
}
/* the event slot of an interrupt: zero, one, then one per pipe */
static uint32_t* slot(SnxCtrProcess* p, uint32_t type, uint32_t pipe) {
    return &p->dsp.interrupt[type < DSP_INTERRUPT_PIPE ? type : DSP_INTERRUPT_PIPE + pipe];
}
/* one handle, copied or moved, after the command's normal words: what Azahar's PopObject takes */
static bool one_handle(SnxCtrIpc* c, uint32_t word) {
    if (!(c->in[word] & ~0x10u)) return true;
    snx_ctr_ipc_bad(c);
    return false;
}
static void register_interrupt(SnxCtrIpc* c) {
    if (!snx_ctr_ipc_expect(c, 2, 2) || !one_handle(c, 3)) return;
    SnxCtrProcess* p = c->process;
    uint32_t type = c->in[1], pipe = c->in[2];
    if (type > DSP_INTERRUPT_PIPE || pipe >= SNX_CTR_DSP_PIPES) {
        stop(c, "RegisterInterruptEvents names an interrupt or a pipe the DSP does not have");
        return;
    }
    uint32_t* event = slot(p, type, pipe);
    if (!c->handle_in[0]) { /* unregisters */
        if (*event) snx_ctr_object_release(p, *event);
        *event = 0;
        reply(c, 1, 0);
        return;
    }
    uint32_t id = snx_ctr_handle_object(p, c->handle_in[0]);
    if (!id || p->objects[id].type != CTR_OBJ_EVENT) {
        snx_ctr_ipc_error_reply(c, DSP_ERR_HANDLE);
        return;
    }
    unsigned registered = 0;
    for (unsigned i = 0; i < DSP_INTERRUPT_PIPE + SNX_CTR_DSP_PIPES; ++i) registered += !!p->dsp.interrupt[i];
    if (registered >= DSP_MAX_EVENTS) {
        snx_ctr_ipc_error_reply(c, DSP_ERR_TOO_MANY);
        return;
    }
    ++p->objects[id].refs;
    if (*event) snx_ctr_object_release(p, *event);
    *event = id;
    reply(c, 1, 0);
}
static void semaphore_event(SnxCtrIpc* c) {
    if (!snx_ctr_ipc_expect(c, 0, 0)) return;
    SnxCtrProcess* p = c->process;
    uint32_t handle = 0, result;
    /* Signaled by the application once it has written a region. The HLE DSP reads the region at its frame instead,
       as Azahar's does, so nothing waits on it */
    if (!p->dsp.semaphore) {
        result = snx_ctr_object_create(p, CTR_OBJ_EVENT, &handle);
        if (!result) {
            p->dsp.semaphore = snx_ctr_handle_object(p, handle);
            ++p->objects[p->dsp.semaphore].refs; /* the service's own, while the process lives */
        }
    } else
        result = snx_ctr_handle_add(p, p->dsp.semaphore, &handle);
    if (result) {
        snx_ctr_ipc_error_reply(c, result);
        return;
    }
    reply(c, 1, 2);
    c->out[2] = 0; /* one copied handle */
    snx_ctr_ipc_output_handle(c, 3, handle);
    if (c->state != SNX_CTR_IPC_RETURN) snx_ctr_ipc_drop_handle(c, handle);
}
static void write_pipe(SnxCtrIpc* c) {
    if (!snx_ctr_ipc_expect(c, 2, 2)) return;
    SnxCtrProcess* p = c->process;
    uint32_t pipe = c->in[1], size = c->in[3] >> 14;
    uint8_t data[0x400];
    if ((c->in[3] & 0x3fff) != 0x402) {
        snx_ctr_ipc_bad(c);
        return;
    }
    if (size > sizeof data) {
        stop(c, "WriteProcessPipe sends more than the DSP's pipes take");
        return;
    }
    if (size && snx_ctr_read_ram(p->space, c->in[4], data, size) != SNX_CTR_MEM_OK) {
        c->transport_result = 0xe0e01bf5u;
        return;
    }
    /* What the DSP sees of these bytes (confirmed by RE in Azahar): applications pass whatever is on their stack */
    if (pipe == SNX_CTR_DSP_PIPE_AUDIO && size >= 4) data[2] = data[3] = 0;
    if (pipe == SNX_CTR_DSP_PIPE_BINARY && size >= 8) {
        data[4] = 1;
        data[5] = data[6] = data[7] = 0;
    }
    const char* missing = snx_ctr_dsp_pipe_write(p->dsp.core, pipe, data, size);
    if (missing) {
        stop(c, missing);
        return;
    }
    reply(c, 1, 0);
}
/* The application's static buffer 0, which a pipe read lands in */
static bool receive_buffer(SnxCtrIpc* c, uint32_t size, uint32_t* address) {
    unsigned char d[8];
    if (snx_ctr_read_ram(c->process->space, c->process->cpu.state.cpu.tpidruro + 0x180, d, sizeof d) !=
        SNX_CTR_MEM_OK) {
        c->transport_result = 0xe0e01bf5u;
        return false;
    }
    uint32_t descriptor = snx_ld_le32(d);
    *address = snx_ld_le32(d + 4);
    if ((descriptor & 0x3fff) != 2 || (descriptor >> 14) < size) {
        snx_ctr_ipc_bad(c);
        return false;
    }
    return true;
}
static void read_pipe(SnxCtrIpc* c, bool if_possible) {
    if (!snx_ctr_ipc_expect(c, 3, 0)) return;
    SnxCtrProcess* p = c->process;
    uint32_t pipe = c->in[1], size = c->in[3] & 0xffff, address;
    if (snx_ctr_dsp_pipe_readable(p->dsp.core, pipe) < size) {
        /* hardware hangs on a ReadPipe the pipe can't fill */
        if (!if_possible) {
            stop(c, "ReadPipe asks for more than the DSP's pipe holds");
            return;
        }
        size = 0;
    }
    if (!receive_buffer(c, size, &address)) return;
    uint8_t data[0x10000];
    size_t n = snx_ctr_dsp_pipe_read(p->dsp.core, pipe, data, size);
    if (n && snx_ctr_write_ram(p->space, address, data, n) != SNX_CTR_MEM_OK) {
        c->transport_result = 0xe0e01bf5u;
        return;
    }
    unsigned word = 2;
    reply(c, if_possible ? 2 : 1, 2);
    if (if_possible) c->out[word++] = (uint32_t)n;
    c->out[word++] = (uint32_t)n << 14 | 2;
    c->out[word] = address;
}

void snx_ctr_dsp_dispatch(SnxCtrIpc* c) {
    SnxCtrProcess* p = c->process;
    switch (c->in[0] >> 16) {
        case 0x01: /* RecvData: the application reads this after a shutdown or sleep to see the DSP has stopped */
        case 0x02: /* RecvDataIsReady */
            if (!snx_ctr_ipc_expect(c, 1, 0)) return;
            if (c->in[1]) {
                stop(c, "RecvData reads a DSP register other than 0");
                return;
            }
            reply(c, 2, 0);
            c->out[2] = (c->in[0] >> 16) == 1 ? snx_ctr_dsp_state(p->dsp.core) != SNX_CTR_DSP_ON : 1;
            break;
        case 0x07: /* SetSemaphore: nothing to do in HLE */
            if (!snx_ctr_ipc_expect(c, 1, 0)) return;
            reply(c, 1, 0);
            break;
        case 0x0c: /* ConvertProcessAddressFromDspDram: DSP words from DSP RAM + 0x40000 (one region offset, 0) */
            if (!snx_ctr_ipc_expect(c, 1, 0)) return;
            reply(c, 2, 0);
            c->out[2] = (c->in[1] << 1) + SNX_CTR_DSP_RAM + 0x40000u;
            break;
        case 0x0d: write_pipe(c); break;
        case 0x0e: read_pipe(c, false); break;
        case 0x0f: /* GetPipeReadableSize */
            if (!snx_ctr_ipc_expect(c, 2, 0)) return;
            reply(c, 2, 0);
            c->out[2] = (uint16_t)snx_ctr_dsp_pipe_readable(p->dsp.core, c->in[1]);
            break;
        case 0x10: read_pipe(c, true); break;
        case 0x11: /* LoadComponent: the HLE DSP needs no program, it only answers that one is loaded */
            if (!snx_ctr_ipc_expect(c, 3, 2)) return;
            if ((c->in[4] & 15) != 10 || (c->in[4] >> 4) < c->in[1]) {
                snx_ctr_ipc_bad(c);
                return;
            }
            reply(c, 2, 2);
            c->out[2] = 1;
            c->out[3] = c->in[4];
            c->out[4] = c->in[5];
            break;
        case 0x12: /* UnloadComponent */
            if (!snx_ctr_ipc_expect(c, 0, 0)) return;
            reply(c, 1, 0);
            break;
        case 0x13: /* FlushDataCache */
        case 0x14: /* InvalidateDCache: host memory is coherent with the DSP's reads, nothing to do */
            if (!snx_ctr_ipc_expect(c, 2, 2) || !one_handle(c, 3)) return;
            if (c->handle_in[0] != CTR_PROCESS_HANDLE) {
                snx_ctr_ipc_error_reply(c, DSP_ERR_HANDLE);
                return;
            }
            reply(c, 1, 0);
            break;
        case 0x15: register_interrupt(c); break;
        case 0x16: semaphore_event(c); break;
        case 0x17: /* SetSemaphoreMask */
            if (!snx_ctr_ipc_expect(c, 1, 0)) return;
            p->dsp.semaphore_mask = (uint16_t)c->in[1];
            reply(c, 1, 0);
            break;
        case 0x1f: /* GetHeadphoneStatus: none plugged in */
            if (!snx_ctr_ipc_expect(c, 0, 0)) return;
            reply(c, 2, 0);
            c->out[2] = 0;
            break;
        case 0x20: /* ForceHeadphoneOut */
            if (!snx_ctr_ipc_expect(c, 1, 0)) return;
            reply(c, 1, 0);
            break;
        default: c->state = SNX_CTR_IPC_UNKNOWN; break;
    }
}

void snx_ctr_dsp_component_file(SnxCtrProcess* p) {
    /* A CDC is an RSA signature, then "DSP1", the component's size and its segments. This is only the header, with no
       code: libctru reads the file whole and passes it to LoadComponent, which the HLE DSP answers without running
       anything. A dumped dspfirm.cdc in the title's SD is used as it is */
    unsigned char cdc[0x300] = {0};
    memcpy(cdc + 0x100, "DSP1", 4);
    cdc[0x105] = 0x03; /* 0x300 bytes */
    pm_lock(&p->tree->lock);
    FsNode *dir = NULL, *file = NULL;
    if (!fs_tree_mkdirs(p->tree, "sdmc/3ds", &dir) && fs_tree_lookup(p->tree, dir, "dspfirm.cdc", &file) &&
        !fs_tree_create(p->tree, dir, "dspfirm.cdc", 0, sizeof cdc, &file)) {
        FILE* host = fs_tree_open(p->tree, file, "r+b");
        if (host) {
            (void)fwrite(cdc, sizeof cdc, 1, host);
            fclose(host);
        }
    }
    pm_unlock(&p->tree->lock);
}
