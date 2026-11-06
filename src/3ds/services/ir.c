/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* ir:USER with no IR accessory in range, as Azahar answers then (core/hle/service/ir/ir_user.cpp at azahar-emu/azahar
   6e42de8). InitializeIrNopShared writes the shared block's header and its empty receive queue, the three events are
   real kernel events, and a connection request only marks the header as trying to connect. The status event is never
   signaled, so a game that waits on it times out and goes on without the accessory. */
#include "3ds/services/ir.h"
#include "3ds/runtime/internal.h"
#include "3ds/services/system.h"

#define IR_HEADER_SIZE      0x10u /* SharedMemoryHeader: two words and eight bytes */
#define IR_BUFFER_INFO_SIZE 0x10u /* the receive BufferInfo has four words */
#define IR_METADATA_SIZE    (IR_HEADER_SIZE + IR_BUFFER_INFO_SIZE)
#define IR_WRONG_PERMISSION 0xd900182eu /* ResultWrongPermission */
/* the bytes of SharedMemoryHeader after its two result words */
enum { IR_CONNECTION_STATUS = 8, IR_TRYING_TO_CONNECT = 9, IR_CONNECTED = 12, IR_INITIALIZED = 14 };
#define IR_NOT_CONNECTED 0xc8a10c0du /* InvalidState, as SendIrNop answers with nothing connected */
#define IR_NO_DATA       0xc8810fefu /* NotFound, NoData: the receive queue never holds a packet */
#define IR_COMMANDS      0x1au       /* the last command ir:USER has (SetOwnMachineId) */

static uint32_t shared_address(SnxCtrProcess* p, uint32_t object) {
    uint32_t address = p->objects[object].data.shared.address;
    if (address) return address;
    for (unsigned i = 0; i < sizeof p->shared_maps / sizeof *p->shared_maps; ++i)
        if (p->shared_maps[i].object == object && p->shared_maps[i].va) return p->shared_maps[i].va;
    return 0;
}
/* The service is another process: it writes the block's header with the block's other-process permission, which can
   exceed the owner's own mapping (libctru makes the IR block read-only for the owner and writable for the service).
   The single guest mapping cannot carry both rights, so the write goes through the physical pages it maps. */
