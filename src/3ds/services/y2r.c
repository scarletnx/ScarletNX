/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   y2r:u, the Y2R engine's service: its configuration, the streams it reads and writes by DMA, and its conversions, run
   on the GPU (gpu/vk/y2r.c). Ported from Azahar core/hle/service/cam/y2r_u.cpp (GPL-2.0-or-later, Copyright
   2015-2024 Citra Emulator Project / Azahar Emulator Project, azahar-emu/azahar a4b335e): its answers, its checks,
   and the end it signals a while after a conversion starts, whether the application asked for the interrupt or not. */
#include "3ds/services/y2r.h"
#include "3ds/gpu/vk/vk.h"
#include "3ds/gpu/vk/vk_y2r.h"
#include "3ds/runtime/internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define Y2R_OUT_OF_RANGE  0xe0e053fdu /* OutOfRange, CAM, InvalidArgument, Usage */
#define Y2R_INVALID_ENUM  0xe0e053edu /* InvalidEnumValue, CAM, InvalidArgument, Usage */
/* Azahar's MinY2RDelay, 50000 ARM11 cycles at 268111856 Hz: the least a conversion takes, which one game's intro
   needs */
#define Y2R_DELAY_NS      186490u

static void reply(SnxCtrIpc* c, unsigned normal) {
    c->out[0] = (c->in[0] & 0xffff0000u) | normal << 6;
    c->out[1] = 0;
}
static void value(SnxCtrIpc* c, uint32_t v) {
    reply(c, 2);
    c->out[2] = v;
}
static void stop(SnxCtrIpc* c, const char* reason) {
    fprintf(stderr, "3ds y2r: %s\n", reason);
    c->state = SNX_CTR_IPC_UNSUPPORTED;
}
static uint32_t make_event(SnxCtrProcess* p) {
    SnxCtrY2rService* y = &p->y2r;
    if (y->event) return 0;
    uint32_t handle = 0, result = snx_ctr_object_create(p, CTR_OBJ_EVENT, &handle);
    if (result) return result;
    y->event = snx_ctr_handle_object(p, handle);
    ++p->objects[y->event].refs; /* the service's own, while the process lives */
    return snx_ctr_handle_close(p, handle);
}
/* SetInputLineWidth and SetInputLines as Azahar checks them (the hardware takes 1024 lines as "keep the last") */
static uint32_t line_width(SnxCtrY2rService* y, uint16_t width) {
    if (!width || width > 1024 || width % 8) return Y2R_OUT_OF_RANGE;
    y->config.input_line_width = width;
    return 0;
}
static uint32_t input_lines(SnxCtrY2rService* y, uint16_t lines) {
    if (!lines || lines > 1024) return Y2R_OUT_OF_RANGE;
    if (lines != 1024) y->config.input_lines = lines;
    return 0;
}
static uint32_t standard(SnxCtrY2rService* y, uint32_t index) {
    if (index >= 4) return Y2R_INVALID_ENUM;
    memcpy(y->config.coefficients, snx_ctr_y2r_standard[index], sizeof y->config.coefficients);
    return 0;
}
/* SetSending*, SetReceiving: address, image size, transfer unit, gap, then the process the address is in, ours */
static void set_buffer(SnxCtrIpc* c, SnxCtrY2rBuffer* b) {
    if (!snx_ctr_ipc_expect(c, 4, 2)) return;
    if (c->in[5] || c->in[6] != CTR_PROCESS_HANDLE) {
        stop(c, "a stream named another process than the application");
        return;
    }
    *b = (SnxCtrY2rBuffer){c->in[1], c->in[2], (uint16_t)c->in[3], (uint16_t)c->in[4]};
    reply(c, 1);
}
/* the physical address of each page a stream spans, from its address's page; false for one that is not mapped as the
   engine uses it */
