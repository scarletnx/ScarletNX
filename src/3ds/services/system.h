/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_CTR_SYSTEM_H
#define SNX_CTR_SYSTEM_H
#include "3ds/ipc/ctr_ipc.h"
#include "3ds/runtime/ctr_process.h"
#define SNX_CTR_SYSTEM_UNKNOWN 0xffffffffu
/* A session of the system service by that name, one the runtime has no model of: its result, or
   SNX_CTR_SYSTEM_UNKNOWN for a name that is no such service */
uint32_t snx_ctr_system_open(SnxCtrProcess* p, const uint8_t* name, uint32_t length, uint32_t* session);
/* A stand-in session for a service by a name the runtime does not know at all, whose commands answer as a system
   service's do, as a Switch game's stand-in does (sm.c): SNX_CTR_SYSTEM_UNKNOWN when stubs are off for it (the console
   would wait for the service to register) or the stand-ins are all taken */
uint32_t snx_ctr_system_stand_in(SnxCtrProcess* p, const uint8_t* name, uint32_t length, uint32_t* session);
const char* snx_ctr_system_name(const SnxCtrProcess* p, uint32_t index);
/* A command the runtime has no model of: success and zeros, as Azahar's ReportUnimplementedFunction answers, reported
   the first time each key (the service and the command) is seen */
void snx_ctr_system_unmodeled(SnxCtrIpc* c, const char* service, uint32_t key);
void snx_ctr_system_dispatch(SnxCtrIpc* c);
#endif
