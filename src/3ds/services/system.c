/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The console's system services the runtime has no model of yet. Each is there, as on the console, so the game's
   GetServiceHandle for it gets a session, and each command it sends is answered with success and nothing more (zeros
   in every output word), as Azahar's ReportUnimplementedFunction answers, reported the first time. A service whose
   answers a game depends on gets a model of its own, and leaves this list. */
#include "3ds/services/system.h"
#include "3ds/services/uds.h"
#include "3ds/runtime/internal.h"
#include <stdio.h>
#include <string.h>

/* the services of a retail console an application may name in its ExHeader (3dbrew's Services list) */
static const char* const names[] = {
    "ac:u",     "ac:i",     "act:u",    "boss:U", "boss:P", "cam:u",  "cecd:u",  "cecd:s", "csnd:SND",
    "dlp:FKCL", "dlp:SRVR", "dlp:CLNT", "frd:u",  "frd:a",  "http:C", "ir:USER", "ir:u",
    "ldr:ro",   "mic:u",    "mvd:STD",  "ndm:u",  "news:u", "news:s", "nim:aoc", "nim:u",  "nwm::UDS",
    "pxi:dev",  "qtm:u",    "soc:U",    "ssl:C",  "nfc:u",  "nfc:m",  "cdc:CSN",
};
_Static_assert(sizeof names / sizeof *names + 16 < 256, "a name index per session, stand-ins included");
uint32_t snx_ctr_system_open(SnxCtrProcess* p, const uint8_t* name, uint32_t length, uint32_t* session) {
    for (uint32_t i = 0; i < sizeof names / sizeof *names; ++i)
        if (strlen(names[i]) == length && !memcmp(names[i], name, length)) {
            uint32_t result = snx_ctr_session_create(p, SNX_CTR_SERVICE_SYSTEM, session);
            if (!result) p->objects[snx_ctr_handle_object(p, *session)].data.session.name = i;
            if (!result && strcmp(names[i], "nwm::UDS"))
                fprintf(stderr, "3ds srv: %s has no model: its commands answer success\n", names[i]);
            return result;
        }
    return SNX_CTR_SYSTEM_UNKNOWN;
}
uint32_t snx_ctr_system_stand_in(SnxCtrProcess* p, const uint8_t* name, uint32_t length, uint32_t* session) {
    const uint32_t known = sizeof names / sizeof *names;
    if (!snx_ctr_ipc_stubbed(p, SNX_CTR_SERVICE_SYSTEM) || length >= sizeof *p->stand_ins)
        return SNX_CTR_SYSTEM_UNKNOWN;
    uint32_t i = 0;
    while (i < p->stand_in_count && (strlen(p->stand_ins[i]) != length || memcmp(p->stand_ins[i], name, length))) ++i;
    if (i == sizeof p->stand_ins / sizeof *p->stand_ins) return SNX_CTR_SYSTEM_UNKNOWN;
    if (i == p->stand_in_count) {
        memcpy(p->stand_ins[i], name, length);
        p->stand_ins[i][length] = 0;
        ++p->stand_in_count;
    }
    uint32_t result = snx_ctr_session_create(p, SNX_CTR_SERVICE_SYSTEM, session);
    if (!result) {
        p->objects[snx_ctr_handle_object(p, *session)].data.session.name = known + i;
        fprintf(stderr, "3ds srv: the runtime has no service %s: its commands answer success\n", p->stand_ins[i]);
    }
    return result;
}
const char* snx_ctr_system_name(const SnxCtrProcess* p, uint32_t index) {
    const uint32_t known = sizeof names / sizeof *names;
    return index < known ? names[index] : index - known < p->stand_in_count ? p->stand_ins[index - known] : "?";
}
void snx_ctr_system_dispatch(SnxCtrIpc* c) {
    SnxCtrProcess* p = c->process;
    if (!strcmp(snx_ctr_system_name(p, p->objects[c->object].data.session.name), "nwm::UDS")) {
        snx_ctr_uds_dispatch(c);
        return;
    }
    uint32_t name = p->objects[c->object].data.session.name, command = c->in[0] >> 16;
    uint32_t key = (SNX_CTR_SERVICE_COUNT + name) << 16 | command, seen = 0;
    while (seen < p->stub_logged_count && p->stub_logged[seen] != key) ++seen;
    if (seen == p->stub_logged_count) {
        if (seen < sizeof p->stub_logged / sizeof *p->stub_logged) p->stub_logged[p->stub_logged_count++] = key;
        fprintf(stderr, "3ds ipc: %s command 0x%04x (header 0x%08x) answered success\n", snx_ctr_system_name(p, name),
                command, c->in[0]);
    }
    memset(c->out, 0, sizeof c->out);
    c->out[0] = command << 16 | 0x40;
}