static bool write_shared(SnxCtrProcess* p, uint32_t va, const unsigned char* bytes, size_t size) {
    for (size_t done = 0; done < size;) {
        uint32_t at = va + (uint32_t)done, pa = 0;
        size_t run = 4096 - (at & 4095);
        if (run > size - done) run = size - done;
        if (snx_ctr_query(p->space, at, &pa, NULL) != SNX_CTR_MEM_OK ||
            snx_ctr_phys_write(p->memory, pa, bytes + done, run) != SNX_CTR_MEM_OK)
            return false;
        done += run;
    }
    return true;
}
static void reply(SnxCtrIpc* c, unsigned normal, unsigned translated) {
    c->out[0] = (c->in[0] & 0xffff0000u) | normal << 6 | translated;
    c->out[1] = 0;
}
static uint32_t make_event(SnxCtrProcess* p, uint32_t* event) {
    if (*event) return 0;
    uint32_t handle = 0, result = snx_ctr_object_create(p, CTR_OBJ_EVENT, &handle);
    if (result) return result;
    *event = snx_ctr_handle_object(p, handle);
    ++p->objects[*event].refs; /* the service's own, while the process lives */
    return snx_ctr_handle_close(p, handle);
}
static void get_event(SnxCtrIpc* c, uint32_t* event) {
    if (!snx_ctr_ipc_expect(c, 0, 0)) return;
    uint32_t handle = 0, result = make_event(c->process, event);
    if (!result) result = snx_ctr_handle_add(c->process, *event, &handle);
    if (result) {
        snx_ctr_ipc_error_reply(c, result);
        return;
    }
    reply(c, 1, 2);
    c->out[2] = 0; /* one copied handle */
    snx_ctr_ipc_output_handle(c, 3, handle);
    if (c->state != SNX_CTR_IPC_RETURN) snx_ctr_ipc_drop_handle(c, handle);
}
static void initialize(SnxCtrIpc* c) {
    if (!snx_ctr_ipc_expect(c, 6, 2)) return;
    SnxCtrProcess* p = c->process;
    if (c->handle_count != 1) {
        c->state = SNX_CTR_IPC_UNSUPPORTED;
        return;
    }
    uint32_t object = snx_ctr_handle_object(p, c->handle_in[0]);
    if (!object || p->objects[object].type != CTR_OBJ_SHARED) {
        c->state = SNX_CTR_IPC_UNSUPPORTED;
        return;
    }
    /* Only the 32 startup bytes are published, so the declared receive size must leave room for them and stay inside
       the block the application actually lent; no packet is modelled and no byte beyond the metadata is touched */
    CtrObject* block = &p->objects[object];
    if (c->in[1] < IR_METADATA_SIZE || c->in[1] > block->data.shared.size) {
        c->state = SNX_CTR_IPC_UNSUPPORTED;
        return;
    }
    /* the block's other-process permission is the service's: don't care grants no write */
    if (!(block->data.shared.other_permissions & SNX_CTR_WRITE)) {
        snx_ctr_ipc_error_reply(c, IR_WRONG_PERMISSION);
        return;
    }
    uint32_t address = shared_address(p, object);
    unsigned char metadata[IR_METADATA_SIZE] = {0}; /* header, then the empty receive BufferInfo */
    metadata[IR_INITIALIZED] = 1;
    if (!address || !write_shared(p, address, metadata, sizeof metadata)) {
        c->transport_result = 0xe0e01bf5u; /* the block's pages are not there to write */
        return;
    }
    if (p->ir.shared != object) {
        if (p->ir.shared) snx_ctr_object_release(p, p->ir.shared);
        p->ir.shared = object;
        ++block->refs; /* the service's own, while it serves the session */
    }
    reply(c, 1, 0);
}
static void finalize(SnxCtrIpc* c) {
    if (!snx_ctr_ipc_expect(c, 0, 0)) return;
    SnxCtrProcess* p = c->process;
    if (p->ir.shared) {
        snx_ctr_object_release(p, p->ir.shared);
        p->ir.shared = 0;
    }
    reply(c, 1, 0);
}
/* a byte of the adopted block's header; nothing before InitializeIrNopShared gave a block */
static void set_header(SnxCtrProcess* p, unsigned offset, unsigned char value) {
    uint32_t address = p->ir.shared ? shared_address(p, p->ir.shared) : 0;
    if (address) (void)write_shared(p, address + offset, &value, 1);
}
static void require_connection(SnxCtrIpc* c) {
    if (!snx_ctr_ipc_expect(c, 1, 0)) return;
    set_header(c->process, IR_CONNECTION_STATUS, 1);
    set_header(c->process, IR_TRYING_TO_CONNECT, 2);
    reply(c, 1, 0);
}
static void disconnect(SnxCtrIpc* c) {
    if (!snx_ctr_ipc_expect(c, 0, 0)) return;
    set_header(c->process, IR_CONNECTION_STATUS, 0);
    set_header(c->process, IR_CONNECTED, 0);
    reply(c, 1, 0);
}
static void send_nop(SnxCtrIpc* c) {
    if (snx_ctr_ipc_expect(c, 1, 2)) snx_ctr_ipc_error_reply(c, IR_NOT_CONNECTED);
}
static void release_received(SnxCtrIpc* c) {
    if (!snx_ctr_ipc_expect(c, 1, 0)) return;
    if (c->in[1])
        snx_ctr_ipc_error_reply(c, IR_NO_DATA);
    else
        reply(c, 1, 0);
}
void snx_ctr_ir_dispatch(SnxCtrIpc* c) {
    uint32_t command = c->in[0] >> 16;
    switch (command) {
        case 0x0002: finalize(c); break;
        case 0x0006: require_connection(c); break;
        case 0x0009: disconnect(c); break;
        case 0x000a: get_event(c, &c->process->ir.receive_event); break;
        case 0x000b: get_event(c, &c->process->ir.send_event); break;
        case 0x000c: get_event(c, &c->process->ir.status_event); break;
        case 0x000d: send_nop(c); break;
        case 0x0018: initialize(c); break;
        case 0x0019: release_received(c); break;
        default:
            if (command && command <= IR_COMMANDS)
                snx_ctr_system_unmodeled(c, "ir:USER", (uint32_t)SNX_CTR_SERVICE_IR << 16 | command);
            else
                c->state = SNX_CTR_IPC_UNKNOWN;
            break;
    }
}
