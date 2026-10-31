/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* cam:u: the replies of its commands, the checks of their arguments, the transfer arithmetic, and a receiving that ends
   in a black frame and its event, as Azahar's cam.cpp has them with no camera */
#include "3ds/runtime/internal.h"
#include "3ds/ipc/ctr_ipc.h"
#include "3ds/services/cam.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define COMMAND (SNX_CTR_INITIAL_TLS + 0x80)
#define DEST    0x300000u
static uint32_t word(SnxCtrProcess* p, unsigned index) {
    unsigned char b[4];
    assert(snx_ctr_read_ram(p->space, COMMAND + index * 4, b, 4) == SNX_CTR_MEM_OK);
    return (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
}
/* sends the header and up to seven words after it on the session; the reply's header, its result in word(p, 1) */
static uint32_t send(SnxCtrProcess* p, uint32_t session, uint32_t header, const uint32_t args[8]) {
    uint32_t in[9] = {header};
    for (unsigned i = 0; args && i < 8; ++i) in[i + 1] = args[i];
    unsigned char bytes[36];
    for (unsigned i = 0; i < 9; ++i)
        for (unsigned j = 0; j < 4; ++j) bytes[i * 4 + j] = (unsigned char)(in[i] >> (j * 8));
    assert(snx_ctr_write_ram(p->space, COMMAND, bytes, sizeof bytes) == SNX_CTR_MEM_OK);
    p->cpu.state.cpu.r[0] = session;
    assert(snx_ctr_svc(p, 0x32) == SNX_CTR_SVC_RETURN && !p->cpu.state.cpu.r[0]);
    return word(p, 0);
}
#define OK(header, args) (send(p, s, header, args) == (((header) & 0xffff0000u) | 0x40) && !word(p, 1))
#define OKW(header, ...) OK(header, ((const uint32_t[8]){__VA_ARGS__}))
#define ENUM_ERROR 0xe0e053edu
#define RANGE_ERROR 0xe0e053fdu
int main(void) {
    unsigned char code[0x3000] = {0};
    SnxCtrImage image = {.segment = {{0x100000, 1, 16}, {0x200000, 1, 4}, {0x300000, 1, 4}},
                         .stack_size = 0x4000,
                         .main_priority = 0x30,
                         .bss_size = 0x1000,
                         .code = code,
                         .code_size = sizeof code};
    SnxCtrProcess* p = snx_ctr_process_new(&image, SNX_CTR_OLD);
    assert(p);
    uint32_t s = 0;
    assert(!snx_ctr_session_create(p, SNX_CTR_SERVICE_CAM, &s));
    /* DriverInitialize, and the transfer arithmetic: a 256x192 picture is 98304 bytes, a unit of 2560 at most */
    assert(OK(0x00390000, NULL));
    const uint32_t ds[8] = {256, 192}, vga[8] = {640, 480}, odd[8] = {10, 10};
    assert(send(p, s, 0x000a0080, ds) == 0x000a0080 && !word(p, 1) && word(p, 2) == 4); /* 5 would fit, but 192 lines do not divide by it */
    assert(send(p, s, 0x000d0080, ds) == 0x000d0080 && !word(p, 1) && word(p, 2) == 2048);
    assert(send(p, s, 0x000a0080, vga) == 0x000a0080 && !word(p, 1) && word(p, 2) == 2);
    assert(send(p, s, 0x000d0080, vga) == 0x000d0080 && !word(p, 1) && word(p, 2) == 2560);
    send(p, s, 0x000a0080, odd);
    assert(word(p, 1) == RANGE_ERROR);
    /* a port is named by a bit; none, or a number past the two, is an enum error */
    const uint32_t port1[8] = {1}, port2[8] = {2}, none[8] = {0}, both[8] = {3}, four[8] = {4};
    send(p, s, 0x00030040, none);
    assert(word(p, 1) == ENUM_ERROR);
    send(p, s, 0x00030040, both);
    assert(word(p, 1) == ENUM_ERROR);
    send(p, s, 0x00010040, four);
    assert(word(p, 1) == ENUM_ERROR);
    /* transfer bytes: 256 at first, set in units of 256 up to 2560 that divide the picture */
    assert(send(p, s, 0x000c0040, port1) == 0x000c0080 && word(p, 2) == 256);
    const uint32_t bytes[8] = {1, 2048, 256, 192}, bad_bytes[8] = {1, 100, 256, 192};
    assert(OK(0x000b0100, bytes));
    assert(send(p, s, 0x000c0040, port1) == 0x000c0080 && word(p, 2) == 2048);
    send(p, s, 0x000b0100, bad_bytes);
    assert(word(p, 1) == RANGE_ERROR);
    const uint32_t lines[8] = {1, 4, 256, 192};
    assert(OK(0x00090100, lines));
    assert(send(p, s, 0x000c0040, port1) == 0x000c0080 && word(p, 2) == 2048);
    /* trimming: a window inside the context's picture, and the centred form */
    const uint32_t trim_on[8] = {1, 1}, window[8] = {1, 10, 20, 100, 120}, outside[8] = {1, 10, 20, 100, 400};
    assert(OK(0x000e0080, trim_on));
    assert(send(p, s, 0x000f0040, port1) == 0x000f0080 && word(p, 2) == 1);
    assert(OK(0x00100140, window));
    assert(send(p, s, 0x00110040, port1) == 0x00110140 && !word(p, 1) && word(p, 2) == 10 && word(p, 3) == 20 &&
           word(p, 4) == 100 && word(p, 5) == 120);
    send(p, s, 0x00100140, outside);
    assert(word(p, 1) == RANGE_ERROR);
    const uint32_t center[8] = {1, 100, 80, 256, 192};
    assert(OK(0x00120140, center));
    send(p, s, 0x00110040, port1);
    assert(word(p, 2) == 78 && word(p, 3) == 56 && word(p, 4) == 178 && word(p, 5) == 136);
    /* the camera settings: sizes change the context's picture, which the trimming is checked against */
    const uint32_t size_vga[8] = {1, 0, 1}, bad_size[8] = {1, 9, 1}, bad_camera[8] = {0, 0, 1};
    assert(OK(0x001f00c0, size_vga));
    assert(OKW(0x00140080, 1, 1)); /* SwitchContext(camera 0, context 1) */
    assert(OKW(0x00130040, 1));
    send(p, s, 0x001f00c0, bad_size);
    assert(word(p, 1) == ENUM_ERROR);
    send(p, s, 0x001f00c0, bad_camera);
    assert(word(p, 1) == ENUM_ERROR);
    assert(OKW(0x00200080, 7, 8));           /* SetFrameRate */
    assert(OKW(0x001d00c0, 2, 1, 3));        /* FlipImage */
    assert(OKW(0x002200c0, 2, 3, 1));        /* SetEffect */
    assert(OKW(0x002500c0, 2, 1, 1));        /* SetOutputFormat */
    assert(OKW(0x001e0200, 4, 400, 240, 0, 0, 400, 240, 3)); /* SetDetailSize */
    assert(OKW(0x00380040, 1));              /* PlayShutterSound */
    send(p, s, 0x00380040, (const uint32_t[8]){3});
    assert(word(p, 1) == ENUM_ERROR);
    /* the vsync and error events are real, the same one each time, and not signalled before a capture runs */
    assert(send(p, s, 0x00050040, port1) == 0x00050042 && !word(p, 1));
    uint32_t vsync = snx_ctr_handle_object(p, word(p, 3));
    assert(vsync && p->objects[vsync].type == CTR_OBJ_EVENT && !p->objects[vsync].data.event.signaled);
    assert(send(p, s, 0x00050040, port1) == 0x00050042 && snx_ctr_handle_object(p, word(p, 3)) == vsync);
    assert(send(p, s, 0x00060040, port1) == 0x00060042 && !word(p, 1));
    /* SetReceiving: a buffer the process can write, a size the unit divides, the process handle; the black frame
       arrives, and the completion event is signalled, one frame after a capture is started */
    unsigned char filled[0x100];
    memset(filled, 0xaa, sizeof filled);
    assert(snx_ctr_write_ram(p->space, DEST, filled, sizeof filled) == SNX_CTR_MEM_OK);
    const uint32_t receive[8] = {DEST, 1, 0x100, 0x100, 0, 0xffff8001u};
    assert(send(p, s, 0x00070102, receive) == 0x00070042 && !word(p, 1));
    uint32_t done = snx_ctr_handle_object(p, word(p, 3));
    assert(done && p->objects[done].type == CTR_OBJ_EVENT && !p->objects[done].data.event.signaled);
    assert(send(p, s, 0x00080040, port1) == 0x00080080 && !word(p, 2)); /* not finished */
    snx_ctr_cam_update(p, snx_ctr_now_ns() + 1000000000u);            /* no capture runs: nothing happens */
    assert(!p->objects[done].data.event.signaled);
    assert(OK(0x00010040, port1));                                      /* StartCapture */
    assert(send(p, s, 0x00030040, port1) == 0x00030080 && word(p, 2) == 1);
    assert(snx_ctr_cam_deadline(p) < UINT64_MAX);
    snx_ctr_cam_update(p, snx_ctr_now_ns() + 1000000000u);
    assert(p->objects[done].data.event.signaled && p->objects[vsync].data.event.signaled);
    unsigned char frame[0x100];
    assert(snx_ctr_read_ram(p->space, DEST, frame, sizeof frame) == SNX_CTR_MEM_OK);
    for (unsigned i = 0; i < sizeof frame; ++i) assert(!frame[i]);
    assert(send(p, s, 0x00080040, port1) == 0x00080080 && word(p, 2) == 1);
    /* a size the unit does not divide, a bad port, a bad process handle */
    const uint32_t bad_unit[8] = {DEST, 1, 0x100, 0x30, 0, 0xffff8001u}, bad_port[8] = {DEST, 3, 0x100, 0x100, 0, 0xffff8001u},
                   bad_handle[8] = {DEST, 1, 0x100, 0x100, 0, 0x1234u};
    send(p, s, 0x00070102, bad_unit);
    assert(word(p, 1) == 0xe0e053ecu);
    send(p, s, 0x00070102, bad_port);
    assert(word(p, 1) == ENUM_ERROR);
    send(p, s, 0x00070102, bad_handle);
    assert(word(p, 1) == 0xd8e007f7u);
    /* StopCapture ends it, and nothing is due */
    assert(OK(0x00020040, port1));
    assert(send(p, s, 0x00030040, port1) == 0x00030080 && !word(p, 2));
    assert(snx_ctr_cam_deadline(p) == UINT64_MAX);
    /* the second port is the outer right camera's, with its own events */
    assert(send(p, s, 0x00050040, port2) == 0x00050042 && snx_ctr_handle_object(p, word(p, 3)) != vsync);
    assert(OK(0x003a0000, NULL)); /* DriverFinalize */
    snx_ctr_process_free(p);
    puts("CTR cam:u commands, arithmetic, events and the black frame passed");
    return 0;
}
