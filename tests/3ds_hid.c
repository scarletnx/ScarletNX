/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* hid:USER: the replies of its commands, and the samples its shared memory gets at each update, with a scripted press
   as the player's input (Azahar's hid.cpp, 3dbrew's HID Shared Memory) */
#include "3ds/input/input.h"
#include "3ds/runtime/internal.h"
#include "3ds/ipc/ctr_ipc.h"
#include "3ds/services/hid.h"
#include "snx_settings.h"
#include "support/3ds_guest.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(void) {
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
    puts("CTR hid:USER commands, pad samples from scripted input and sensor samples passed");
    return 0;
}
