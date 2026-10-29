/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_CTR_IPC_H
#define SNX_CTR_IPC_H
#include <stdbool.h>
#include <stdint.h>
#define SNX_CTR_IPC_WORDS          64u
#define SNX_CTR_IPC_BAD_DESCRIPTOR 0xd9001830u
#define SNX_CTR_IPC_INVALID_STATE  0xe0a01bf5u
enum {
    SNX_CTR_SERVICE_NONE,
    SNX_CTR_SERVICE_SRV,
    SNX_CTR_SERVICE_FS,
    SNX_CTR_SERVICE_APT,
    SNX_CTR_SERVICE_PTM,
    SNX_CTR_SERVICE_HID,
    SNX_CTR_SERVICE_FILE,
    SNX_CTR_SERVICE_GSP,
    SNX_CTR_SERVICE_CFG,
    SNX_CTR_SERVICE_DSP,
    SNX_CTR_SERVICE_SYSTEM, /* the system services the runtime has no model of (services/system.c) */
    SNX_CTR_SERVICE_AM,     /* am:app (services/am.c) */
    SNX_CTR_SERVICE_Y2R,    /* y2r:u (services/y2r.c) */
    SNX_CTR_SERVICE_LDR_RO, /* ldr:ro (services/ldr.c) */
    SNX_CTR_SERVICE_COUNT
};
/* SNX_CTR_IPC_UNKNOWN: a command the service does not have, as opposed to a form of a known one it does not
   support (SNX_CTR_IPC_UNSUPPORTED) */
typedef enum {
    SNX_CTR_IPC_RETURN,
    SNX_CTR_IPC_UNSUPPORTED,
    SNX_CTR_IPC_BLOCKED,
    SNX_CTR_IPC_FAULT,
    SNX_CTR_IPC_UNKNOWN
} SnxCtrIpcState;
#define SNX_CTR_IPC_HANDLES 8u
struct SnxCtrProcess;
/* Serialized stack-owned request/reply copies. Never a flat guest pointer.
   Header: command[31:16], normal words[11:6], translation words[5:0].
   Result is reply normal word 1. PID descriptors are kernel supplied, not the
   raw client's claimed PID. Unknown commands answer success, as Azahar's
   ReportUnimplementedFunction does and a Switch game's do (auto_stub_ipc), unless
   the ctr_auto_stub_ipc setting is off: then they stop the run, except for the
   services the process's IPC stub list names. */
typedef struct {
    struct SnxCtrProcess* process;
    uint32_t object, caller_pid, transport_result;
    uint32_t in[SNX_CTR_IPC_WORDS], out[SNX_CTR_IPC_WORDS];
    /* Incoming copied and moved handles in descriptor order: raw words, resolved and type-checked by the handler.
       Zero: none. A moved handle leaves the caller's table once a handler has taken the request, even when it
       answers with a transport error. */
    uint32_t handle_in[SNX_CTR_IPC_HANDLES], handle_count, handle_moved;
    SnxCtrIpcState state;
} SnxCtrIpc;
/* Adds the --ipc-stubs list ("all", or service families: srv, fs, apt, ptm, hid, gsp, cfg, dsp, comma separated)
   to a mask over SNX_CTR_SERVICE_*; false, with the mask unchanged, for an empty list or a name it does not know */
int snx_ctr_ipc_stub_mask(const char* list, uint32_t* mask);
/* whether an unknown command of the service (SNX_CTR_SERVICE_*) answers success: ctr_auto_stub_ipc, else the process's
   stub list */
bool snx_ctr_ipc_stubbed(const struct SnxCtrProcess* p, uint32_t service);
int snx_ctr_ipc_expect(SnxCtrIpc* c, uint32_t normal, uint32_t translated);
void snx_ctr_ipc_bad(SnxCtrIpc* c);
void snx_ctr_ipc_get_bytes(SnxCtrIpc* c, uint32_t first, uint8_t* bytes, uint32_t size);
void snx_ctr_ipc_put_bytes(SnxCtrIpc* c, uint32_t first, const uint8_t* bytes, uint32_t size);
void snx_ctr_ipc_error_reply(SnxCtrIpc* c, uint32_t result);
/* Output handles are newly owned client-table references. Publication happens
   only after full TLS write preflight. Failed handlers must relinquish them. */
void snx_ctr_ipc_drop_handle(SnxCtrIpc* c, uint32_t handle);
void snx_ctr_ipc_output_handle(SnxCtrIpc* c, uint32_t word, uint32_t handle);
#endif
