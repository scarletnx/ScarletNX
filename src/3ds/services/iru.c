/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Copyright 2014-2026 Citra Emulator Project / Azahar Emulator Project
   ir:USER, the infrared port the Circle Pad Pro is on. Ported from Azahar's core/hle/service/ir/ir_user.cpp
   (GPL-2.0-or-later) with no extra HID connected: the game gets its shared memory marked initialized and the three
   events it waits on, a connection request leaves the status at "not connected", and nothing ever arrives or is
   sent. The commands Azahar leaves out are left out here */
#include "3ds/services/iru.h"
#include "3ds/runtime/internal.h"
#include "common/utilities/endian.h"

/* the shared memory's header (3dbrew's IRUSER Shared Memory): two error results, then single bytes */
enum { IRU_CONNECTION_STATUS = 8, IRU_TRYING_TO_CONNECT = 9, IRU_CONNECTION_ROLE = 10, IRU_CONNECTED = 12, IRU_INITIALIZED = 14 };
enum { IRU_MEMORY = 0, IRU_RECEIVE, IRU_SEND, IRU_STATUS }; /* the retained objects: the memory, then the events */
#define IRU_INVALID_STATE 0xcd5062d9u /* description 13, IR, invalid state, status level */

static void reply(SnxCtrIpc* c, unsigned normal, unsigned translated) {
    c->out[0] = (c->in[0] & 0xffff0000u) | normal << 6 | translated;
    c->out[1] = 0;
}
static void poke(SnxCtrProcess* p, uint32_t offset, unsigned char value) {
    uint32_t memory = p->iru.objects[IRU_MEMORY];
    if (memory) (void)snx_ctr_phys_write(p->memory, p->objects[memory].data.shared.physical + offset, &value, 1);
}
/* an event the game asked for, made at the first request and kept for the process */
static void event(SnxCtrIpc* c, unsigned slot) {
    if (!snx_ctr_ipc_expect(c, 0, 0)) return;
    SnxCtrProcess* p = c->process;
    uint32_t handle = 0, result;
    if (!p->iru.objects[slot]) {
        result = snx_ctr_object_create(p, CTR_OBJ_EVENT, &handle);
        if (!result) {
            p->iru.objects[slot] = snx_ctr_handle_object(p, handle);
            ++p->objects[p->iru.objects[slot]].refs;
        }
    } else
        result = snx_ctr_handle_add(p, p->iru.objects[slot], &handle);
    if (result) {
        snx_ctr_ipc_error_reply(c, result);
        return;
    }
    reply(c, 1, 2);
    c->out[2] = 0; /* one copied handle */
    snx_ctr_ipc_output_handle(c, 3, handle);
    if (c->state != SNX_CTR_IPC_RETURN) snx_ctr_ipc_drop_handle(c, handle);
}
/* InitializeIrNopShared(sizes, packet counts, baud rate, memory): the block is the game's, marked initialized */
static void initialize_shared(SnxCtrIpc* c) {
    if (!snx_ctr_ipc_expect(c, 6, 2) || c->in[7] != 0) {
        if (c->state == SNX_CTR_IPC_RETURN) c->state = SNX_CTR_IPC_UNSUPPORTED;
        return;
    }
    SnxCtrProcess* p = c->process;
    uint32_t object = snx_ctr_handle_object(p, c->in[8]);
    if (!object || p->objects[object].type != CTR_OBJ_SHARED) {
        snx_ctr_ipc_error_reply(c, 0xd8e007f7u);
        return;
    }
    if (p->iru.objects[IRU_MEMORY]) --p->objects[p->iru.objects[IRU_MEMORY]].refs;
    p->iru.objects[IRU_MEMORY] = object;
    ++p->objects[object].refs;
    poke(p, IRU_INITIALIZED, 1);
    reply(c, 1, 0);
}
void snx_ctr_iru_dispatch(SnxCtrIpc* c) {
    SnxCtrProcess* p = c->process;
    switch (c->in[0] >> 16) {
        case 0x01: /* InitializeIrNop: Azahar has no handler */
        default: c->state = SNX_CTR_IPC_UNKNOWN; break;
        case 0x02: /* FinalizeIrNop */
            if (!snx_ctr_ipc_expect(c, 0, 0)) return;
            if (p->iru.objects[IRU_MEMORY]) --p->objects[p->iru.objects[IRU_MEMORY]].refs;
            p->iru.objects[IRU_MEMORY] = 0;
            reply(c, 1, 0);
            break;
        case 0x06: /* RequireConnection(device): only a Circle Pad Pro could answer, and none is attached */
            if (!snx_ctr_ipc_expect(c, 1, 0)) return;
            poke(p, IRU_CONNECTION_STATUS, 1);
            poke(p, IRU_TRYING_TO_CONNECT, 2);
            reply(c, 1, 0);
            break;
        case 0x09: /* Disconnect */
            if (!snx_ctr_ipc_expect(c, 0, 0)) return;
            poke(p, IRU_CONNECTION_STATUS, 0);
            poke(p, IRU_CONNECTED, 0);
            reply(c, 1, 0);
            break;
        case 0x0a: event(c, IRU_RECEIVE); break;
        case 0x0b: event(c, IRU_SEND); break;
        case 0x0c: event(c, IRU_STATUS); break;
        case 0x0d: /* SendIrNop(size, buffer): nothing is connected to send to */
            if (!snx_ctr_ipc_expect(c, 1, 2)) return;
            reply(c, 1, 0);
            c->out[1] = IRU_INVALID_STATE;
            break;
        case 0x18: initialize_shared(c); break;
        case 0x19: /* ReleaseReceivedData(count): no packet was ever delivered, so Azahar's answer is not found */
            if (!snx_ctr_ipc_expect(c, 1, 0)) return;
            reply(c, 1, 0);
            c->out[1] = 0xc880cd7du; /* description 0x7d, IR, not found, status level */
            break;
    }
}
