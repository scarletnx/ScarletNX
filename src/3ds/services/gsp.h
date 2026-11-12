/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_CTR_GSP_H
#define SNX_CTR_GSP_H
#include "3ds/ipc/ctr_ipc.h"
#include <stdbool.h>
#include <stdint.h>
void snx_ctr_gsp_dispatch(SnxCtrIpc* c);
/* One host-paced vblank: shows latched framebuffers and queues PDC interrupts.
   Not cycle accurate. */
void snx_ctr_gsp_vblank(struct SnxCtrProcess* p);
/* a gsp::Gpu session's last reference went: it gives up the GPU right, its relay thread slot and its event (Azahar
   GSP_GPU::ClientDisconnected and SessionData's destructor) */
void snx_ctr_gsp_session_closed(struct SnxCtrProcess* p, uint32_t session);
/* The GX command an application's enqueue adds to its command queue at queue_va (the 0x20 bytes it copies there),
   run as TriggerCmdReqQueue runs the queue it makes: the command in its slot, the queue's index past it with nothing
   pending, its error and stop flag, the GPU's work submitted and its interrupts raised. DECLINED, with nothing
   changed, unless the queue is the GPU right holder's and the trigger would empty it now (nothing pending, not
   stopped): the application's code then queues and triggers it. STOPPED: the run ends (p->stopped says why) */
typedef enum { SNX_CTR_GX_DONE, SNX_CTR_GX_DECLINED, SNX_CTR_GX_STOPPED } SnxCtrGxQueued;
SnxCtrGxQueued snx_ctr_gsp_run_queued(struct SnxCtrProcess* p, uint32_t queue_va, const unsigned char command[0x20]);
#endif
