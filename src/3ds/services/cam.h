/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_CTR_CAM_H
#define SNX_CTR_CAM_H
#include "3ds/ipc/ctr_ipc.h"
struct SnxCtrProcess;
/* cam:u (services/cam.c): the cameras with no camera attached, their frames black */
void snx_ctr_cam_dispatch(SnxCtrIpc* c);
/* the receivings that end and the vsyncs that come by now_ns: the black frame written, the events signalled */
void snx_ctr_cam_update(struct SnxCtrProcess* p, uint64_t now_ns);
/* the host monotonic ns of the next of them, UINT64_MAX for none */
uint64_t snx_ctr_cam_deadline(const struct SnxCtrProcess* p);
#endif
