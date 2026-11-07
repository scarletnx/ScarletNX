/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* cam:u, the cameras, as Azahar answers with its blank camera (core/hle/service/cam/cam.cpp, cam.h and cam_u.cpp,
   core/frontend/camera/blank_camera.cpp at azahar-emu/azahar 6e42de8): the commands, the settings a game stores,
   the ports' states and the transfer arithmetic are Azahar's, and a frame the game receives is black (0x8000 a pixel
   in YUV422, 0 in RGB565). An activated port's vsync event fires, and a receiving completes, at its camera's frame
   rate's latency, as Azahar schedules them. Where Azahar would read past a table (a size or a frame rate it has no
   entry for) or divide by zero (a width of 0), the command answers InvalidEnumValue or OutOfRange instead */
#include "3ds/services/cam.h"
#include "3ds/runtime/internal.h"
#include "common/utilities/endian.h"
#include <string.h>

#define CAM_INVALID_ENUM  0xe0e053edu /* InvalidEnumValue, CAM, InvalidArgument, Usage */
#define CAM_OUT_OF_RANGE  0xe0e053fdu /* OutOfRange */
#define MIN_TRANSFER_UNIT 256u
#define MAX_BUFFER_SIZE   2560u
#define FORMAT_RGB565     1 /* OutputFormat: YUV422 is 0 */
#define FRAME_RATES       13

/* Azahar's PRESET_RESOLUTION: width, height and the crop of the 640x480 sensor, inclusive */
static const SnxCtrCamContext PRESETS[8] = {
    {640, 480, 0, 0, 639, 479, 0, 0, 0},  {320, 240, 0, 0, 639, 479, 0, 0, 0},  {160, 120, 0, 0, 639, 479, 0, 0, 0},
    {352, 288, 26, 0, 613, 479, 0, 0, 0}, {176, 144, 26, 0, 613, 479, 0, 0, 0}, {256, 192, 0, 0, 639, 479, 0, 0, 0},
    {512, 384, 0, 0, 639, 479, 0, 0, 0},  {400, 240, 0, 48, 639, 431, 0, 0, 0},
};
/* LATENCY_BY_FRAME_RATE: the ms from a capture or a vsync to the next, by SetFrameRate's value */
static const uint8_t LATENCY_MS[FRAME_RATES] = {67, 67, 67, 100, 118, 200, 50, 50, 33, 33, 67, 50, 33};

static void reply(SnxCtrIpc* c, unsigned normal, unsigned translated, uint32_t result) {
    memset(c->out + 1, 0, (normal + translated) * sizeof *c->out);
    c->out[0] = (c->in[0] & 0xffff0000u) | normal << 6 | translated;
    c->out[1] = result;
}
/* a port, camera or context select (Azahar's CommandParamBitSet over a u8): valid below 1 << bits, single with one
   bit set */
