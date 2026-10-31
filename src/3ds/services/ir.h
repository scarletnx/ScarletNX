/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_CTR_IR_H
#define SNX_CTR_IR_H
#include <stdint.h>
struct SnxCtrProcess;
/* ir:rst's sample, when it is due by now_ns: the pad state (ZL, ZR and the C-stick's directions) and the C-stick's
   position in the next entry of its shared memory, the update event signalled. Every update_period ms from Initialize
   to Shutdown */
void snx_ctr_ir_update(struct SnxCtrProcess* p, uint64_t now_ns);
/* the host monotonic ns of the next sample, UINT64_MAX for none */
uint64_t snx_ctr_ir_deadline(const struct SnxCtrProcess* p);
#endif
