/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_CTR_SOC_H
#define SNX_CTR_SOC_H
#include "3ds/ipc/ctr_ipc.h"
#include <stdbool.h>
struct SnxCtrSoc;
void snx_ctr_soc_dispatch(SnxCtrIpc* c);
/* the blocked calls whose socket became ready, or whose time ran out: answered and their threads woken. Called on
   each of the scheduler's passes, before it looks at deadlines */
void snx_ctr_soc_poll(struct SnxCtrProcess* p);
/* whether a thread waits on a socket: the scheduler then wakes every millisecond to look again */
bool snx_ctr_soc_waiting(const struct SnxCtrProcess* p);
/* the sockets the game has open */
unsigned snx_ctr_soc_open(const struct SnxCtrProcess* p);
void snx_ctr_soc_close(struct SnxCtrProcess* p);
/* The name a game resolves, as the console's service and its modules see it: Pretendo's rewrite of the NAT check hosts
   with ctr_online=pretendo, then NULL for the console maker's own domains, which resolve to nothing */
const char* snx_ctr_soc_host(const char* name);
/* whether the console is online: network=1 and not in airplane mode */
bool snx_ctr_online(void);
#endif
