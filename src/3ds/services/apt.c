/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   APT GetLockHandle facts: Azahar service/apt/{apt,applet_manager}.cpp,
   GPL-2.0-or-later, revision recorded in docs/3DS.md. */
#include "3ds/runtime/internal.h"
#include "common/utilities/endian.h"
#include "3ds/services/apt.h"
#include "3ds/services/font.h"
#include "services/apt.swipc.inc"
#define APT_NO_SHARED_FONT 0xffffffffu /* Azahar's answer (-1) when it has no font to serve */
static uint32_t snx_ctr_apt_wire_GetLockHandle(SnxCtrIpc* c, uint32_t attributes, uint32_t* corrected, uint32_t* state,
                                               uint32_t* lock) {
    SnxCtrProcess* p = c->process;
    /* Only application callers: no library-position correction or Home Menu. */
    if (attributes) {
        c->state = SNX_CTR_IPC_UNSUPPORTED;
        return 0;
    }
    uint32_t result;
    if (p->apt_lock)
        result = snx_ctr_handle_add(p, p->apt_lock, lock);
    else {
        result = snx_ctr_object_create(p, CTR_OBJ_MUTEX, lock);
        if (!result) {
            p->apt_lock = snx_ctr_handle_object(p, *lock);
            ++p->objects[p->apt_lock].refs; /* manager owns the shared lock */
        }
    }
    if (result) return result;
    *corrected = attributes;
    *state = 0; /* no outstanding library-applet transition */
    return 0;
}
static uint32_t snx_ctr_apt_wire_Initialize(SnxCtrIpc* c, uint32_t app_id, uint32_t attributes, uint32_t* notification,
                                            uint32_t* parameter) {
    SnxCtrProcess* p = c->process;
    if (app_id != 0x300 || attributes) {
        c->state = SNX_CTR_IPC_UNSUPPORTED;
        return 0;
    }
    if (p->apt_phase) return 0xc8a0cffcu; /* occupied application slot */
    uint32_t result = snx_ctr_object_create(p, CTR_OBJ_EVENT, notification);
    if (result) return result;
    result = snx_ctr_object_create(p, CTR_OBJ_EVENT, parameter);
    if (result) return result; /* generated error path closes notification */
    p->apt_notification = snx_ctr_handle_object(p, *notification);
    p->apt_parameter = snx_ctr_handle_object(p, *parameter);
    ++p->objects[p->apt_notification].refs;
    ++p->objects[p->apt_parameter].refs;
    p->apt_phase = 1; /* first application auto-enabled */
    /* APT initializes/enables the first application itself when no applet is
       active, and queues its initial Wakeup. No Home Menu is invented. */
    p->objects[p->apt_parameter].data.event.signaled = 1;
    return 0;
}
static uint32_t snx_ctr_apt_wire_Enable(SnxCtrIpc* c, uint32_t attributes) {
    if (attributes) {
        c->state = SNX_CTR_IPC_UNSUPPORTED;
        return 0;
    }
    /* Initialize auto-enables the first/only application. Re-enabling that
       existing registration is idempotent; no absent applet is registered. */
    return c->process->apt_phase ? 0 : SNX_CTR_IPC_INVALID_STATE;
}
static uint32_t snx_ctr_apt_wire_NotifyToWait(SnxCtrIpc* c, uint32_t app_id) {
    SnxCtrProcess* p = c->process;
    if (app_id != 0x300) {
        c->state = SNX_CTR_IPC_UNSUPPORTED;
        return 0;
    }
    if (!p->apt_phase) return SNX_CTR_IPC_INVALID_STATE;
    if (p->apt_phase > 2) {
        c->state = SNX_CTR_IPC_UNSUPPORTED;
        return 0;
    }
    /* Record the registered application's startup readiness. This is not a
       kernel wait completion; the queued parameter must still be received. */
    p->apt_phase = 2;
    return 0;
}
static uint32_t snx_ctr_apt_wire_GetSharedFont(SnxCtrIpc* c, uint32_t* address, uint32_t* font) {
    return snx_ctr_font_shared(c->process, address, font) ? 0 : APT_NO_SHARED_FONT;
}
static uint32_t snx_ctr_apt_wire_IsNew3DS(SnxCtrIpc* c, uint32_t* is_new) {
    *is_new = snx_ctr_memory_profile(c->process->memory)->model == SNX_CTR_NEW;
    return 0;
}
static void receive_parameter(SnxCtrIpc* c) {
    if (!snx_ctr_ipc_expect(c, 2, 0)) return;
    SnxCtrProcess* p = c->process;
    if (c->in[1] != 0x300 || c->in[2] > 4096) {
        c->state = SNX_CTR_IPC_UNSUPPORTED;
        return;
    }
    if (p->apt_phase != 2) {
        snx_ctr_ipc_error_reply(c, p->apt_phase >= 3 ? 0xc8a0cfefu : SNX_CTR_IPC_INVALID_STATE);
        return;
    }
    unsigned char descriptor[8], zero[4096] = {0};
    uint32_t tls = p->cpu.state.cpu.tpidruro;
    if (snx_ctr_read_ram(p->space, tls + 0x180, descriptor, sizeof descriptor) != SNX_CTR_MEM_OK) {
        c->transport_result = 0xe0e01bf5u;
        return;
    }
    uint32_t d = snx_ld_le32(descriptor), address = snx_ld_le32(descriptor + 4), size = c->in[2];
    if ((d & 0x3fff) != 2 || (d >> 14) < size) {
        snx_ctr_ipc_bad(c);
        return;
    }
    /* APT pads its parameter buffer to the requested length. This initial
       Wakeup has no payload or object. Whole-range checked RAM write is atomic
       on a bad address/permission/MMIO; consume only after it succeeds. */
    if (size && snx_ctr_write_ram(p->space, address, zero, size) != SNX_CTR_MEM_OK) {
        c->transport_result = 0xe0e01bf5u;
        return;
    }
    c->out[0] = 0x000d0104;
    c->out[2] = 0;    /* sender: none, no Home Menu process is claimed */
    c->out[3] = 1;    /* the queued initial Wakeup, implicit in the startup phase */
    c->out[4] = 0;    /* actual payload size */
    c->out[5] = 0x10; /* move descriptor; absent optional object is wire NULL */
    c->out[6] = 0;
    c->out[7] = (size << 14) | 2; /* static buffer 0 */
    c->out[8] = address;
    p->apt_phase = 3; /* wakeup received, startup transition not yet released */
}
/* AppletUtility: a utility command, its input in static buffer 1, its output (output_size bytes) in the caller's
   receive buffer 0. As Azahar answers them: success, and an output of zeros, but for TryLockTransition (6), which
   says the transitions are locked, there being none that waits. The first UnlockTransition (7) of the startup's
   (0x10) ends the startup transition. SleepIfShellClosed (4) does not sleep: the shell stays open */
