/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* hid:USER: the replies of its commands, and the samples its shared memory gets at each update, with a scripted press
   as the player's input (Azahar's hid.cpp, 3dbrew's HID Shared Memory) */
#include "3ds/input/input.h"
#include "3ds/gpu/screens.h"
#include <math.h>
#include "3ds/runtime/internal.h"
#include "3ds/ipc/ctr_ipc.h"
#include "3ds/services/hid.h"
#include "snx_settings.h"
#include "support/3ds_guest.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
/* Where a point of the window's image is on the bottom screen (400x240 above 320x240, scaled as one 400x480 block) */
static void touch_locate(void) {
    unsigned x, y;
    SnxCtrWindowLayout l = {800, 960, 400, 240, 320, 240, SNX_CTR_SCREENS_STACKED, false}; /* twice the size: the bottom screen starts at (80, 480) */
    assert(snx_ctr_touch_locate(&l, 80, 480, &x, &y) && x == 0 && y == 0);
    assert(snx_ctr_touch_locate(&l, 280, 580, &x, &y) && x == 100 && y == 50);
    assert(snx_ctr_touch_locate(&l, 718.9, 958.9, &x, &y) && x == 319 && y == 239);
    assert(!snx_ctr_touch_locate(&l, 79, 600, &x, &y) && x == 0 && y == 60); /* left of it: held to its edge */
    assert(!snx_ctr_touch_locate(&l, 400, 100, &x, &y) && x == 160 && y == 0); /* on the top screen */
    assert(!snx_ctr_touch_locate(&l, 799, 959, &x, &y) && x == 319 && y == 239);
    l.image_w = 1000; /* a wider window centres the pair: 100 pixels of black each side */
    assert(snx_ctr_touch_locate(&l, 180, 480, &x, &y) && x == 0 && y == 0);
    assert(snx_ctr_touch_locate(&l, 100, 480, &x, &y) == false && x == 0);
    l.image_w = 0; /* no window yet */
    assert(!snx_ctr_touch_locate(&l, 100, 600, &x, &y) && !x && !y);
}
/* The screens' arrangements: where each one falls in the image, and the touch screen following the bottom one */
static void screens_layouts(void) {
    SnxCtrScreenRect r[2];
    SnxCtrWindowLayout l = {720, 480, 400, 240, 320, 240, SNX_CTR_SCREENS_SIDE_BY_SIDE, false};
    snx_ctr_screens_place(&l, r); /* 720x240 in 720x480: one to one, centred up and down */
    assert(r[0].x == 0 && r[0].y == 120 && r[0].w == 400 && r[0].h == 240);
    assert(r[1].x == 400 && r[1].y == 120 && r[1].w == 320 && r[1].h == 240);
    l.mode = SNX_CTR_SCREENS_LARGE_TOP; /* the bottom screen 0.4 of its size at the lower right */
    snx_ctr_screens_place(&l, r);
    assert(fabs(r[0].x) < 1e-6 && fabs(r[1].x - r[0].w) < 1e-6 && fabs(r[1].y + r[1].h - r[0].y - r[0].h) < 1e-6);
    assert(r[1].w < r[0].w / 3 && fabs(r[1].w / r[1].h - 320.0 / 240) < 1e-9);
    l.mode = SNX_CTR_SCREENS_TOP_ONLY;
    snx_ctr_screens_place(&l, r);
    assert(r[0].w > 0 && !r[1].w);
    unsigned x, y;
    assert(!snx_ctr_touch_locate(&l, 10, 10, &x, &y)); /* no bottom screen to touch */
    l.mode = SNX_CTR_SCREENS_BOTTOM_ONLY;
    assert(snx_ctr_touch_locate(&l, 360, 240, &x, &y) && x == 160 && y == 120); /* 320x240 at 2x, from 40 */
    l.mode = SNX_CTR_SCREENS_LARGE_BOTTOM;
    snx_ctr_screens_place(&l, r);
    assert(snx_ctr_touch_locate(&l, r[1].x + r[1].w / 2, r[1].y + r[1].h / 2, &x, &y) && x == 160 && y == 120);
    /* whole-number scale: 400x480 in 1000x1000 is 2x, on whole pixels, the pair centred */
    l = (SnxCtrWindowLayout){1000, 1000, 400, 240, 320, 240, SNX_CTR_SCREENS_STACKED, true};
    snx_ctr_screens_place(&l, r);
    assert(r[0].w == 800 && r[0].h == 480 && r[0].x == 100 && r[0].y == 20 && r[1].w == 640 && r[1].x == 180);
    l.image_w = 300; /* smaller than one pixel each: scaled down all the same */
    l.image_h = 400;
    snx_ctr_screens_place(&l, r);
    assert(r[0].w > 0 && r[0].w <= 300);
}
/* A gamepad's motion in Azahar's axes: a pad held upright reads +y of gravity, which is the console's rest value (y at
   -1 g, -512 raw), and the rates come out in degrees a second with x and z negated */
