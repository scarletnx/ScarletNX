/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* ir:rst: the replies of its commands, and the samples its shared memory gets while a game has it initialized, with a
   scripted ZL as the player's input (Azahar's ir_rst.cpp, 3dbrew's IR Shared Memory) */
#include "3ds/input/input.h"
#include "3ds/runtime/internal.h"
#include "3ds/ipc/ctr_ipc.h"
#include "3ds/services/hid.h"
#include "3ds/services/ir.h"
#include "snx_settings.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define COMMAND (SNX_CTR_INITIAL_TLS + 0x80)
static uint32_t word(SnxCtrProcess* p, unsigned index) {
    unsigned char b[4];
    assert(snx_ctr_read_ram(p->space, COMMAND + index * 4, b, 4) == SNX_CTR_MEM_OK);
    return (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
}
/* the reply's header to a command of the header and up to two arguments, sent on the session */
static uint32_t send(SnxCtrProcess* p, uint32_t session, uint32_t header, uint32_t a, uint32_t b) {
    const uint32_t in[3] = {header, a, b};
    unsigned char bytes[12];
    for (unsigned i = 0; i < 3; ++i)
        for (unsigned j = 0; j < 4; ++j) bytes[i * 4 + j] = (unsigned char)(in[i] >> (j * 8));
    assert(snx_ctr_write_ram(p->space, COMMAND, bytes, sizeof bytes) == SNX_CTR_MEM_OK);
    p->cpu.state.cpu.r[0] = session;
    assert(snx_ctr_svc(p, 0x32) == SNX_CTR_SVC_RETURN && !p->cpu.state.cpu.r[0]);
    assert(!word(p, 1));
    return word(p, 0);
}
static uint32_t shared(SnxCtrProcess* p, uint32_t offset, unsigned size) {
    unsigned char b[4] = {0};
    uint32_t pa = p->objects[p->ir_objects[0]].data.shared.physical;
    assert(snx_ctr_phys_read(p->memory, pa + offset, b, size) == SNX_CTR_MEM_OK);
    return (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
}
int main(void) {
    /* the C-stick's directions are the circle pad's, 4 bits down: right, left, up, down at bits 24 to 27, none near
       the center, two on a diagonal */
    assert(snx_ctr_stick_directions(0x9c, 0) >> 4 == 1u << 24 && snx_ctr_stick_directions(-0x9c, 0) >> 4 == 1u << 25);
    assert(snx_ctr_stick_directions(0, 0x9c) >> 4 == 1u << 26 && snx_ctr_stick_directions(0, -0x9c) >> 4 == 1u << 27);
    assert(snx_ctr_stick_directions(0x70, 0x70) >> 4 == (1u << 24 | 1u << 26) && !snx_ctr_stick_directions(10, -10));
    unsigned char code[0x3000] = {0};
    SnxCtrImage image = {.segment = {{0x100000, 1, 16}, {0x200000, 1, 4}, {0x300000, 1, 4}},
                         .stack_size = 0x4000,
                         .main_priority = 0x30,
                         .bss_size = 0x1000,
                         .code = code,
                         .code_size = sizeof code};
    /* ZL from the start, then ZR (the pad's own bits stay out of ir:rst's state): each held for the press length */
    strcpy(g_settings.ctr_script, "0:4001,100000:8000");
    g_settings.press_length = 60000;
    snx_ctr_input_init();
    SnxCtrProcess* p = snx_ctr_process_new(&image, SNX_CTR_NEW);
    assert(p);
    uint32_t session = 0;
    assert(!snx_ctr_session_create(p, SNX_CTR_SERVICE_IR, &session));
    assert(snx_ctr_ir_deadline(p) == UINT64_MAX); /* nothing samples before Initialize */
    /* the shared memory and the update event, moved: a header of one result word and three translate words */
    assert(send(p, session, 0x00010000, 0, 0) == 0x00010043 && word(p, 2) == 0x04000010);
    for (unsigned i = 0; i < 2; ++i) {
        uint32_t id = snx_ctr_handle_object(p, word(p, 3 + i));
        assert(id && id == p->ir_objects[i] && p->objects[id].type == (i ? CTR_OBJ_EVENT : CTR_OBJ_SHARED));
    }
    assert(snx_ctr_ir_deadline(p) == UINT64_MAX);
    /* Initialize with a period of 5 ms and cooked C-stick data: the first sample is one period on */
    assert(send(p, session, 0x00020080, 5, 0) == 0x00020040 && p->ir.period_ms == 5);
    uint64_t due = snx_ctr_ir_deadline(p);
    assert(due == p->ir.next_ns && due != UINT64_MAX);
    snx_ctr_ir_update(p, due - 1); /* too early */
    assert(!p->objects[p->ir_objects[1]].data.event.signaled && !shared(p, 0x18, 4));
    /* the first sample: entry 0 has ZL (the pad's A, in the script's 1, is hid:USER's), new, the index says so and the
       update event is signalled */
    snx_ctr_ir_update(p, due);
    assert(shared(p, 0x10, 4) == 0 && shared(p, 0x18, 4) == SNX_CTR_PAD_ZL);
    assert(shared(p, 0x1c, 4) == SNX_CTR_PAD_ZL && !shared(p, 0x20, 4) && !shared(p, 0x24, 4)); /* no C-stick */
    assert(shared(p, 0x00, 4) | shared(p, 0x04, 4)); /* the index wrapped to 0: the tick it did */
    assert(p->objects[p->ir_objects[1]].data.event.signaled);
    assert(p->ir.next_ns == due + 5000000u);
    /* the next one: held, so nothing new or released */
    snx_ctr_ir_update(p, p->ir.next_ns);
    assert(shared(p, 0x10, 4) == 1 && shared(p, 0x28, 4) == SNX_CTR_PAD_ZL && !shared(p, 0x2c, 4) && !shared(p, 0x30, 4));
    /* a second GetHandles hands out the same objects */
    assert(send(p, session, 0x00010000, 0, 0) == 0x00010043 && snx_ctr_handle_object(p, word(p, 3)) == p->ir_objects[0]);
    /* Shutdown stops the samples */
    assert(send(p, session, 0x00030000, 0, 0) == 0x00030040 && snx_ctr_ir_deadline(p) == UINT64_MAX);
    snx_ctr_process_free(p);
    snx_ctr_input_free();
    puts("CTR ir:rst commands and ZL samples from scripted input passed");
    return 0;
}
