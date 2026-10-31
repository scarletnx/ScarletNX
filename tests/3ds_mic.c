/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* mic:u: the replies of its commands, the state a game stores and the buffer full event, as Azahar's mic_u.cpp has them
   with no input device */
#include "3ds/runtime/internal.h"
#include "3ds/ipc/ctr_ipc.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define COMMAND (SNX_CTR_INITIAL_TLS + 0x80)
static uint32_t word(SnxCtrProcess* p, unsigned index) {
    unsigned char b[4];
    assert(snx_ctr_read_ram(p->space, COMMAND + index * 4, b, 4) == SNX_CTR_MEM_OK);
    return (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
}
/* the reply's header to a command of the header and up to five words after it, sent on the session */
static uint32_t send(SnxCtrProcess* p, uint32_t session, uint32_t header, const uint32_t args[5]) {
    uint32_t in[6] = {header};
    for (unsigned i = 0; args && i < 5; ++i) in[i + 1] = args[i];
    unsigned char bytes[24];
    for (unsigned i = 0; i < 6; ++i)
        for (unsigned j = 0; j < 4; ++j) bytes[i * 4 + j] = (unsigned char)(in[i] >> (j * 8));
    assert(snx_ctr_write_ram(p->space, COMMAND, bytes, sizeof bytes) == SNX_CTR_MEM_OK);
    p->cpu.state.cpu.r[0] = session;
    assert(snx_ctr_svc(p, 0x32) == SNX_CTR_SVC_RETURN && !p->cpu.state.cpu.r[0]);
    assert(!word(p, 1));
    return word(p, 0);
}
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
    uint32_t session = 0;
    assert(!snx_ctr_session_create(p, SNX_CTR_SERVICE_MIC, &session));
    /* nothing samples, and the settings start at zero */
    assert(send(p, session, 0x00060000, NULL) == 0x00060080 && !word(p, 2));
    assert(send(p, session, 0x00090000, NULL) == 0x00090080 && !word(p, 2));
    assert(send(p, session, 0x000b0000, NULL) == 0x000b0080 && !word(p, 2));
    assert(send(p, session, 0x000e0000, NULL) == 0x000e0080 && !word(p, 2));
    /* MapSharedMem takes the size and a handle, and UnmapSharedMem nothing: both answer a result only */
    const uint32_t map[5] = {0x1000, 0, 0, 0, 0};
    assert(send(p, session, 0x00010042, map) == 0x00010040);
    assert(send(p, session, 0x00020000, NULL) == 0x00020040);
    /* StartSampling (encoding, rate, offset, size, loop) starts it, AdjustSampling changes the rate, StopSampling ends it */
    const uint32_t start[5] = {1, 2, 0, 0x800, 1}, adjust[5] = {3, 0, 0, 0, 0};
    assert(send(p, session, 0x00030140, start) == 0x00030040);
    assert(send(p, session, 0x00060000, NULL) == 0x00060080 && word(p, 2) == 1 && p->mic.sample_rate == 2);
    assert(send(p, session, 0x00040040, adjust) == 0x00040040 && p->mic.sample_rate == 3);
    assert(send(p, session, 0x00050000, NULL) == 0x00050040);
    assert(send(p, session, 0x00060000, NULL) == 0x00060080 && !word(p, 2));
    /* the settings are kept and read back (a gain is a byte) */
    const uint32_t gain[5] = {0x1a5, 0, 0, 0, 0}, on[5] = {1, 0, 0, 0, 0};
    assert(send(p, session, 0x00080040, gain) == 0x00080040);
    assert(send(p, session, 0x00090000, NULL) == 0x00090080 && word(p, 2) == 0xa5);
    assert(send(p, session, 0x000a0040, on) == 0x000a0040);
    assert(send(p, session, 0x000b0000, NULL) == 0x000b0080 && word(p, 2) == 1);
    assert(send(p, session, 0x000d0040, on) == 0x000d0040);
    assert(send(p, session, 0x000e0000, NULL) == 0x000e0080 && word(p, 2) == 1);
    assert(send(p, session, 0x000f0040, on) == 0x000f0040 && p->mic.allow_shell_closed == 1);
    assert(send(p, session, 0x00100040, on) == 0x00100040);
    /* the buffer full event is a real event, the same one each time */
    assert(send(p, session, 0x00070000, NULL) == 0x00070042 && !word(p, 2));
    uint32_t id = snx_ctr_handle_object(p, word(p, 3));
    assert(id && id == p->mic.event && p->objects[id].type == CTR_OBJ_EVENT);
    assert(send(p, session, 0x00070000, NULL) == 0x00070042 && snx_ctr_handle_object(p, word(p, 3)) == id);
    snx_ctr_process_free(p);
    puts("CTR mic:u commands, settings and the buffer full event passed");
    return 0;
}
