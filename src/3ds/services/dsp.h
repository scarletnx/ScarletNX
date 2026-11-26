/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_CTR_DSP_SERVICE_H
#define SNX_CTR_DSP_SERVICE_H
#include "3ds/audio/dsp.h"
#include "3ds/ipc/ctr_ipc.h"
#include <stddef.h>
#include <stdint.h>
struct SnxCtrProcess;
/* Before the first dsp::DSP session: makes the HLE DSP, whose frames run from then on */
uint32_t snx_ctr_dsp_open(struct SnxCtrProcess* p);
void snx_ctr_dsp_dispatch(SnxCtrIpc* c);
/* One host-paced audio frame, due at p->dsp.next_frame_ns (snx_ctr_schedule). Not cycle accurate */
void snx_ctr_dsp_frame(struct SnxCtrProcess* p, uint64_t now_ns);
/* libctru's ndsp loads sdmc:/3ds/dspfirm.cdc and gives up on audio without it: a stand-in goes there when the title's
   SD has none (the HLE DSP never runs the component) */
void snx_ctr_dsp_component_file(struct SnxCtrProcess* p);
void snx_ctr_dsp_close(struct SnxCtrProcess* p);
/* the DSP of a save state, read into a core of its own that raises p's interrupts (audio/dsp.h's state parse) */
struct StateIo;
SnxCtrDsp* snx_ctr_dsp_state_open(struct SnxCtrProcess* p, struct StateIo* io, char* why, size_t whysize);
#endif
