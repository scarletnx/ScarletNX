/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   CTR header/PID/handle descriptor facts referenced against Citra/Azahar
   ipc.h and kernel/hle_ipc.cpp, GPL-2.0-or-later, revision in docs/3DS.md. */
#include "3ds/ipc/ctr_ipc.h"
#include "3ds/runtime/internal.h"
#include "common/utilities/endian.h"
#include "services/srv.swipc.h"
#include "3ds/services/fs.h"
#include "3ds/services/file.h"
#include "3ds/services/apt.h"
#include "services/ptm.swipc.h"
#include "services/hid.swipc.h"
#include "3ds/services/gsp.h"
#include "3ds/services/dsp.h"
#include "3ds/services/cfg.h"
#include "3ds/services/system.h"
#include "3ds/services/am.h"
#include "3ds/services/y2r.h"
#include "3ds/services/ldr.h"
#include "snx_settings.h"
#include <stdio.h>
#include <string.h>

void snx_ctr_ipc_bad(SnxCtrIpc* c) { c->transport_result = SNX_CTR_IPC_BAD_DESCRIPTOR; }
int snx_ctr_ipc_expect(SnxCtrIpc* c, uint32_t normal, uint32_t translated) {
    if (((c->in[0] >> 6) & 63) != normal || (c->in[0] & 63) != translated) {
        snx_ctr_ipc_bad(c);
        return 0;
    }
    return 1;
}
void snx_ctr_ipc_get_bytes(SnxCtrIpc* c, uint32_t first, uint8_t* bytes, uint32_t size) {
    if (first >= SNX_CTR_IPC_WORDS || size > (SNX_CTR_IPC_WORDS - first) * 4) {
        snx_ctr_ipc_bad(c);
        return;
    }
    for (uint32_t i = 0; i < size; ++i) bytes[i] = (uint8_t)(c->in[first + i / 4] >> ((i & 3) * 8));
}
void snx_ctr_ipc_put_bytes(SnxCtrIpc* c, uint32_t first, const uint8_t* bytes, uint32_t size) {
    if (first >= SNX_CTR_IPC_WORDS || size > (SNX_CTR_IPC_WORDS - first) * 4) {
        c->state = SNX_CTR_IPC_FAULT;
        return;
    }
    for (uint32_t i = 0; i < size; ++i) c->out[first + i / 4] |= (uint32_t)bytes[i] << ((i & 3) * 8);
}
void snx_ctr_ipc_error_reply(SnxCtrIpc* c, uint32_t result) {
    if (c->transport_result || c->state != SNX_CTR_IPC_RETURN) return;
    memset(c->out, 0, sizeof c->out);
    c->out[0] = (c->in[0] & 0xffff0000u) | 0x40;
    c->out[1] = result;
}
void snx_ctr_ipc_drop_handle(SnxCtrIpc* c, uint32_t handle) {
    if (handle) (void)snx_ctr_handle_close(c->process, handle);
}
void snx_ctr_ipc_output_handle(SnxCtrIpc* c, uint32_t word, uint32_t handle) {
    if (word < 3 || word >= SNX_CTR_IPC_WORDS || !handle || handle == CTR_PROCESS_HANDLE ||
        handle == CTR_THREAD_HANDLE || !snx_ctr_handle_object(c->process, handle)) {
        c->state = SNX_CTR_IPC_FAULT;
        return;
    }
    c->out[word] = handle;
}
uint32_t snx_ctr_session_create(SnxCtrProcess* p, uint32_t service, uint32_t* out) {
    *out = 0;
    if (service != SNX_CTR_SERVICE_SRV && service != SNX_CTR_SERVICE_FS && service != SNX_CTR_SERVICE_APT &&
        service != SNX_CTR_SERVICE_PTM && service != SNX_CTR_SERVICE_HID && service != SNX_CTR_SERVICE_GSP &&
        service != SNX_CTR_SERVICE_DSP && service != SNX_CTR_SERVICE_CFG && service != SNX_CTR_SERVICE_SYSTEM &&
        service != SNX_CTR_SERVICE_AM && service != SNX_CTR_SERVICE_Y2R && service != SNX_CTR_SERVICE_LDR_RO)
        return 0xe0e01bf4u;
    unsigned count = 0;
    for (unsigned i = 1; i < CTR_OBJECTS; ++i)
        if (p->objects[i].type == CTR_OBJ_SESSION && p->objects[i].data.session.service == service) ++count;
    if (count >= (service == SNX_CTR_SERVICE_SRV   ? 64u
                  : service == SNX_CTR_SERVICE_FS  ? 32u
                  : service == SNX_CTR_SERVICE_GSP ? 2u
                  : service == SNX_CTR_SERVICE_SYSTEM ? 64u /* all of them, two of each at most in use */
                                                   : 2u))
        return 0xd0401834u;
    uint32_t result = snx_ctr_object_create(p, CTR_OBJ_SESSION, out);
    if (!result) {
        CtrObject* object = &p->objects[snx_ctr_handle_object(p, *out)];
        object->data.session.service = service;
        object->data.session.gsp_thread = UINT32_MAX;
    }
    return result;
}
uint32_t snx_ctr_named_connect(SnxCtrProcess* p, uint32_t address, uint32_t* out) {
    char name[12];
    *out = 0;
    for (unsigned i = 0; i < sizeof name; ++i) {
        if ((uint64_t)address + i > UINT32_MAX ||
            snx_ctr_read_ram(p->space, address + i, &name[i], 1) != SNX_CTR_MEM_OK)
            return 0xe0e01bf5u;
        if (!name[i]) return !strcmp(name, "srv:") ? snx_ctr_session_create(p, SNX_CTR_SERVICE_SRV, out) : 0xd88007fau;
    }
    return 0xe0e0181eu;
}
enum { WIRE_OK, WIRE_BAD, WIRE_ADDRESS, WIRE_HANDLES };
/* The request's translation descriptors, any well-formed one (Azahar kernel/hle_ipc.cpp, ipc.h): handles copied or
   moved (up to SNX_CTR_IPC_HANDLES, captured for the handler), the caller's PID, static buffers the service reads,
   PXI buffers it reads or also writes, and mapped buffers it reads, writes or both. Every buffer is checked against
   the caller's memory for the access it grants before any handler runs. Handlers still check the exact form each
   command takes. */
