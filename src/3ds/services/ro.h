/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_CTR_RO_H
#define SNX_CTR_RO_H
#include "3ds/ipc/ctr_ipc.h"
/* ldr:ro (services/ro.c): the loader of CRO libraries, which it maps, relocates and links into the game */
void snx_ctr_ro_dispatch(SnxCtrIpc* c);
#endif
