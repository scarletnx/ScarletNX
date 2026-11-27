/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_CTR_MIC_H
#define SNX_CTR_MIC_H
#include "3ds/ipc/ctr_ipc.h"
#include <stdint.h>
/* mic:u (services/mic.c): the microphone, recording the host's (common/utilities/mic.h) into the block a game lent */
void snx_ctr_mic_dispatch(SnxCtrIpc* c);
/* the samples due by now_ns while it samples, into the block, its offset after them in the block's last word */
void snx_ctr_mic_update(struct SnxCtrProcess* p, uint64_t now_ns);
/* the host monotonic ns the next samples are due, UINT64_MAX for none */
uint64_t snx_ctr_mic_deadline(const struct SnxCtrProcess* p);
/* the recording of a state just loaded: the one before closed, and one opened at the state's rate when it samples */
void snx_ctr_mic_reopen(struct SnxCtrProcess* p);
/* the host's recording closed, when the process ends */
void snx_ctr_mic_close(struct SnxCtrProcess* p);
#endif