static bool valid(uint32_t word, unsigned bits) { return (word & 0xff) < 1u << bits; }
static bool single(uint32_t word, unsigned bits) {
    uint32_t v = word & 0xff;
    return valid(word, bits) && v && !(v & (v - 1));
}
static unsigned first(uint32_t word) {
    unsigned i = 0;
    while (!((word >> i) & 1)) ++i;
    return i;
}
static uint64_t latency_ns(const SnxCtrCam* cam, unsigned camera) {
    return (uint64_t)LATENCY_MS[cam->camera[camera].frame_rate] * 1000000u;
}
/* one of a port's events, made at the first request (Azahar makes them with the service), and a handle to it */
static uint32_t event(SnxCtrProcess* p, uint32_t* slot, uint32_t reset, uint32_t* handle) {
    if (*slot) return snx_ctr_handle_add(p, *slot, handle);
    uint32_t result = snx_ctr_object_create(p, CTR_OBJ_EVENT, handle);
    if (result) return result;
    *slot = snx_ctr_handle_object(p, *handle);
    ++p->objects[*slot].refs; /* the service's own, while the process lives */
    p->objects[*slot].data.event.reset_type = reset;
    return 0;
}
/* a reply of the result and one copied event handle (none with an error, as Azahar's PushCopyObjects(nullptr)) */
static void reply_event(SnxCtrIpc* c, uint32_t* slot, uint32_t reset, bool ok) {
    uint32_t handle = 0, result = ok ? event(c->process, slot, reset, &handle) : CAM_INVALID_ENUM;
    if (result && ok) {
        snx_ctr_ipc_error_reply(c, result);
        return;
    }
    reply(c, 1, 2, result);
    if (!handle) return;
    snx_ctr_ipc_output_handle(c, 3, handle);
    if (c->state != SNX_CTR_IPC_RETURN) snx_ctr_ipc_drop_handle(c, handle);
}
static void signal_event(SnxCtrProcess* p, uint32_t id) {
    if (!id) return;
    p->objects[id].data.event.signaled = 1;
    snx_ctr_wake_waiters(p, id);
}
static void clear(SnxCtrProcess* p, uint32_t id) {
    if (id) p->objects[id].data.event.signaled = 0;
}
static void start_receiving(SnxCtrProcess* p, unsigned port) {
    SnxCtrCamPort* q = &p->cam.port[port];
    q->receiving = 1;
    q->done_ns = snx_ctr_now_ns() + latency_ns(&p->cam, q->camera);
}
static void cancel_receiving(SnxCtrProcess* p, unsigned port) {
    p->cam.port[port].receiving = 0;
    p->cam.port[port].done_ns = 0;
}
static void activate_port(SnxCtrProcess* p, unsigned port, unsigned camera) {
    SnxCtrCamPort* q = &p->cam.port[port];
    if (q->busy && q->camera != camera) {
        cancel_receiving(p, port);
        q->busy = 0;
    }
    q->active = 1;
    q->camera = (uint8_t)camera;
    q->vsync_ns = snx_ctr_now_ns() + latency_ns(&p->cam, camera);
}
/* Azahar's CompletionEventCallBack with the blank camera's frame: the camera's current context's size, trimmed to the
   port's rectangle when trimming is on (none for a rectangle outside the picture), as much of it as the buffer holds */