static void applet_utility(SnxCtrIpc* c) {
    if (!snx_ctr_ipc_expect(c, 3, 2)) return;
    SnxCtrProcess* p = c->process;
    uint32_t command = c->in[1], input_size = c->in[2], output_size = c->in[3];
    if (input_size > 0x1000 || output_size > 0x1000) {
        c->state = SNX_CTR_IPC_UNSUPPORTED;
        return;
    }
    if (c->in[4] != ((input_size << 14) | 0x402)) {
        snx_ctr_ipc_bad(c);
        return;
    }
    unsigned char input[4] = {0}, descriptor[8];
    if (snx_ctr_read_ram(p->space, c->in[5], input, input_size < 4 ? input_size : 4) != SNX_CTR_MEM_OK ||
        snx_ctr_read_ram(p->space, p->cpu.state.cpu.tpidruro + 0x180, descriptor, 8) != SNX_CTR_MEM_OK) {
        c->transport_result = 0xe0e01bf5u;
        return;
    }
    uint32_t d = snx_ld_le32(descriptor), address = snx_ld_le32(descriptor + 4);
    if ((d & 0x3fff) != 2 || (d >> 14) < output_size) {
        snx_ctr_ipc_bad(c);
        return;
    }
    if (command == 4 && (input[0] || p->shell_closed)) { /* sleep is not modelled */
        c->state = SNX_CTR_IPC_UNSUPPORTED;
        return;
    }
    unsigned char out[0x1000] = {0};
    if (command == 6 && output_size) out[0] = 1;
    if (output_size && snx_ctr_write_ram(p->space, address, out, output_size) != SNX_CTR_MEM_OK) {
        c->transport_result = 0xe0e01bf5u;
        return;
    }
    if (command == 7 && input_size == 4 && snx_ld_le32(input) == 0x10 && p->apt_phase == 3) p->apt_phase = 4;
    c->out[0] = 0x004b0082;
    c->out[2] = 0; /* the utility's own result */
    c->out[3] = (output_size << 14) | 2;
    c->out[4] = address;
}
void snx_ctr_apt_dispatch(SnxCtrIpc* c) {
    if (c->in[0] >> 16 == 0xd)
        receive_parameter(c);
    else if (c->in[0] >> 16 == 0x4b)
        applet_utility(c);
    else
        snx_ctr_apt_wire_dispatch(c);
}