static void motion_axes(void) {
    SnxCtrInput input = {0};
    const float upright[3] = {0, 9.80665f, 0}, still[3] = {0};
    snx_ctr_input_motion(upright, still, &input);
    assert(input.motion && input.accel[0] == 0 && input.accel[1] == -1.0f && input.accel[2] == 0);
    assert(input.gyro[0] == 0 && input.gyro[1] == 0 && input.gyro[2] == 0);
    const float tilted[3] = {9.80665f, 0, -4.903325f}, turning[3] = {1.0f, 2.0f, 3.0f};
    snx_ctr_input_motion(tilted, turning, &input);
    assert(input.accel[0] == 1.0f && input.accel[1] == 0 && input.accel[2] == -0.5f);
    assert(fabsf(input.gyro[0] + 57.29578f) < 1e-3f && fabsf(input.gyro[1] - 114.59156f) < 1e-3f &&
           fabsf(input.gyro[2] + 171.88734f) < 1e-3f);
}
int main(void) {
    touch_locate();
    screens_layouts();
    motion_axes();
    unsigned char code[0x3000] = {0};
    SnxCtrImage image = ctr_image(code, sizeof code, 0x1000);
    /* A and Start from the start, then nothing: each held for the press length */
    strcpy(g_settings.ctr_script, "0:9,100000:1");
    g_settings.press_length = 60000;
    snx_ctr_input_init();
    SnxCtrProcess* p = snx_ctr_process_new(&image, SNX_CTR_OLD);
    assert(p);
    uint32_t session = 0;
    assert(!snx_ctr_session_create(p, SNX_CTR_SERVICE_HID, &session));
    assert(snx_ctr_hid_deadline(p) == UINT64_MAX); /* nothing samples before the game asks for the handles */
    /* the shared memory and the five events, as Azahar's GetIPCHandles answers */
    assert(ctr_send_header(p, session, 0x000a0000) == 0x000a0047 && ctr_command_word(p, 2) == 0x14000000);
    for (unsigned i = 0; i < 6; ++i) {
        uint32_t id = snx_ctr_handle_object(p, ctr_command_word(p, 3 + i));
        assert(id && id == p->hid_objects[i] && p->objects[id].type == (i ? CTR_OBJ_EVENT : CTR_OBJ_SHARED));
    }
    uint64_t due = snx_ctr_hid_deadline(p);
    assert(due == p->hid.next_pad_ns && due != UINT64_MAX);
    /* the first sample: entry 0 has A and Start, both new, the index says so, and both pad events are signalled */
    snx_ctr_hid_update(p, due);
    assert(ctr_hid_shared(p, 0x10, 4) == 0 && ctr_hid_shared(p, 0x1c, 4) == (SNX_CTR_PAD_A | SNX_CTR_PAD_START));
    assert(ctr_hid_shared(p, 0x28, 4) == 9 && ctr_hid_shared(p, 0x2c, 4) == 9 && !ctr_hid_shared(p, 0x30, 4) &&
           !ctr_hid_shared(p, 0x34, 4));
    assert(ctr_hid_shared(p, 0x00, 4) | ctr_hid_shared(p, 0x04, 4)); /* the index wrapped to 0: the tick it did */
    assert(p->objects[p->hid_objects[1]].data.event.signaled && p->objects[p->hid_objects[2]].data.event.signaled);
    assert(!p->objects[p->hid_objects[3]].data.event.signaled && !ctr_hid_shared(p, 0xa8 + 0x20 + 4, 4)); /* no touch */
    assert(p->hid.next_pad_ns > due);
    /* the next one: held, so nothing new or released */
    snx_ctr_hid_update(p, p->hid.next_pad_ns);
    assert(ctr_hid_shared(p, 0x10, 4) == 1 && ctr_hid_shared(p, 0x38, 4) == 9 && !ctr_hid_shared(p, 0x3c, 4) &&
           !ctr_hid_shared(p, 0x40, 4));
    /* a touch of the bottom screen: its pixel and valid in the next touch entry, the index moved on; letting go says
       not valid in the one after */
    assert(ctr_hid_shared(p, 0xa8 + 0x10, 4) == 1);
    snx_ctr_input_touch(true, 100, 50);
    snx_ctr_hid_update(p, p->hid.next_pad_ns);
    assert(ctr_hid_shared(p, 0xa8 + 0x10, 4) == 2);
    assert(ctr_hid_shared(p, 0xa8 + 0x20 + 2 * 8, 2) == 100 && ctr_hid_shared(p, 0xa8 + 0x20 + 2 * 8 + 2, 2) == 50);
    assert(ctr_hid_shared(p, 0xa8 + 0x20 + 2 * 8 + 4, 4) == 1);
    snx_ctr_input_touch(false, 0, 0);
    snx_ctr_hid_update(p, p->hid.next_pad_ns);
    assert(ctr_hid_shared(p, 0xa8 + 0x10, 4) == 3 && !ctr_hid_shared(p, 0xa8 + 0x20 + 3 * 8 + 4, 4));
    /* the sensors sample between their enable and their disable */
    assert(!p->hid.next_accelerometer_ns && !p->hid.next_gyroscope_ns);
    assert(ctr_send_header(p, session, 0x00110000) == 0x00110040 && p->hid.next_accelerometer_ns);
    assert(ctr_send_header(p, session, 0x00130000) == 0x00130040 && p->hid.next_gyroscope_ns);
    snx_ctr_hid_update(p, p->hid.next_accelerometer_ns > p->hid.next_gyroscope_ns ? p->hid.next_accelerometer_ns
                                                                                   : p->hid.next_gyroscope_ns);
    assert(ctr_hid_shared(p, 0x118, 4) == 0 && (int16_t)ctr_hid_shared(p, 0x128 + 2, 2) == -512 &&
           !ctr_hid_shared(p, 0x128, 2));
    assert((int16_t)ctr_hid_shared(p, 0x120 + 4, 2) == -1024); /* the raw entry Azahar derives: z from -y */
    assert(ctr_hid_shared(p, 0x168, 4) == 0 && p->objects[p->hid_objects[4]].data.event.signaled);
    assert(ctr_send_header(p, session, 0x00120000) == 0x00120040 && !p->hid.next_accelerometer_ns);
    assert(ctr_send_header(p, session, 0x00140000) == 0x00140040 && !p->hid.next_gyroscope_ns);
    /* the gyroscope's constants and the volume slider */
    assert(ctr_send_header(p, session, 0x00150000) == 0x00150080 && ctr_command_word(p, 2) == 0x41660000);
    assert(ctr_send_header(p, session, 0x00160000) == 0x00160180);
    const uint32_t calibration[5] = {0x1a2c0000u, 0x0000e5d4u, 0xe5d41a2cu, 0x1a2c0000u, 0x0000e5d4u};
    for (unsigned i = 0; i < 5; ++i) assert(ctr_command_word(p, 2 + i) == calibration[i]);
    assert(ctr_send_header(p, session, 0x00170000) == 0x00170080 && ctr_command_word(p, 2) == 0x3f);
    /* a second GetIPCHandles hands out the same objects */
    assert(ctr_send_header(p, session, 0x000a0000) == 0x000a0047 &&
           snx_ctr_handle_object(p, ctr_command_word(p, 3)) == p->hid_objects[0]);
    snx_ctr_process_free(p);
    snx_ctr_input_free();
    puts("CTR hid:USER commands, pad and touch samples from scripted input and sensor samples passed");
    return 0;
}