static bool stream_pages(SnxCtrProcess* p, const SnxCtrY2rBuffer* b, uint64_t span, bool write, uint32_t** pages,
                         uint32_t* count) {
    uint32_t first = b->address & ~4095u;
    uint64_t end = (uint64_t)b->address + span;
    uint32_t n = (uint32_t)((end - first + 4095) / 4096);
    *pages = malloc(n * sizeof **pages);
    if (!*pages) return false;
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t pa;
        unsigned permissions;
        if (end > 0x100000000ull ||
            snx_ctr_query(p->space, first + i * 4096u, &pa, &permissions) != SNX_CTR_MEM_OK ||
            !(permissions & (write ? SNX_CTR_WRITE : SNX_CTR_READ)))
            return false;
        (*pages)[i] = pa & ~4095u;
    }
    *count = n;
    return true;
}
static void start(SnxCtrIpc* c) {
    if (!snx_ctr_ipc_expect(c, 0, 0)) return;
    SnxCtrProcess* p = c->process;
    SnxCtrY2rService* y = &p->y2r;
    SnxCtrY2rBuffer streams[SNX_CTR_Y2R_STREAMS];
    memcpy(streams, y->streams, sizeof streams);
    if (y->config.input_format == SNX_CTR_Y2R_YUYV) streams[SNX_CTR_Y2R_Y] = y->yuyv;
    uint64_t spans[SNX_CTR_Y2R_STREAMS];
    if (!snx_ctr_y2r_check(&y->config, streams, spans)) {
        stop(c, "StartConversion asks for a conversion the engine does not take (see gpu/y2r.h)");
        return;
    }
    uint32_t* pages[SNX_CTR_Y2R_STREAMS] = {NULL, NULL, NULL, NULL};
    uint32_t counts[SNX_CTR_Y2R_STREAMS] = {0, 0, 0, 0};
    bool mapped = true;
    for (unsigned s = 0; s < SNX_CTR_Y2R_STREAMS && mapped; ++s)
        if (spans[s]) mapped = stream_pages(p, &streams[s], spans[s], s == SNX_CTR_Y2R_DST, &pages[s], &counts[s]);
    const uint32_t* const pages_in[SNX_CTR_Y2R_STREAMS] = {pages[0], pages[1], pages[2], pages[3]};
    SnxCtrXferResult result = mapped ? snx_ctr_vk_y2r(&y->config, streams, pages_in, counts) : SNX_CTR_XFER_BOUNDS;
    for (unsigned s = 0; s < SNX_CTR_Y2R_STREAMS; ++s) free(pages[s]);
    if (result != SNX_CTR_XFER_OK) {
        stop(c, result == SNX_CTR_XFER_BOUNDS ? "a conversion's stream is not in the application's memory"
                                              : "the conversion needs the GPU, which is not there");
        return;
    }
    /* a conversion started again before its end replaces it: one end is signaled, the last one's. It runs now, and
       ends when the GPU has done it (the application may reuse its inputs then), and not before the engine's time */
    y->serial = snx_ctr_vk_submit();
    y->done_ns = snx_ctr_now_ns() + Y2R_DELAY_NS;
    reply(c, 1);
}
void snx_ctr_y2r_done(SnxCtrProcess* p) {
    (void)snx_ctr_vk_wait(p->y2r.serial, UINT64_MAX); /* the GPU's conversion, done by now nearly always */
    p->y2r.done_ns = 0;
    if (!p->y2r.event) return;
    p->objects[p->y2r.event].data.event.signaled = 1;
    snx_ctr_wake_waiters(p, p->y2r.event);
}
static void driver_initialize(SnxCtrIpc* c) {
    if (!snx_ctr_ipc_expect(c, 0, 0)) return;
    SnxCtrY2rService* y = &c->process->y2r;
    uint32_t result = make_event(c->process);
    if (result) {
        snx_ctr_ipc_error_reply(c, result);
        return;
    }
    y->config = (SnxCtrY2rConfig){SNX_CTR_Y2R_422_8, SNX_CTR_Y2R_RGBA8, 0, 0, 1024, 1024, {0}, 0};
    memset(y->streams, 0, sizeof y->streams);
    c->process->objects[y->event].data.event.signaled = 0;
    reply(c, 1);
}
void snx_ctr_y2r_dispatch(SnxCtrIpc* c) {
    SnxCtrProcess* p = c->process;
    SnxCtrY2rService* y = &p->y2r;
    SnxCtrY2rConfig* k = &y->config;
    uint32_t command = c->in[0] >> 16;
    /* the one-word settings and their getters */
    uint8_t* bytes[] = {&k->input_format,           &k->output_format,         &k->rotation, &k->block_alignment,
                        &y->spacial_dithering,       &y->temporal_dithering,    &y->transfer_end_interrupt};
    if (command >= 0x01 && command <= 0x0e) {
        uint8_t* field = bytes[(command - 1) / 2];
        if (command & 1) {
            if (!snx_ctr_ipc_expect(c, 1, 0)) return;
            *field = (uint8_t)c->in[1];
            reply(c, 1);
        } else if (snx_ctr_ipc_expect(c, 0, 0))
            value(c, *field);
        return;
    }
    switch (command) {
        case 0x0f: { /* GetTransferEndEvent */
            if (!snx_ctr_ipc_expect(c, 0, 0)) return;
            uint32_t handle = 0, result = make_event(p);
            if (!result) result = snx_ctr_handle_add(p, y->event, &handle);
            if (result) {
                snx_ctr_ipc_error_reply(c, result);
                return;
            }
            reply(c, 1);
            c->out[0] |= 2;
            c->out[2] = 0; /* one copied handle */
            snx_ctr_ipc_output_handle(c, 3, handle);
            if (c->state != SNX_CTR_IPC_RETURN) snx_ctr_ipc_drop_handle(c, handle);
            return;
        }
        case 0x10: set_buffer(c, &y->streams[SNX_CTR_Y2R_Y]); return;
        case 0x11: set_buffer(c, &y->streams[SNX_CTR_Y2R_U]); return;
        case 0x12: set_buffer(c, &y->streams[SNX_CTR_Y2R_V]); return;
        case 0x13: set_buffer(c, &y->yuyv); return;
        case 0x18: set_buffer(c, &y->streams[SNX_CTR_Y2R_DST]); return;
        case 0x14: case 0x15: case 0x16: case 0x17: /* IsFinishedSending*: the DMA is done when the conversion is */
        case 0x19:                                  /* IsFinishedReceiving */
            if (snx_ctr_ipc_expect(c, 0, 0)) value(c, 1);
            return;
        case 0x1a:
            if (!snx_ctr_ipc_expect(c, 1, 0)) return;
            snx_ctr_ipc_error_reply(c, line_width(y, (uint16_t)c->in[1]));
            return;
        case 0x1b:
            if (snx_ctr_ipc_expect(c, 0, 0)) value(c, k->input_line_width);
            return;
        case 0x1c:
            if (!snx_ctr_ipc_expect(c, 1, 0)) return;
            snx_ctr_ipc_error_reply(c, input_lines(y, (uint16_t)c->in[1]));
            return;
        case 0x1d:
            if (snx_ctr_ipc_expect(c, 0, 0)) value(c, k->input_lines);
            return;
        case 0x1e: /* SetCoefficient: eight 16-bit coefficients, two a word */
            if (!snx_ctr_ipc_expect(c, 4, 0)) return;
            for (unsigned i = 0; i < 8; ++i) k->coefficients[i] = (int16_t)(c->in[1 + i / 2] >> (i % 2 * 16));
            reply(c, 1);
            return;
        case 0x1f: case 0x21: { /* GetCoefficient, GetStandardCoefficient */
            const int16_t* set = k->coefficients;
            if (!snx_ctr_ipc_expect(c, command == 0x21 ? 1 : 0, 0)) return;
            if (command == 0x21) {
                if (c->in[1] >= 4) {
                    snx_ctr_ipc_error_reply(c, Y2R_INVALID_ENUM);
                    return;
                }
                set = snx_ctr_y2r_standard[c->in[1]];
            }
            reply(c, 5);
            for (unsigned i = 0; i < 4; ++i)
                c->out[2 + i] = (uint16_t)set[i * 2] | (uint32_t)(uint16_t)set[i * 2 + 1] << 16;
            return;
        }
        case 0x20:
            if (!snx_ctr_ipc_expect(c, 1, 0)) return;
            snx_ctr_ipc_error_reply(c, standard(y, c->in[1]));
            return;
        case 0x22:
            if (!snx_ctr_ipc_expect(c, 1, 0)) return;
            k->alpha = (uint16_t)c->in[1];
            reply(c, 1);
            return;
        case 0x23:
            if (snx_ctr_ipc_expect(c, 0, 0)) value(c, k->alpha);
            return;
        case 0x24: /* SetDitheringWeightParams: sixteen 16-bit weights the conversion does not use, as in Azahar */
            if (!snx_ctr_ipc_expect(c, 8, 0)) return;
            for (unsigned i = 0; i < 16; ++i) y->dithering[i] = (uint16_t)(c->in[1 + i / 2] >> (i % 2 * 16));
            reply(c, 1);
            return;
        case 0x25:
            if (!snx_ctr_ipc_expect(c, 0, 0)) return;
            reply(c, 9);
            for (unsigned i = 0; i < 8; ++i) c->out[2 + i] = y->dithering[i * 2] | (uint32_t)y->dithering[i * 2 + 1] << 16;
            return;
        case 0x26: start(c); return;
        case 0x27: /* StopConversion: the running one's end never comes */
            if (!snx_ctr_ipc_expect(c, 0, 0)) return;
            y->done_ns = 0;
            reply(c, 1);
            return;
        case 0x28:
            if (snx_ctr_ipc_expect(c, 0, 0)) value(c, y->done_ns != 0);
            return;
        case 0x29: { /* SetPackageParameter: the four modes, width, lines, standard coefficient, padding, alpha */
            if (!snx_ctr_ipc_expect(c, 3, 0)) return;
            k->input_format = (uint8_t)c->in[1], k->output_format = (uint8_t)(c->in[1] >> 8);
            k->rotation = (uint8_t)(c->in[1] >> 16), k->block_alignment = (uint8_t)(c->in[1] >> 24);
            uint32_t result = line_width(y, (uint16_t)c->in[2]);
            if (!result) result = input_lines(y, (uint16_t)(c->in[2] >> 16));
            if (!result) result = standard(y, c->in[3] & 0xff);
            if (!result) k->alpha = (uint16_t)(c->in[3] >> 16);
            snx_ctr_ipc_error_reply(c, result);
            return;
        }
        case 0x2a: /* PingProcess */
            if (snx_ctr_ipc_expect(c, 0, 0)) value(c, 0);
            return;
        case 0x2b: driver_initialize(c); return;
        case 0x2c: /* DriverFinalize */
            if (snx_ctr_ipc_expect(c, 0, 0)) reply(c, 1);
            return;
        case 0x2d: /* GetPackageParameter: the configuration's first twelve bytes, as Azahar pushes them */
            if (!snx_ctr_ipc_expect(c, 0, 0)) return;
            reply(c, 4);
            c->out[2] = k->input_format | (uint32_t)k->output_format << 8 | (uint32_t)k->rotation << 16 |
                        (uint32_t)k->block_alignment << 24;
            c->out[3] = k->input_line_width | (uint32_t)k->input_lines << 16;
            c->out[4] = (uint16_t)k->coefficients[0] | (uint32_t)(uint16_t)k->coefficients[1] << 16;
            return;
        default: c->state = SNX_CTR_IPC_UNKNOWN; return;
    }
}
