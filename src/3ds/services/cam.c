/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Copyright 2014-2026 Citra Emulator Project / Azahar Emulator Project
   cam:u, the cameras, with no camera attached. Ported from Azahar's core/hle/service/cam/cam.cpp and cam_u.cpp
   (GPL-2.0-or-later) with its blank frames: the commands, the checks of their arguments, the settings a game stores,
   the transfer arithmetic and the events are Azahar's, and a frame the game receives is black (zero-filled). A
   receiving completes, and a vsync is signalled, one frame time after the capture starts or the receiving is set.
   GetLatestVsyncTiming and GetStereoCameraCalibrationData, which answer into static buffers, and the commands Azahar
   leaves unimplemented are left to the unknown-command path */
#include "3ds/services/cam.h"
#include "3ds/runtime/internal.h"

#define ERROR_INVALID_ENUM_VALUE 0xe0e053edu
#define ERROR_INVALID_SIZE       0xe0e053ecu
#define ERROR_OUT_OF_RANGE       0xe0e053fdu
#define MIN_TRANSFER_UNIT        256u
#define MAX_BUFFER_SIZE          2560u
#define MAX_FRAME_BYTES          (8u << 20)
#define MAX_RATE                 12u

/* the frame rate of each SetFrameRate value, in frames a second (3dbrew's Camera services) */
static const uint8_t k_fps[MAX_RATE + 1] = {15, 15, 15, 10, 9, 5, 20, 20, 30, 30, 15, 10, 10};
/* SetSize's presets */
static const uint16_t k_size[9][2] = {{640, 480}, {320, 240}, {160, 120}, {352, 288}, {176, 144},
                                      {256, 192}, {512, 384}, {400, 240}, {320, 240}};

