/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_CTR_CSND_H
#define SNX_CTR_CSND_H
#include "3ds/ipc/ctr_ipc.h"
#include <stdint.h>
/* csnd:SND (services/csnd.c): the CSND sound channels, rendered and mixed into the DSP's output */
void snx_ctr_csnd_dispatch(SnxCtrIpc* c);
/* the fade's next step and the hardware's frame, when due by now_ns */
void snx_ctr_csnd_update(struct SnxCtrProcess* p, uint64_t now_ns);
/* the host monotonic ns of the next of them, UINT64_MAX for none */
uint64_t snx_ctr_csnd_deadline(const struct SnxCtrProcess* p);
#endif
