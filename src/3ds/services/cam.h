/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_CTR_CAM_H
#define SNX_CTR_CAM_H
#include "3ds/ipc/ctr_ipc.h"
#include <stdint.h>
/* cam:u (services/cam.c): the cameras, with Azahar's blank camera behind them */
void snx_ctr_cam_dispatch(SnxCtrIpc* c);
/* an activated port's vsync and a receiving's end, when due by now_ns */
void snx_ctr_cam_update(struct SnxCtrProcess* p, uint64_t now_ns);
/* the host monotonic ns of the next of them, UINT64_MAX for none */
uint64_t snx_ctr_cam_deadline(const struct SnxCtrProcess* p);
#endif
