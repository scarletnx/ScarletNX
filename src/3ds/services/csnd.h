/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_CTR_CSND_H
#define SNX_CTR_CSND_H
#include "3ds/ipc/ctr_ipc.h"
/* csnd:SND (services/csnd.c): the old sound hardware's service, its shared memory and command lists, with no sound */
void snx_ctr_csnd_dispatch(SnxCtrIpc* c);
#endif