static void reply(SnxCtrIpc* c, unsigned normal, unsigned translated) {
    c->out[0] = (c->in[0] & 0xffff0000u) | normal << 6 | translated;
    c->out[1] = 0;
}
static void result(SnxCtrIpc* c, uint32_t code) {
    if (code)
        snx_ctr_ipc_error_reply(c, code);
    else
        reply(c, 1, 0);
}
static void value(SnxCtrIpc* c, uint32_t v) {
    reply(c, 2, 0);
    c->out[2] = v;
}
/* the ports (bit 0 the first, bit 1 the second) and cameras (bits 0 to 2) of a select, and the contexts (1 and 2) */
static bool valid_ports(uint32_t select) { return select >= 1 && select <= 3; }
static bool valid_cameras(uint32_t select) { return select >= 1 && select <= 7; }
static bool valid_contexts(uint32_t select) { return select >= 1 && select <= 3; }
static uint64_t frame_ns(const SnxCtrCam* cam, unsigned port) {
    return 1000000000u / k_fps[cam->camera[cam->port[port].camera].frame_rate];
}
static void defaults(SnxCtrCam* cam) {
    *cam = (SnxCtrCam){.initialized = 1};
    for (unsigned i = 0; i < 3; ++i) {
        cam->camera[i].frame_rate = 8; /* 30 fps */
        cam->camera[i].contexts[0] = (SnxCtrCamContext){i == 1, 0, 0, 5, k_size[5][0], k_size[5][1]};
        cam->camera[i].contexts[1] = (SnxCtrCamContext){i == 1, 0, 0, 0, k_size[0][0], k_size[0][1]};
    }
    cam->port[0].camera = 0;
    cam->port[1].camera = 2;
    for (unsigned i = 0; i < 2; ++i) cam->port[i].transfer_bytes = 256;
}
/* the event of a port, made at the first request for it and kept for the process */
static uint32_t event_handle(SnxCtrProcess* p, uint32_t* slot, uint32_t reset, uint32_t* handle) {
    uint32_t r;
    if (!*slot) {
        r = snx_ctr_object_create(p, CTR_OBJ_EVENT, handle);
        if (r) return r;
        *slot = snx_ctr_handle_object(p, *handle);
        ++p->objects[*slot].refs; /* the service's own, while the process lives */
        p->objects[*slot].data.event.reset_type = reset;
        return 0;
    }
    return snx_ctr_handle_add(p, *slot, handle);
}
static void reply_event(SnxCtrIpc* c, uint32_t* slot, uint32_t reset) {
    uint32_t handle = 0, r = event_handle(c->process, slot, reset, &handle);
    if (r) {
        snx_ctr_ipc_error_reply(c, r);
        return;
    }
    reply(c, 1, 2);
    c->out[2] = 0; /* one copied handle */
    snx_ctr_ipc_output_handle(c, 3, handle);
    if (c->state != SNX_CTR_IPC_RETURN) snx_ctr_ipc_drop_handle(c, handle);
}
static void signal(SnxCtrProcess* p, uint32_t event) {
    if (!event) return;
    p->objects[event].data.event.signaled = 1;
    snx_ctr_wake_waiters(p, event);
}
/* the frame a receiving gets: black */
static void deliver(SnxCtrProcess* p, SnxCtrCamPort* port) {
    static const unsigned char zeros[4096];
    for (uint32_t done = 0; done < port->dest_size; done += sizeof zeros) {
        uint32_t chunk = port->dest_size - done < sizeof zeros ? port->dest_size - done : (uint32_t)sizeof zeros;
        if (snx_ctr_write_ram(p->space, port->dest + done, zeros, chunk) != SNX_CTR_MEM_OK) break;
    }
}
void snx_ctr_cam_update(SnxCtrProcess* p, uint64_t now_ns) {
    SnxCtrCam* cam = &p->cam;
    for (unsigned i = 0; i < 2; ++i) {
        SnxCtrCamPort* port = &cam->port[i];
        uint64_t frame = frame_ns(cam, i);
        if (port->vsync_ns && now_ns >= port->vsync_ns) {
            signal(p, port->vsync_event);
            port->vsync_ns += frame;
            if (port->vsync_ns + frame < now_ns) port->vsync_ns = now_ns + frame;
        }
        if (port->done_ns && now_ns >= port->done_ns) {
            deliver(p, port);
            port->done_ns = 0;
            port->receiving = 0;
            signal(p, port->completion_event);
        }
    }
}
uint64_t snx_ctr_cam_deadline(const SnxCtrProcess* p) {
    uint64_t deadline = UINT64_MAX;
    for (unsigned i = 0; i < 2; ++i) {
        const SnxCtrCamPort* port = &p->cam.port[i];
        if (port->vsync_ns && port->vsync_ns < deadline) deadline = port->vsync_ns;
        if (port->done_ns && port->done_ns < deadline) deadline = port->done_ns;
    }
    return deadline;
}
/* runs f on each port a select names; the first error ends it */
static uint32_t each_port(SnxCtrIpc* c, uint32_t select, uint32_t (*f)(SnxCtrIpc*, SnxCtrCamPort*, unsigned)) {
    if (!valid_ports(select)) return ERROR_INVALID_ENUM_VALUE;
    for (unsigned i = 0; i < 2; ++i)
        if (select >> i & 1) {
            uint32_t r = f(c, &c->process->cam.port[i], i);
            if (r) return r;
        }
    return 0;
}
static uint32_t start_capture(SnxCtrIpc* c, SnxCtrCamPort* port, unsigned index) {
    uint64_t now = snx_ctr_now_ns(), frame = frame_ns(&c->process->cam, index);
    if (!port->active) {
        port->active = 1;
        port->vsync_ns = now + frame;
        if (port->receiving) port->done_ns = now + frame;
    }
    return 0;
}
static uint32_t stop_capture(SnxCtrIpc* c, SnxCtrCamPort* port, unsigned index) {
    (void)c, (void)index;
    port->active = 0;
    port->vsync_ns = port->done_ns = 0;
    return 0;
}
static uint32_t clear_buffer(SnxCtrIpc* c, SnxCtrCamPort* port, unsigned index) {
    (void)c, (void)port, (void)index;
    return 0;
}
/* the rules for what a transfer unit may be, for a picture of width x height at two bytes a pixel */
static bool picture_ok(uint32_t width, uint32_t height) {
    return width && height && (uint64_t)width * height * 2 % MIN_TRANSFER_UNIT == 0;
}
static void max_lines(SnxCtrIpc* c) {
    if (!snx_ctr_ipc_expect(c, 2, 0)) return;
    uint32_t width = c->in[1] & 0xffff, height = c->in[2] & 0xffff;
    if (!picture_ok(width, height)) {
        snx_ctr_ipc_error_reply(c, ERROR_OUT_OF_RANGE);
        return;
    }
    uint32_t lines = MAX_BUFFER_SIZE / (width * 2);
    if (lines > height) lines = height;
    while (lines && (height % lines || width * lines * 2 % MIN_TRANSFER_UNIT)) --lines;
    if (!lines)
        snx_ctr_ipc_error_reply(c, ERROR_OUT_OF_RANGE);
    else
        value(c, lines);
}
static void max_bytes(SnxCtrIpc* c) {
    if (!snx_ctr_ipc_expect(c, 2, 0)) return;
    uint32_t width = c->in[1] & 0xffff, height = c->in[2] & 0xffff;
    if (!picture_ok(width, height)) {
        snx_ctr_ipc_error_reply(c, ERROR_OUT_OF_RANGE);
        return;
    }
    uint32_t total = width * height * 2, bytes = MAX_BUFFER_SIZE;
    while (bytes && total % bytes) bytes -= MIN_TRANSFER_UNIT;
    if (!bytes)
        snx_ctr_ipc_error_reply(c, ERROR_OUT_OF_RANGE);
    else
        value(c, bytes);
}
static void set_receiving(SnxCtrIpc* c) {
    if (!snx_ctr_ipc_expect(c, 4, 2)) return;
    SnxCtrProcess* p = c->process;
    uint32_t dest = c->in[1], select = c->in[2], size = c->in[3], unit = c->in[4] & 0xffff;
    if (select != 1 && select != 2) {
        snx_ctr_ipc_error_reply(c, ERROR_INVALID_ENUM_VALUE);
        return;
    }
    if (c->handle_count != 1 || (c->handle_in[0] != CTR_PROCESS_HANDLE && !snx_ctr_handle_object(p, c->handle_in[0]))) {
        snx_ctr_ipc_error_reply(c, 0xd8e007f7u);
        return;
    }
    if (!size || size > MAX_FRAME_BYTES || !unit || size % unit) {
        snx_ctr_ipc_error_reply(c, ERROR_INVALID_SIZE);
        return;
    }
    if (snx_ctr_check_ram(p->space, dest, size, SNX_CTR_WRITE) != SNX_CTR_MEM_OK) {
        snx_ctr_ipc_error_reply(c, 0xe0e01bf5u);
        return;
    }
    unsigned index = select - 1;
    SnxCtrCamPort* port = &p->cam.port[index];
    uint32_t handle = 0, r = event_handle(p, &port->completion_event, CTR_RESET_STICKY, &handle);
    if (r) {
        snx_ctr_ipc_error_reply(c, r);
        return;
    }
    port->dest = dest;
    port->dest_size = size;
    port->receiving = 1;
    p->objects[port->completion_event].data.event.signaled = 0;
    port->done_ns = port->active ? snx_ctr_now_ns() + frame_ns(&p->cam, index) : 0;
    reply(c, 1, 2);
    c->out[2] = 0;
    snx_ctr_ipc_output_handle(c, 3, handle);
    if (c->state != SNX_CTR_IPC_RETURN) snx_ctr_ipc_drop_handle(c, handle);
}
/* a port select naming one port: the command's argument, else the enum error */
static SnxCtrCamPort* single_port(SnxCtrIpc* c, uint32_t select, unsigned* index) {
    if (select != 1 && select != 2) {
        snx_ctr_ipc_error_reply(c, ERROR_INVALID_ENUM_VALUE);
        return NULL;
    }
    *index = select - 1;
    return &c->process->cam.port[*index];
}
static const SnxCtrCamContext* port_context(const SnxCtrCam* cam, unsigned index) {
    unsigned camera = cam->port[index].camera;
    return &cam->camera[camera].contexts[cam->camera[camera].context];
}
/* the Set* commands of a camera: f applies a value to each context a select names, of each camera a select names */
static void camera_setting(SnxCtrIpc* c, unsigned normal, uint32_t limit, unsigned field, bool contexts) {
    if (!snx_ctr_ipc_expect(c, normal, 0)) return;
    SnxCtrCam* cam = &c->process->cam;
    uint32_t select = c->in[1], v = c->in[2], contexts_select = contexts ? c->in[normal] : 1;
    if (!valid_cameras(select) || (contexts && !valid_contexts(contexts_select))) {
        snx_ctr_ipc_error_reply(c, ERROR_INVALID_ENUM_VALUE);
        return;
    }
    if (v >= limit) {
        snx_ctr_ipc_error_reply(c, ERROR_INVALID_ENUM_VALUE);
        return;
    }
    for (unsigned i = 0; i < 3; ++i) {
        if (!(select >> i & 1)) continue;
        if (field == 0) { /* frame rate: a camera's */
            cam->camera[i].frame_rate = (uint8_t)v;
            continue;
        }
        for (unsigned j = 0; j < 2; ++j) {
            if (!(contexts_select >> j & 1)) continue;
            SnxCtrCamContext* ctx = &cam->camera[i].contexts[j];
            if (field == 1) ctx->flip = (uint8_t)v;
            else if (field == 2) ctx->effect = (uint8_t)v;
            else if (field == 3) ctx->format = (uint8_t)v;
            else {
                ctx->size_preset = (uint8_t)v;
                ctx->width = k_size[v][0];
                ctx->height = k_size[v][1];
            }
        }
    }
    reply(c, 1, 0);
}
static void set_detail_size(SnxCtrIpc* c) {
    if (!snx_ctr_ipc_expect(c, 8, 0)) return;
    SnxCtrCam* cam = &c->process->cam;
    uint32_t select = c->in[1], contexts = c->in[8];
    if (!valid_cameras(select) || !valid_contexts(contexts)) {
        snx_ctr_ipc_error_reply(c, ERROR_INVALID_ENUM_VALUE);
        return;
    }
    for (unsigned i = 0; i < 3; ++i)
        for (unsigned j = 0; j < 2; ++j)
            if ((select >> i & 1) && (contexts >> j & 1)) {
                cam->camera[i].contexts[j].width = (uint16_t)c->in[2];
                cam->camera[i].contexts[j].height = (uint16_t)c->in[3];
            }
    reply(c, 1, 0);
}
void snx_ctr_cam_dispatch(SnxCtrIpc* c) {
    SnxCtrProcess* p = c->process;
    SnxCtrCam* cam = &p->cam;
    SnxCtrCamPort* port;
    unsigned index;
    if (!cam->initialized) defaults(cam); /* a game that skips DriverInitialize gets the same */
    switch (c->in[0] >> 16) {
        case 0x01: /* StartCapture(ports) */
            if (!snx_ctr_ipc_expect(c, 1, 0)) return;
            result(c, each_port(c, c->in[1], start_capture));
            break;
        case 0x02: /* StopCapture(ports) */
            if (!snx_ctr_ipc_expect(c, 1, 0)) return;
            result(c, each_port(c, c->in[1], stop_capture));
            break;
        case 0x03: /* IsBusy(port) */
            if (!snx_ctr_ipc_expect(c, 1, 0)) return;
            if ((port = single_port(c, c->in[1], &index))) value(c, port->active);
            break;
        case 0x04: /* ClearBuffer(ports) */
            if (!snx_ctr_ipc_expect(c, 1, 0)) return;
            result(c, each_port(c, c->in[1], clear_buffer));
            break;
        case 0x05: /* GetVsyncInterruptEvent(port) */
            if (!snx_ctr_ipc_expect(c, 1, 0)) return;
            if ((port = single_port(c, c->in[1], &index))) reply_event(c, &port->vsync_event, CTR_RESET_ONESHOT);
            break;
        case 0x06: /* GetBufferErrorInterruptEvent(port): never signalled, a blank camera loses no data */
            if (!snx_ctr_ipc_expect(c, 1, 0)) return;
            if ((port = single_port(c, c->in[1], &index))) reply_event(c, &port->error_event, CTR_RESET_ONESHOT);
            break;
        case 0x07: set_receiving(c); break;
        case 0x08: /* IsFinishedReceiving(port) */
            if (!snx_ctr_ipc_expect(c, 1, 0)) return;
            if ((port = single_port(c, c->in[1], &index))) value(c, !port->receiving);
            break;
        case 0x09: /* SetTransferLines(port, lines, width, height) */
            if (!snx_ctr_ipc_expect(c, 4, 0)) return;
            if (!(port = single_port(c, c->in[1], &index))) break;
            {
                uint32_t lines = c->in[2], width = c->in[3] & 0xffff, height = c->in[4] & 0xffff;
                uint64_t bytes = (uint64_t)lines * width * 2;
                if (!lines || !width || !height || lines > height || height % lines || bytes > MAX_BUFFER_SIZE ||
                    bytes % MIN_TRANSFER_UNIT)
                    snx_ctr_ipc_error_reply(c, ERROR_OUT_OF_RANGE);
                else {
                    port->transfer_bytes = (uint16_t)bytes;
                    reply(c, 1, 0);
                }
            }
            break;
        case 0x0a: max_lines(c); break;
        case 0x0b: /* SetTransferBytes(port, bytes, width, height) */
            if (!snx_ctr_ipc_expect(c, 4, 0)) return;
            if (!(port = single_port(c, c->in[1], &index))) break;
            {
                uint32_t bytes = c->in[2], width = c->in[3] & 0xffff, height = c->in[4] & 0xffff;
                if (!bytes || bytes > MAX_BUFFER_SIZE || bytes % MIN_TRANSFER_UNIT || !width || !height ||
                    (uint64_t)width * height * 2 % bytes)
                    snx_ctr_ipc_error_reply(c, ERROR_OUT_OF_RANGE);
                else {
                    port->transfer_bytes = (uint16_t)bytes;
                    reply(c, 1, 0);
                }
            }
            break;
        case 0x0c: /* GetTransferBytes(port) */
            if (!snx_ctr_ipc_expect(c, 1, 0)) return;
            if ((port = single_port(c, c->in[1], &index))) value(c, port->transfer_bytes);
            break;
        case 0x0d: max_bytes(c); break;
        case 0x0e: /* SetTrimming(ports, bool) */
            if (!snx_ctr_ipc_expect(c, 2, 0)) return;
            if (!valid_ports(c->in[1])) {
                snx_ctr_ipc_error_reply(c, ERROR_INVALID_ENUM_VALUE);
                break;
            }
            for (unsigned i = 0; i < 2; ++i)
                if (c->in[1] >> i & 1) cam->port[i].trimming = c->in[2] & 1;
            reply(c, 1, 0);
            break;
        case 0x0f: /* IsTrimming(port) */
            if (!snx_ctr_ipc_expect(c, 1, 0)) return;
            if ((port = single_port(c, c->in[1], &index))) value(c, port->trimming);
            break;
        case 0x10: /* SetTrimmingParams(port, x0, y0, x1, y1) */
            if (!snx_ctr_ipc_expect(c, 5, 0)) return;
            if (!(port = single_port(c, c->in[1], &index))) break;
            {
                const SnxCtrCamContext* ctx = port_context(cam, index);
                uint32_t x0 = c->in[2] & 0xffff, y0 = c->in[3] & 0xffff, x1 = c->in[4] & 0xffff, y1 = c->in[5] & 0xffff;
                if (x0 >= x1 || y0 >= y1 || x1 > ctx->width || y1 > ctx->height)
                    snx_ctr_ipc_error_reply(c, ERROR_OUT_OF_RANGE);
                else {
                    port->x0 = (uint16_t)x0, port->y0 = (uint16_t)y0, port->x1 = (uint16_t)x1, port->y1 = (uint16_t)y1;
                    reply(c, 1, 0);
                }
            }
            break;
        case 0x11: /* GetTrimmingParams(port) */
            if (!snx_ctr_ipc_expect(c, 1, 0)) return;
            if (!(port = single_port(c, c->in[1], &index))) break;
            reply(c, 5, 0);
            c->out[2] = port->x0, c->out[3] = port->y0, c->out[4] = port->x1, c->out[5] = port->y1;
            break;
        case 0x12: /* SetTrimmingParamsCenter(port, trim width, trim height, camera width, camera height) */
            if (!snx_ctr_ipc_expect(c, 5, 0)) return;
            if (!(port = single_port(c, c->in[1], &index))) break;
            {
                uint32_t tw = c->in[2] & 0xffff, th = c->in[3] & 0xffff, cw = c->in[4] & 0xffff, ch = c->in[5] & 0xffff;
                if (!tw || !th || tw > cw || th > ch)
                    snx_ctr_ipc_error_reply(c, ERROR_OUT_OF_RANGE);
                else {
                    port->x0 = (uint16_t)((cw - tw) / 2), port->y0 = (uint16_t)((ch - th) / 2);
                    port->x1 = (uint16_t)(port->x0 + tw), port->y1 = (uint16_t)(port->y0 + th);
                    reply(c, 1, 0);
                }
            }
            break;
        case 0x13: /* Activate(cameras): none, or one camera of a port (3 would be two of the first) */
            if (!snx_ctr_ipc_expect(c, 1, 0)) return;
            if (c->in[1] > 7 || c->in[1] == 3) {
                snx_ctr_ipc_error_reply(c, ERROR_INVALID_ENUM_VALUE);
                break;
            }
            for (unsigned i = 0; i < 3; ++i)
                if (c->in[1] >> i & 1) cam->port[i == 2].camera = (uint8_t)i;
            reply(c, 1, 0);
            break;
        case 0x14: /* SwitchContext(cameras, context) */
            if (!snx_ctr_ipc_expect(c, 2, 0)) return;
            if (!valid_cameras(c->in[1]) || (c->in[2] != 1 && c->in[2] != 2)) {
                snx_ctr_ipc_error_reply(c, ERROR_INVALID_ENUM_VALUE);
                break;
            }
            for (unsigned i = 0; i < 3; ++i)
                if (c->in[1] >> i & 1) cam->camera[i].context = (uint8_t)(c->in[2] - 1);
            reply(c, 1, 0);
            break;
        case 0x1d: camera_setting(c, 3, 4, 1, true); break;   /* FlipImage(cameras, flip, contexts) */
        case 0x1e: set_detail_size(c); break;
        case 0x1f: camera_setting(c, 3, 9, 4, true); break;   /* SetSize(cameras, size, contexts) */
        case 0x20: camera_setting(c, 2, MAX_RATE + 1, 0, false); break; /* SetFrameRate(cameras, rate) */
        case 0x22: camera_setting(c, 3, 7, 2, true); break;   /* SetEffect(cameras, effect, contexts) */
        case 0x25: camera_setting(c, 3, 2, 3, true); break;   /* SetOutputFormat(cameras, format, contexts) */
        case 0x29: /* SynchronizeVsyncTiming(camera, camera) */
            if (!snx_ctr_ipc_expect(c, 2, 0)) return;
            reply(c, 1, 0);
            break;
        case 0x33: case 0x34: case 0x35: { /* SetPackageParameterWithoutContext, WithContext, WithContextDetail */
            static const uint8_t normal[] = {5, 6, 8};
            if (!snx_ctr_ipc_expect(c, normal[(c->in[0] >> 16) - 0x33], 0)) return;
            reply(c, 1, 0);
            break;
        }
        case 0x36: /* GetSuitableY2rStandardCoefficient: ITU-R BT.601 with 8 bit scaling, as Azahar answers */
            if (!snx_ctr_ipc_expect(c, 0, 0)) return;
            value(c, 0);
            break;
        case 0x38: /* PlayShutterSound(sound) */
            if (!snx_ctr_ipc_expect(c, 1, 0)) return;
            result(c, c->in[1] > 2 ? ERROR_INVALID_ENUM_VALUE : 0);
            break;
        case 0x39: /* DriverInitialize: the defaults of the cameras and ports */
            if (!snx_ctr_ipc_expect(c, 0, 0)) return;
            {
                SnxCtrCamPort old[2] = {cam->port[0], cam->port[1]};
                defaults(cam);
                for (unsigned i = 0; i < 2; ++i) { /* the events stay the game's */
                    cam->port[i].completion_event = old[i].completion_event;
                    cam->port[i].error_event = old[i].error_event;
                    cam->port[i].vsync_event = old[i].vsync_event;
                }
            }
            reply(c, 1, 0);
            break;
        case 0x3a: /* DriverFinalize */
            if (!snx_ctr_ipc_expect(c, 0, 0)) return;
            for (unsigned i = 0; i < 2; ++i) cam->port[i].active = cam->port[i].receiving = 0;
            for (unsigned i = 0; i < 2; ++i) cam->port[i].vsync_ns = cam->port[i].done_ns = 0;
            reply(c, 1, 0);
            break;
        default: c->state = SNX_CTR_IPC_UNKNOWN; break;
    }
}
