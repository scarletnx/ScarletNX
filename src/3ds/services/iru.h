/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_CTR_IRU_H
#define SNX_CTR_IRU_H
#include "3ds/ipc/ctr_ipc.h"
/* ir:USER (services/iru.c): the infrared port with no Circle Pad Pro attached, as Azahar's is with no extra HID */
void snx_ctr_iru_dispatch(SnxCtrIpc* c);
#endif