static unsigned translation(SnxCtrIpc* c, uint32_t normal, uint32_t translated) {
    uint32_t position = 1 + normal, end = position + translated;
    while (position < end) {
        uint32_t d = c->in[position++], payload = 1, size = 0;
        unsigned access = 0;
        /* Buffer forms must be classified before copy/move handle forms. */
        if (d & 8) {
            if ((d & 15) != 10 && (d & 15) != 12 && (d & 15) != 14) return WIRE_BAD;
            size = d >> 4; /* mapped: 10 the service reads, 12 writes, 14 both */
            access = (d & 2 ? SNX_CTR_READ : 0) | (d & 4 ? SNX_CTR_WRITE : 0);
        } else if ((d & 15) == 2) {
            size = d >> 14; /* static buffer, id in bits 10..13 */
            access = SNX_CTR_READ;
        } else if ((d & 15) == 4 || (d & 15) == 6) {
            size = d >> 8; /* PXI buffer: 6 read-only, 4 read-write (libctru IPC_Desc_PXIBuffer) */
            access = (d & 15) == 6 ? SNX_CTR_READ : SNX_CTR_READ | SNX_CTR_WRITE;
        } else if ((d & 0x3f) == 0x20) {
            /* the calling process's ID: the kernel writes it over whatever the client put there (Azahar reads only
               the low six bits of the descriptor) */
            if (position < end) c->in[position] = c->caller_pid;
        } else if (!(d & 0x03ffffefu)) { /* copied (0x00) or moved (0x10) handles, count - 1 in bits 26..31 */
            payload = (d >> 26) + 1;
            if (payload > end - position) return WIRE_BAD;
            if (c->handle_count + payload > SNX_CTR_IPC_HANDLES) return WIRE_HANDLES;
            for (uint32_t i = 0; i < payload; ++i) {
                if (d & 0x10) c->handle_moved |= 1u << c->handle_count;
                c->handle_in[c->handle_count++] = c->in[position + i];
            }
        } else
            return WIRE_BAD;
        if (payload > end - position) return WIRE_BAD;
        if (size && snx_ctr_check_ram(c->process->space, c->in[position], size, access) != SNX_CTR_MEM_OK)
            return WIRE_ADDRESS;
        position += payload;
    }
    return WIRE_OK;
}
/* the --ipc-stubs name of each SNX_CTR_SERVICE_*: a file session is fs's */
static const char* const families[] = {"",    "srv", "fs",  "apt",    "ptm", "hid", "fs",
                                       "gsp", "cfg", "dsp", "system", "am",  "y2r", "ldr"};