static void complete(SnxCtrProcess* p, unsigned port) {
    SnxCtrCamPort* q = &p->cam.port[port];
    const SnxCtrCamera* camera = &p->cam.camera[q->camera];
    const SnxCtrCamContext* context = &camera->contexts[camera->context];
    uint32_t width = context->width, height = context->height, frame = width * height * 2, bytes;
    if (q->trimming) {
        bool inside = q->x1 > q->x0 && q->y1 > q->y0 && q->x1 <= width && q->y1 <= height;
        uint32_t line = inside ? (uint32_t)(q->x1 - q->x0) * 2 : 0, rows = inside ? (uint32_t)(q->y1 - q->y0) : 0;
        int64_t source = (int64_t)frame - ((int64_t)q->y0 * width + q->x0) * 2, left = q->dest_size;
        bytes = 0;
        for (uint32_t y = 0; y < rows; ++y) {
            int64_t n = line < left ? line : left;
            if (source < n) n = source;
            if (n <= 0) break;
            bytes += (uint32_t)n;
            left -= n;
            source -= (int64_t)width * 2;
        }
    } else
        bytes = q->dest_size < frame ? q->dest_size : frame;
    unsigned char black[2], chunk[1024];
    snx_st_le16(black, context->format == FORMAT_RGB565 ? 0 : 0x8000);
    for (unsigned i = 0; i < sizeof chunk; ++i) chunk[i] = black[i & 1];
    for (uint32_t done = 0; done < bytes;) {
        uint32_t n = bytes - done < sizeof chunk ? bytes - done : (uint32_t)sizeof chunk;
        if (snx_ctr_write_ram(p->space, q->dest + done, chunk, n) != SNX_CTR_MEM_OK) break;
        done += n;
    }
    q->receiving = 0;
    q->done_ns = 0;
    signal_event(p, q->completion_event);
}
void snx_ctr_cam_update(SnxCtrProcess* p, uint64_t now_ns) {
    for (unsigned i = 0; i < 2; ++i) {
        SnxCtrCamPort* q = &p->cam.port[i];
        if (q->vsync_ns && now_ns >= q->vsync_ns) {
            if (!q->active)
                q->vsync_ns = 0;
            else {
                /* the vsync's time, in us, the newest first of the five kept */
                memmove(q->vsync_us + 1, q->vsync_us, sizeof q->vsync_us - sizeof *q->vsync_us);
                q->vsync_us[0] = (int64_t)(now_ns / 1000);
                if (q->vsync_count < 5) ++q->vsync_count;
                signal_event(p, q->vsync_event);
                q->vsync_ns = now_ns + latency_ns(&p->cam, q->camera);
            }
        }
        if (q->done_ns && now_ns >= q->done_ns) complete(p, i);
    }
}
uint64_t snx_ctr_cam_deadline(const SnxCtrProcess* p) {
    uint64_t deadline = UINT64_MAX;
    for (unsigned i = 0; i < 2; ++i) {
        const SnxCtrCamPort* q = &p->cam.port[i];
        if (q->vsync_ns && q->vsync_ns < deadline) deadline = q->vsync_ns;
        if (q->done_ns && q->done_ns < deadline) deadline = q->done_ns;
    }
    return deadline;
}
/* GetMaxLines and GetMaxBytes for a width x height picture of two bytes a pixel (0: OutOfRange) */
static uint32_t max_lines(uint32_t width, uint32_t height) {
    if (!width || !height || width * height * 2 % MIN_TRANSFER_UNIT) return 0;
    uint32_t lines = MAX_BUFFER_SIZE / width;
    if (lines > height) lines = height;
    while (lines && (height % lines || lines * width * 2 % MIN_TRANSFER_UNIT)) --lines;
    return lines;
}
static uint32_t max_bytes(uint32_t width, uint32_t height) {
    if (width * height * 2 % MIN_TRANSFER_UNIT) return 0;
    uint32_t bytes = MAX_BUFFER_SIZE;
    while (width * height * 2 % bytes) bytes -= MIN_TRANSFER_UNIT;
    return bytes;
}
/* sets a context field of each camera and context a select names */
enum { SET_FLIP, SET_EFFECT, SET_FORMAT, SET_SIZE, SET_DETAIL };
static void contexts(SnxCtrIpc* c, unsigned field, uint32_t cameras, uint32_t value, uint32_t context_select) {
    SnxCtrCam* cam = &c->process->cam;
    if (!valid(cameras, 3) || !valid(context_select, 2) || (field == SET_SIZE && (value & 0xff) >= 8)) {
        reply(c, 1, 0, CAM_INVALID_ENUM);
        return;
    }
    for (unsigned i = 0; i < 3; ++i)
        for (unsigned j = 0; j < 2; ++j) {
            if (!((cameras >> i) & 1) || !((context_select >> j) & 1)) continue;
            SnxCtrCamContext* x = &cam->camera[i].contexts[j];
            if (field == SET_FLIP)
                x->flip = (uint8_t)value;
            else if (field == SET_EFFECT)
                x->effect = (uint8_t)value;
            else if (field == SET_FORMAT)
                x->format = (uint8_t)value;
            else if (field == SET_SIZE) {
                uint8_t flip = x->flip, effect = x->effect, format = x->format;
                *x = PRESETS[value & 0xff];
                x->flip = flip, x->effect = effect, x->format = format;
            } else { /* SetDetailSize: the six u16 in words 2 to 7 */
                x->width = (uint16_t)c->in[2], x->height = (uint16_t)c->in[3];
                x->crop_x0 = (uint16_t)c->in[4], x->crop_y0 = (uint16_t)c->in[5];
                x->crop_x1 = (uint16_t)c->in[6], x->crop_y1 = (uint16_t)c->in[7];
            }
        }
    reply(c, 1, 0, 0);
}
/* SetPackageParameterWithContext (20 bytes) and WithContextDetail (28): camera and context selects, flip and effect
   bytes, then the size preset or the detail resolution */
static void package(SnxCtrIpc* c, bool detail) {
    unsigned char raw[28];
    for (unsigned i = 0; i < (detail ? 7u : 5u); ++i) snx_st_le32(raw + i * 4, c->in[1 + i]);
    uint32_t cameras = raw[0], context_select = raw[1];
    if (!valid(cameras, 3) || !valid(context_select, 2) || (!detail && raw[4] >= 8)) {
        reply(c, 1, 0, CAM_INVALID_ENUM);
        return;
    }
    for (unsigned i = 0; i < 3; ++i)
        for (unsigned j = 0; j < 2; ++j) {
            if (!((cameras >> i) & 1) || !((context_select >> j) & 1)) continue;
            SnxCtrCamContext* x = &c->process->cam.camera[i].contexts[j];
            uint8_t format = x->format;
            if (detail) {
                x->width = snx_ld_le16(raw + 4), x->height = snx_ld_le16(raw + 6);
                x->crop_x0 = snx_ld_le16(raw + 8), x->crop_y0 = snx_ld_le16(raw + 10);
                x->crop_x1 = snx_ld_le16(raw + 12), x->crop_y1 = snx_ld_le16(raw + 14);
            } else
                *x = PRESETS[raw[4]];
            x->flip = raw[2], x->effect = raw[3], x->format = format;
        }
    reply(c, 1, 0, 0);
}
/* GetLatestVsyncTiming(port, count): the last count vsyncs' times (s64, us, newest first) in the caller's static
   buffer 0, as many as there are */
