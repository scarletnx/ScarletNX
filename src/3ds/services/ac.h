/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_CTR_AC_H
#define SNX_CTR_AC_H
#include "3ds/ipc/ctr_ipc.h"
void snx_ctr_ac_dispatch(SnxCtrIpc* c);
void snx_ctr_ac_close(struct SnxCtrProcess* p);
#endif