_Static_assert(sizeof families / sizeof *families == SNX_CTR_SERVICE_COUNT, "a family name per service");
int snx_ctr_ipc_stub_mask(const char* list, uint32_t* mask) {
    uint32_t names = 0;
    if (!list || !*list) return 0;
    if (!strcmp(list, "all")) {
        *mask = UINT32_MAX;
        return 1;
    }
    for (;;) {
        size_t length = strcspn(list, ",");
        bool known = false;
        for (unsigned i = 1; i < sizeof families / sizeof *families; ++i)
            if (strlen(families[i]) == length && !strncmp(families[i], list, length)) {
                names |= 1u << i;
                known = true;
            }
        if (!known) return 0; /* also an empty name: a leading, doubled or trailing comma */
        if (!list[length]) break;
        list += length + 1;
    }
    *mask |= names;
    return 1;
}
bool snx_ctr_ipc_stubbed(const SnxCtrProcess* p, uint32_t service) {
    return g_settings.ctr_answer_unknown_commands || (service < 32 && (p->ipc_stubs >> service & 1));
}
SnxCtrSvcResult snx_ctr_ipc_send(SnxCtrProcess* p, uint32_t handle, uint32_t* result) {
    *result = 0;
    uint32_t id = snx_ctr_handle_object(p, handle);
    if (!id || (p->objects[id].type != CTR_OBJ_SESSION && p->objects[id].type != CTR_OBJ_FILE)) {
        *result = 0xd8e007f7u;
        return SNX_CTR_SVC_RETURN;
    }
    uint32_t tls = p->cpu.state.cpu.tpidruro;
    uint32_t address = tls + 0x80;
    unsigned permissions;
    unsigned char bytes[SNX_CTR_IPC_WORDS * 4];
    /* A 0x200-aligned TLS slot contains the entire 0x100-byte command buffer.
       Preflight RAM read AND write before a handler can change state/handles.
       All mappings, handlers and writes are serialized by the runtime. */
    if ((tls & 511) || (uint64_t)tls + 0x180 > 0x100000000ull ||
        snx_ctr_query(p->space, address, NULL, &permissions) != SNX_CTR_MEM_OK || (permissions & 3) != 3 ||
        snx_ctr_read_ram(p->space, address, bytes, sizeof bytes) != SNX_CTR_MEM_OK) {
        *result = 0xe0e01bf5u;
        return SNX_CTR_SVC_RETURN;
    }
    SnxCtrIpc c = {.process = p, .object = id, .caller_pid = CTR_PROCESS_ID};
    for (unsigned i = 0; i < SNX_CTR_IPC_WORDS; ++i) c.in[i] = snx_ld_le32(bytes + i * 4);
    p->last_ipc_header = c.in[0];
    p->last_ipc_service =
        p->objects[id].type == CTR_OBJ_FILE ? SNX_CTR_SERVICE_FILE : p->objects[id].data.session.service;
    uint32_t normal = (c.in[0] >> 6) & 63, translated = c.in[0] & 63;

    if ((c.in[0] & 0xf000) || 1 + normal + translated > SNX_CTR_IPC_WORDS) {
        *result = SNX_CTR_IPC_BAD_DESCRIPTOR;
        return SNX_CTR_SVC_RETURN;
    }
    unsigned wire = translation(&c, normal, translated);
    if (wire == WIRE_HANDLES) {
        fprintf(stderr, "3ds ipc: header 0x%08x sends more than %u handles, which the runtime does not translate\n",
                c.in[0], SNX_CTR_IPC_HANDLES);
        return SNX_CTR_SVC_UNSUPPORTED;
    }
    if (wire != WIRE_OK) {
        *result = wire == WIRE_BAD ? SNX_CTR_IPC_BAD_DESCRIPTOR : 0xe0e01bf5u; /* ResultInvalidAddress */
        if (g_settings.ctr_ipc_trace) {
            fprintf(stderr, "3ds ipc trace: thread %u header 0x%08x refused by the kernel (0x%08x):", (unsigned)p->current_thread,
                    c.in[0], *result);
            for (uint32_t i = 1; i <= normal + translated; ++i) fprintf(stderr, " %08x", c.in[i]);
            fputc('\n', stderr);
        }
        return SNX_CTR_SVC_RETURN;
    }
    switch (p->last_ipc_service) {
        case SNX_CTR_SERVICE_SRV: snx_ctr_srv_dispatch(&c); break;
        case SNX_CTR_SERVICE_FS: snx_ctr_fs_dispatch(&c); break;
        case SNX_CTR_SERVICE_FILE: snx_ctr_file_dispatch(&c); break;
        case SNX_CTR_SERVICE_APT: snx_ctr_apt_dispatch(&c); break;
        case SNX_CTR_SERVICE_PTM: snx_ctr_ptm_dispatch(&c); break;
        case SNX_CTR_SERVICE_HID: snx_ctr_hid_dispatch(&c); break;
        case SNX_CTR_SERVICE_GSP: snx_ctr_gsp_dispatch(&c); break;
        case SNX_CTR_SERVICE_DSP: snx_ctr_dsp_dispatch(&c); break;
        case SNX_CTR_SERVICE_CFG: snx_ctr_cfg_dispatch(&c); break;
        case SNX_CTR_SERVICE_SYSTEM: snx_ctr_system_dispatch(&c); break;
        case SNX_CTR_SERVICE_AM: snx_ctr_am_dispatch(&c); break;
        case SNX_CTR_SERVICE_Y2R: snx_ctr_y2r_dispatch(&c); break;
        case SNX_CTR_SERVICE_LDR_RO: snx_ctr_ldr_dispatch(&c); break;
        default: return SNX_CTR_SVC_UNSUPPORTED;
    }
    if (c.state == SNX_CTR_IPC_UNKNOWN) {
        const char* family =
            p->last_ipc_service < sizeof families / sizeof *families ? families[p->last_ipc_service] : "?";
        const char* kind = p->last_ipc_service == SNX_CTR_SERVICE_FILE ? " file" : "";
        uint32_t key = p->last_ipc_service << 16 | c.in[0] >> 16, seen = 0;
        if (!snx_ctr_ipc_stubbed(p, p->last_ipc_service)) {
            fprintf(stderr,
                    "3ds ipc: %s%s has no command 0x%04x (header 0x%08x). Run with --ipc-stubs %s (or "
                    "--ctr_auto_stub_ipc=1) to answer it with success.\n",
                    family, kind, c.in[0] >> 16, c.in[0], family);
            return SNX_CTR_SVC_UNSUPPORTED;
        }
        /* Azahar's ReportUnimplementedFunction: one normal word, success. Each command is reported the first time */
        while (seen < p->stub_logged_count && p->stub_logged[seen] != key) ++seen;
        if (seen == p->stub_logged_count) {
            if (seen < sizeof p->stub_logged / sizeof *p->stub_logged) p->stub_logged[p->stub_logged_count++] = key;
            fprintf(stderr, "3ds ipc: stub answered %s%s command 0x%04x (header 0x%08x)\n", family, kind, c.in[0] >> 16,
                    c.in[0]);
        }
        memset(c.out, 0, sizeof c.out);
        c.out[0] = (c.in[0] & 0xffff0000u) | 0x40;
        c.state = SNX_CTR_IPC_RETURN;
    }
    if (c.state == SNX_CTR_IPC_UNSUPPORTED) { /* a form of the command the handler doesn't take: what it was sent */
        const char* family =
            p->last_ipc_service < sizeof families / sizeof *families ? families[p->last_ipc_service] : "?";
        fprintf(stderr, "3ds ipc: %s command 0x%04x has a form the runtime does not take:", family, c.in[0] >> 16);
        for (uint32_t i = 0; i <= normal + translated; ++i) fprintf(stderr, " %08x", c.in[i]);
        fputc('\n', stderr);
        return SNX_CTR_SVC_UNSUPPORTED;
    }
    if (c.state == SNX_CTR_IPC_BLOCKED) return SNX_CTR_SVC_BLOCKED;
    if (c.state == SNX_CTR_IPC_FAULT) return SNX_CTR_SVC_UNSUPPORTED;
    /* The handles the request moved leave the caller's table once the service has taken the request, whatever it
       answers: the kernel moves them while it translates the request, before the service runs. They stay until
       here because handlers resolve them by value. */
    for (uint32_t i = 0; i < c.handle_count; ++i)
        if ((c.handle_moved >> i & 1) && c.handle_in[i]) (void)snx_ctr_handle_close(p, c.handle_in[i]);
    *result = c.transport_result;
    if (g_settings.ctr_ipc_trace) { /* the request and its answer: the reply's header and result, or the transport's */
        uint32_t service = p->last_ipc_service;
        fprintf(stderr, "3ds ipc trace: thread %u %s 0x%08x -> 0x%08x %s0x%08x\n", (unsigned)p->current_thread,
                service == SNX_CTR_SERVICE_SYSTEM ? snx_ctr_system_name(p, p->objects[id].data.session.name)
                : service < sizeof families / sizeof *families ? families[service]
                                                               : "?",
                c.in[0], *result ? 0 : c.out[0], *result ? "transport " : "", *result ? *result : c.out[1]);
    }
    if (*result) return SNX_CTR_SVC_RETURN;
    for (unsigned i = 0; i < SNX_CTR_IPC_WORDS; ++i) snx_st_le32(bytes + i * 4, c.out[i]);
    if (snx_ctr_write_ram(p->space, address, bytes, sizeof bytes) != SNX_CTR_MEM_OK) return SNX_CTR_SVC_UNSUPPORTED;
    return SNX_CTR_SVC_RETURN;
}