static void latest_vsyncs(SnxCtrIpc* c) {
    SnxCtrProcess* p = c->process;
    uint32_t count = c->in[2];
    unsigned char d[8];
    if (snx_ctr_read_ram(p->space, p->cpu.state.cpu.tpidruro + 0x180, d, sizeof d) != SNX_CTR_MEM_OK) {
        c->transport_result = 0xe0e01bf5u;
        return;
    }
    uint32_t address = snx_ld_le32(d + 4);
    if (!single(c->in[1], 2) || count > 5) {
        reply(c, 1, 2, CAM_OUT_OF_RANGE);
        c->out[2] = 2; /* static buffer 0, no bytes */
        c->out[3] = address;
        return;
    }
    const SnxCtrCamPort* q = &p->cam.port[first(c->in[1])];
    unsigned char out[5 * 8] = {0};
    for (uint32_t i = 0; i < count && i < q->vsync_count; ++i) snx_st_le64(out + i * 8, (uint64_t)q->vsync_us[i]);
    uint32_t size = count * 8;
    if ((snx_ld_le32(d) & 0x3fff) != 2 || snx_ld_le32(d) >> 14 < size) {
        snx_ctr_ipc_bad(c);
        return;
    }
    if (size && snx_ctr_write_ram(p->space, address, out, size) != SNX_CTR_MEM_OK) {
        c->transport_result = 0xe0e01bf5u;
        return;
    }
    reply(c, 1, 2, 0);
    c->out[2] = size << 14 | 2;
    c->out[3] = address;
}
void snx_ctr_cam_dispatch(SnxCtrIpc* c) {
    SnxCtrProcess* p = c->process;
    SnxCtrCam* cam = &p->cam;
    switch (c->in[0] >> 16) {
        case 0x01: /* StartCapture(ports): a port activated and not busy starts, and a receiving waiting for it */
            if (!snx_ctr_ipc_expect(c, 1, 0)) return;
            if (valid(c->in[1], 2))
                for (unsigned i = 0; i < 2; ++i)
                    if (((c->in[1] >> i) & 1) && !cam->port[i].busy && cam->port[i].active) {
                        cam->port[i].busy = 1;
                        if (cam->port[i].pending) {
                            cam->port[i].pending = 0;
                            start_receiving(p, i);
                        }
                    }
            reply(c, 1, 0, valid(c->in[1], 2) ? 0 : CAM_INVALID_ENUM);
            break;
        case 0x02: /* StopCapture(ports) */
            if (!snx_ctr_ipc_expect(c, 1, 0)) return;
            if (valid(c->in[1], 2))
                for (unsigned i = 0; i < 2; ++i)
                    if (((c->in[1] >> i) & 1) && cam->port[i].busy) {
                        cancel_receiving(p, i);
                        cam->port[i].busy = 0;
                    }
            reply(c, 1, 0, valid(c->in[1], 2) ? 0 : CAM_INVALID_ENUM);
            break;
        case 0x03: /* IsBusy(ports): every port named busy */
            if (!snx_ctr_ipc_expect(c, 1, 0)) return;
            reply(c, 2, 0, valid(c->in[1], 2) ? 0 : CAM_INVALID_ENUM);
            if (valid(c->in[1], 2)) {
                bool busy = true;
                for (unsigned i = 0; i < 2; ++i)
                    if ((c->in[1] >> i) & 1) busy = busy && cam->port[i].busy;
                c->out[2] = busy;
            }
            break;
        case 0x04: /* ClearBuffer(ports) */
            if (!snx_ctr_ipc_expect(c, 1, 0)) return;
            reply(c, 1, 0, 0);
            break;
        case 0x05: /* GetVsyncInterruptEvent(port) */
        case 0x06: /* GetBufferErrorInterruptEvent(port): never signalled, a blank camera loses nothing */
            if (!snx_ctr_ipc_expect(c, 1, 0)) return;
            {
                bool one = single(c->in[1], 2);
                SnxCtrCamPort* q = &cam->port[one ? first(c->in[1]) : 0];
                reply_event(c, (c->in[0] >> 16) == 5 ? &q->vsync_event : &q->error_event, CTR_RESET_ONESHOT, one);
            }
            break;
        case 0x07: /* SetReceiving(dest, port, size, unit, process): starts now on a busy port, else at StartCapture */
            if (!snx_ctr_ipc_expect(c, 4, 2)) return;
            {
                bool one = single(c->in[2], 2);
                unsigned i = one ? first(c->in[2]) : 0;
                SnxCtrCamPort* q = &cam->port[i];
                if (one) {
                    cancel_receiving(p, i);
                    q->dest = c->in[1];
                    q->dest_size = c->in[3];
                    if (q->busy)
                        start_receiving(p, i);
                    else
                        q->pending = 1;
                }
                reply_event(c, &q->completion_event, CTR_RESET_STICKY, one);
                if (one && c->state == SNX_CTR_IPC_RETURN && !c->out[1]) clear(p, q->completion_event);
            }
            break;
        case 0x08: /* IsFinishedReceiving(port) */
            if (!snx_ctr_ipc_expect(c, 1, 0)) return;
            reply(c, 2, 0, single(c->in[1], 2) ? 0 : CAM_INVALID_ENUM);
            if (single(c->in[1], 2)) {
                const SnxCtrCamPort* q = &cam->port[first(c->in[1])];
                c->out[2] = !(q->receiving || q->pending);
            }
            break;
        case 0x09: /* SetTransferLines(ports, lines, width, height) */
        case 0x0b: /* SetTransferBytes(ports, bytes, width, height) */
            if (!snx_ctr_ipc_expect(c, 4, 0)) return;
            if (valid(c->in[1], 2))
                for (unsigned i = 0; i < 2; ++i)
                    if ((c->in[1] >> i) & 1)
                        cam->port[i].transfer_bytes = (c->in[0] >> 16) == 9
                                                          ? (uint16_t)((c->in[2] & 0xffff) * (c->in[3] & 0xffff) * 2)
                                                          : (uint16_t)c->in[2];
            reply(c, 1, 0, valid(c->in[1], 2) ? 0 : CAM_INVALID_ENUM);
            break;
        case 0x0a: /* GetMaxLines(width, height) */
        case 0x0d: /* GetMaxBytes(width, height) */
            if (!snx_ctr_ipc_expect(c, 2, 0)) return;
            {
                uint32_t w = c->in[1] & 0xffff, h = c->in[2] & 0xffff;
                uint32_t v = (c->in[0] >> 16) == 0x0a ? max_lines(w, h) : max_bytes(w, h);
                reply(c, 2, 0, v ? 0 : CAM_OUT_OF_RANGE);
                c->out[2] = v;
            }
            break;
        case 0x0c: /* GetTransferBytes(port) */
            if (!snx_ctr_ipc_expect(c, 1, 0)) return;
            reply(c, 2, 0, single(c->in[1], 2) ? 0 : CAM_INVALID_ENUM);
            if (single(c->in[1], 2)) c->out[2] = cam->port[first(c->in[1])].transfer_bytes;
            break;
        case 0x0e: /* SetTrimming(ports, bool) */
            if (!snx_ctr_ipc_expect(c, 2, 0)) return;
            if (valid(c->in[1], 2))
                for (unsigned i = 0; i < 2; ++i)
                    if ((c->in[1] >> i) & 1) cam->port[i].trimming = (c->in[2] & 0xff) != 0;
            reply(c, 1, 0, valid(c->in[1], 2) ? 0 : CAM_INVALID_ENUM);
            break;
        case 0x0f: /* IsTrimming(port) */
            if (!snx_ctr_ipc_expect(c, 1, 0)) return;
            reply(c, 2, 0, single(c->in[1], 2) ? 0 : CAM_INVALID_ENUM);
            if (single(c->in[1], 2)) c->out[2] = cam->port[first(c->in[1])].trimming;
            break;
        case 0x10: /* SetTrimmingParams(ports, x0, y0, x1, y1) */
        case 0x12: /* SetTrimmingParamsCenter(ports, trim width, trim height, camera width, camera height) */
            if (!snx_ctr_ipc_expect(c, 5, 0)) return;
            if (valid(c->in[1], 2))
                for (unsigned i = 0; i < 2; ++i) {
                    if (!((c->in[1] >> i) & 1)) continue;
                    SnxCtrCamPort* q = &cam->port[i];
                    int a = (int)(c->in[2] & 0xffff), b = (int)(c->in[3] & 0xffff);
                    int e = (int)(c->in[4] & 0xffff), f = (int)(c->in[5] & 0xffff);
                    if ((c->in[0] >> 16) == 0x10)
                        q->x0 = (uint16_t)a, q->y0 = (uint16_t)b, q->x1 = (uint16_t)e, q->y1 = (uint16_t)f;
                    else { /* centred, in Azahar's int arithmetic stored to u16 */
                        q->x0 = (uint16_t)((e - a) / 2), q->y0 = (uint16_t)((f - b) / 2);
                        q->x1 = (uint16_t)(q->x0 + a), q->y1 = (uint16_t)(q->y0 + b);
                    }
                }
            reply(c, 1, 0, valid(c->in[1], 2) ? 0 : CAM_INVALID_ENUM);
            break;
        case 0x11: /* GetTrimmingParams(port) */
            if (!snx_ctr_ipc_expect(c, 1, 0)) return;
            reply(c, 5, 0, single(c->in[1], 2) ? 0 : CAM_INVALID_ENUM);
            if (single(c->in[1], 2)) {
                const SnxCtrCamPort* q = &cam->port[first(c->in[1])];
                c->out[2] = q->x0, c->out[3] = q->y0, c->out[4] = q->x1, c->out[5] = q->y1;
            }
            break;
        case 0x13: /* Activate(cameras): 0 deactivates both ports; the outer right camera is the second port's, the
                      others the first's, and both of those at once are refused */
            if (!snx_ctr_ipc_expect(c, 1, 0)) return;
            {
                uint32_t s = c->in[1] & 0xff;
                if (!valid(s, 3) || (s & 3) == 3) {
                    reply(c, 1, 0, CAM_INVALID_ENUM);
                    break;
                }
                if (!s)
                    for (unsigned i = 0; i < 2; ++i) {
                        if (cam->port[i].busy) {
                            cancel_receiving(p, i);
                            cam->port[i].busy = 0;
                        }
                        cam->port[i].active = 0;
                        cam->port[i].vsync_ns = 0;
                    }
                if (s & 3) activate_port(p, 0, s & 1 ? 0 : 1);
                if (s & 4) activate_port(p, 1, 2);
                reply(c, 1, 0, 0);
            }
            break;
        case 0x14: /* SwitchContext(cameras, context) */
            if (!snx_ctr_ipc_expect(c, 2, 0)) return;
            if (valid(c->in[1], 3) && single(c->in[2], 2))
                for (unsigned i = 0; i < 3; ++i)
                    if ((c->in[1] >> i) & 1) cam->camera[i].context = (uint8_t)first(c->in[2]);
            reply(c, 1, 0, valid(c->in[1], 3) && single(c->in[2], 2) ? 0 : CAM_INVALID_ENUM);
            break;
        case 0x1d: /* FlipImage(cameras, flip, contexts) */
            if (!snx_ctr_ipc_expect(c, 3, 0)) return;
            contexts(c, SET_FLIP, c->in[1], c->in[2], c->in[3]);
            break;
        case 0x1e: /* SetDetailSize(cameras, width, height, crop x0, y0, x1, y1, contexts) */
            if (!snx_ctr_ipc_expect(c, 8, 0)) return;
            contexts(c, SET_DETAIL, c->in[1], 0, c->in[8]);
            break;
        case 0x1f: /* SetSize(cameras, preset, contexts) */
            if (!snx_ctr_ipc_expect(c, 3, 0)) return;
            contexts(c, SET_SIZE, c->in[1], c->in[2], c->in[3]);
            break;
        case 0x20: /* SetFrameRate(cameras, rate) */
            if (!snx_ctr_ipc_expect(c, 2, 0)) return;
            if (!valid(c->in[1], 3) || (c->in[2] & 0xff) >= FRAME_RATES) {
                reply(c, 1, 0, CAM_INVALID_ENUM);
                break;
            }
            for (unsigned i = 0; i < 3; ++i)
                if ((c->in[1] >> i) & 1) cam->camera[i].frame_rate = (uint8_t)c->in[2];
            reply(c, 1, 0, 0);
            break;
        case 0x22: /* SetEffect(cameras, effect, contexts) */
            if (!snx_ctr_ipc_expect(c, 3, 0)) return;
            contexts(c, SET_EFFECT, c->in[1], c->in[2], c->in[3]);
            break;
        case 0x25: /* SetOutputFormat(cameras, format, contexts) */
            if (!snx_ctr_ipc_expect(c, 3, 0)) return;
            contexts(c, SET_FORMAT, c->in[1], c->in[2], c->in[3]);
            break;
        case 0x29: /* SynchronizeVsyncTiming(camera, camera) */
            if (!snx_ctr_ipc_expect(c, 2, 0)) return;
            reply(c, 1, 0, 0);
            break;
        case 0x2a:
            if (!snx_ctr_ipc_expect(c, 2, 0)) return;
            latest_vsyncs(c);
            break;
        case 0x2b: /* GetStereoCameraCalibrationData: Azahar's values, 64 bytes */
            if (!snx_ctr_ipc_expect(c, 0, 0)) return;
            {
                static const float values[10] = {1.001776f, 0.008322907f, -87.70484f, -7.640977f, 0.0f,
                                                 0.0f,      64.66875f,    64.76067f,  250.0f,     35.0f};
                reply(c, 17, 0, 0);
                for (unsigned i = 0; i < 10; ++i) memcpy(&c->out[3 + i], &values[i], 4);
                c->out[13] = 640u | 480u << 16; /* image width and height */
            }
            break;
        case 0x33: /* SetPackageParameterWithoutContext: 44 bytes Azahar takes and keeps none of */
            if (!snx_ctr_ipc_expect(c, 11, 0)) return;
            reply(c, 1, 0, 0);
            break;
        case 0x34:
            if (!snx_ctr_ipc_expect(c, 5, 0)) return;
            package(c, false);
            break;
        case 0x35:
            if (!snx_ctr_ipc_expect(c, 7, 0)) return;
            package(c, true);
            break;
        case 0x36: /* GetSuitableY2rStandardCoefficient: ITU-R BT.601 with 8 bit scaling, as Azahar answers */
            if (!snx_ctr_ipc_expect(c, 0, 0)) return;
            reply(c, 2, 0, 0);
            break;
        case 0x38: /* PlayShutterSound(sound) */
            if (!snx_ctr_ipc_expect(c, 1, 0)) return;
            reply(c, 1, 0, 0);
            break;
        case 0x39: /* DriverInitialize: every camera's contexts to their defaults, every port cleared */
            if (!snx_ctr_ipc_expect(c, 0, 0)) return;
            for (unsigned i = 0; i < 3; ++i) {
                cam->camera[i].context = 0;
                for (unsigned j = 0; j < 2; ++j) {
                    cam->camera[i].contexts[j] = PRESETS[j ? 0 : 5]; /* VGA, DS_LCD */
                    cam->camera[i].contexts[j].flip = i == 1;        /* the inner camera mirrored */
                }
            }
            for (unsigned i = 0; i < 2; ++i) {
                SnxCtrCamPort* q = &cam->port[i];
                clear(p, q->completion_event), clear(p, q->error_event), clear(p, q->vsync_event);
                uint32_t events[3] = {q->completion_event, q->error_event, q->vsync_event};
                uint8_t camera = q->camera;
                *q = (SnxCtrCamPort){.completion_event = events[0],
                                     .error_event = events[1],
                                     .vsync_event = events[2],
                                     .camera = camera,
                                     .transfer_bytes = 256};
            }
            cam->initialized = 1;
            reply(c, 1, 0, 0);
            break;
        case 0x3a: /* DriverFinalize */
            if (!snx_ctr_ipc_expect(c, 0, 0)) return;
            cancel_receiving(p, 0);
            cancel_receiving(p, 1);
            cam->initialized = 0;
            reply(c, 1, 0, 0);
            break;
        default: c->state = SNX_CTR_IPC_UNKNOWN; break;
    }
}
