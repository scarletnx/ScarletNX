/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_CTR_MIC_H
#define SNX_CTR_MIC_H
#include "3ds/ipc/ctr_ipc.h"
/* mic:u (services/mic.c): the microphone with no input device, as Azahar's is with its "None" input */
void snx_ctr_mic_dispatch(SnxCtrIpc* c);
#endif
