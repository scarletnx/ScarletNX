/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* cam:u (services/cam.c) through srv:, as a game reaches it: the replies of its commands and their errors, a port
   activated and capturing, a receiving that completes at the frame rate's latency with the blank camera's black frame
   (YUV422 and RGB565, whole and trimmed), the vsyncs and their times, as Azahar's cam.cpp gives them */
#include "3ds/ipc/ctr_ipc.h"
#include "3ds/runtime/internal.h"
#include "3ds/services/cam.h"
#include "support/3ds_guest.h"
#include <assert.h>
#include <stdio.h>

#define DEST    0x300000u /* the data page: the frame's buffer */
#define TIMINGS 0x300800u /* static buffer 0, for GetLatestVsyncTiming */
#define INVALID 0xe0e053edu
#define OUT_OF  0xe0e053fdu

static uint32_t call(SnxCtrProcess* p, uint32_t session, const uint32_t* words, unsigned count) {
    ctr_call(p, session, words, count, SNX_CTR_SVC_RETURN, 0);
    return ctr_command_word(p, 0);
}
/* the next due vsync or receiving's end */
static void run_due(SnxCtrProcess* p) { snx_ctr_cam_update(p, snx_ctr_cam_deadline(p)); }
static uint32_t byte_at(SnxCtrProcess* p, uint32_t va) { return ctr_word(p, va & ~3u) >> (va & 3) * 8 & 0xff; }
static void fill(SnxCtrProcess* p, uint32_t size) {
    for (uint32_t i = 0; i < size; i += 4) ctr_put(p, DEST + i, 0xaaaaaaaau);
}
int main(void) {
    unsigned char code[0x3000] = {0};
    SnxCtrImage image = ctr_image(code, sizeof code, 0x1000);
    SnxCtrProcess* p = snx_ctr_process_new(&image, SNX_CTR_OLD);
    assert(p);
    uint32_t srv = ctr_srv(p);
    const uint32_t registration[] = {0x00010002, 0x20, 0};
    ctr_call(p, srv, registration, 3, SNX_CTR_SVC_RETURN, 0);
    uint32_t cam = ctr_service_handle(p, srv, "cam:u");
    assert(ctr_send_header(p, cam, 0x00390000) == 0x00390040); /* DriverInitialize */
    /* the transfer arithmetic: 640x480 takes 4 lines (2560 bytes over its width) and 2560 bytes; a width of 0 has no
       answer, and a picture that is no whole number of 256-byte units neither */
    const uint32_t lines_vga[] = {0x000a0080, 640, 480}, bytes_vga[] = {0x000d0080, 640, 480};
    const uint32_t lines_none[] = {0x000a0080, 0, 4}, bytes_odd[] = {0x000d0080, 3, 3};
    assert(call(p, cam, lines_vga, 3) == 0x000a0080 && !ctr_command_word(p, 1) && ctr_command_word(p, 2) == 4);
    assert(call(p, cam, bytes_vga, 3) == 0x000d0080 && !ctr_command_word(p, 1) && ctr_command_word(p, 2) == 2560);
    assert(call(p, cam, lines_none, 3) == 0x000a0080 && ctr_command_word(p, 1) == OUT_OF);
    assert(call(p, cam, bytes_odd, 3) == 0x000d0080 && ctr_command_word(p, 1) == OUT_OF);
    /* the selects: two cameras of the first port at once, a frame rate past the table, a port select of 4 */
    const uint32_t both[] = {0x00130040, 3}, rate_13[] = {0x00200080, 1, 13}, busy_4[] = {0x00030040, 4};
    assert(call(p, cam, both, 2) == 0x00130040 && ctr_command_word(p, 1) == INVALID);
    assert(call(p, cam, rate_13, 3) == 0x00200040 && ctr_command_word(p, 1) == INVALID);
    assert(call(p, cam, busy_4, 2) == 0x00030080 && ctr_command_word(p, 1) == INVALID);

    /* the outer camera on the first port at 30 fps (33 ms), capturing */
    const uint32_t rate_30[] = {0x00200080, 1, 8}, activate[] = {0x00130040, 1}, start[] = {0x00010040, 1};
    const uint32_t busy[] = {0x00030040, 1};
    assert(call(p, cam, rate_30, 3) == 0x00200040 && !ctr_command_word(p, 1));
    assert(call(p, cam, activate, 2) == 0x00130040 && !ctr_command_word(p, 1));
    assert(call(p, cam, start, 2) == 0x00010040);
    assert(call(p, cam, busy, 2) == 0x00030080 && ctr_command_word(p, 2) == 1);
    /* the vsync event: signalled at each vsync, whose time GetLatestVsyncTiming gives in static buffer 0 */
    const uint32_t vsync[] = {0x00050040, 1};
    assert(call(p, cam, vsync, 2) == 0x00050042 && !ctr_command_word(p, 1));
    uint32_t vsync_event = snx_ctr_handle_object(p, ctr_command_word(p, 3));
    assert(vsync_event && vsync_event == p->cam.port[0].vsync_event);
    uint64_t due = p->cam.port[0].vsync_ns;
    run_due(p);
    assert(p->objects[vsync_event].data.event.signaled && p->cam.port[0].vsync_ns == due + 33000000u);
    ctr_put(p, SNX_CTR_INITIAL_TLS + 0x180, 16u << 14 | 2);
    ctr_put(p, SNX_CTR_INITIAL_TLS + 0x184, TIMINGS);
    const uint32_t timings[] = {0x002a0080, 1, 2};
    assert(call(p, cam, timings, 3) == 0x002a0042 && !ctr_command_word(p, 1) &&
           ctr_command_word(p, 2) == (16u << 14 | 2));
    uint64_t us = (uint64_t)ctr_word(p, TIMINGS) | (uint64_t)ctr_word(p, TIMINGS + 4) << 32;
    assert(us == due / 1000 && !ctr_word(p, TIMINGS + 8) && !ctr_word(p, TIMINGS + 12)); /* one vsync so far */

    /* a receiving of 0x200 bytes on the busy port: not finished until the latency passes, then the YUV422 black */
    fill(p, 0x400);
    const uint32_t receive[] = {0x00070102, DEST, 1, 0x200, 0x100, 0, CTR_PROCESS_HANDLE}, finished[] = {0x00080040, 1};
    assert(call(p, cam, receive, 7) == 0x00070042 && !ctr_command_word(p, 1));
    uint32_t done_event = snx_ctr_handle_object(p, ctr_command_word(p, 3));
    assert(done_event && done_event == p->cam.port[0].completion_event);
    assert(call(p, cam, finished, 2) == 0x00080080 && !ctr_command_word(p, 2));
    while (p->cam.port[0].receiving) run_due(p);
    assert(p->objects[done_event].data.event.signaled);
    assert(call(p, cam, finished, 2) == 0x00080080 && ctr_command_word(p, 2) == 1);
    assert(byte_at(p, DEST) == 0 && byte_at(p, DEST + 1) == 0x80 && byte_at(p, DEST + 0x1ff) == 0x80 &&
           byte_at(p, DEST + 0x200) == 0xaa);
    /* RGB565 trimmed to 16x2 of the 256x192 picture: 64 bytes of 0, the rest left */
    fill(p, 0x400);
    const uint32_t rgb[] = {0x002500c0, 1, 1, 1}, trimming[] = {0x000e0080, 1, 1};
    const uint32_t rect[] = {0x00100140, 1, 0, 0, 16, 2};
    assert(call(p, cam, rgb, 4) == 0x00250040 && !ctr_command_word(p, 1));
    assert(call(p, cam, trimming, 3) == 0x000e0040 && call(p, cam, rect, 6) == 0x00100040);
    assert(call(p, cam, receive, 7) == 0x00070042);
    while (p->cam.port[0].receiving) run_due(p);
    assert(!ctr_word(p, DEST) && !ctr_word(p, DEST + 60) && byte_at(p, DEST + 64) == 0xaa);
    /* centred trimming: 16x2 of 256x192 */
    const uint32_t centre[] = {0x00120140, 1, 16, 2, 256, 192}, get_rect[] = {0x00110040, 1};
    assert(call(p, cam, centre, 6) == 0x00120040);
    assert(call(p, cam, get_rect, 2) == 0x00110140 && ctr_command_word(p, 2) == 120 && ctr_command_word(p, 3) == 95 &&
           ctr_command_word(p, 4) == 136 && ctr_command_word(p, 5) == 97);
    /* a receiving set before the capture waits for StartCapture */
    const uint32_t stop[] = {0x00020040, 1};
    assert(call(p, cam, stop, 2) == 0x00020040 && call(p, cam, receive, 7) == 0x00070042);
    assert(p->cam.port[0].pending && !p->cam.port[0].receiving);
    assert(call(p, cam, start, 2) == 0x00010040 && p->cam.port[0].receiving && !p->cam.port[0].pending);
    /* the calibration data, Azahar's: 17 words, the picture's 640x480 in the twelfth of the data */
    assert(ctr_send_header(p, cam, 0x002b0000) == 0x002b0440 && ctr_command_word(p, 13) == (640u | 480u << 16));
    /* Activate(0) stops the ports: no vsync is due any more */
    const uint32_t off[] = {0x00130040, 0};
    assert(call(p, cam, off, 2) == 0x00130040 && !p->cam.port[0].active && !p->cam.port[0].busy &&
           snx_ctr_cam_deadline(p) == UINT64_MAX);
    assert(ctr_send_header(p, cam, 0x003a0000) == 0x003a0040); /* DriverFinalize */
    snx_ctr_process_free(p);
    puts("3ds cam:u: its commands, the black frames and the vsyncs passed");
    return 0;
}
