/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_CTR_FRD_H
#define SNX_CTR_FRD_H
#include "3ds/ipc/ctr_ipc.h"
#include <stdbool.h>
struct SnxCtrFrd;
void snx_ctr_frd_dispatch(SnxCtrIpc* c);
/* a game's login whose answer came: its event signalled. Called on each of the scheduler's passes */
void snx_ctr_frd_poll(struct SnxCtrProcess* p);
/* whether a login is on its way: the scheduler then wakes every millisecond to look again */
bool snx_ctr_frd_waiting(const struct SnxCtrProcess* p);
void snx_ctr_frd_close(struct SnxCtrProcess* p);
#endif
