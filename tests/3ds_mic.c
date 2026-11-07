/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* mic:u (services/mic.c): the replies of its commands, the settings a game stores and the buffer full event, as
   Azahar's mic_u.cpp answers with no input device, reached through srv: as a game does */
#include "3ds/ipc/ctr_ipc.h"
#include "3ds/runtime/internal.h"
#include "support/3ds_guest.h"
#include <assert.h>
#include <stdio.h>

/* the reply's header to a request of these words */
static uint32_t call(SnxCtrProcess* p, uint32_t session, const uint32_t* words, unsigned count) {
    ctr_call(p, session, words, count, SNX_CTR_SVC_RETURN, 0);
    assert(!ctr_command_word(p, 1));
    return ctr_command_word(p, 0);
}
int main(void) {
    unsigned char code[0x3000] = {0};
    SnxCtrImage image = ctr_image(code, sizeof code, 0x1000);
    SnxCtrProcess* p = snx_ctr_process_new(&image, SNX_CTR_OLD);
    assert(p);
    uint32_t srv = ctr_srv(p);
    const uint32_t registration[] = {0x00010002, 0x20, 0};
    ctr_call(p, srv, registration, 3, SNX_CTR_SVC_RETURN, 0);
    uint32_t mic = ctr_service_handle(p, srv, "mic:u");
    /* nothing samples, and the settings start at zero */
    assert(ctr_send_header(p, mic, 0x00060000) == 0x00060080 && !ctr_command_word(p, 2));
    assert(ctr_send_header(p, mic, 0x00090000) == 0x00090080 && !ctr_command_word(p, 2));
    assert(ctr_send_header(p, mic, 0x000b0000) == 0x000b0080 && !ctr_command_word(p, 2));
    assert(ctr_send_header(p, mic, 0x000e0000) == 0x000e0080 && !ctr_command_word(p, 2));
    /* MapSharedMem takes the size and a handle, UnmapSharedMem nothing: both answer a result alone */
    const uint32_t map[] = {0x00010042, 0x1000, 0, CTR_PROCESS_HANDLE};
    assert(call(p, mic, map, 4) == 0x00010040);
    assert(ctr_send_header(p, mic, 0x00020000) == 0x00020040);
    /* StartSampling (encoding, rate, offset, size, loop) starts it, AdjustSampling changes the rate, StopSampling ends
       it */
    const uint32_t start[] = {0x00030140, 1, 2, 0, 0x800, 1}, adjust[] = {0x00040040, 3};
    assert(call(p, mic, start, 6) == 0x00030040);
    assert(ctr_send_header(p, mic, 0x00060000) == 0x00060080 && ctr_command_word(p, 2) == 1);
    assert(p->mic.encoding == 1 && p->mic.sample_rate == 2);
    assert(call(p, mic, adjust, 2) == 0x00040040 && p->mic.sample_rate == 3);
    assert(ctr_send_header(p, mic, 0x00050000) == 0x00050040);
    assert(ctr_send_header(p, mic, 0x00060000) == 0x00060080 && !ctr_command_word(p, 2));
    /* the settings are kept and read back: a gain is a byte, a bool the word's low byte (0x100 is false) */
    const uint32_t gain[] = {0x00080040, 0x1a5}, power[] = {0x000a0040, 2}, clamp[] = {0x000d0040, 0x100};
    const uint32_t shell[] = {0x000f0040, 1}, version[] = {0x00100040, 0x11};
    assert(call(p, mic, gain, 2) == 0x00080040);
    assert(ctr_send_header(p, mic, 0x00090000) == 0x00090080 && ctr_command_word(p, 2) == 0xa5);
    assert(call(p, mic, power, 2) == 0x000a0040);
    assert(ctr_send_header(p, mic, 0x000b0000) == 0x000b0080 && ctr_command_word(p, 2) == 1);
    assert(call(p, mic, clamp, 2) == 0x000d0040);
    assert(ctr_send_header(p, mic, 0x000e0000) == 0x000e0080 && !ctr_command_word(p, 2));
    assert(call(p, mic, shell, 2) == 0x000f0040 && p->mic.allow_shell_closed == 1);
    assert(call(p, mic, version, 2) == 0x00100040);
    /* SetIirFilterMic gives its mapped buffer back */
    const uint32_t filter[] = {0x000c0042, 0x20, 0x20a, 0x300000};
    assert(call(p, mic, filter, 4) == 0x000c0042 && ctr_command_word(p, 2) == 0x20a &&
           ctr_command_word(p, 3) == 0x300000);
    /* the buffer full event is a real event, the same one each time */
    assert(ctr_send_header(p, mic, 0x00070000) == 0x00070042 && !ctr_command_word(p, 2));
    uint32_t id = snx_ctr_handle_object(p, ctr_command_word(p, 3));
    assert(id && id == p->mic.event && p->objects[id].type == CTR_OBJ_EVENT);
    assert(ctr_send_header(p, mic, 0x00070000) == 0x00070042 && snx_ctr_handle_object(p, ctr_command_word(p, 3)) == id);
    snx_ctr_process_free(p);
    puts("3ds mic:u: its commands, settings and buffer full event passed");
    return 0;
}
