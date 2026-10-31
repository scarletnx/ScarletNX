/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Copyright 2015-2026 Citra Emulator Project / Azahar Emulator Project
   ir:rst: the New 3DS's ZL, ZR and C-stick, in the shared memory a game polls and with an event at each sample. The
   commands, the layout (3dbrew's IR Shared Memory: the tick of the last index wrap and the one before it, the index
   last written, then eight entries of the pad state, what it gained and lost and the C-stick's position) and the pace
   are ported from Azahar's service/ir/ir_rst.cpp (GPL-2.0-or-later). The input is the player's (input/input.c). Like
   Azahar, it gives the C-stick as a cooked position (raw_c_stick, the raw form, is not modelled) */
#include "3ds/services/ir.h"
#include "3ds/input/input.h"
#include "3ds/runtime/internal.h"
#include "3ds/services/hid.h"
#include "common/utilities/common.h"
#include "common/utilities/endian.h"
#include "services/ir.swipc.inc"
#include <math.h>

/* the shared memory's layout */
#define IR_INDEX  0x10u /* the index last written */
#define IR_ENTRY  0x18u /* entries[8] of 0x10 bytes: state, additions, removals, then the C-stick's x and y as s16 */
#define IR_RIM    0x9c  /* the C-stick's rim, in the position */
#define IR_MEMORY 4096u /* the block's size (the structure is 0x98) */

static uint32_t shared_physical(const SnxCtrProcess* p) {
    return p->ir_objects[0] ? p->objects[p->ir_objects[0]].data.shared.physical : 0;
}
static void put32(SnxCtrProcess* p, uint32_t offset, uint32_t v) {
    unsigned char b[4];
    snx_st_le32(b, v);
    (void)snx_ctr_phys_write(p->memory, shared_physical(p) + offset, b, 4);
}
static void put16(SnxCtrProcess* p, uint32_t offset, uint16_t v) {
    unsigned char b[2];
    snx_st_le16(b, v);
    (void)snx_ctr_phys_write(p->memory, shared_physical(p) + offset, b, 2);
}
static uint32_t get32(SnxCtrProcess* p, uint32_t offset) {
    unsigned char b[4] = {0};
    (void)snx_ctr_phys_read(p->memory, shared_physical(p) + offset, b, 4);
    return snx_ld_le32(b);
}
/* one sample: the entry after the last, with what changed since the one before it, the index moved on (at a wrap to 0
   the ticks too, the current one becoming the previous), then the update event */
static void sample(SnxCtrProcess* p) {
    SnxCtrInput input;
    snx_ctr_input_read(&input);
    int32_t x = (int32_t)roundf(input.cstick[0] * IR_RIM), y = (int32_t)roundf(input.cstick[1] * IR_RIM);
    /* the C-stick's four directions are the circle pad's four, 4 bits down */
    uint32_t state = (input.buttons & SNX_CTR_PAD_NEW) | snx_ctr_stick_directions(x, y) >> 4;
    uint32_t index = p->ir.index, entry = IR_ENTRY + index * 0x10;
    uint32_t old = get32(p, IR_ENTRY + ((index + 7) & 7) * 0x10), changed = state ^ old;
    put32(p, entry, state);
    put32(p, entry + 4, changed & state);
    put32(p, entry + 8, changed & old);
    put16(p, entry + 12, (uint16_t)(int16_t)x);
    put16(p, entry + 14, (uint16_t)(int16_t)y);
    if (!index) {
        put32(p, 8, get32(p, 0));
        put32(p, 12, get32(p, 4));
        double elapsed = snx_seconds() - p->clock_origin;
        uint64_t now = elapsed > 0 ? (uint64_t)(elapsed * (double)SNX_CTR_BASE_CLOCK_HZ) : 0;
        put32(p, 0, (uint32_t)now);
        put32(p, 4, (uint32_t)(now >> 32));
    }
    put32(p, IR_INDEX, index);
    p->ir.index = (index + 1) & 7;
    uint32_t event = p->ir_objects[1];
    if (event) {
        p->objects[event].data.event.signaled = 1;
        snx_ctr_wake_waiters(p, event);
    }
}
void snx_ctr_ir_update(SnxCtrProcess* p, uint64_t now_ns) {
    SnxCtrIr* ir = &p->ir;
    if (!ir->next_ns || now_ns < ir->next_ns) return;
    if (p->ir_objects[0]) sample(p);
    /* one more than a period behind the host (a slow slice) starts again from now instead of delivering the missed
       samples back to back, as hid's do */
    uint64_t period = (uint64_t)ir->period_ms * 1000000u;
    ir->next_ns += period;
    if (ir->next_ns + period < now_ns) ir->next_ns = now_ns + period;
}
uint64_t snx_ctr_ir_deadline(const SnxCtrProcess* p) { return p->ir.next_ns ? p->ir.next_ns : UINT64_MAX; }
static uint32_t snx_ctr_ir_GetHandles(SnxCtrIpc* c, uint32_t* memory, uint32_t* event) {
    SnxCtrProcess* p = c->process;
    uint32_t* out[] = {memory, event};
    for (unsigned i = 0; i < 2; ++i) {
        uint32_t result = p->ir_objects[0] ? snx_ctr_handle_add(p, p->ir_objects[i], out[i])
                                           : snx_ctr_object_create(p, i ? CTR_OBJ_EVENT : CTR_OBJ_SHARED, out[i]);
        if (result) return result; /* generated output cleanup is transactional */
    }
    if (!p->ir_objects[0]) {
        uint32_t physical = snx_ctr_shared_pages(p, IR_MEMORY);
        if (!physical) return 0xd86007f3u;
        for (unsigned i = 0; i < 2; ++i) {
            p->ir_objects[i] = snx_ctr_handle_object(p, *out[i]);
            ++p->objects[p->ir_objects[i]].refs;
        }
        /* a system-pool page outside the application's accounting, zeroed as board RAM is, the game's to read only */
        CtrObject* shared = &p->objects[p->ir_objects[0]];
        shared->data.shared.physical = physical;
        shared->data.shared.size = IR_MEMORY;
        shared->data.shared.permissions = SNX_CTR_READ;
    }
    return 0;
}
static uint32_t snx_ctr_ir_Initialize(SnxCtrIpc* c, uint32_t update_period, uint32_t raw_c_stick) {
    (void)raw_c_stick;
    SnxCtrIr* ir = &c->process->ir;
    /* a period of 0 would sample with no pause between: it samples every millisecond */
    ir->period_ms = update_period ? update_period : 1;
    ir->index = 0;
    ir->next_ns = snx_ctr_now_ns() + (uint64_t)ir->period_ms * 1000000u;
    return 0;
}
static uint32_t snx_ctr_ir_Shutdown(SnxCtrIpc* c) {
    c->process->ir.next_ns = 0;
    return 0;
}
