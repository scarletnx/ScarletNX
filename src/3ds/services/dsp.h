/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_CTR_DSP_SERVICE_H
#define SNX_CTR_DSP_SERVICE_H
#include "3ds/ipc/ctr_ipc.h"
#include <stdint.h>
struct SnxCtrProcess;
/* Past this much behind the host clock, the frames missed are dropped instead of being caught up */
#define SNX_CTR_DSP_MAX_LATE_NS 100000000u
/* Before the first dsp::DSP session: makes the HLE DSP, whose frames run from then on */
uint32_t snx_ctr_dsp_open(struct SnxCtrProcess* p);
void snx_ctr_dsp_dispatch(SnxCtrIpc* c);
/* When the next audio frame runs (snx_ctr_schedule): at p->dsp.next_frame_ns once the application has answered the
   frame before, else one frame after that one at the earliest. The console's application answers every frame before
   the next; run back to back after the host fell behind, two frames raise one interrupt (the event is still
   signaled) and the second's statuses overwrite the first's, so the application refills its streams late. A frame
   behind catches up as fast as the application answers. UINT64_MAX without a DSP */
uint64_t snx_ctr_dsp_deadline(struct SnxCtrProcess* p);
/* One host-paced audio frame, due at snx_ctr_dsp_deadline. Not cycle accurate */
void snx_ctr_dsp_frame(struct SnxCtrProcess* p, uint64_t now_ns);
/* libctru's ndsp loads sdmc:/3ds/dspfirm.cdc and gives up on audio without it: a stand-in goes there when the title's
   SD has none (the HLE DSP never runs the component) */
void snx_ctr_dsp_component_file(struct SnxCtrProcess* p);
void snx_ctr_dsp_close(struct SnxCtrProcess* p);
#endif
