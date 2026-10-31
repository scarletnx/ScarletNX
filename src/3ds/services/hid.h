/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_CTR_HID_H
#define SNX_CTR_HID_H
#include <stdint.h>
struct SnxCtrProcess;
/* hid:USER's samples due by now_ns written to its shared memory, each with its event signalled: the pad's every 4.27
   ms from the first GetIPCHandles, the accelerometer's and the gyroscope's while a game has them enabled */
void snx_ctr_hid_update(struct SnxCtrProcess* p, uint64_t now_ns);
/* the host monotonic ns of the next sample, UINT64_MAX for none */
uint64_t snx_ctr_hid_deadline(const struct SnxCtrProcess* p);
/* A stick at raw position (x, y), -156 to 156 on each axis (0x9c is the rim), as the pad state's four direction bits
   in the circle pad's place (bits 28 to 31: right, left, up, down), none near the center (Azahar's
   GetStickDirectionState). The C-stick's are the same shifted down 4 bits */
uint32_t snx_ctr_stick_directions(int32_t x, int32_t y);
#endif
