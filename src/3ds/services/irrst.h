/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_CTR_IRRST_H
#define SNX_CTR_IRRST_H
#include <stdint.h>
struct SnxCtrProcess;
/* ir:rst's sample when one is due by now_ns, every update_period ms from Initialize to Shutdown: ZL, ZR and the
   C-stick's direction bits, what they gained and lost since the sample before, and the C-stick's position, in the next
   entry of its shared memory, the update event signalled */
void snx_ctr_irrst_update(struct SnxCtrProcess* p, uint64_t now_ns);
/* the host monotonic ns of the next sample, UINT64_MAX for none */
uint64_t snx_ctr_irrst_deadline(const struct SnxCtrProcess* p);
#endif
